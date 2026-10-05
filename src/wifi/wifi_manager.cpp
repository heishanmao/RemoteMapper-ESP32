#include "wifi_manager.h"
#include "app_config.h"
#include "log/app_log.h"
#include "led_indicator.h"
#include "ble/ble_remote_client.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_coexist.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <vector>

static_assert((int)WIFI_POLICY_ON_DEMAND == WIFI_DEFAULT_POLICY,
              "WIFI_DEFAULT_POLICY must match WIFI_POLICY_ON_DEMAND");

#define DNS_PORT        53
#define MDNS_HOSTNAME   "remotemapper"

static DNSServer        s_dns_server;
static Preferences      s_prefs;
static bool             s_prefs_ready = false;
static IPAddress        s_ap_ip(192, 168, 4, 1);
static IPAddress        s_ap_netmask(255, 255, 255, 0);

static bool             s_sta_configured = false;
static uint32_t         s_last_sta_check = 0;
static bool             s_wifi_enabled = true;
static bool             s_ap_running = false;
static bool             s_mdns_started = false;
static uint32_t         s_sta_disconnected_since = 0;

// Wi-Fi Power Management state
static wifi_policy_t      s_policy           = (wifi_policy_t)WIFI_DEFAULT_POLICY;
static uint32_t           s_timeout_min      = WIFI_DEFAULT_TIMEOUT_MIN;
static bool               s_timeout_enabled  = WIFI_DEFAULT_TIMEOUT_ENABLED ? true : false;
static wifi_radio_state_t s_radio_state      = WIFI_STATE_OFF;
static uint32_t           s_last_activity_ms = 0;

enum wifi_scan_state_t {
    WIFI_SCAN_IDLE = 0,
    WIFI_SCAN_WAIT_RADIO,
    WIFI_SCAN_RUNNING_STATE,
    WIFI_SCAN_COMPLETE_STATE,
    WIFI_SCAN_FAILED_STATE
};
static const size_t WIFI_SCAN_CACHE_MAX = 32;
static wifi_scan_state_t s_scan_state = WIFI_SCAN_IDLE;
static String s_scan_ssid[WIFI_SCAN_CACHE_MAX];
static int16_t s_scan_rssi[WIFI_SCAN_CACHE_MAX] = {};
static bool s_scan_secure[WIFI_SCAN_CACHE_MAX] = {};
static size_t s_scan_count = 0;
static int s_scan_error = 0;
static const char* s_scan_error_reason = "";
static uint32_t s_scan_deadline_ms = 0;
static uint32_t s_scan_completed_at_ms = 0;
static bool s_scan_has_complete = false;

// Deferred radio wake requested by the USB stack (host re-enumeration).
// Consumed by wifi_manager_task() in the main-loop context so it can never
// race the synchronous radio bring-up inside wifi_manager_init() at boot.
static volatile bool      s_pending_usb_wake = false;
// Manual wake requested by the key-press gesture. fire detected from the
// BLE/audio task; deferred here so the radio re-init (WiFi.mode/begin, can
// block for seconds) never stalls audio pumping on Core 0.
static volatile bool      s_pending_manual_wake = false;
// Generic on-demand wake requested while the radio was OFF. The ACTUAL radio
// bring-up must run from wifi_manager_task() (main-loop context). wifi_manager
// is the single owner of the WiFi stack: waking from a separate task made
// WiFi.mode()/softAP churn run concurrently with loop()'s wifi_manager_task()
// and web handlers, which races esp_wifi de-init against itself and deadlocks
// the driver while NimBLE is active ("timeout when WiFi un-init" + Task WDT
// panic inside esp_wifi_deinit_internal on the coredump we recovered).
static volatile bool      s_pending_radio_wake = false;

static void wifi_apply_config(void);    // forward decl (defined below)
static void wifi_reconnect_light(void); // forward decl (defined below)
static void wifi_scan_tick(void);

// The manager deadline covers both radio wake and the scan itself. The Arduino
// scan API retains its default per-channel dwell; a scanComplete() failure is
// handled by stopping the driver scan in the owner task below.
static const uint32_t WIFI_SCAN_DEADLINE_MS = 15000;

static bool wifi_is_valid_timeout(uint32_t minutes) {
    return minutes == WIFI_TIMEOUT_NEVER || minutes == 1 || minutes == 2 ||
           minutes == 5 || minutes == 10 || minutes == 30;
}

static void wifi_start_ap(const String& ap_pass) {
    if (s_ap_running) {
        s_dns_server.stop();
        WiFi.softAPdisconnect(false);
    }
    WiFi.mode(s_sta_configured ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAPConfig(s_ap_ip, s_ap_ip, s_ap_netmask);
    if (ap_pass.length() >= 8) {
        WiFi.softAP(AP_SSID, ap_pass.c_str());
        app_log("WIFI", "AP Started: %s (WPA2-PSK, IP: 192.168.4.1)", AP_SSID);
    } else {
        WiFi.softAP(AP_SSID, "");
        app_log("WIFI", "AP Started: %s (Open Network, IP: 192.168.4.1)", AP_SSID);
    }
    s_dns_server.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns_server.start(DNS_PORT, "*", s_ap_ip);
    s_ap_running = true;
}

static void wifi_stop_ap(void) {
    if (!s_ap_running) return;
    s_dns_server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_ap_running = false;
    app_log("WIFI", "STA connected! SoftAP '%s' closed to optimize power.", AP_SSID);
    app_log("WIFI", "Access Web UI via: http://%s or http://%s.local", WiFi.localIP().toString().c_str(), MDNS_HOSTNAME);
}

static void wifi_apply_config(void) {
    String sta_ssid = s_prefs.getString("ssid", "");
    String sta_pass = s_prefs.getString("pass", "");
    String ap_pass  = s_prefs.getString("ap_pass", "");

    // Set Wi-Fi Mode (AP + STA)
    WiFi.mode(WIFI_AP_STA);

    // 1. Configure and start AP Mode
    wifi_start_ap(ap_pass);

    // 2. Connect to Home Wi-Fi if saved
    if (sta_ssid.length() > 0) {
        s_sta_configured = true;
        app_log("WIFI", "Connecting to Home Wi-Fi: %s...", sta_ssid.c_str());
        WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());
    } else {
        app_log("WIFI", "No Home Wi-Fi configured, running in AP Setup mode");
    }

    // 3. Start mDNS (once per boot)
    if (!s_mdns_started && MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        app_log("WIFI", "mDNS responder started: http://%s.local", MDNS_HOSTNAME);
        s_mdns_started = true;
    }

    // 4. Power optimizations: modem sleep + capped TX power (device stays near PC/router)
    WiFi.setSleep(true);
    if (WiFi.setTxPower(WIFI_POWER_8_5dBm)) {
        app_log("WIFI", "TX power capped to 8.5dBm, modem sleep enabled");
    } else {
        app_log("WIFI", "Modem sleep enabled (TX power set failed)");
    }
}

