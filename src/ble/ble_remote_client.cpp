#include "ble_remote_client.h"
#include "audio/audio_pipeline.h"
#include "led_indicator.h"
#include "keymap/key_state_machine.h"
#include "usb/usb_composite.h"
#include "log/app_log.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>

static ble_remote_state_t              s_ble_state = BLE_STATE_DISCONNECTED;
static NimBLEClient*                   s_client = nullptr;
static NimBLERemoteCharacteristic*     s_char_cmd = nullptr;
static NimBLERemoteCharacteristic*     s_char_aud = nullptr;
static NimBLERemoteCharacteristic*     s_char_ctl = nullptr;
static NimBLERemoteCharacteristic*     s_char_bat = nullptr;
static int                             s_battery_pct = -1;
static uint32_t                        s_last_battery_poll_ms = 0;
static volatile bool                   s_is_encrypted = false;

static Preferences                     s_ble_prefs;
static String                          s_bound_mac = "";
static String                          s_bound_name = "";
static uint8_t                         s_bound_addr_type = BLE_ADDR_RANDOM;
static String                          s_connected_name = "";
static String                          s_connected_mac = "";

// In-memory discovered BLE device cache (thread-safe, fed by Core 0 continuous scan)
struct DiscoveredBleDevice {
    String   name;
    String   mac;
    int      rssi;
    uint8_t  type;
    uint32_t last_seen_ms;
};
static std::vector<DiscoveredBleDevice> s_discovered_devices;
static portMUX_TYPE                    s_disc_mux = portMUX_INITIALIZER_UNLOCKED;

// BLE advertisements around the house often carry "names" that are actually
// binary payloads (temperature counters, control bytes, ...). ArduinoJson only
// escapes the seven classic sequences, so a raw control character (< 0x20 or
// 0x7F) inside a name produces invalid JSON and the WebUI device list dies
// with "Bad control character in string literal". Keep only printable ASCII;
// UTF-8 lead/continuation bytes (>= 0x80) pass through untouched.
static String sanitize_ble_name(const String& in) {
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); ++i) {
        unsigned char c = (unsigned char)in.charAt(i);
        if (c >= 0x20 && c != 0x7F) {
            out += (char)c;
        }
    }
    if (out.length() == 0) {
        return "Unnamed BLE Device";
    }
    return out;
}

// Asynchronous Request flags from other tasks (e.g. WebServer on Core 1)
static volatile bool                   s_req_unpair = false;
static volatile bool                   s_req_reconnect = false;
static volatile bool                   s_do_connect = false;
static NimBLEAdvertisedDevice*         s_pending_adv_device = nullptr;
static String                          s_pending_mac = "";
static uint8_t                         s_pending_addr_type = BLE_ADDR_RANDOM;

static uint8_t                         s_session_id = 0;
static uint32_t                        s_last_audio_ms = 0;
static uint32_t                        s_last_extend_ms = 0;
static uint32_t                        s_last_scan_ms = 0;
static uint32_t                        s_last_keepalive_ms = 0;
static size_t                          s_frame_size = AUDIO_DEFAULT_FRAME_BYTES;

// Power save: while connected, no continuous scan. WebUI requests short
// on-demand bursts (BLE_SCAN_BURST_SECS) to refresh its device list.
#define BLE_SCAN_BURST_SECS 3
static volatile bool                   s_req_scan_burst = false;
static uint32_t                        s_scan_burst_until_ms = 0;
// Set by wifi_manager (Core 1) when the radio wakes; consumed by the BLE task
// (Core 0) to drop the disconnected-scan backoff back to tier-0 fast scanning.
static volatile bool                   s_req_wifi_wake_rescan = false;

// MiOT (0xFE95) advertisement sniffer: while ON we never auto-connect to the
// remote (a connected remote stops broadcasting) so its burst packets can be
// captured for gesture analysis (single/double/long).
static volatile bool                   s_sniff_adv = false;
static uint32_t                        s_last_sniff_log_ms = 0;

// GATT explorer: while ON, each fresh connection runs one full enumeration of
// every service/characteristic/descriptor (properties + readable values). Its
// whole purpose is to discover REMAINING untapped capabilities of the remote:
// 0x180A Device Information, HID Report Map (0x2A4A), and the three never
// decoded vendor services 0xfe59 / 0x01bf / 8a7a0001-2c42-c2a2-0f36-41928c259b78.
static volatile bool                   s_gatt_dump_enabled = false;
static volatile bool                   s_gatt_dump_pending = false;


// Passive vendor listener: while the explorer is ON we additionally SUBSCRIBE to
// every NOTIFY characteristic of the never-decoded vendor services so their
// spontaneous traffic is logged. This is strictly one-way: subscribing only
// writes the standard 0x2902 CCCD, and no vendor command characteristic is ever
// written (no OTA / DFU / config payloads are sent).
static NimBLERemoteCharacteristic*     s_spy_chars[8] = { nullptr };
static uint8_t                         s_spy_count = 0;
static const char*                     s_spy_tags[8] = { nullptr };

static void gatt_spy_stop(void);
static void read_device_information(void);
static void gatt_explore_all(bool allow_refresh);

// Remote Device Information (0x180A) read on every connect. Read-only queries,
// never written back. For RC003 (2 Pro) the firmware reports fw "2671" etc.
static String                          s_dev_model = "";
static String                          s_dev_manuf = "";
static String                          s_dev_serial = "";
static String                          s_dev_hw = "";
static String                          s_dev_fw = "";
static String                          s_dev_sw = "";

// Disconnected scan backoff: the longer the remote stays away, the less often we
// scan (window stays BLE_SCAN_WINDOW_MS). Keeps reconnect fast right after a
// drop while cutting radio duty when the remote is off/asleep.
#define BLE_SCAN_TIER1_AFTER_MS   30000UL    // >30s away -> medium duty
#define BLE_SCAN_TIER2_AFTER_MS   300000UL   // >5min away -> low duty
#define BLE_SCAN_MED_INTERVAL_MS  2400       // 200/2400 = 8.3% duty
#define BLE_SCAN_SLOW_INTERVAL_MS 3200       // 200/3200 = 6.25% duty (caps reconnect lag)
static uint32_t                        s_disconnected_since_ms = 0;
static uint8_t                         s_scan_tier = 0;

extern key_mapper_engine_t g_key_engine;

// Forward Declarations
static void start_scan(uint16_t interval_ms = BLE_SCAN_INTERVAL_MS);
static bool do_connect_adv_device(NimBLEAdvertisedDevice* advDevice);
static bool do_connect_mac(const String& mac_str, uint8_t addr_type);
static bool setup_services_and_handshake();

// Battery Notification Callback (0x180F / 0x2A19)
static void on_battery_notify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (pData && length >= 1) {
        s_battery_pct = (int)pData[0];
        app_log("BATTERY", "Remote battery level updated: %d%%", s_battery_pct);
        led_indicator_set_low_battery(s_battery_pct >= 0 && s_battery_pct <= 15);
    }
}

// Audio Notification Callback (ATVV Char 0x03)
static void on_audio_notify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (length == 0) return;
    s_last_audio_ms = millis();
    audio_pipeline_feed_adpcm(&g_audio_pipeline, pData, length);
}

// Control Notification Callback (ATVV Char 0x04)
static void on_ctl_notify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (length < 1) return;
    uint8_t op = pData[0];

    // AUDIO_START with HTT reason: byte1 == 0x03
    if (op == 0x04 && length >= 2 && pData[1] == 0x03) {
        s_session_id = (length >= 4) ? pData[3] : 0;
        s_ble_state = BLE_STATE_TALKING;
        s_last_audio_ms = millis();
        s_last_extend_ms = millis();

        key_engine_feed_key(&g_key_engine, MI_KEY_VOICE, true, millis());
        app_log("ATVV", ">>> Voice button PRESSED (session %d)", s_session_id);
    }
    // AUDIO_STOP / MIC_CLOSED / release op (0x00 or 0x08):
    else if (op == 0x00 || op == 0x08) {
        if (s_ble_state == BLE_STATE_TALKING) {
            s_ble_state = BLE_STATE_CONNECTED;
            key_engine_feed_key(&g_key_engine, MI_KEY_VOICE, false, millis());
            app_log("ATVV", "<<< Voice button RELEASED (op=0x%02X)", op);
        } else {
            app_log("ATVV", "Microphone inactive / standby (op=0x%02X)", op);
        }
        // NOTE: Do NOT send cmd_open (MIC_OPEN) here. Keep microphone off so the remote can sleep.
    }
    // CAPS_RESP: op == 0x0B
    else if (op == 0x0B && length >= 7) {
        uint16_t ver = (pData[1] << 8) | pData[2];
        uint16_t fs = (pData[5] << 8) | pData[6];
        if (fs > 0) s_frame_size = fs;
        app_log("ATVV", "CAPS: ver=0x%04X, frame_size=%d", ver, (int)s_frame_size);
    }
    // AUDIO_SYNC: op == 0x0A
    else if (op == 0x0A && length >= 7) {
        int16_t pred = (int16_t)((pData[4] << 8) | pData[5]);
        int8_t step_idx = (int8_t)pData[6];
        audio_pipeline_sync(&g_audio_pipeline, pred, step_idx);
        app_log("ATVV", "SYNC: pred=%d, step=%d", pred, step_idx);
    }
}

