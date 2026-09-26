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

static_assert((int)WIFI_POLICY_ON_DEMAND == WIFI_DEFAULT_POLICY,
              "WIFI_DEFAULT_POLICY must match WIFI_POLICY_ON_DEMAND");

#define DNS_PORT        53
#define MDNS_HOSTNAME   "remotemapper"

static DNSServer        s_dns_server;
static Preferences      s_prefs;
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
static wifi_radio_state_t s_radio_state      = WIFI_STATE_OFF;
static uint32_t           s_last_activity_ms = 0;

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

static bool wifi_is_valid_timeout(uint32_t minutes) {
    return minutes == WIFI_TIMEOUT_NEVER || minutes == 1 || minutes == 5 ||
           minutes == 10 || minutes == 30;
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
    s_prefs.begin("wifi_conf", false);

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
    app_log("WIFI", "Wi-Fi policy %s (timeout %u min, state %s)",
            wifi_manager_policy_str(s_policy), (unsigned int)s_timeout_min,
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
    if (s_policy == policy) {
        return true;
    }
    s_prefs.putUInt("policy", (uint32_t)policy);
    s_policy = policy;
    s_wifi_enabled = (policy != WIFI_POLICY_DISABLED);
    // Applying the change requires a reboot: the caller (CLI/web) must restart.
    // Switching the WiFi stack on/off at runtime is unsafe while BLE is running.
    app_log("WIFI", "Wi-Fi policy set to %s; reboot required to apply",
            wifi_manager_policy_str(policy));
    return true;
}

uint32_t wifi_manager_get_timeout_min(void) {
    return s_timeout_min;
}

bool wifi_manager_set_timeout_min(uint32_t minutes) {
    if (!wifi_is_valid_timeout(minutes)) {
        return false;
    }
    if (s_timeout_min == minutes) {
        return true;
    }
    s_prefs.putUInt("timeout_min", minutes);
    s_timeout_min = minutes;
    app_log("WIFI", "Wi-Fi idle timeout set to %u minute(s)", (unsigned int)minutes);
    return true;
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

    // ON_DEMAND: power down the STA radio after the idle timeout.
    // Any user input (wifi_manager_request_wifi) wakes it back up.
    // Sleep only disconnects + modem-sleeps: the WiFi driver stays initialized.
    // A later light reconnect (wifi_reconnect_light) then needs no radio
    // re-init, which is the churn that deadlocked esp_wifi against NimBLE.
    if (s_policy == WIFI_POLICY_ON_DEMAND &&
        s_timeout_min != WIFI_TIMEOUT_NEVER &&
        s_radio_state == WIFI_STATE_ON &&
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
    if (!s_wifi_enabled) {
        return "{\"networks\":[],\"error\":\"wifi_disabled\"}";
    }
    app_log("WIFI", "Scanning for 2.4GHz Wi-Fi networks...");
    int n = WiFi.scanNetworks();
    JsonDocument doc;
    JsonArray arr = doc["networks"].to<JsonArray>();

    for (int i = 0; i < n; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"] = WiFi.SSID(i);
        obj["rssi"] = WiFi.RSSI(i);
        obj["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }

    String out;
    serializeJson(doc, out);
    WiFi.scanDelete();
    return out;
}

bool wifi_manager_save_sta_config(const String& ssid, const String& password) {
    if (!s_wifi_enabled) return false;
    if (ssid.length() == 0) return false;

    s_prefs.putString("ssid", ssid);
    s_prefs.putString("pass", password);
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

bool wifi_manager_restore_backup(const String& ssid, const String& sta_pass,
                                 const String& ap_pass, wifi_policy_t policy,
                                 uint32_t timeout_min) {
    if ((uint32_t)policy > WIFI_POLICY_DISABLED) {
        return false;
    }
    if (!wifi_is_valid_timeout(timeout_min)) {
        return false;
    }
    String ap = ap_pass;
    ap.trim();
    if (ap.length() > 0 && ap.length() < 8) {
        return false;
    }
    s_prefs.putString("ssid", ssid);
    s_prefs.putString("pass", sta_pass);
    s_prefs.putString("ap_pass", ap);
    s_prefs.putUInt("policy", (uint32_t)policy);
    s_prefs.putUInt("timeout_min", timeout_min);
    s_policy = policy;
    s_timeout_min = timeout_min;
    s_wifi_enabled = (policy != WIFI_POLICY_DISABLED);
    s_sta_configured = (ssid.length() > 0);
    app_log("WIFI", "Wi-Fi config restored from backup (policy=%s, timeout=%u min, ssid=%s)",
            wifi_manager_policy_str(policy), (unsigned int)timeout_min,
            ssid.length() > 0 ? ssid.c_str() : "(none)");
    return true;
}

bool wifi_manager_save_ap_config(const String& ap_password) {
    if (!s_wifi_enabled) return false;
    String p = ap_password;
    p.trim();
    if (p.length() > 0 && p.length() < 8) {
        return false;
    }
    s_prefs.putString("ap_pass", p);

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