// Light wake for ON_DEMAND radio sleep. Sleep keeps the WiFi driver initialized
// (only disconnect + modem sleep), so waking must NOT tear the radio down:
// WiFi.mode()/softAP re-init churn is what deadlocked esp_wifi against NimBLE
// and hard-froze the device. Here we just reconnect the STA (begin is
// non-blocking, the link comes up via events) and leave mode/AP untouched.
static void wifi_reconnect_light(void) {
    String sta_ssid = s_prefs.getString("ssid", "");
    String sta_pass = s_prefs.getString("pass", "");
    if (sta_ssid.length() > 0) {
        s_sta_configured = true;
        app_log("WIFI", "Wake: light reconnect to %s (no radio re-init)...", sta_ssid.c_str());
        WiFi.setSleep(true);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        WiFi.disconnect(false);
        WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());
        return;
    }
    // No saved STA network: first-run/config mode, do the full bring-up.
    app_log("WIFI", "Wake: no STA configured, running full radio config");
    wifi_apply_config();
}

void wifi_manager_init(void) {
    s_prefs_ready = s_prefs.begin("wifi_conf", false);
    if (!s_prefs_ready) {
        app_log("WIFI", "Could not open Wi-Fi preferences; persistent settings are unavailable");
    }

    // The ESP32-S3 has a single 2.4 GHz radio shared by WiFi and BLE. With the
    // default WiFi-preferred arbitration, a WiFi beacon or data TX during a BLE
    // connection event costs us that event's notification, and BLE notifications
    // are never retransmitted. For this device BLE carries the microphone stream,
    // so a lost notification is a permanent hole in the ADPCM stream that no
    // amount of post-processing can fill, which is what limited speech
    // recognition accuracy. Prefer BT and let the WebUI take the slower path:
    // it is all request/response traffic that tolerates a little more latency.
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    // Policy and timeout are normally seeded by config_manager_init() migration.
    // Defensive defaults keep the manager robust if that ever did not run.
    uint32_t stored_policy = s_prefs.getUInt("policy", WIFI_DEFAULT_POLICY);
    if (stored_policy > WIFI_POLICY_DISABLED) {
        stored_policy = WIFI_DEFAULT_POLICY;
    }
    s_policy = (wifi_policy_t)stored_policy;

    uint32_t stored_timeout = s_prefs.getUInt("timeout_min", WIFI_DEFAULT_TIMEOUT_MIN);
    if (!wifi_is_valid_timeout(stored_timeout)) {
        stored_timeout = WIFI_DEFAULT_TIMEOUT_MIN;
    }
    s_timeout_min = stored_timeout;

    // Auto-shutdown is opt-in: units that never had the key boot with the
    // switch OFF, so the radio stays up and is only powered down by the user.
    s_timeout_enabled = s_prefs.getBool(
        "timeout_en", WIFI_DEFAULT_TIMEOUT_ENABLED ? true : false);

    s_wifi_enabled = (s_policy != WIFI_POLICY_DISABLED);

    if (!s_wifi_enabled) {
        // Never touch the WiFi stack when DISABLED: de-initializing WiFi before BLE
        // starts (or tearing it down at runtime) breaks 2.4GHz coexistence and can
        // stop the BLE link from coming up. The radio simply stays uninitialized.
        s_radio_state = WIFI_STATE_OFF;
        app_log("WIFI", "Wi-Fi policy DISABLED; radio left uninitialized (USB CDC/UART: 'wifi on')");
        led_indicator_set_wifi_sleep(true);
        return;
    }

    // ON_DEMAND boots the radio on; the idle timeout in wifi_manager_task()
    // powers it down and any user activity (wifi_manager_request_wifi) wakes it.
    wifi_apply_config();
    s_radio_state = WIFI_STATE_ON;
    s_last_activity_ms = millis();
    led_indicator_set_wifi_sleep(false);
    app_log("WIFI", "Wi-Fi policy %s (timeout %s%u min, state %s)",
            wifi_manager_policy_str(s_policy),
            s_timeout_enabled ? "" : "(off) ", (unsigned int)s_timeout_min,
            wifi_manager_state_str(s_radio_state));
}

bool wifi_manager_get_enabled(void) {
    return s_policy != WIFI_POLICY_DISABLED;
}

bool wifi_manager_is_ap_running(void) {
    return s_ap_running;
}