static uint8_t s_last_hogp_key = 0;

// HOGP HID Report Notification Callback
static void on_hogp_report_notify(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (length < 1) return;

    // 1. Non-keyboard / vendor packets (len > 8, e.g. fallback HOGP voice or sensor reports)
    // NEVER feed these into audio_pipeline! Legitimate voice strictly arrives via on_audio_notify (ATVV Char ab5e0003).
    // Feeding raw/headered HID reports into IMA-ADPCM causes decoder divergence and ear-piercing white noise.
    if (length > 8) {
        static uint32_t s_last_hogp_large_log = 0;
        uint32_t now = millis();
        if (now - s_last_hogp_large_log > 1000) {
            s_last_hogp_large_log = now;
            app_log("HOGP", "Ignoring non-key report len=%d from Char %s (Audio exclusively handled by ATVV ab5e0003)", 
                    (int)length, pChar->getUUID().toString().c_str());
        }
        return;
    }

    // Dump raw bytes for diagnostics (only for genuine key reports len <= 8)
    String hex_str = "";
    for (size_t i = 0; i < length; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02X ", pData[i]);
        hex_str += buf;
    }
    app_log("HOGP_RAW", "Report (len %d): %s", (int)length, hex_str.c_str());

    // 2. Standard Keyboard & Consumer Reports (len <= 8)
    uint8_t raw_key = 0;
    bool is_pressed = false;

    if (length == 8) {
        // Standard 8-byte Keyboard Report: [modifiers, reserved, key0..key5]
        if (pData[2] != 0) {
            raw_key = pData[2];
            is_pressed = true;
        } else {
            raw_key = s_last_hogp_key;
            is_pressed = false;
        }
    } else if (length == 2) {
        if (pData[1] != 0) {
            raw_key = pData[1];
            is_pressed = true;
        } else if (pData[0] != 0) {
            raw_key = pData[0];
            is_pressed = true;
        } else {
            raw_key = s_last_hogp_key;
            is_pressed = false;
        }
    } else if (length == 1) {
        if (pData[0] != 0) {
            raw_key = pData[0];
            is_pressed = true;
        } else {
            raw_key = s_last_hogp_key;
            is_pressed = false;
        }
    } else if (length >= 3 && length <= 7) {
        if (pData[2] != 0) {
            raw_key = pData[2];
            is_pressed = true;
        } else if (pData[0] != 0) {
            raw_key = pData[0];
            is_pressed = true;
        } else {
            raw_key = s_last_hogp_key;
            is_pressed = false;
        }
    }

    // Auto-release previous key if a new key is pressed without an explicit all-zero release report
    if (s_last_hogp_key != 0 && is_pressed && raw_key != s_last_hogp_key) {
        app_log("HOGP", "Auto-Release key 0x%02X due to new key 0x%02X", s_last_hogp_key, raw_key);
        key_engine_feed_key(&g_key_engine, s_last_hogp_key, false, millis());
        s_last_hogp_key = 0;
    }

    if (is_pressed) {
        s_last_hogp_key = raw_key;
    } else {
        s_last_hogp_key = 0;
    }

    if (raw_key != 0) {
        app_log("HOGP", "Key event: 0x%02X (%s)", raw_key, is_pressed ? "DOWN" : "UP");
        if (raw_key == MI_KEY_VOICE || raw_key == MI_KEY_VOICE_ALT) {
            app_log("VOICE", "Voice button event: 0x%02X (%s)", raw_key, is_pressed ? "DOWN" : "UP");
            if (is_pressed) {
                if (s_char_cmd != nullptr) {
                    // MIC_OPEN (v1.0): 0x0C + big-endian codec id (0x0001 = 16 kHz).
                    // The two-byte form {0x0C,0x00} relied on the remote defaulting
                    // the codec; the explicit 3-byte form is the documented one.
                    uint8_t cmd_open[] = { 0x0C, 0x00, 0x01 };
                    s_char_cmd->writeValue(cmd_open, sizeof(cmd_open), false);
                    app_log("ATVV", "Triggered MIC_OPEN (codec=0x0001/16kHz) on Voice key press");
                } else {
                    app_log("ATVV", "Warning: Voice key pressed but ATVV CMD characteristic unavailable");
                }
            } else {
                if (s_char_cmd != nullptr) {
                    // MIC_CLOSE is opcode 0x0D (0x00 is a control/status opcode, not
                    // "close"), so the mic can stay hot until the remote times out.
                    uint8_t cmd_close[] = { 0x0D };
                    s_char_cmd->writeValue(cmd_close, sizeof(cmd_close), false);
                    app_log("ATVV", "Triggered MIC_CLOSE on Voice key release");
                }
            }
        }
        key_engine_feed_key(&g_key_engine, raw_key, is_pressed, millis());
    }
}

static bool is_target_remote(NimBLEAdvertisedDevice* dev) {
    String name = dev->getName().c_str();
    String addr = dev->getAddress().toString().c_str();
    addr.toLowerCase();

    // 1. If a remote was previously bound: STRICT EXCLUSIVITY!
    // ONLY reconnect to this specific bound remote! NEVER connect to stranger devices!
    if (s_bound_mac.length() > 0) {
        String bound = s_bound_mac;
        bound.toLowerCase();
        if (addr.equals(bound)) {
            return true;
        }
        // If MAC rotated (e.g. RPA) but device name matches our bound remote:
        if (s_bound_name.length() > 0 && name.length() > 0 && name.equalsIgnoreCase(s_bound_name)) {
            app_log("BLE", "Bound remote name matched (%s), updating target MAC to %s", name.c_str(), addr.c_str());
            return true;
        }
        // STRICT: If bound, NEVER match any other stranger devices!
        return false;
    }

    // 2. Unbound state (initial pairing only):
    // Name MUST contain explicit Xiaomi / Remote keywords (dropped loose "RC" to avoid false positives)
    if (name.indexOf("小米") >= 0 || name.indexOf("遥控") >= 0 ||
        name.indexOf("MI RC") >= 0 || name.indexOf("Xiaomi") >= 0 ||
        name.indexOf("Remote") >= 0) {
        return true;
    }

    // 3. Service UUID matches proprietary ATVV (0xab5e0001)
    // (Note: Generic 0x1812 HID is explicitly removed to prevent hijacking neighbor devices!)
    if (dev->haveServiceUUID()) {
        if (dev->isAdvertisingService(NimBLEUUID(ATVV_SVC_UUID))) {
            return true;
        }
    }

    // 4. Common Xiaomi Bluetooth OUI prefixes
    if (addr.startsWith("c0:5d:39") || addr.startsWith("64:90:c1") ||
        addr.startsWith("7c:49:eb") || addr.startsWith("50:ec:50") ||
        addr.startsWith("04:cf:8c") || addr.startsWith("28:6c:07") ||
        addr.startsWith("34:ce:00") || addr.startsWith("5c:c3:06")) {
        return true;
    }

    return false;
}

// MiOT sniffer: dump one advertisement frame (service UUIDs + service data,
// with the 0xFE95 Xiaomi payload hex) to the ring log. Rate-limited so a
// burst of contactable duplicates does not flood the log.
static void sniff_dump_packet(NimBLEAdvertisedDevice* dev) {
    uint32_t now = millis();
    if (now - s_last_sniff_log_ms < 150) return;
    s_last_sniff_log_ms = now;

    String name = dev->getName().c_str();
    String addr = dev->getAddress().toString().c_str();
    String tag = is_target_remote(dev) ? "[TARGET]" : "";
    String line = "SNIFF " + tag + " " + addr + "(" + String(dev->getAddress().getType()) + ", RSSI " + String(dev->getRSSI()) + ")";
    if (name.length() > 0) line += " name=" + sanitize_ble_name(name);

    if (dev->haveServiceUUID()) {
        line += " svc=[";
        int n = dev->getServiceUUIDCount();
        for (int i = 0; i < n; ++i) {
            if (i) line += " ";
            line += dev->getServiceUUID(i).toString().c_str();
        }
        line += "]";
    }

    int sdCount = dev->getServiceDataCount();
    for (int i = 0; i < sdCount; ++i) {
        NimBLEUUID u = dev->getServiceDataUUID(i);
        std::string sd = dev->getServiceData(i);
        line += " sd(" + String(u.toString().c_str()) + ")=";
        char hex[8];
        for (size_t k = 0; k < sd.size(); ++k) {
            snprintf(hex, sizeof(hex), "%02X", (uint8_t)sd[k]);
            line += hex;
        }
        if (u == NimBLEUUID((uint16_t)0xFE95)) {
            line += " <-- MiBeacon";
        }
    }
    app_log("ADV", "%s", line.c_str());
}