bool wifi_manager_set_enabled(bool enabled) {
    // Legacy whole-radio switch; "enabled" now maps onto the Wi-Fi policy.
    // DISABLED keeps the radio off, any other policy keeps it available.
    return wifi_manager_set_policy(enabled ? WIFI_POLICY_ON_DEMAND : WIFI_POLICY_DISABLED);
}

wifi_policy_t wifi_manager_get_policy(void) {
    return s_policy;
}

bool wifi_manager_set_policy(wifi_policy_t policy) {
    if ((uint32_t)policy > WIFI_POLICY_DISABLED) {
        return false;
    }
    return wifi_manager_update_power_config(true, policy, false, 0, false, false);
}

uint32_t wifi_manager_get_timeout_min(void) {
    return s_timeout_min;
}

bool wifi_manager_set_timeout_min(uint32_t minutes) {
    if (!wifi_is_valid_timeout(minutes)) {
        return false;
    }
    return wifi_manager_update_power_config(false, WIFI_POLICY_ON_DEMAND,
            true, minutes, false, false);
}

bool wifi_manager_get_timeout_enabled(void) {
    return s_timeout_enabled;
}

bool wifi_manager_set_timeout_enabled(bool enabled) {
    return wifi_manager_update_power_config(false, WIFI_POLICY_ON_DEMAND,
            false, 0, true, enabled);
}

wifi_radio_state_t wifi_manager_get_radio_state(void) {
    return s_radio_state;
}

void wifi_manager_mark_activity(void) {
    s_last_activity_ms = millis();
}

// Ring of recent key-press times used by the wake gesture (same-key match).
static uint16_t s_gesture_key  = 0;
static uint32_t s_press_times[WIFI_WAKE_PRESS_THRESHOLD] = {0};
static uint8_t  s_press_count = 0;

void wifi_manager_notify_key_press(uint16_t gesture_key) {
    uint32_t now = millis();

    if (s_radio_state == WIFI_STATE_ON) {
        // Radio already up: a press is ordinary activity, keep it alive.
        wifi_manager_mark_activity();
        return;
    }
    if (s_policy == WIFI_POLICY_DISABLED) {
        return;
    }

    // Radio powered down: accumulate presses of the SAME key.
    if (s_press_count > 0) {
        if ((now - s_press_times[0]) > WIFI_WAKE_PRESS_WINDOW_MS ||
            gesture_key != s_gesture_key) {
            // Gesture window expired or a different key -> start a fresh gesture.
            s_press_count = 0;
            s_gesture_key = gesture_key;
        }
    } else {
        s_gesture_key = gesture_key;
    }
    if (s_press_count < WIFI_WAKE_PRESS_THRESHOLD) {
        s_press_times[s_press_count++] = now;
    }
    if (s_press_count >= WIFI_WAKE_PRESS_THRESHOLD) {
        app_log("WIFI", "Wake gesture detected (%d quick presses of same key) -> waking radio",
                (int)WIFI_WAKE_PRESS_THRESHOLD);
        s_press_count = 0;
        // Deferred: the actual radio bring-up runs in wifi_manager_task()
        // (main-loop context) so it never stalls the BLE/audio task on Core 0.
        s_pending_manual_wake = true;
    }
}

uint32_t wifi_manager_get_last_activity_ms(void) {
    return s_last_activity_ms;
}

bool wifi_manager_request_wifi(wifi_wake_reason_t reason) {
    if (s_policy == WIFI_POLICY_DISABLED) {
        app_log("WIFI", "Wi-Fi request ignored: policy DISABLED (reason=%d)", (int)reason);
        return false;
    }
    // On-demand wake: radio was powered down after idle timeout, bring it back.
    // The actual reconnect runs in wifi_manager_task() (main-loop context), the
    // single owner of the WiFi stack. Here we only mark the request -- safe to
    // call from the Core 0 BLE task.
    if (s_radio_state != WIFI_STATE_ON) {
        if (s_radio_state == WIFI_STATE_ENABLING) {
            wifi_manager_mark_activity(); // already waking
            return true;
        }
        s_radio_state = WIFI_STATE_ENABLING;
        s_pending_radio_wake = true;
        app_log("WIFI", "On-demand wake requested (reason=%d), radio bringing up on main loop...", (int)reason);
        return true;
    }
    wifi_manager_mark_activity();
    return s_radio_state != WIFI_STATE_OFF;
}

void wifi_manager_notify_usb_mounted(void) {
    // Only set the flag; the actual radio wake is deferred to
    // wifi_manager_task() (main-loop context) to avoid racing the
    // synchronous bring-up in wifi_manager_init() during boot.
    s_pending_usb_wake = true;
}

const char* wifi_manager_policy_str(wifi_policy_t policy) {
    switch (policy) {
        case WIFI_POLICY_ALWAYS_ON: return "always_on";
        case WIFI_POLICY_ON_DEMAND: return "on_demand";
        case WIFI_POLICY_DISABLED:  return "disabled";
        default:                    return "unknown";
    }
}

const char* wifi_manager_state_str(wifi_radio_state_t state) {
    switch (state) {
        case WIFI_STATE_OFF:           return "off";
        case WIFI_STATE_ENABLING:      return "enabling";
        case WIFI_STATE_ON:            return "on";
        case WIFI_STATE_SHUTTING_DOWN: return "shutting_down";
        default:                       return "unknown";
    }
}

static bool prefs_read_string_key(const char* key, bool* exists, String* value);
static bool prefs_read_uint_key(const char* key, bool* exists, uint32_t* value);
static bool prefs_read_bool_key(const char* key, bool* exists, bool* value);

static bool prefs_put_string_verified(const char* key, const String& value) {
    if (!s_prefs_ready) return false;
    bool existed = false;
    String current;
    if (!prefs_read_string_key(key, &existed, &current)) return false;
    if (existed && current == value) return true;
    nvs_handle_t handle;
    if (nvs_open("wifi_conf", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t write_result = nvs_set_str(handle, key, value.c_str());
    if (write_result == ESP_OK) write_result = nvs_commit(handle);
    nvs_close(handle);
    if (write_result != ESP_OK) return false;
    bool verify_exists = false;
    String verify;
    return prefs_read_string_key(key, &verify_exists, &verify) && verify_exists && verify == value;
}

static bool prefs_put_uint_verified(const char* key, uint32_t value) {
    if (!s_prefs_ready) return false;
    bool existed = false;
    uint32_t current = 0;
    if (!prefs_read_uint_key(key, &existed, &current)) return false;
    if (existed && current == value) return true;
    size_t written = s_prefs.putUInt(key, value);
    if (written != sizeof(value)) return false;
    bool verify_exists = false;
    uint32_t verify = 0;
    return prefs_read_uint_key(key, &verify_exists, &verify) && verify_exists && verify == value;
}

static bool prefs_put_bool_verified(const char* key, bool value) {
    if (!s_prefs_ready) return false;
    bool existed = false;
    bool current = false;
    if (!prefs_read_bool_key(key, &existed, &current)) return false;
    if (existed && current == value) return true;
    size_t written = s_prefs.putBool(key, value);
    if (written != sizeof(uint8_t)) return false;
    bool verify_exists = false;
    bool verify = false;
    return prefs_read_bool_key(key, &verify_exists, &verify) && verify_exists && verify == value;
}

static bool prefs_open_readonly(nvs_handle_t* handle) {
    return s_prefs_ready && handle && nvs_open("wifi_conf", NVS_READONLY, handle) == ESP_OK;
}

static bool prefs_read_string_key(const char* key, bool* exists, String* value) {
    if (!exists || !value) return false;
    *exists = false;
    if (!s_prefs_ready) return false;
    nvs_handle_t handle;
    if (!prefs_open_readonly(&handle)) return false;
    size_t length = 0;
    esp_err_t result = nvs_get_str(handle, key, nullptr, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        *value = "";
        return true;
    }
    if (result != ESP_OK || length == 0 || length > 4096) {
        nvs_close(handle);
        return false;
    }
    std::vector<char> buffer(length);
    result = nvs_get_str(handle, key, buffer.data(), &length);
    nvs_close(handle);
    if (result != ESP_OK || length == 0 || buffer[length - 1] != '\0') return false;
    *value = String(buffer.data());
    *exists = true;
    return true;
}

static bool prefs_read_uint_key(const char* key, bool* exists, uint32_t* value) {
    if (!exists || !value) return false;
    *exists = false;
    if (!s_prefs_ready) return false;
    nvs_handle_t handle;
    if (!prefs_open_readonly(&handle)) return false;
    uint32_t current = 0;
    const esp_err_t result = nvs_get_u32(handle, key, &current);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    *exists = true;
    *value = current;
    return true;
}

static bool prefs_read_bool_key(const char* key, bool* exists, bool* value) {
    if (!exists || !value) return false;
    *exists = false;
    if (!s_prefs_ready) return false;
    nvs_handle_t handle;
    if (!prefs_open_readonly(&handle)) return false;
    uint8_t current = 0;
    const esp_err_t result = nvs_get_u8(handle, key, &current);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    if (current > 1) return false;
    *exists = true;
    *value = current != 0;
    return true;
}

static bool prefs_restore_string(const char* key, bool existed, const String& value) {
    if (existed) return prefs_put_string_verified(key, value);
    if (!s_prefs_ready) return false;
    bool remains = false;
    String ignored;
    if (!prefs_read_string_key(key, &remains, &ignored)) return false;
    if (!remains) return true;
    if (!s_prefs.remove(key)) return false;
    return prefs_read_string_key(key, &remains, &ignored) && !remains;
}

static bool prefs_restore_uint(const char* key, bool existed, uint32_t value) {
    if (existed) return prefs_put_uint_verified(key, value);
    if (!s_prefs_ready) return false;
    bool remains = false;
    uint32_t ignored = 0;
    if (!prefs_read_uint_key(key, &remains, &ignored)) return false;
    if (!remains) return true;
    if (!s_prefs.remove(key)) return false;
    return prefs_read_uint_key(key, &remains, &ignored) && !remains;
}

static bool prefs_restore_bool(const char* key, bool existed, bool value) {
    if (existed) return prefs_put_bool_verified(key, value);
    if (!s_prefs_ready) return false;
    bool remains = false;
    bool ignored = false;
    if (!prefs_read_bool_key(key, &remains, &ignored)) return false;
    if (!remains) return true;
    if (!s_prefs.remove(key)) return false;
    return prefs_read_bool_key(key, &remains, &ignored) && !remains;
}

bool wifi_manager_update_power_config(bool set_policy, wifi_policy_t policy,
                                      bool set_timeout_min, uint32_t timeout_min,
                                      bool set_timeout_enabled, bool timeout_enabled) {
    if ((set_policy && (uint32_t)policy > WIFI_POLICY_DISABLED) ||
            (set_timeout_min && !wifi_is_valid_timeout(timeout_min))) {
        return false;
    }
    if (!set_policy && !set_timeout_min && !set_timeout_enabled) return true;
    if (!s_prefs_ready) {
        app_log("WIFI", "Cannot save power settings: Wi-Fi preferences are unavailable");
        return false;
    }

    bool had_policy = false;
    bool had_timeout = false;
    bool had_timeout_enabled = false;
    uint32_t old_policy = WIFI_DEFAULT_POLICY;
    uint32_t old_timeout = WIFI_DEFAULT_TIMEOUT_MIN;
    bool old_timeout_enabled = WIFI_DEFAULT_TIMEOUT_ENABLED ? true : false;
    if ((set_policy && !prefs_read_uint_key("policy", &had_policy, &old_policy)) ||
            (set_timeout_min && !prefs_read_uint_key("timeout_min", &had_timeout, &old_timeout)) ||
            (set_timeout_enabled && !prefs_read_bool_key("timeout_en", &had_timeout_enabled,
                                                        &old_timeout_enabled))) {
        app_log("WIFI", "Cannot save power settings: failed to read previous NVS value");
        return false;
    }

    bool stored = true;
    bool touched_policy = false;
    bool touched_timeout = false;
    bool touched_timeout_enabled = false;
    if (set_policy) {
        touched_policy = true;
        stored = prefs_put_uint_verified("policy", (uint32_t)policy);
    }
    if (stored && set_timeout_min) {
        touched_timeout = true;
        stored = prefs_put_uint_verified("timeout_min", timeout_min);
    }
    if (stored && set_timeout_enabled) {
        touched_timeout_enabled = true;
        stored = prefs_put_bool_verified("timeout_en", timeout_enabled);
    }
    if (!stored) {
        bool rolled_back = true;
        if (touched_policy) rolled_back = prefs_restore_uint("policy", had_policy, old_policy) && rolled_back;
        if (touched_timeout) rolled_back = prefs_restore_uint("timeout_min", had_timeout, old_timeout) && rolled_back;
        if (touched_timeout_enabled) {
            rolled_back = prefs_restore_bool("timeout_en", had_timeout_enabled,
                    old_timeout_enabled) && rolled_back;
        }
        app_log("WIFI", "Power settings NVS write failed; rollback %s (power-loss atomicity is not available)",
                rolled_back ? "completed" : "incomplete");
        return false;
    }

    const bool policy_changed = set_policy && s_policy != policy;
    const bool timeout_changed = set_timeout_min && s_timeout_min != timeout_min;
    const bool timeout_enabled_changed = set_timeout_enabled && s_timeout_enabled != timeout_enabled;
    if (set_policy) {
        s_policy = policy;
        s_wifi_enabled = policy != WIFI_POLICY_DISABLED;
    }
    if (set_timeout_min) s_timeout_min = timeout_min;
    if (set_timeout_enabled) s_timeout_enabled = timeout_enabled;
    if (policy_changed) {
        app_log("WIFI", "Wi-Fi policy set to %s; reboot required to apply",
                wifi_manager_policy_str(policy));
    }
    if (timeout_changed) {
        app_log("WIFI", "Wi-Fi idle timeout set to %u minute(s)%s", (unsigned int)timeout_min,
                s_timeout_enabled ? "" : " (auto-shutdown still OFF)");
    }
    if (timeout_enabled_changed) {
        // This is only a gate on the idle check; the initialized radio remains
        // untouched. Anchor a newly enabled window after persistence succeeds.
        if (timeout_enabled) {
            s_last_activity_ms = millis();
            app_log("WIFI", "Wi-Fi idle auto-shutdown ENABLED (%u min, policy %s)",
                    (unsigned int)s_timeout_min, wifi_manager_policy_str(s_policy));
        } else {
            app_log("WIFI", "Wi-Fi idle auto-shutdown DISABLED (radio stays on; 'wifi off' powers it down)");
        }
    }
    return true;
}

static void wifi_scan_begin(void) {
    WiFi.scanDelete();
    s_scan_error = 0;
    s_scan_error_reason = "";
    const int result = WiFi.scanNetworks(true);
    if (result == WIFI_SCAN_FAILED) {
        s_scan_state = WIFI_SCAN_FAILED_STATE;
        s_scan_error = result;
        s_scan_error_reason = "start_failed";
        WiFi.scanDelete();
        app_log("WIFI", "Async network scan failed to start (%d)", result);
        return;
    }
    s_scan_state = WIFI_SCAN_RUNNING_STATE;
}

static void wifi_scan_stop_and_delete(void) {
    // scanDelete() only frees Arduino's cached results. Stop the driver scan
    // itself as well, without restarting Wi-Fi or disturbing the STA link.
    const esp_err_t result = esp_wifi_scan_stop();
    if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_STARTED &&
        result != ESP_ERR_WIFI_NOT_INIT) {
        app_log("WIFI", "Failed to stop async scan (%d)", (int)result);
    }
    WiFi.scanDelete();
}

static void wifi_scan_tick(void) {
    if ((s_scan_state == WIFI_SCAN_WAIT_RADIO || s_scan_state == WIFI_SCAN_RUNNING_STATE) &&
        (int32_t)(millis() - s_scan_deadline_ms) >= 0) {
        if (s_scan_state == WIFI_SCAN_RUNNING_STATE) wifi_scan_stop_and_delete();
        s_scan_state = WIFI_SCAN_FAILED_STATE;
        s_scan_error = WIFI_SCAN_FAILED;
        s_scan_error_reason = "timeout";
        app_log("WIFI", "Wi-Fi scan timed out");
        return;
    }
    if (s_scan_state == WIFI_SCAN_WAIT_RADIO) {
        if (s_radio_state == WIFI_STATE_ON) {
            wifi_scan_begin();
        } else if (s_radio_state == WIFI_STATE_OFF) {
            s_scan_state = WIFI_SCAN_FAILED_STATE;
            s_scan_error = WIFI_SCAN_FAILED;
            s_scan_error_reason = "radio_unavailable";
        }
        return;
    }
    if (s_scan_state != WIFI_SCAN_RUNNING_STATE) return;

    const int result_count = WiFi.scanComplete();
    if (result_count == WIFI_SCAN_RUNNING) return;
    if (result_count < 0) {
        s_scan_state = WIFI_SCAN_FAILED_STATE;
        s_scan_error = result_count;
        s_scan_error_reason = "scan_failed";
        wifi_scan_stop_and_delete();
        app_log("WIFI", "Async network scan failed (%d)", result_count);
        return;
    }

    const size_t kept = result_count < (int)WIFI_SCAN_CACHE_MAX
            ? (size_t)result_count : WIFI_SCAN_CACHE_MAX;
    for (size_t i = 0; i < kept; ++i) {
        s_scan_ssid[i] = WiFi.SSID((int)i);
        s_scan_rssi[i] = (int16_t)WiFi.RSSI((int)i);
        s_scan_secure[i] = WiFi.encryptionType((int)i) != WIFI_AUTH_OPEN;
    }
    s_scan_count = kept;
    s_scan_state = WIFI_SCAN_COMPLETE_STATE;
    s_scan_error = 0;
    s_scan_error_reason = "";
    s_scan_completed_at_ms = millis();
    s_scan_has_complete = true;
    WiFi.scanDelete();
    app_log("WIFI", "Async network scan complete (%u results cached)",
            (unsigned int)s_scan_count);
}

String wifi_manager_scan_status_json(bool request_new_scan) {
    if (!s_wifi_enabled) {
        return "{\"status\":\"failed\",\"scanning\":false,\"networks\":[],\"error\":\"wifi_disabled\"}";
    }

    if (request_new_scan && s_scan_state != WIFI_SCAN_RUNNING_STATE &&
        s_scan_state != WIFI_SCAN_WAIT_RADIO) {
        wifi_manager_mark_activity();
        if (s_radio_state == WIFI_STATE_ON || s_radio_state == WIFI_STATE_ENABLING) {
            s_scan_state = WIFI_SCAN_WAIT_RADIO;
        } else if (wifi_manager_request_wifi(WIFI_WAKE_WEB_UI)) {
            s_scan_state = WIFI_SCAN_WAIT_RADIO;
        } else {
            s_scan_state = WIFI_SCAN_FAILED_STATE;
            s_scan_error = WIFI_SCAN_FAILED;
            s_scan_error_reason = "radio_unavailable";
        }
        if (s_scan_state == WIFI_SCAN_WAIT_RADIO) {
            s_scan_deadline_ms = millis() + WIFI_SCAN_DEADLINE_MS;
            s_scan_error = 0;
            s_scan_error_reason = "";
        }
    }

    JsonDocument doc;
    const bool scanning = s_scan_state == WIFI_SCAN_WAIT_RADIO ||
                          s_scan_state == WIFI_SCAN_RUNNING_STATE;
    const char* status = "idle";
    switch (s_scan_state) {
        case WIFI_SCAN_WAIT_RADIO: status = "starting"; break;
        case WIFI_SCAN_RUNNING_STATE: status = "scanning"; break;
        case WIFI_SCAN_COMPLETE_STATE: status = "complete"; break;
        case WIFI_SCAN_FAILED_STATE: status = "failed"; break;
        default: break;
    }
    doc["status"] = status;
    doc["scanning"] = scanning;
    doc["has_results"] = s_scan_has_complete;
    doc["last_complete_ms"] = s_scan_completed_at_ms;
    doc["last_complete_age_ms"] = s_scan_has_complete
            ? (uint32_t)(millis() - s_scan_completed_at_ms) : 0;
    if (s_scan_state == WIFI_SCAN_FAILED_STATE) {
        doc["error"] = s_scan_error_reason;
        doc["error_code"] = s_scan_error;
    }
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (size_t i = 0; i < s_scan_count; ++i) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = s_scan_ssid[i];
        obj["rssi"] = s_scan_rssi[i];
        obj["secure"] = s_scan_secure[i];
    }
    String out;
    serializeJson(doc, out);
    return out;
}