// Advertised Device Scan Callbacks
class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) override {
        String name = advertisedDevice->getName().c_str();
        String addr = advertisedDevice->getAddress().toString().c_str();
        uint32_t now = millis();

        // Update in-memory discovered BLE devices cache for WebUI (thread-safe, non-blocking)
        portENTER_CRITICAL(&s_disc_mux);
        bool found = false;
        for (auto& item : s_discovered_devices) {
            if (item.mac.equalsIgnoreCase(addr)) {
                if (name.length() > 0) {
                    String clean = sanitize_ble_name(name);
                    if (clean.length() > 0) item.name = clean;
                }
                item.rssi = advertisedDevice->getRSSI();
                item.type = (uint8_t)advertisedDevice->getAddress().getType();
                item.last_seen_ms = now;
                found = true;
                break;
            }
        }
        if (!found) {
            if (s_discovered_devices.size() >= 30) {
                s_discovered_devices.erase(s_discovered_devices.begin());
            }
            DiscoveredBleDevice d;
            d.name = (name.length() > 0) ? sanitize_ble_name(name) : "Unnamed BLE Device";
            d.mac = addr;
            d.rssi = advertisedDevice->getRSSI();
            d.type = (uint8_t)advertisedDevice->getAddress().getType();
            d.last_seen_ms = now;
            s_discovered_devices.push_back(d);
        }

        // Purge devices not seen for over 20 seconds
        for (auto it = s_discovered_devices.begin(); it != s_discovered_devices.end(); ) {
            if (now - it->last_seen_ms > 20000) {
                it = s_discovered_devices.erase(it);
            } else {
                ++it;
            }
        }
        portEXIT_CRITICAL(&s_disc_mux);

        // MiOT sniffer: dump payloads and never steal the connection, otherwise
        // the remote stops broadcasting once we hold a GATT link.
        if (s_sniff_adv) {
            sniff_dump_packet(advertisedDevice);
            return;
        }

        if (name.length() > 0) {
            app_log("BLE_SCAN", "Device: %s (%s, RSSI: %d, Type: %d)", 
                    name.c_str(), addr.c_str(), advertisedDevice->getRSSI(), 
                    (int)advertisedDevice->getAddress().getType());
        }

        if (s_ble_state <= BLE_STATE_SCANNING && is_target_remote(advertisedDevice) && !s_do_connect && !s_sniff_adv) {
            app_log("BLE", "Matching Target Remote: %s (%s), queueing connection...", name.c_str(), addr.c_str());
            NimBLEDevice::getScan()->stop();
            if (s_pending_adv_device) delete s_pending_adv_device;
            s_pending_adv_device = new NimBLEAdvertisedDevice(*advertisedDevice);
            s_do_connect = true;
        }
    }
};

// Client Connection Callbacks
class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pClient) override {
        app_log("BLE", "Remote GATT Connected!");
        s_ble_state = BLE_STATE_CONNECTING;
        s_is_encrypted = false;
        // NOTE: Do NOT call secureConnection() here. It blocks the NimBLE host task thread.
    }

    void onDisconnect(NimBLEClient* pClient) override {
        int err = pClient ? pClient->getLastError() : 0;
        app_log("BLE", "Remote Disconnected (lastErr: %d, %s)", err, NimBLEUtils::returnCodeToString(err));
        s_ble_state = BLE_STATE_DISCONNECTED;
        s_do_connect = false;
        s_is_encrypted = false;
        s_char_cmd = nullptr;
        s_char_aud = nullptr;
        s_char_ctl = nullptr;
        s_char_bat = nullptr;
        s_battery_pct = -1;
        gatt_spy_stop();
        s_dev_model = "";
        s_dev_manuf = "";
        s_dev_serial = "";
        s_dev_hw = "";
        s_dev_fw = "";
        s_dev_sw = "";
        s_last_hogp_key = 0;
        key_engine_release_all(&g_key_engine, millis());
        usb_hid_keyboard_release();
        usb_hid_consumer_release();
        audio_pipeline_stop_session(&g_audio_pipeline);
        led_indicator_set_low_battery(false);
        led_indicator_set(LED_STATE_WAIT_CONNECTION);
        // NOTE: Do NOT call start_scan() directly inside GAP disconnect callback.
        // ble_remote_task() on Core 0 automatically restarts scanning on the next tick.
    }

    bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) override {
        return true; // Accept remote requested conn params
    }

    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        s_is_encrypted = desc->sec_state.encrypted;
        if (desc->sec_state.encrypted) {
            app_log("BLE_SEC", "Link Encrypted & Bonded! (Bonded:%d)", desc->sec_state.bonded);
        } else {
            app_log("BLE_SEC", "Encryption not established");
        }
    }
};

static void start_scan(uint16_t interval_ms) {
    if (s_do_connect || s_ble_state == BLE_STATE_CONNECTING || s_ble_state >= BLE_STATE_CONNECTED) {
        return;
    }
    s_ble_state = BLE_STATE_SCANNING;
    led_indicator_set(LED_STATE_WAIT_CONNECTION);
    s_last_scan_ms = millis();
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(false); // passive: no probe requests (saves TX power)
    pScan->setInterval(interval_ms);
    pScan->setWindow(BLE_SCAN_WINDOW_MS);
    // Non-blocking continuous scan (3-arg overload, null completion callback):
    // keeps the BLE task responsive so connect requests and WebUI scan bursts
    // are still serviced while searching for the remote.
    pScan->start(0, nullptr, false);
    app_log("BLE", "Continuous passive scanning active (interval %ums, duty %u%%)...",
            (unsigned)interval_ms, (unsigned)(BLE_SCAN_WINDOW_MS * 100 / interval_ms));
}

// Short on-demand scan burst for the WebUI device list. Runs ALONGSIDE an
// active connection and therefore must not touch s_ble_state; it only feeds
// the discovered-device cache. Auto-stops after BLE_SCAN_BURST_SECS.
static void start_web_scan_burst() {
    uint32_t now = millis();
    NimBLEScan* pScan = NimBLEDevice::getScan();
    // The controller's scan state can be stale (isScanning() desync, e.g. after
    // a handshake or an interrupted continuous scan), so always force a clean
    // timed burst rather than trusting it. When disconnected the continuous
    // scan is restarted by step 3 once the burst window closes.
    if (pScan->isScanning()) {
        pScan->stop();
    }
    pScan->setActiveScan(false);
    pScan->setInterval(BLE_SCAN_INTERVAL_MS);
    pScan->setWindow(BLE_SCAN_WINDOW_MS);
    // Non-blocking timed burst (3-arg overload with a null completion callback):
    // the controller auto-stops after the window, so the BLE task keeps running.
    pScan->start(BLE_SCAN_BURST_SECS, nullptr, false);
    s_scan_burst_until_ms = now + BLE_SCAN_BURST_SECS * 1000;
    app_log("BLE", "WebUI scan burst (%us) started...", (unsigned)BLE_SCAN_BURST_SECS);
}