void wifi_manager_task(void) {
    if (!s_wifi_enabled) {
        return;
    }
    if (s_ap_running) {
        s_dns_server.processNextRequest();
    }

    uint32_t now = millis();

    // Pending on-demand wake (radio was OFF). Performed here, not in a helper
    // task, so every WiFi call in the system has a single owner (this loop
    // context: wifi_manager_task + web_server_task + cli_manager_task all run
    // in main.cpp::loop). That serialization is what prevents the WiFi+BLE
    // coexistence deadlock that froze the device after radio power-down.
    if (s_pending_radio_wake) {
        s_pending_radio_wake = false;
        if (s_policy == WIFI_POLICY_DISABLED) {
            s_radio_state = WIFI_STATE_OFF;
        } else if (s_radio_state == WIFI_STATE_ENABLING || s_radio_state == WIFI_STATE_OFF) {
            s_sta_disconnected_since = 0;
            if (s_sta_configured) {
                wifi_reconnect_light();
            } else {
                wifi_apply_config();
            }
            s_radio_state = WIFI_STATE_ON;
            s_last_activity_ms = millis();
            led_indicator_set_wifi_sleep(false);
            // The radio transition can starve/drop the BLE link (2.4GHz
            // coexistence); nudge the BLE task to dump its scan backoff and
            // search fast again.
            ble_remote_notify_wifi_wake();
            app_log("WIFI", "Radio back ON (main-loop wake complete)");
        }
    }

    // Deferred USB wake (host re-enumerated us): safe to act on here because
    // setup() has completed and wifi_manager_init() is no longer mid-bring-up.
    if (s_pending_usb_wake) {
        s_pending_usb_wake = false;
        wifi_manager_request_wifi(WIFI_WAKE_SYSTEM);
    }

    // Deferred manual wake from the key-press gesture (see notify_key_press).
    if (s_pending_manual_wake) {
        s_pending_manual_wake = false;
        wifi_manager_request_wifi(WIFI_WAKE_MANUAL);
    }

    // WiFi.scanNetworks(true) is started and harvested only from this owner
    // task. HTTP handlers merely request a scan or serialize the cached result.
    wifi_scan_tick();

    // ON_DEMAND: power down the STA radio after the idle timeout.
    // Any user input (wifi_manager_request_wifi) wakes it back up.
    // Sleep only disconnects + modem-sleeps: the WiFi driver stays initialized.
    // A later light reconnect (wifi_reconnect_light) then needs no radio
    // re-init, which is the churn that deadlocked esp_wifi against NimBLE.
    // The timeout_en switch gates the whole block: with it off (default) the
    // radio only goes down through an explicit 'wifi off' or a policy change.
    if (s_policy == WIFI_POLICY_ON_DEMAND &&
        s_timeout_enabled &&
        s_timeout_min != WIFI_TIMEOUT_NEVER &&
        s_radio_state == WIFI_STATE_ON &&
        s_scan_state != WIFI_SCAN_WAIT_RADIO &&
        s_scan_state != WIFI_SCAN_RUNNING_STATE &&
        s_sta_configured &&
        WiFi.status() == WL_CONNECTED &&
        (now - s_last_activity_ms) >= (uint32_t)s_timeout_min * 60000U) {
        app_log("WIFI", "ON_DEMAND idle timeout (%u min) reached -> powering down radio",
                (unsigned int)s_timeout_min);
        s_radio_state = WIFI_STATE_SHUTTING_DOWN;
        WiFi.disconnect(true);
        s_radio_state = WIFI_STATE_OFF;
        app_log("WIFI", "Wi-Fi radio asleep (driver stays up, modem sleep). Press a key to wake it.");
        return;
    }

    // STA monitor: skip while the radio is deliberately asleep (ON_DEMAND OFF)
    // so the poll neither pokes the modem out of sleep nor lets the fail-safe
    // timer drift until the next wake.
    if (s_radio_state == WIFI_STATE_OFF) {
        return;
    }

    if (s_sta_configured && (now - s_last_sta_check > 1000)) {
        s_last_sta_check = now;
        if (WiFi.status() == WL_CONNECTED && WiFi.localIP()[0] != 0) {
            s_sta_disconnected_since = 0;
            if (s_ap_running) {
                wifi_stop_ap();
            }
        } else {
            // STA not connected
            if (s_sta_disconnected_since == 0) {
                s_sta_disconnected_since = now;
            } else if ((now - s_sta_disconnected_since > 15000) && !s_ap_running &&
                       s_radio_state != WIFI_STATE_OFF) {
                // Disconnected for more than 15 seconds, fail-safe re-enable AP mode.
                // Skip while the radio is deliberately asleep (ON_DEMAND OFF): the
                // whole point of sleep is a quiet radio; waking restores the AP.
                app_log("WIFI", "Home Wi-Fi disconnected (>15s). Re-enabling AP mode for configuration...");
                wifi_start_ap(s_prefs.getString("ap_pass", ""));
            }
        }
    }
}

String wifi_manager_get_ap_ip(void) {
    if (!s_wifi_enabled) return "Disabled";
    if (!s_ap_running) return "已关闭(省电模式)";
    return WiFi.softAPIP().toString();
}

String wifi_manager_get_sta_ip(void) {
    if (!s_wifi_enabled) return "Disabled";
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "Disconnected";
}

String wifi_manager_get_mdns_url(void) {
    return "http://" MDNS_HOSTNAME ".local";
}

bool wifi_manager_is_sta_connected(void) {
    if (!s_wifi_enabled) return false;
    return WiFi.status() == WL_CONNECTED;
}

int8_t wifi_manager_get_sta_rssi(void) {
    if (!s_wifi_enabled) return 0;
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.RSSI();
    }
    return 0;
}

String wifi_manager_scan_json(void) {
    return wifi_manager_scan_status_json(true);
}

bool wifi_manager_save_sta_config(const String& ssid, const String& password) {
    if (!s_wifi_enabled) return false;
    if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) return false;
    if (!s_prefs_ready) {
        app_log("WIFI", "Cannot save STA credentials: Wi-Fi preferences are unavailable");
        return false;
    }

    bool had_ssid = false;
    bool had_pass = false;
    String old_ssid;
    String old_pass;
    if (!prefs_read_string_key("ssid", &had_ssid, &old_ssid) ||
            !prefs_read_string_key("pass", &had_pass, &old_pass)) {
        app_log("WIFI", "Cannot save STA credentials: failed to read previous NVS value");
        return false;
    }
    const bool stored = prefs_put_string_verified("ssid", ssid) &&
                        prefs_put_string_verified("pass", password);
    if (!stored) {
        const bool rollback_ssid = prefs_restore_string("ssid", had_ssid, old_ssid);
        const bool rollback_pass = prefs_restore_string("pass", had_pass, old_pass);
        app_log("WIFI", "STA credential NVS write failed; rollback %s (power-loss atomicity is not available)",
                rollback_ssid && rollback_pass ? "completed" : "incomplete");
        return false;
    }
    s_sta_configured = true;
    s_sta_disconnected_since = 0;

    app_log("WIFI", "Saved new Wi-Fi credentials for: %s, connecting...", ssid.c_str());
    WiFi.disconnect();
    WiFi.mode(s_ap_running ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    return true;
}