static bool setup_services_and_handshake() {
    if (!s_client || !s_client->isConnected()) return false;

    // 1. Security & Bonding (executed safely on Core 0 task context)
    app_log("BLE_SEC", "Initiating secure connection / bonding...");
    if (!s_client->secureConnection()) {
        app_log("BLE_SEC", "secureConnection returned %d, continuing service discovery...", s_client->getLastError());
    } else {
        app_log("BLE_SEC", "secureConnection established successfully");
    }

    // Wait briefly for SMP encryption to finish so protected vendor services (ATVV) are fully accessible
    uint32_t sec_wait_start = millis();
    while (!s_is_encrypted && (millis() - sec_wait_start < 600)) {
        vTaskDelay(pdMS_TO_TICKS(40));
        if (!s_client->isConnected()) return false;
    }
    if (s_is_encrypted) {
        app_log("BLE_SEC", "SMP encryption active, proceeding to GATT discovery");
    } else {
        app_log("BLE_SEC", "Encryption wait timeout (600ms), attempting GATT discovery anyway");
    }

    // 2. Discover services
    std::vector<NimBLERemoteService*>* pServices = s_client->getServices(true);
    if (!pServices) {
        app_log("BLE", "No GATT services found");
        return false;
    }

    app_log("BLE", "Discovered %d GATT Service(s)", (int)pServices->size());

    // FULL GATT dump, in the one window where re-discovery is still harmless:
    // services are listed, but nothing has been subscribed yet, so
    // deleteCharacteristics() cannot invalidate a live notification binding.
    // Opt-in via `gattdump on`; runs at most once per connection.
    if (s_gatt_dump_enabled) {
        app_log("GATTX", "Handshake-window full dump requested");
        gatt_explore_all(true);
        // The full dump subsumes the runtime dump, so do not let the task loop
        // enumerate the vendor services a second time on this same connection.
        s_gatt_dump_pending = false;
    }

    NimBLERemoteService* atvv_svc = nullptr;
    NimBLERemoteService* hid_svc = nullptr;
    NimBLERemoteService* bat_svc = nullptr;

    for (auto* pSvc : *pServices) {
        String svc_uuid = pSvc->getUUID().toString().c_str();
        svc_uuid.toLowerCase();
        app_log("GATT_SVC", "Service: %s", svc_uuid.c_str());

        if (pSvc->getUUID().equals(NimBLEUUID(ATVV_SVC_UUID)) || svc_uuid.indexOf("ab5e0001") >= 0) {
            atvv_svc = pSvc;
        } else if (pSvc->getUUID().equals(NimBLEUUID((uint16_t)HOGP_SVC_UUID)) || svc_uuid.indexOf("1812") >= 0) {
            hid_svc = pSvc;
        } else if (pSvc->getUUID().equals(NimBLEUUID((uint16_t)0x180F)) || svc_uuid.indexOf("180f") >= 0) {
            bat_svc = pSvc;
        }
    }

    int sub_count = 0;

    // STEP 1: Process ATVV Voice Service FIRST while BLE ATT pipe is 100% idle!
    if (atvv_svc) {
        app_log("ATVV", "Discovering ATVV characteristics...");
        std::vector<NimBLERemoteCharacteristic*>* pChars = nullptr;
        for (int retry = 0; retry < 3; retry++) {
            pChars = atvv_svc->getCharacteristics(true);
            if (pChars && !pChars->empty()) break;
            vTaskDelay(pdMS_TO_TICKS(60));
        }

        if (pChars) {
            for (auto* pChar : *pChars) {
                String char_uuid = pChar->getUUID().toString().c_str();
                char_uuid.toLowerCase();
                app_log("GATT_CHAR", "  ATVV Char: %s (N:%d, I:%d, W:%d)", 
                        char_uuid.c_str(), pChar->canNotify() ? 1 : 0, pChar->canIndicate() ? 1 : 0, 
                        (pChar->canWrite() || pChar->canWriteNoResponse()) ? 1 : 0);

                if (char_uuid.indexOf("ab5e0002") >= 0) {
                    s_char_cmd = pChar;
                    app_log("ATVV", "Matched ATVV CMD Char: %s", char_uuid.c_str());
                } else if (char_uuid.indexOf("ab5e0003") >= 0) {
                    s_char_aud = pChar;
                    if (pChar->canNotify()) {
                        pChar->subscribe(true, on_audio_notify, false);
                        sub_count++;
                        app_log("ATVV", "Subscribed to ATVV AUD Char: %s", char_uuid.c_str());
                        vTaskDelay(pdMS_TO_TICKS(25));
                    }
                } else if (char_uuid.indexOf("ab5e0004") >= 0) {
                    s_char_ctl = pChar;
                    if (pChar->canNotify()) {
                        pChar->subscribe(true, on_ctl_notify, false);
                        sub_count++;
                        app_log("ATVV", "Subscribed to ATVV CTL Char: %s", char_uuid.c_str());
                        vTaskDelay(pdMS_TO_TICKS(25));
                    }
                }
            }
        }
        app_log("ATVV", "ATVV discovery result: cmd=%p, aud=%p, ctl=%p", s_char_cmd, s_char_aud, s_char_ctl);
    } else {
        app_log("ATVV", "Warning: ATVV Service (ab5e0001) not found in GATT services!");
    }

    // STEP 2: Process HID Service (0x1812)
    if (hid_svc) {
        app_log("HOGP", "Discovering HID characteristics...");
        std::vector<NimBLERemoteCharacteristic*>* pChars = nullptr;
        for (int retry = 0; retry < 3; retry++) {
            pChars = hid_svc->getCharacteristics(true);
            if (pChars && !pChars->empty()) break;
            vTaskDelay(pdMS_TO_TICKS(60));
        }

        if (pChars) {
            for (auto* pChar : *pChars) {
                String char_uuid = pChar->getUUID().toString().c_str();
                char_uuid.toLowerCase();
                bool can_notif = pChar->canNotify();
                bool can_ind = pChar->canIndicate();
                bool can_wr = pChar->canWrite() || pChar->canWriteNoResponse();

                // Protocol Mode (0x2A4E) -> write Report Mode (0x01)
                if (char_uuid.indexOf("2a4e") >= 0 && can_wr) {
                    uint8_t mode = 0x01;
                    pChar->writeValue(&mode, 1, false);
                    app_log("HOGP", "Set Protocol Mode to Report Mode (0x01)");
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                // HID Control Point (0x2A4C) -> write Exit Suspend (0x00)
                else if (char_uuid.indexOf("2a4c") >= 0 && can_wr) {
                    uint8_t cp = 0x00;
                    pChar->writeValue(&cp, 1, false);
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                // HOGP Report (0x2A4D or 0x2A22) -> subscribe with interval
                else if ((char_uuid.indexOf("2a4d") >= 0 || char_uuid.indexOf("2a22") >= 0) && (can_notif || can_ind)) {
                    pChar->subscribe(true, on_hogp_report_notify, false);
                    sub_count++;
                    app_log("HOGP", "Subscribed to Report Char: %s", char_uuid.c_str());
                    vTaskDelay(pdMS_TO_TICKS(25));
                }
            }
        }
    } else {
        app_log("HOGP", "Warning: HID Service (0x1812) not found in GATT services!");
    }

    // STEP 3: Process Battery Service (0x180F)
    if (bat_svc) {
        app_log("BATTERY", "Discovering Battery characteristics...");
        std::vector<NimBLERemoteCharacteristic*>* pChars = nullptr;
        for (int retry = 0; retry < 3; retry++) {
            pChars = bat_svc->getCharacteristics(true);
            if (pChars && !pChars->empty()) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (pChars) {
            for (auto* pChar : *pChars) {
                String char_uuid = pChar->getUUID().toString().c_str();
                char_uuid.toLowerCase();
                if (pChar->getUUID().equals(NimBLEUUID((uint16_t)0x2A19)) || char_uuid.indexOf("2a19") >= 0) {
                    s_char_bat = pChar;
                    if (pChar->canRead()) {
                        NimBLEAttValue val = pChar->readValue();
                        if (val.length() >= 1) {
                            s_battery_pct = (int)val[0];
                            app_log("BATTERY", "Remote battery initial level: %d%%", s_battery_pct);
                            led_indicator_set_low_battery(s_battery_pct >= 0 && s_battery_pct <= 15);
                        }
                    }
                    s_last_battery_poll_ms = millis();
                    if (pChar->canNotify()) {
                        pChar->subscribe(true, on_battery_notify, false);
                        sub_count++;
                        app_log("BATTERY", "Subscribed to Battery Level notifications");
                        vTaskDelay(pdMS_TO_TICKS(25));
                    }
                }
            }
        }
    } else {
        app_log("BATTERY", "Battery Service (0x180F) not found in GATT services");
    }

    // Read (never write) the remote's Device Information: model / firmware / hw
    // version, surfaced in the WebUI pairing card.
    read_device_information();

    app_log("BLE", "Total Subscribed Characteristic(s): %d", sub_count);

    // 3. Negotiate data length and connection parameters
    s_client->setDataLen(251);
    // 40 units = 50ms connection interval (was 37.5ms): modest increase to cut
    // idle link TX further while keeping key/voice latency acceptable.
    s_client->updateConnParams(40, 40, 0, 400);

    // 4. ATVV Handshake: query CAPS capability only. Do NOT force mic open at boot.
    if (s_char_cmd) {
        uint8_t cmd_caps[] = { 0x0A, 0x01, 0x00, 0x00, 0x03, 0x03 };
        s_char_cmd->writeValue(cmd_caps, sizeof(cmd_caps), false);
        app_log("ATVV", "Handshake: GET_CAPS query sent (Mic remains in low-power standby)");
    }

    s_ble_state = BLE_STATE_CONNECTED;
    led_indicator_set(LED_STATE_CONNECTED);
    s_last_keepalive_ms = millis();

    // Note: an enabled GATT dump was already executed in the handshake window
    // above (pre-subscription, full coverage). Re-arming s_gatt_dump_pending
    // here would run a second, vendor-only dump on the same connection.
    return true;
}

static bool do_connect_adv_device(NimBLEAdvertisedDevice* advDevice) {
    if (!advDevice) return false;
    s_ble_state = BLE_STATE_CONNECTING;

    if (s_client == nullptr) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(new ClientCallbacks(), false);
        s_client->setConnectTimeout(4);
    } else if (s_client->isConnected()) {
        s_client->disconnect();
    }
    s_client->setConnectTimeout(4);

    app_log("BLE", "Connecting to Advertised Device: %s (%s, Type: %d)...", 
            advDevice->getName().c_str(), advDevice->getAddress().toString().c_str(), 
            (int)advDevice->getAddress().getType());

    if (!s_client->connect(advDevice)) {
        app_log("BLE", "Connection Failed to %s", advDevice->getAddress().toString().c_str());
        s_ble_state = BLE_STATE_DISCONNECTED;
        start_scan();
        return false;
    }

    s_connected_name = sanitize_ble_name(advDevice->getName().c_str());
    s_connected_mac = advDevice->getAddress().toString().c_str();
    s_bound_addr_type = advDevice->getAddress().getType();
    if (advDevice->getName().length() == 0) {
        s_connected_name = (s_bound_name.length() > 0) ? s_bound_name : "Xiaomi Voice Remote";
    }

    // Save bound MAC to NVS:
    // Only update NVS if not previously bound, or if reconnecting to our bound remote whose MAC rotated
    if (s_bound_mac.length() == 0 || !s_bound_mac.equalsIgnoreCase(s_connected_mac)) {
        s_bound_mac = s_connected_mac;
        s_bound_name = s_connected_name;
        s_ble_prefs.putString("bound_mac", s_bound_mac);
        s_ble_prefs.putString("bound_name", s_bound_name);
        s_ble_prefs.putUChar("bound_type", s_bound_addr_type);
        app_log("BLE", "Bound and saved remote: %s (%s, Type: %d)", s_bound_name.c_str(), s_bound_mac.c_str(), (int)s_bound_addr_type);
    }

    return setup_services_and_handshake();
}

static bool do_connect_mac(const String& mac_str, uint8_t addr_type) {
    s_ble_state = BLE_STATE_CONNECTING;
    NimBLEDevice::getScan()->stop();

    if (s_client == nullptr) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(new ClientCallbacks(), false);
        s_client->setConnectTimeout(4);
    } else if (s_client->isConnected()) {
        s_client->disconnect();
    }
    s_client->setConnectTimeout(4);

    // Try primary addr_type
    NimBLEAddress addr1(mac_str.c_str(), addr_type);
    app_log("BLE", "Connecting to MAC: %s (Type: %d)...", mac_str.c_str(), (int)addr_type);
    bool ok = s_client->connect(addr1);

    // If failed, try alternative addr_type (Public vs Random)
    if (!ok) {
        uint8_t alt_type = (addr_type == BLE_ADDR_RANDOM) ? BLE_ADDR_PUBLIC : BLE_ADDR_RANDOM;
        NimBLEAddress addr2(mac_str.c_str(), alt_type);
        app_log("BLE", "Retrying with alternate Type: %d...", (int)alt_type);
        ok = s_client->connect(addr2);
        if (ok) addr_type = alt_type;
    }

    if (!ok) {
        app_log("BLE", "Direct link to %s timed out. Background scanner active, awaiting remote broadcast...", mac_str.c_str());
        s_ble_state = BLE_STATE_DISCONNECTED;
        start_scan();
        return false;
    }

    s_connected_mac = mac_str;
    s_connected_name = "Xiaomi Voice Remote";
    portENTER_CRITICAL(&s_disc_mux);
    for (const auto& d : s_discovered_devices) {
        if (d.mac.equalsIgnoreCase(mac_str) && d.name.length() > 0 && d.name != "Unnamed BLE Device") {
            s_connected_name = d.name;
            break;
        }
    }
    portEXIT_CRITICAL(&s_disc_mux);

    s_bound_mac = s_connected_mac;
    s_bound_name = s_connected_name;
    s_bound_addr_type = addr_type;
    s_ble_prefs.putString("bound_mac", s_bound_mac);
    s_ble_prefs.putString("bound_name", s_bound_name);
    s_ble_prefs.putUChar("bound_type", s_bound_addr_type);
    app_log("BLE", "Manually paired and saved: %s (%s, Type: %d)", s_bound_name.c_str(), s_bound_mac.c_str(), (int)s_bound_addr_type);

    return setup_services_and_handshake();
}

// Read the remote's Device Information Service (0x180A) - read-only, never written.
// The remote answers 0x180A inconsistently right after pairing: the first pass
// often comes back empty because the characteristic list has not settled yet, so
// retry a few times with a short pause before giving up.
static void read_device_information() {
    s_dev_model = "";
    s_dev_manuf = "";
    s_dev_serial = "";
    s_dev_hw = "";
    s_dev_fw = "";
    s_dev_sw = "";
    if (!s_client || !s_client->isConnected()) return;

    for (int attempt = 1; attempt <= 4; ++attempt) {
        NimBLERemoteService* dis = s_client->getService(NimBLEUUID((uint16_t)0x180A));
        if (!dis) {
            app_log("GATTX", "Device Information Service (0x180A) not found; remote model info unavailable");
            return;
        }
        std::vector<NimBLERemoteCharacteristic*>* pChars = dis->getCharacteristics(true);
        if (pChars && !pChars->empty()) {
            struct { const char* sub; String* dst; } fields[] = {
                { "2a29", &s_dev_manuf  }, // Manufacturer Name
                { "2a24", &s_dev_model  }, // Model Number
                { "2a25", &s_dev_serial }, // Serial Number
                { "2a27", &s_dev_hw     }, // Hardware Revision
                { "2a26", &s_dev_fw     }, // Firmware Revision
                { "2a28", &s_dev_sw     }, // Software Revision
            };
            for (auto* pChar : *pChars) {
                if (!pChar) continue;
                String cu = pChar->getUUID().toString().c_str();
                cu.toLowerCase();
                for (auto& f : fields) {
                    if (f.dst->length() > 0) continue;
                    if (cu.indexOf(f.sub) < 0) continue;
                    if (!pChar->canRead()) break;
                    NimBLEAttValue val = pChar->readValue();
                    if (val.length() > 0 && val.length() <= 32) {
                        String s((const char*)val.data(), val.length());
                        s.trim();
                        *f.dst = s;
                    }
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(15));
            }
        }
        // Model number plus firmware revision identify the remote well enough.
        if (s_dev_model.length() > 0 && s_dev_fw.length() > 0) break;
        if (attempt < 4) {
            app_log("GATTX", "Device info incomplete on pass %d, retrying...", attempt);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    app_log("GATTX", "Remote device info: model=%s, mfr=%s, serial=%s, hw=%s, fw=%s, sw=%s",
            s_dev_model.c_str(), s_dev_manuf.c_str(), s_dev_serial.c_str(),
            s_dev_hw.c_str(), s_dev_fw.c_str(), s_dev_sw.c_str());
}

// Hex + printable-ASCII rendering helper for one characteristic/descriptor value.
static void gatt_dump_bytes(const char* tag, const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        app_log(tag, "  value: (empty)");
        return;
    }
    app_log(tag, "  value: len=%u bytes", (unsigned)len);
    char hex[4];
    String line = "";
    for (size_t i = 0; i < len; ++i) {
        snprintf(hex, sizeof(hex), "%02X", data[i]);
        line += hex;
        if ((i % 24) == 23 || i == len - 1) {
            app_log(tag, "    %s", line.c_str());
            line = "";
        }
    }
    String ascii;
    ascii.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        char c = (char)data[i];
        ascii += (c >= 0x20 && c != 0x7F) ? c : '.';
    }
    if (ascii.length() > 64) ascii = ascii.substring(0, 64) + "...";
    app_log(tag, "    ascii: %s", ascii.c_str());
}

// Passive vendor notify listener. Logs whatever the remote pushes on the
// never-decoded vendor services. Read-only by design: no command characteristic
// of these services is ever written.
static void on_vendor_spy_notify(NimBLERemoteCharacteristic* pChar,
                                 uint8_t* data, uint16_t len, bool is_notify) {
    (void)is_notify;
    const char* tag = "VNDSPY";
    for (uint8_t i = 0; i < s_spy_count; ++i) {
        if (s_spy_chars[i] == pChar && s_spy_tags[i]) { tag = s_spy_tags[i]; break; }
    }
    app_log(tag, "notify len=%u", (unsigned)len);
    gatt_dump_bytes(tag, data, (size_t)len);
}

static void gatt_spy_stop(void) {
    for (uint8_t i = 0; i < s_spy_count; ++i) {
        if (s_spy_chars[i]) {
            s_spy_chars[i]->unsubscribe();
        }
        s_spy_chars[i] = nullptr;
        s_spy_tags[i] = nullptr;
    }
    if (s_spy_count > 0) app_log("VNDSPY", "Passive vendor listener stopped");
    s_spy_count = 0;
}

// Remember a vendor NOTIFY characteristic for passive listening. Collection
// happens inside the explorer walk (no second service discovery, which comes
// back empty right after the intensive dump); the actual subscribe() is done
// once the walk is finished. Nothing is committed until the walk produced at
// least one hit, so a degraded re-walk cannot silently kill a live listener.
static NimBLERemoteCharacteristic*     s_spy_pend[8] = { nullptr };
static const char*                     s_spy_pend_tag[8] = { nullptr };
static uint8_t                         s_spy_pend_count = 0;

static void gatt_spy_collect(NimBLERemoteCharacteristic* pChar, const char* tag) {
    if (!pChar || !pChar->canNotify()) return;
    for (uint8_t i = 0; i < s_spy_pend_count; ++i) {
        if (s_spy_pend[i] == pChar) return; // already tracked
    }
    if (s_spy_pend_count >= 8) return;
    s_spy_pend[s_spy_pend_count] = pChar;
    s_spy_pend_tag[s_spy_pend_count] = tag;
    s_spy_pend_count++;
}

// Subscribe to every collected vendor NOTIFY characteristic. Only the standard
// CCCD is written by NimBLE's subscribe(); no vendor command is ever written.
static void gatt_spy_subscribe_all(void) {
    if (s_spy_pend_count == 0) {
        app_log("VNDSPY", "No vendor NOTIFY characteristic discovered; passive listener unchanged");
        return;
    }
    gatt_spy_stop(); // retire the previous link's subscriptions
    for (uint8_t i = 0; i < s_spy_pend_count; ++i) {
        s_spy_chars[i] = s_spy_pend[i];
        s_spy_tags[i]  = s_spy_pend_tag[i];
    }
    s_spy_count = s_spy_pend_count;
    for (uint8_t i = 0; i < s_spy_count; ++i) {
        String cu = s_spy_chars[i]->getUUID().toString().c_str();
        app_log("VNDSPY", "Subscribing (notify only) %s char %s",
                s_spy_tags[i] ? s_spy_tags[i] : "?", cu.c_str());
        s_spy_chars[i]->subscribe(true, on_vendor_spy_notify, false);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    s_spy_pend_count = 0;
    app_log("VNDSPY", "Passive vendor listener active on %u characteristic(s)", (unsigned)s_spy_count);
}

// Full GATT enumeration. Only runs on the Core 0 BLE task (like the handshake),
// when the remote is connected and the ATVV handshake is done.
//
// allow_refresh selects HOW MUCH we are allowed to re-discover:
//   true  -> handshake window, BEFORE any subscription exists. Every service may
//            be re-discovered, so the dump covers the complete attribute
//            database. This is the only safe point for a full dump.
//   false -> runtime, while ATVV/HOGP/battery subscriptions are live. Only
//            services the handshake never walked may be discovered.
static void gatt_explore_all(bool allow_refresh) {
    if (!s_client || !s_client->isConnected()) return;

    app_log("GATTX", "===== GATT EXPLORER: %s dump start =====",
            allow_refresh ? "FULL (handshake window, pre-subscription)" : "RUNTIME (vendor only)");
    app_log("GATTX", "Looking for untapped capabilities: 0x180A Device Info / 0x2A4A Report Map / 0xfe59 / 0x01bf / 8a7a0001");
    s_spy_pend_count = 0;
    int svc_count = 0;
    int char_count = 0;
    int read_count = 0;

    // NEVER refresh the service cache here. getServices(true) purges and rebuilds
    // every NimBLERemoteService/Characteristic object, which silently invalidates
    // the ATVV / HOGP / battery pointers subscribed during the handshake - the
    // remote then stops delivering key reports. The handshake already discovered
    // the services, so the cached list is complete.
    std::vector<NimBLERemoteService*>* pServices = s_client->getServices(false);
    if (!pServices) {
        app_log("GATTX", "No services discovered on this connection");
        return;
    }

    for (auto* pSvc : *pServices) {
        if (!pSvc) continue;
        String su = pSvc->getUUID().toString().c_str();
        app_log("GATTX", "Service %s ...", su.c_str());
        svc_count++;

        // Identify the three never-decoded vendor services for passive listening.
        String su_lc = su; su_lc.toLowerCase();
        const char* vendor_tag = nullptr;
        if (su_lc.indexOf("fe59") >= 0)      vendor_tag = "V-FE59";
        else if (su_lc.indexOf("8a7a0001") >= 0) vendor_tag = "V-8A7A";
        else if (su_lc.indexOf("01bf") >= 0)  vendor_tag = "V-01BF";

        // Refresh rules. getCharacteristics(true) calls deleteCharacteristics(),
        // which destroys and recreates every NimBLERemoteCharacteristic wrapper.
        // In the handshake window that is harmless (nothing is subscribed yet) and
        // gives complete coverage. At runtime it would silently invalidate the
        // live ATVV/HOGP/battery pointers, so fall back to discovery only for
        // services the handshake never walked.
        std::vector<NimBLERemoteCharacteristic*>* pChars =
            allow_refresh ? pSvc->getCharacteristics(true) : pSvc->getCharacteristics(false);
        if (!pChars || pChars->empty()) {
            pChars = pSvc->getCharacteristics(true);
        }
        if (!pChars || pChars->empty()) {
            app_log("GATTX", "  (no characteristics discovered)");
            continue;
        }

        for (auto* pChar : *pChars) {
            if (!pChar) continue;
            String cu = pChar->getUUID().toString().c_str();
            String props;
            if (pChar->canRead())            props += "READ ";
            if (pChar->canWrite())           props += "WRITE ";
            if (pChar->canWriteNoResponse()) props += "WRITENR ";
            if (pChar->canNotify())          props += "NOTIFY ";
            if (pChar->canIndicate())        props += "INDICATE ";
            if (pChar->canBroadcast())       props += "BROADCAST ";
            app_log("GATTX", "  Char %s [%s]", cu.c_str(), props.c_str());
            char_count++;

            // Vendor services: remember the NOTIFY ones for passive listening.
            if (vendor_tag != nullptr) {
                gatt_spy_collect(pChar, vendor_tag);
            }

            if (pChar->canRead()) {
                NimBLEAttValue val = pChar->readValue();
                read_count++;
                gatt_dump_bytes("GATTX", val.data(), val.length());
            }

            std::vector<NimBLERemoteDescriptor*>* pDescs = pChar->getDescriptors(true);
            if (pDescs) {
                for (auto* pDesc : *pDescs) {
                    if (!pDesc) continue;
                    String du = pDesc->getUUID().toString().c_str();
                    app_log("GATTX", "    Desc %s", du.c_str());
                    NimBLEAttValue dval = pDesc->readValue();
                    if (dval.length() > 0) {
                        gatt_dump_bytes("GATTX", dval.data(), dval.length());
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(8));
        }
    }

    // Keep the mic pipeline coherent after a long enumeration: re-query caps
    // so the ATVV link stays awake and voice-ready. In the handshake window
    // ATVV has not been discovered yet, so the handshake issues GET_CAPS itself.
    if (!allow_refresh && s_char_cmd) {
        uint8_t cmd_caps[] = { 0x0A, 0x01, 0x00, 0x00, 0x03, 0x03 };
        s_char_cmd->writeValue(cmd_caps, sizeof(cmd_caps), false);
        app_log("GATTX", "Re-sent GET_CAPS after dump");
    }
    // One-line summary: a full dump emits several hundred lines and overruns the
    // 120-entry log ring, so the totals are the only part that survives rotation.
    app_log("GATTX", "SUMMARY mode=%s services=%d chars=%d values_read=%d vendor_notify=%d",
            allow_refresh ? "FULL" : "RUNTIME", svc_count, char_count, read_count, (int)s_spy_pend_count);
    app_log("GATTX", "===== GATT EXPLORER: dump complete =====");

    // From here on, passively log whatever the vendor services push at us.
    gatt_spy_subscribe_all();
}
extern "C" {


void ble_remote_init(void) {
    s_ble_prefs.begin("ble_conf", false);
    s_bound_mac = s_ble_prefs.getString("bound_mac", "");
    s_bound_name = s_ble_prefs.getString("bound_name", "");
    s_bound_addr_type = s_ble_prefs.getUChar("bound_type", BLE_ADDR_RANDOM);

    if (s_bound_mac.length() > 0) {
        app_log("BLE", "Loaded previously bound remote: %s (%s, Type: %d)", s_bound_name.c_str(), s_bound_mac.c_str(), (int)s_bound_addr_type);
    }

    NimBLEDevice::init("ESP32-RemoteBridge");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    // Just Works bonding: bonding=true, mitm=false, sc=true
    // (MITM must be false because BLE_HS_IO_NO_INPUT_OUTPUT cannot support MITM authentication)
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
    
    // Start continuous fast scan immediately (instant catch when remote advertises)
    start_scan();
}

void ble_remote_notify_wifi_wake(void) {
    s_req_wifi_wake_rescan = true;
}

bool ble_remote_sniff_enabled(void) {
    return s_sniff_adv;
}

void ble_remote_sniff_set(bool on) {
    s_sniff_adv = on;
    if (!on) {
        // Normal behaviour resumes next task tick: reconnect flow re-enables
        // once the remote is seen again.
        s_disconnected_since_ms = 0;
    }
}

bool ble_remote_gatt_dump_enabled(void) {
    return s_gatt_dump_enabled;
}

void ble_remote_gatt_dump_request(bool on) {
    s_gatt_dump_enabled = on;
    s_gatt_dump_pending = on; // dump on the current (or next) connection
    if (!on) gatt_spy_stop();
}


void ble_remote_task(void) {
    uint32_t now = millis();

    // 0. MiOT sniffer: never connect, keep fast scanning so we capture the
    // remote's burst advertisements; if a link somehow exists, drop it.
    if (s_sniff_adv) {
        s_do_connect = false;
        s_pending_mac = "";
        if (s_pending_adv_device) {
            delete s_pending_adv_device;
            s_pending_adv_device = nullptr;
        }
        if (s_ble_state >= BLE_STATE_CONNECTING) {
            if (s_client && s_client->isConnected()) s_client->disconnect();
            s_ble_state = BLE_STATE_DISCONNECTED;
            s_is_encrypted = false;
        }
        s_disconnected_since_ms = 0; // keep tier-0 fast scanning
        if (!NimBLEDevice::getScan()->isScanning()) {
            start_scan(BLE_SCAN_INTERVAL_MS);
        }
    }

    // 1. Process asynchronous unpair / reconnect requests from Core 1
    if (s_req_unpair || s_req_reconnect) {
        bool is_unpair = s_req_unpair;
        s_req_unpair = false;
        s_req_reconnect = false;
        if (s_client && s_client->isConnected()) {
            s_client->disconnect();
        }
        if (s_pending_adv_device) {
            delete s_pending_adv_device;
            s_pending_adv_device = nullptr;
        }
        s_pending_mac = "";
        s_do_connect = false;
        if (is_unpair) {
            s_connected_name = "";
            s_connected_mac = "";
        }
        s_ble_state = BLE_STATE_DISCONNECTED;
        start_scan();
    }

    // 2. Process asynchronous connection requests from FreeRTOS task
    if (s_do_connect) {
        s_do_connect = false;
        // A continuous-scan restart can slip in between the scan callback
        // queueing this connect and this tick (the state machine has not
        // advanced yet). Kill it: scanning through a connect keeps the radio
        // receiver on for the whole session (major idle power waste).
        if (NimBLEDevice::getScan()->isScanning()) {
            NimBLEDevice::getScan()->stop();
        }
        if (s_client && s_client->isConnected()) {
            s_client->disconnect();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_pending_adv_device) {
            NimBLEAdvertisedDevice* adv = s_pending_adv_device;
            s_pending_adv_device = nullptr;
            do_connect_adv_device(adv);
            delete adv;
        } else if (s_pending_mac.length() > 0) {
            String mac = s_pending_mac;
            uint8_t type = s_pending_addr_type;
            s_pending_mac = "";
            do_connect_mac(mac, type);
        }
    }

    // 3. Auto Re-scan with backoff: while the remote is away, scan less often
    // the longer it stays away (fast right after a drop, then medium/low duty).
    if (s_ble_state < BLE_STATE_CONNECTING && !s_do_connect && !s_req_unpair && !s_req_reconnect) {
        if (s_req_wifi_wake_rescan) {
            // Wi-Fi just woke and may have starved/ dropped the BLE link; the
            // remote is likely advertising again right now. Drop backoff -> fast.
            s_req_wifi_wake_rescan = false;
            s_disconnected_since_ms = 0;
            s_scan_tier = 0xFF; // force restart below
            app_log("BLE", "Wi-Fi wake: BLE rescans at fast tier to re-catch remote");
        }
        if (s_disconnected_since_ms == 0) {
            s_disconnected_since_ms = now;
        }
        uint32_t away = now - s_disconnected_since_ms;
        uint16_t want_interval = BLE_SCAN_INTERVAL_MS;
        uint8_t tier = 0;
        if (away >= BLE_SCAN_TIER2_AFTER_MS) {
            want_interval = BLE_SCAN_SLOW_INTERVAL_MS;
            tier = 2;
        } else if (away >= BLE_SCAN_TIER1_AFTER_MS) {
            want_interval = BLE_SCAN_MED_INTERVAL_MS;
            tier = 1;
        }

        bool need_start = !NimBLEDevice::getScan()->isScanning();
        if (need_start || tier != s_scan_tier) {
            // Do not fight an active WebUI burst.
            if (s_scan_burst_until_ms == 0 || now >= s_scan_burst_until_ms) {
                s_scan_tier = tier;
                start_scan(want_interval);
            }
        }
    } else {
        // Connected or connecting: reset the away-timer for the next drop.
        s_disconnected_since_ms = 0;
        s_scan_tier = 0;
    }

    // 3b. Power save: never keep the radio scanning while connecting or
    // connected. While connected, only an active WebUI burst is tolerated.
    if (s_ble_state >= BLE_STATE_CONNECTING && NimBLEDevice::getScan()->isScanning()) {
        bool burst_active = (s_ble_state >= BLE_STATE_CONNECTED) &&
                            (s_scan_burst_until_ms != 0) && (now < s_scan_burst_until_ms);
        if (!burst_active) {
            NimBLEDevice::getScan()->stop();
        }
    }

    // 3c. WebUI requested a scan burst (fresh device list).
    if (s_req_scan_burst) {
        s_req_scan_burst = false;
        // Allowed while disconnected/scanning or connected; not while a
        // connection is in progress (would disturb the handshake).
        if (s_ble_state != BLE_STATE_CONNECTING && s_ble_state != BLE_STATE_TALKING &&
            !s_do_connect && !s_req_unpair && !s_req_reconnect) {
            start_web_scan_burst();
        }
    }

    // 3d. Diagnostic: report how many devices a finished burst collected.
    static bool s_burst_was_active = false;
    uint32_t burst_ms = millis();
    bool burst_now = (s_scan_burst_until_ms != 0) && (burst_ms < s_scan_burst_until_ms);
    if (s_burst_was_active && !burst_now) {
        portENTER_CRITICAL(&s_disc_mux);
        size_t n = s_discovered_devices.size();
        portEXIT_CRITICAL(&s_disc_mux);
        app_log("BLE", "Scan burst finished: %u device(s) cached", (unsigned)n);
        s_scan_burst_until_ms = 0;
    }
    s_burst_was_active = burst_now;

    // 4. Background recovery if ATVV was missed during handshake (runs safely on Core 0 FreeRTOS task)
    if (s_ble_state >= BLE_STATE_CONNECTED && s_client && s_client->isConnected() && s_char_cmd == nullptr) {
        static uint32_t s_last_atvv_recovery = 0;
        if (now - s_last_atvv_recovery > 3000) {
            s_last_atvv_recovery = now;
            app_log("ATVV", "Background recovery: re-checking ATVV characteristics...");
            NimBLERemoteService* atvv = s_client->getService(NimBLEUUID(ATVV_SVC_UUID));
            if (atvv) {
                std::vector<NimBLERemoteCharacteristic*>* pChars = atvv->getCharacteristics(true);
                if (pChars) {
                    for (auto* pChar : *pChars) {
                        String char_uuid = pChar->getUUID().toString().c_str();
                        char_uuid.toLowerCase();
                        if (char_uuid.indexOf("ab5e0002") >= 0) s_char_cmd = pChar;
                        else if (char_uuid.indexOf("ab5e0003") >= 0) {
                            s_char_aud = pChar;
                            if (pChar->canNotify()) pChar->subscribe(true, on_audio_notify, false);
                        } else if (char_uuid.indexOf("ab5e0004") >= 0) {
                            s_char_ctl = pChar;
                            if (pChar->canNotify()) pChar->subscribe(true, on_ctl_notify, false);
                        }
                    }
                    if (s_char_cmd) {
                        app_log("ATVV", "Background recovery succeeded: cmd=%p, aud=%p, ctl=%p", s_char_cmd, s_char_aud, s_char_ctl);
                        uint8_t cmd_caps[] = { 0x0A, 0x01, 0x00, 0x00, 0x03, 0x03 };
                        s_char_cmd->writeValue(cmd_caps, sizeof(cmd_caps), false);
                    }
                }
            }
        }
    }

    // 5. Periodic Battery polling (every 30 minutes if connected, fallback for notifications)
    if (s_ble_state >= BLE_STATE_CONNECTED && s_client && s_client->isConnected() && s_char_bat != nullptr) {
        if (now - s_last_battery_poll_ms >= 1800000) {
            s_last_battery_poll_ms = now;
            if (s_char_bat->canRead()) {
                NimBLEAttValue val = s_char_bat->readValue();
                if (val.length() >= 1) {
                    s_battery_pct = (int)val[0];
                    app_log("BATTERY", "Periodic battery poll: %d%%", s_battery_pct);
                    led_indicator_set_low_battery(s_battery_pct >= 0 && s_battery_pct <= 15);
                }
            }
        }
    }

    // 6. GATT explorer: run the one-shot enumeration once the remote is
    // connected and the ATVV handshake is complete. Not while talking (disturbs
    // the voice pipe). Re-dumps on every fresh connection while enabled.
    if (s_gatt_dump_pending && s_ble_state == BLE_STATE_CONNECTED &&
        s_client && s_client->isConnected() && s_char_cmd != nullptr) {
        // Cooldown: a walk hammers the remote with dozens of reads, so a second
        // trigger arriving right after would walk an unresponsive link and log a
        // misleading empty dump. Defer instead of running back-to-back.
        static uint32_t s_last_dump_ms = 0;
        if (millis() - s_last_dump_ms >= 10000) {
            s_gatt_dump_pending = false;
            s_last_dump_ms = millis();
            gatt_explore_all(false);
        }
    }

    // 7. ATVV keepalive: the remote aborts an active microphone session after a
    // hardware timeout (~15-60s) unless MIC_EXTEND (0x0E 0x00) keeps arriving.
    // Without it, long voice sessions are cut off by the remote itself. Refresh
    // the timer whenever audio actually flows so a live session never spams.
    if (s_ble_state == BLE_STATE_TALKING && s_char_cmd != nullptr &&
        s_client && s_client->isConnected()) {
        if (now - s_last_audio_ms > 300) {
            // No audio for 300ms: the remote is holding the mic open but idle.
            if (now - s_last_extend_ms >= 10000) {
                uint8_t cmd_extend[] = { 0x0E, 0x00 };
                s_char_cmd->writeValue(cmd_extend, sizeof(cmd_extend), false);
                s_last_extend_ms = now;
                app_log("ATVV", "MIC_EXTEND keepalive sent (holding voice session open)");
            }
        } else {
            s_last_extend_ms = now; // audio flowing, no keepalive needed
        }
    }
}

ble_remote_state_t ble_remote_get_state(void) {
    return s_ble_state;
}

void ble_remote_trigger_reconnect(void) {
    app_log("BLE", "Triggering BLE reconnection on Core 0...");
    s_req_reconnect = true;
}

String ble_remote_scan_devices_json(void) {
    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();

    uint32_t now = millis();
    portENTER_CRITICAL(&s_disc_mux);
    for (const auto& dev : s_discovered_devices) {
        // Only report devices the scan actually saw recently; without the
        // continuous scan the cache is no longer self-purging on every event.
        if (now - dev.last_seen_ms > 20000) {
            continue;
        }
        JsonObject obj = arr.add<JsonObject>();
        obj["name"] = dev.name;
        obj["mac"] = dev.mac;
        obj["rssi"] = dev.rssi;
        obj["type"] = (int)dev.type;
    }
    portEXIT_CRITICAL(&s_disc_mux);

    String out;
    serializeJson(doc, out);
    return out;
}

void ble_remote_request_scan_burst(void) {
    s_req_scan_burst = true;
}

bool ble_remote_connect_target(const String& mac_str, uint8_t addr_type, const String& dev_name) {
    if (mac_str.length() == 0) return false;

    app_log("BLE", "Manual target requested: %s (%s, Type: %d)", 
            dev_name.length() > 0 ? dev_name.c_str() : "Unknown", 
            mac_str.c_str(), (int)addr_type);

    // Delete any old bond for this target address so fresh pairing can proceed cleanly
    NimBLEDevice::deleteBond(NimBLEAddress(mac_str.c_str(), addr_type));

    // Save target as bound remote immediately in memory and NVS
    s_bound_mac = mac_str;
    s_bound_name = (dev_name.length() > 0 && dev_name != "Unnamed BLE Device") ? dev_name : "Xiaomi Voice Remote";
    s_bound_addr_type = addr_type;
    s_ble_prefs.putString("bound_mac", s_bound_mac);
    s_ble_prefs.putString("bound_name", s_bound_name);
    s_ble_prefs.putUChar("bound_type", s_bound_addr_type);
    app_log("BLE", "Bound target saved to NVS: %s (%s)", s_bound_name.c_str(), s_bound_mac.c_str());

    // Queue connection to be handled safely on Core 0
    s_pending_mac = mac_str;
    s_pending_addr_type = addr_type;
    s_do_connect = true;
    return true;
}

bool ble_remote_connect_mac(const String& mac_str) {
    uint8_t addr_type = BLE_ADDR_PUBLIC;
    String dev_name = "Xiaomi Voice Remote";
    portENTER_CRITICAL(&s_disc_mux);
    for (const auto& d : s_discovered_devices) {
        if (d.mac.equalsIgnoreCase(mac_str)) {
            addr_type = d.type;
            if (d.name.length() > 0 && d.name != "Unnamed BLE Device") {
                dev_name = d.name;
            }
            break;
        }
    }
    portEXIT_CRITICAL(&s_disc_mux);
    return ble_remote_connect_target(mac_str, addr_type, dev_name);
}

void ble_remote_unpair(void) {
    s_bound_mac = "";
    s_bound_name = "";
    s_ble_prefs.remove("bound_mac");
    s_ble_prefs.remove("bound_name");
    s_ble_prefs.remove("bound_type");
    NimBLEDevice::deleteAllBonds();
    app_log("BLE", "Unpaired: deleted all NimBLE bonds and cleared saved MAC from NVS");
    s_req_unpair = true;
}

String ble_remote_get_connected_info(void) {
    JsonDocument doc;
    doc["connected"] = (s_ble_state >= BLE_STATE_CONNECTED);
    doc["state"] = (int)s_ble_state;
    doc["name"] = s_connected_name;
    doc["mac"] = s_connected_mac;
    doc["bound_mac"] = s_bound_mac;
    doc["bound_name"] = s_bound_name;
    doc["battery_pct"] = s_battery_pct;
    doc["dev_model"] = s_dev_model;
    doc["dev_manufacturer"] = s_dev_manuf;
    doc["dev_serial"] = s_dev_serial;
    doc["dev_hw"] = s_dev_hw;
    doc["dev_fw"] = s_dev_fw;
    doc["dev_sw"] = s_dev_sw;
    String out;
    serializeJson(doc, out);
    return out;
}

int ble_remote_get_battery_pct(void) {
    return s_battery_pct;
}

String ble_remote_get_device_info_model(void) {
    return s_dev_model;
}
String ble_remote_get_device_info_manufacturer(void) {
    return s_dev_manuf;
}
String ble_remote_get_device_info_serial(void) {
    return s_dev_serial;
}
String ble_remote_get_device_info_hw(void) {
    return s_dev_hw;
}
String ble_remote_get_device_info_fw(void) {
    return s_dev_fw;
}
String ble_remote_get_device_info_sw(void) {
    return s_dev_sw;
}

} // extern "C"