String wifi_manager_get_ap_pass(void) {
    return s_prefs.getString("ap_pass", "");
}

String wifi_manager_get_sta_ssid(void) {
    return s_prefs.getString("ssid", "");
}

String wifi_manager_get_sta_pass(void) {
    return s_prefs.getString("pass", "");
}

bool wifi_manager_validate_backup(const String& ssid, const String& sta_pass,
                                  const String& ap_pass,
                                  wifi_policy_t policy, uint32_t timeout_min) {
    if ((uint32_t)policy > WIFI_POLICY_DISABLED) {
        return false;
    }
    if (!wifi_is_valid_timeout(timeout_min)) {
        return false;
    }
    if (ssid.length() > 32 || sta_pass.length() > 64) return false;
    String ap = ap_pass;
    ap.trim();
    if (ap.length() > 0 && (ap.length() < 8 || ap.length() > 63)) {
        return false;
    }
    return true;
}

bool wifi_manager_restore_backup(const String& ssid, const String& sta_pass,
                                 const String& ap_pass, wifi_policy_t policy,
                                 uint32_t timeout_min, bool timeout_enabled,
                                 bool publish_runtime) {
    if (!wifi_manager_validate_backup(ssid, sta_pass, ap_pass, policy, timeout_min)) return false;
    String ap = ap_pass;
    ap.trim();

    bool had_ssid = false, had_pass = false, had_ap = false;
    bool had_policy = false, had_timeout = false, had_timeout_enabled = false;
    String old_ssid, old_pass, old_ap;
    uint32_t old_policy = WIFI_DEFAULT_POLICY;
    uint32_t old_timeout = WIFI_DEFAULT_TIMEOUT_MIN;
    bool old_timeout_enabled = WIFI_DEFAULT_TIMEOUT_ENABLED ? true : false;
    if (!prefs_read_string_key("ssid", &had_ssid, &old_ssid) ||
            !prefs_read_string_key("pass", &had_pass, &old_pass) ||
            !prefs_read_string_key("ap_pass", &had_ap, &old_ap) ||
            !prefs_read_uint_key("policy", &had_policy, &old_policy) ||
            !prefs_read_uint_key("timeout_min", &had_timeout, &old_timeout) ||
            !prefs_read_bool_key("timeout_en", &had_timeout_enabled, &old_timeout_enabled)) {
        app_log("WIFI", "Cannot restore Wi-Fi backup: failed to read previous NVS state");
        return false;
    }

    bool stored = prefs_put_string_verified("ssid", ssid) &&
                  prefs_put_string_verified("pass", sta_pass) &&
                  prefs_put_string_verified("ap_pass", ap) &&
                  prefs_put_uint_verified("policy", (uint32_t)policy) &&
                  prefs_put_uint_verified("timeout_min", timeout_min) &&
                  prefs_put_bool_verified("timeout_en", timeout_enabled);
    if (!stored) {
        bool rolled_back = true;
        rolled_back = prefs_restore_string("ssid", had_ssid, old_ssid) && rolled_back;
        rolled_back = prefs_restore_string("pass", had_pass, old_pass) && rolled_back;
        rolled_back = prefs_restore_string("ap_pass", had_ap, old_ap) && rolled_back;
        rolled_back = prefs_restore_uint("policy", had_policy, old_policy) && rolled_back;
        rolled_back = prefs_restore_uint("timeout_min", had_timeout, old_timeout) && rolled_back;
        rolled_back = prefs_restore_bool("timeout_en", had_timeout_enabled,
                old_timeout_enabled) && rolled_back;
        app_log("WIFI", "Wi-Fi backup NVS write failed; rollback %s (power-loss atomicity is not available)",
                rolled_back ? "completed" : "incomplete");
        return false;
    }
    if (publish_runtime) {
        wifi_manager_apply_backup_state(ssid, policy, timeout_min, timeout_enabled);
    }
    app_log("WIFI", "Wi-Fi config restored from backup (policy=%s, timeout=%u min, ssid=%s)",
            wifi_manager_policy_str(policy), (unsigned int)timeout_min,
            ssid.length() > 0 ? ssid.c_str() : "(none)");
    return true;
}

void wifi_manager_apply_backup_state(const String& ssid, wifi_policy_t policy,
                                     uint32_t timeout_min, bool timeout_enabled) {
    s_policy = policy;
    s_timeout_min = timeout_min;
    s_timeout_enabled = timeout_enabled;
    s_wifi_enabled = (policy != WIFI_POLICY_DISABLED);
    s_sta_configured = (ssid.length() > 0);
}

bool wifi_manager_save_ap_config(const String& ap_password) {
    if (!s_wifi_enabled) return false;
    String p = ap_password;
    p.trim();
    if (p.length() > 0 && (p.length() < 8 || p.length() > 63)) {
        return false;
    }
    bool had_ap = false;
    String old_ap;
    if (!prefs_read_string_key("ap_pass", &had_ap, &old_ap)) {
        app_log("WIFI", "Cannot save AP password: failed to read previous NVS value");
        return false;
    }
    if (!prefs_put_string_verified("ap_pass", p)) {
        const bool rolled_back = prefs_restore_string("ap_pass", had_ap, old_ap);
        app_log("WIFI", "AP password NVS write failed; rollback %s; running AP settings were not changed",
                rolled_back ? "completed" : "incomplete");
        return false;
    }

    if (s_ap_running) {
        WiFi.softAPConfig(s_ap_ip, s_ap_ip, s_ap_netmask);
        if (p.length() >= 8) {
            WiFi.softAP(AP_SSID, p.c_str());
            app_log("WIFI", "AP reconfigured: %s (WPA2-PSK)", AP_SSID);
        } else {
            WiFi.softAP(AP_SSID, "");
            app_log("WIFI", "AP reconfigured: %s (Open Network)", AP_SSID);
        }
    } else {
        app_log("WIFI", "AP password saved: %s", p.length() >= 8 ? "(WPA2-PSK)" : "(Open Network)");
    }
    return true;
}
