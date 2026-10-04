#include "web_server.h"
#include "web_ui.h"
#include "log/app_log.h"
#include "wifi/wifi_manager.h"
#include "version.h"
#include "ble/ble_remote_client.h"
#include "audio/audio_pipeline.h"
#include "usb/uac_microphone.h"
#include "usb/usb_composite.h"
#include "usb/hid_diagnostics.h"
#include "usb/dwc2_diagnostics.h"
#include "core_diagnostics.h"
#include "keymap/key_state_machine.h"
#include "keymap/key_config_storage.h"
#include "nvs/nvs_manager.h"
#include "wol_manager.h"
#include "ota/ota_manager.h"
#include "config/config_backup.h"
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>

#ifndef REMOTEMAPPER_OTA
#define REMOTEMAPPER_OTA 0
#endif

static WebServer s_server(80);
extern key_mapper_engine_t g_key_engine;

static void handle_root() {
    s_server.sendHeader("Connection", "close");
    s_server.send(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handle_status() {
    wifi_manager_mark_activity();
    JsonDocument doc;
    doc["firmware"] = FIRMWARE_NAME;
    doc["version"] = FIRMWARE_VERSION;
    doc["uptime_sec"] = millis() / 1000;
    doc["boot_reset_reason"] = usb_composite_boot_reset_reason();
    doc["boot_usb_recovery_restart"] = usb_composite_boot_usb_recovery_restart();
    doc["ble_state"] = (int)ble_remote_get_state();
    doc["battery_pct"] = ble_remote_get_battery_pct();
    doc["frames_decoded"] = g_audio_pipeline.total_frames_decoded;
    doc["samples_pushed"] = g_audio_pipeline.total_samples_pushed;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["free_psram"] = ESP.getFreePsram();
    doc["ap_ip"] = wifi_manager_get_ap_ip();
    doc["sta_ip"] = wifi_manager_get_sta_ip();
    doc["sta_connected"] = wifi_manager_is_sta_connected();
    doc["sta_ssid"] = wifi_manager_is_sta_connected() ? wifi_manager_get_sta_ssid() : String("");
    doc["sta_rssi"] = (int)wifi_manager_get_sta_rssi();
    doc["ap_ssid"] = AP_SSID;
    doc["ap_running"] = wifi_manager_is_ap_running();
    doc["mdns_url"] = wifi_manager_get_mdns_url();
    doc["wifi_enabled"] = wifi_manager_get_enabled();
    doc["wifi_policy"] = (int)wifi_manager_get_policy();
    doc["wifi_policy_str"] = wifi_manager_policy_str(wifi_manager_get_policy());
    doc["wifi_timeout_min"] = wifi_manager_get_timeout_min();
    doc["wifi_timeout_enabled"] = wifi_manager_get_timeout_enabled();
    doc["wifi_radio_state"] = (int)wifi_manager_get_radio_state();
    String ap_pass = wifi_manager_get_ap_pass();
    doc["ap_secured"] = (ap_pass.length() >= 8);
    doc["ap_pass"] = ap_pass;
    // Run/target OTA partition visibility (diagnostic + upgrade confidence).
    const esp_partition_t* run_p = esp_ota_get_running_partition();
    if (run_p != NULL) {
        doc["ota_running_label"] = run_p->label;
    }
    const esp_partition_t* next_p = esp_ota_get_next_update_partition(NULL);
    if (next_p != NULL) {
        doc["ota_target_label"] = next_p->label;
    }

    usb_guard_config_t guard_cfg;
    usb_guard_stats_t  guard_stats;
    usb_composite_guard_get(&guard_cfg, &guard_stats);
    JsonObject guard = doc["guard"].to<JsonObject>();
    guard["mod_ms"]   = guard_cfg.mod_ms;
    guard["key_ms"]   = guard_cfg.key_ms;
    guard["voice_ms"] = guard_cfg.voice_ms;
    guard["forced"]   = guard_stats.forced_releases;
    guard["last_reason"] = guard_stats.last_reason;

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_logs() {
    wifi_manager_mark_activity();
    String json = app_log_get_json();
    s_server.send(200, "application/json", json);
}

static void handle_logs_clear() {
    app_log_clear();
    s_server.send(200, "application/json", "{\"status\":\"cleared\"}");
}

static void handle_audio() {
    wifi_manager_mark_activity();
    JsonDocument doc;
    uint8_t mute = 0;
    int16_t volume = 0;
    uac_microphone_get_control(&mute, &volume);
    uac_tx_stats_t tx = {};
    uac_microphone_get_stats(&tx);
    doc["usb_completed"] = tx.completed;
    doc["usb_failed"] = tx.failed;
    doc["usb_claim_skips"] = tx.claim_skips;
    doc["usb_recoveries"] = tx.recoveries;
    doc["usb_last_complete_ms"] = tx.last_complete_ms;
    doc["usb_endpoint"] = tx.endpoint;
    doc["usb_recovery_pending"] = tx.recovery_pending;
#if defined(REMOTEMAPPER_EXPERIMENTAL_DWC2)
    remotemapper_dwc2_stats_t dcd = {};
    remotemapper_dwc2_get_stats(&dcd);
    JsonObject controller = doc["dwc2"].to<JsonObject>();
    controller["bus_resets"] = dcd.bus_resets;
    controller["fifo_reset_failures"] = dcd.fifo_reset_failures;
    controller["iso_incomplete_events"] = dcd.iso_incomplete_events;
    controller["iso_retries"] = dcd.iso_retries;
    controller["iso_aborts"] = dcd.iso_aborts;
    controller["first_iso_ms"] = dcd.first_iso_ms;
    controller["first_iso_ep"] = dcd.first_iso_ep;
    controller["first_iso_epctl"] = dcd.first_iso_epctl;
    controller["first_iso_tsiz"] = dcd.first_iso_tsiz;
    controller["first_iso_dsts"] = dcd.first_iso_dsts;
#endif
    uint32_t fifo_rearms = 0, fifo_rearm_completions = 0;
    uac_microphone_get_fifo_rearm_stats(&fifo_rearms, &fifo_rearm_completions);
    doc["usb_fifo_rearms"] = fifo_rearms;
    doc["usb_fifo_rearm_completions"] = fifo_rearm_completions;
    uint32_t pcm_samples = 0, pcm_nonzero = 0;
    uint64_t pcm_absolute_sum = 0;
    uint16_t pcm_peak = 0;
    uac_microphone_get_pcm_stats(&pcm_samples, &pcm_nonzero, &pcm_absolute_sum, &pcm_peak);
    doc["usb_pcm_samples"] = pcm_samples;
    doc["usb_pcm_nonzero"] = pcm_nonzero;
    doc["usb_pcm_absolute_sum"] = pcm_absolute_sum;
    doc["usb_pcm_peak"] = pcm_peak;
    doc["streaming"] = uac_microphone_is_streaming();
    doc["alt"] = (int)uac_microphone_get_alt();
    doc["muted"] = (bool)(mute != 0);
    doc["volume_raw"] = volume;
    doc["session_active"] = g_audio_pipeline.active;
    doc["session_id"] = (int)g_audio_pipeline.session_id;
    doc["buffering"] = g_audio_pipeline.buffering;
    doc["ring_avail"] = (int)audio_ring_buffer_peek_available(&g_audio_pipeline.ring_buf);
    doc["ring_capacity"] = g_audio_pipeline.ring_buf.capacity;
    doc["underruns"] = (int)g_audio_pipeline.underrun_count;
    doc["starve_count"] = (int)g_audio_pipeline.starve_count;
    doc["partial_count"] = (int)g_audio_pipeline.partial_count;
    doc["wire_samples"] = (int)g_audio_pipeline.wire_samples;
    doc["padded_samples"] = (int)g_audio_pipeline.padded_samples;
    doc["frames_decoded"] = (int)g_audio_pipeline.total_frames_decoded;
    doc["samples_pushed"] = (int)g_audio_pipeline.total_samples_pushed;

    // Spectral probe on the decoded sequence: if there is real energy above
    // 6000 Hz then the remote captured above 12 kHz and ships 1/1 pitch, so the
    // 1:1 shortfall is dropped samples rather than a rate mismatch.
    uint16_t spec[AUDIO_SPEC_BINS];
    audio_pipeline_get_spectrum(&g_audio_pipeline, spec);
    String specStr = "[";
    for (int i = 0; i < AUDIO_SPEC_BINS; i++) {
        if (i) specStr += ',';
        specStr += String(spec[i]);
    }
    specStr += "]";
    doc["spec_hz_lo"] = (int)AUDIO_SPEC_BIN0_HZ;
    doc["spec_hz_hi"] = (int)AUDIO_SPEC_MAX_HZ;
    doc["spec_hz_step"] = (int)AUDIO_SPEC_BIN_HZ_STEP;
    doc["spec_peak"] = specStr;
    uint32_t specRaw[AUDIO_SPEC_BINS];
    audio_pipeline_get_spectrum_raw(&g_audio_pipeline, specRaw);
    String rawStr = "[";
    for (int i = 0; i < AUDIO_SPEC_BINS; i++) {
        if (i) rawStr += ',';
        rawStr += String(specRaw[i]);
    }
    rawStr += "]";
    doc["spec_raw"] = rawStr;

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_cores() {
    wifi_manager_mark_activity();
    JsonDocument doc;
    core_diagnostics_json(doc.as<JsonObject>());
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_guard() {
    wifi_manager_mark_activity();
    usb_guard_config_t cfg;
    usb_guard_stats_t   stats;
    usb_composite_guard_get(&cfg, &stats);
    JsonDocument doc;
    doc["mod_ms"]     = cfg.mod_ms;
    doc["key_ms"]     = cfg.key_ms;
    doc["voice_ms"]   = cfg.voice_ms;
    doc["voice_rx_gap_ms"] = HID_GUARD_VOICE_RX_GAP_MS;
    doc["forced"]     = stats.forced_releases;
    doc["last_reason"]= stats.last_reason;
    doc["last_force_ms"] = stats.last_force_ms;
    doc["any_held"]   = stats.any_held;
    doc["held_count"] = stats.held_count;
    doc["keyboard_pending"] = stats.keyboard_pending;
    doc["consumer_pending"] = stats.consumer_pending;
    doc["desired_modifier"] = stats.desired_modifier;
    doc["tx_complete"] = stats.tx_complete;
    doc["tx_failed"] = stats.tx_failed;
    doc["pending_ms"] = stats.pending_ms;
    doc["usb_recoveries"] = stats.usb_recoveries;
    doc["recovery_exhausted"] = stats.recovery_exhausted;
    hid_diagnostics_json(doc["hid_diagnostics"].to<JsonObject>());
#if defined(REMOTEMAPPER_EXPERIMENTAL_DWC2)
    usb_hid_stress_stats_t stress = {};
    usb_hid_stress_get(&stress);
    JsonObject test = doc["usb_stress"].to<JsonObject>();
    test["active"] = stress.active;
    test["user_aborted"] = stress.user_aborted;
    test["test_id"] = stress.test_id;
    test["duration_ms"] = stress.duration_ms;
    test["started_ms"] = stress.started_ms;
    test["ended_ms"] = stress.ended_ms;
    test["attempted"] = stress.attempted;
    test["completed"] = stress.completed;
    test["failed"] = stress.failed;
    test["stop_reason"] = stress.stop_reason;
#endif
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_guard_set() {
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"error\":\"missing_body\"}");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s_server.arg("plain"));
    if (err) {
        s_server.send(400, "application/json", "{\"error\":\"invalid_json\"}");
        return;
    }
    usb_guard_config_t cfg;
    usb_guard_stats_t stats;
    usb_composite_guard_get(&cfg, &stats);
    if (doc["mod_ms"].is<int>())   cfg.mod_ms   = doc["mod_ms"];
    if (doc["key_ms"].is<int>())   cfg.key_ms   = doc["key_ms"];
    if (doc["voice_ms"].is<int>()) cfg.voice_ms = doc["voice_ms"];
    if (!usb_composite_guard_set(&cfg)) {
        s_server.send(500, "application/json", "{\"error\":\"save_failed\"}");
        return;
    }
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handle_audio_resample() {
    uint16_t num = 0, den = 0;
    audio_pipeline_get_resample(&g_audio_pipeline, &num, &den);

    if (s_server.hasArg("plain") && s_server.method() == HTTP_POST) {
        JsonDocument doc;
        if (deserializeJson(doc, s_server.arg("plain"))) {
            s_server.send(400, "application/json", "{\"error\":\"invalid_json\"}");
            return;
        }
        const int n = doc["num"].is<int>() ? doc["num"].as<int>() : (int)num;
        const int d = doc["den"].is<int>() ? doc["den"].as<int>() : (int)den;
        if (n < 0 || n > 4096 || d < 0 || d > 4096) {
            s_server.send(400, "application/json", "{\"error\":\"out_of_range\"}");
            return;
        }
        if (n == 0 || d == 0) {
            // 0/0 means "no rate conversion". The pipeline has no separate disabled
            // state: audio_resample_up treats an unset interval as 1:1, so routing
            // it through the setter keeps the reported value and the actual ratio
            // in agreement. Previously this only rewrote the local reply, leaving
            // the pipeline on the old ratio while claiming enabled:false.
            if (!audio_pipeline_set_resample(&g_audio_pipeline, 1, 1)) {
                s_server.send(500, "application/json", "{\"error\":\"passthrough_failed\"}");
                return;
            }
            audio_pipeline_get_resample(&g_audio_pipeline, &num, &den);
        } else if (!audio_pipeline_set_resample(&g_audio_pipeline, (uint16_t)n, (uint16_t)d)) {
            char body[96];
            snprintf(body, sizeof(body),
                     "{\"error\":\"ratio_rejected\",\"allowed_pct\":[%d,%d]}",
                     AUDIO_RS_MIN_RATIO_PCT, AUDIO_RS_MAX_RATIO_PCT);
            s_server.send(400, "application/json", body);
            return;
        } else {
            audio_pipeline_get_resample(&g_audio_pipeline, &num, &den);
        }
    }

    char body[128];
    snprintf(body, sizeof(body),
             "{\"num\":%u,\"den\":%u,\"enabled\":%s,\"ratio_pct\":%d,"
             "\"outputs_per_frame_est\":%d,\"min_pct\":%d,\"max_pct\":%d}",
             (unsigned)num, (unsigned)den, (num && den) ? "true" : "false",
             (num && den) ? (int)((int)num * 100 / (int)den) : 100,
             (num && den) ? (int)(AUDIO_DEFAULT_FRAME_SAMPS * (int)num / (int)den) : AUDIO_DEFAULT_FRAME_SAMPS,
             AUDIO_RS_MIN_RATIO_PCT, AUDIO_RS_MAX_RATIO_PCT);
    s_server.send(200, "application/json", body);
}

static void handle_wifi_scan() {
    String json = wifi_manager_scan_json();
    s_server.send(200, "application/json", json);
}

static void handle_wifi_config() {
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"error\":\"missing_body\"}");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s_server.arg("plain"));
    if (err) {
        s_server.send(400, "application/json", "{\"error\":\"invalid_json\"}");
        return;
    }

    String ssid = doc["ssid"] | "";
    String pass = doc["pass"] | "";

    if (ssid.length() == 0) {
        s_server.send(400, "application/json", "{\"error\":\"empty_ssid\"}");
        return;
    }

    wifi_manager_save_sta_config(ssid, pass);
    s_server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handle_wifi_ap_config() {
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"error\":\"missing_body\"}");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s_server.arg("plain"));
    if (err) {
        s_server.send(400, "application/json", "{\"error\":\"invalid_json\"}");
        return;
    }

    String ap_pass = doc["ap_pass"] | "";
    ap_pass.trim();

    if (ap_pass.length() > 0 && ap_pass.length() < 8) {
        s_server.send(400, "application/json", "{\"error\":\"password_too_short\",\"message\":\"AP 密码至少需要 8 位字符，或留空设置为开放热点\"}");
        return;
    }

    if (wifi_manager_save_ap_config(ap_pass)) {
        s_server.send(200, "application/json", "{\"status\":\"ok\",\"secured\":" + String(ap_pass.length() >= 8 ? "true" : "false") + "}");
    } else {
        s_server.send(500, "application/json", "{\"error\":\"save_failed\"}");
    }
}

static void handle_keymap_get() {
    String json = key_config_to_json(&g_key_engine);
    s_server.send(200, "application/json", json);
}

static void handle_keymap_save() {
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"error\":\"missing_body\"}");
        return;
    }
    bool ok = key_config_from_json(&g_key_engine, s_server.arg("plain"));
    if (ok) {
        key_config_storage_save(&g_key_engine);
        // The keymap just changed under possibly-held remote buttons: drop every
        // held state so old bindings can never ghost into the new layout.
        usb_composite_force_release_all("keymap-reload");
        s_server.send(200, "application/json", "{\"status\":\"saved\"}");
    } else {
        s_server.send(400, "application/json", "{\"error\":\"invalid_keymap_format\"}");
    }
}

static void handle_keymap_reset() {
    key_config_storage_reset_defaults(&g_key_engine);
    usb_composite_force_release_all("keymap-reset");
    app_log("KEYMAP", "Reset keymap to factory defaults via Web API");
    s_server.send(200, "application/json", "{\"status\":\"reset_ok\"}");
}

static void handle_keymap_telemetry() {
    String json = key_telemetry_to_json(&g_key_engine);
    s_server.send(200, "application/json", json);
}

static void handle_ble_scan() {
    // Connected state keeps the scan radio OFF (power save): ask the BLE task
    // for a short burst so the next poll returns a fresh device list.
    wifi_manager_mark_activity();
    ble_remote_request_scan_burst();
    String json = ble_remote_scan_devices_json();
    s_server.send(200, "application/json", json);
}

static void handle_ble_connect() {
    if (!s_server.hasArg("plain")) {
        app_log("WEB", "BLE connect rejected: missing POST body");
        s_server.send(400, "application/json", "{\"error\":\"missing_body\"}");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s_server.arg("plain"));
    if (err) {
        app_log("WEB", "BLE connect rejected: invalid JSON payload");
        s_server.send(400, "application/json", "{\"error\":\"invalid_json\"}");
        return;
    }

    String mac = doc["mac"] | "";
    String name = doc["name"] | "";
    uint8_t type = doc["type"] | 0;

    if (mac.length() == 0) {
        app_log("WEB", "BLE connect rejected: empty MAC address");
        s_server.send(400, "application/json", "{\"error\":\"empty_mac\"}");
        return;
    }

    app_log("WEB", "Connecting to BLE target: %s (%s, Type: %d)", 
            name.length() > 0 ? name.c_str() : "Unknown", mac.c_str(), (int)type);

    bool ok = ble_remote_connect_target(mac, type, name);
    s_server.send(200, "application/json", ok ? "{\"status\":\"ok\"}" : "{\"status\":\"failed\"}");
}

static void handle_ble_unpair() {
    app_log("WEB", "BLE unpair requested via Web API");
    ble_remote_unpair();
    s_server.send(200, "application/json", "{\"status\":\"ok\"}");
}

static void handle_ble_info() {
    String json = ble_remote_get_connected_info();
    s_server.send(200, "application/json", json);
}

static void handle_ble_reconnect() {
    app_log("BLE", "Triggered manual reconnect scan via Web API");
    ble_remote_trigger_reconnect();
    s_server.send(200, "application/json", "{\"status\":\"reconnecting\"}");
}

static void handle_gatt_dump_get() {
    JsonDocument doc;
    doc["enabled"] = ble_remote_gatt_dump_enabled();
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_gatt_dump_set() {
    bool on = ble_remote_gatt_dump_enabled();
    if (s_server.hasArg("enabled")) {
        String v = s_server.arg("enabled");
        if (v == "1" || v.equalsIgnoreCase("true") || v.equalsIgnoreCase("on")) on = true;
        else if (v == "0" || v.equalsIgnoreCase("false") || v.equalsIgnoreCase("off")) on = false;
    } else {
        on = !on;
    }
    ble_remote_gatt_dump_request(on);
    app_log("BLE", "GATT explorer %s", on ? "ON (full GATT dump on each fresh remote connection)" : "OFF");
    JsonDocument doc;
    doc["enabled"] = on;
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_system_restart() {
    app_log("SYSTEM", "Rebooting ESP32 via Web API...");
    s_server.send(200, "application/json", "{\"status\":\"rebooting\"}");
    delay(500);
    ESP.restart();
}

static void handle_power_get() {
    JsonDocument doc;
    doc["policy"] = (int)wifi_manager_get_policy();
    doc["policy_str"] = wifi_manager_policy_str(wifi_manager_get_policy());
    doc["timeout_min"] = wifi_manager_get_timeout_min();
    doc["timeout_enabled"] = wifi_manager_get_timeout_enabled();
    doc["radio_state"] = (int)wifi_manager_get_radio_state();
    doc["radio_state_str"] = wifi_manager_state_str(wifi_manager_get_radio_state());
    doc["wifi_enabled"] = wifi_manager_get_enabled();
    doc["last_activity_sec"] = wifi_manager_get_last_activity_ms() / 1000;
    doc["idle_sec"] = millis() / 1000 - wifi_manager_get_last_activity_ms() / 1000;
    doc["sta_status"] = (int)WiFi.status();
    doc["sta_connected"] = (WiFi.status() == WL_CONNECTED);
    doc["sta_ip"] = wifi_manager_get_sta_ip();
    doc["ap_running"] = wifi_manager_is_ap_running();
    doc["uptime_sec"] = millis() / 1000;
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_power_set() {
    if (!s_server.hasArg("plain")) {
        s_server.send(400, "application/json", "{\"error\":\"missing_body\"}");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, s_server.arg("plain"));
    if (err) {
        s_server.send(400, "application/json", "{\"error\":\"invalid_json\"}");
        return;
    }

    bool policy_changed = false;
    if (!doc["policy"].isNull()) {
        int p = doc["policy"] | -1;
        if (p < (int)WIFI_POLICY_ALWAYS_ON || p > (int)WIFI_POLICY_DISABLED) {
            s_server.send(400, "application/json", "{\"error\":\"invalid_policy\",\"hint\":\"0=always_on,1=on_demand,2=disabled\"}");
            return;
        }
        wifi_policy_t old_p = wifi_manager_get_policy();
        if (wifi_manager_set_policy((wifi_policy_t)p)) {
            policy_changed = (wifi_manager_get_policy() != old_p);
        }
    }
    if (!doc["timeout_min"].isNull()) {
        uint32_t t = doc["timeout_min"] | 0xFFFFFFFF;
        if (!wifi_manager_set_timeout_min(t)) {
            s_server.send(400, "application/json", "{\"error\":\"invalid_timeout\",\"hint\":\"1,5,10,30,0(never)\"}");
            return;
        }
    }
    // Idle auto-shutdown switch: applies immediately, no reboot needed.
    if (!doc["timeout_enabled"].isNull()) {
        wifi_manager_set_timeout_enabled(doc["timeout_enabled"] | false);
    }

    JsonDocument res;
    res["status"] = "ok";
    res["policy"] = (int)wifi_manager_get_policy();
    res["policy_str"] = wifi_manager_policy_str(wifi_manager_get_policy());
    res["timeout_min"] = wifi_manager_get_timeout_min();
    res["timeout_enabled"] = wifi_manager_get_timeout_enabled();
    res["reboot_required"] = policy_changed;
    String out;
    serializeJson(res, out);
    s_server.send(200, "application/json", out);
}

static void handle_nvs_get() {
    String json = nvs_manager_dump_json();
    s_server.send(200, "application/json", json);
}

static void handle_nvs_save() {
    s_server.send(403, "application/json", "{\"error\":\"disabled\",\"message\":\"全量 NVS 直接回写功能已被禁用，按键配置请使用 /api/keymap/save 导入。\"}");
}


static void handle_nvs_reset() {
    bool ok = nvs_manager_erase_all();
    if (ok) {
        s_server.send(200, "application/json", "{\"status\":\"erased\",\"message\":\"NVS已清空，系统即将重启...\"}");
        delay(500);
        ESP.restart();
    } else {
        s_server.send(500, "application/json", "{\"error\":\"erase_failed\"}");
    }
}

static void handle_wol_test() {
    String mac = "";
    uint16_t port = 9;

    if (s_server.hasArg("plain")) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, s_server.arg("plain"));
        if (!err) {
            if (doc["mac"].is<const char*>()) mac = doc["mac"].as<String>();
            if (doc["port"].is<int>()) port = (uint16_t)doc["port"].as<int>();
        }
    }
    if (mac.length() == 0 && s_server.hasArg("mac")) {
        mac = s_server.arg("mac");
    }
    if (s_server.hasArg("port")) {
        port = (uint16_t)s_server.arg("port").toInt();
    }
    if (port == 0) port = 9;

    mac.trim();
    if (mac.length() == 0) {
        s_server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"缺少 MAC 地址\"}");
        return;
    }

    bool ok = wol_manager_send_str(mac.c_str(), port);
    if (ok) {
        s_server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Wake-on-LAN 唤醒魔术包已广播发送\"}");
    } else {
        s_server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"发送失败，请检查 MAC 地址格式是否正确 (如 AA:BB:CC:DD:EE:FF)\"}");
    }
}

static void handle_ota_status() {
    const ota_status_t* st = ota_manager_get_status();
    JsonDocument doc;
    doc["supported"] = st->enabled;
    doc["in_progress"] = st->in_progress;
    doc["last_success"] = st->last_success;
    doc["last_error"] = ota_manager_error_str();
    doc["version"] = FIRMWARE_VERSION;
    doc["running_label"] = st->running_label ? st->running_label : "";
    doc["running_addr"] = st->running_addr;
    doc["target_label"] = st->target_label ? st->target_label : "";
    doc["target_addr"] = st->target_addr;
    doc["target_capacity"] = st->target_capacity;
    doc["received"] = st->received;
    doc["total"] = st->total;
    doc["rollback_armed"] = st->rollback_armed;
    doc["boot_fail_count"] = st->boot_fail_count;
    // What the IDF bootloader thinks of the currently running image
    // (pending_verify / valid / invalid). Diagnostic for OTA slot switching.
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (run != NULL) {
        esp_ota_img_states_t st2;
        if (esp_ota_get_state_partition(run, &st2) == ESP_OK) {
            switch (st2) {
                case ESP_OTA_IMG_ABORTED:      doc["image_state"] = "aborted"; break;
                case ESP_OTA_IMG_UNDEFINED:     doc["image_state"] = "undefined"; break;
                case ESP_OTA_IMG_INVALID:       doc["image_state"] = "invalid"; break;
                case ESP_OTA_IMG_VALID:         doc["image_state"] = "valid"; break;
                case ESP_OTA_IMG_PENDING_VERIFY: doc["image_state"] = "pending_verify"; break;
                default:                        doc["image_state"] = "unknown"; break;
            }
        }
    }
    // Run the same image verification the bootloader performs against the
    // target OTA slot. If the slot holds a valid image this must be ESP_OK,
    // otherwise it reveals the exact reason the bootloader refuses to boot it.
    {
        const esp_partition_t* tgt = esp_ota_get_next_update_partition(NULL);
        if (tgt != NULL) {
            esp_partition_pos_t pos = {};
            pos.offset = tgt->address;
            pos.size = tgt->size;
            esp_image_metadata_t meta;
            memset(&meta, 0, sizeof(meta));
            esp_err_t ev = esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta);
            doc["target_verify"] = esp_err_to_name(ev);
            doc["target_segments"] = (ev == ESP_OK) ? (int)meta.image.segment_count : -1;
            doc["target_entry"] = (ev == ESP_OK) ? meta.image.entry_addr : 0;
        }
    }
    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

static void handle_ota_upload() {
    if (!s_server.hasUpload()) {
        return;
    }
    HTTPUpload& upload = s_server.upload();
    switch (upload.status) {
        case UPLOAD_FILE_START:
            app_log("OTA", "Upload start: %s (%u bytes)", upload.filename.c_str(), upload.totalSize);
            if (!ota_manager_begin(upload.totalSize)) {
                app_log("OTA", "Upload rejected: %s", ota_manager_error_str());
            }
            break;
        case UPLOAD_FILE_WRITE:
            if (ota_manager_write(upload.buf, upload.currentSize) != upload.currentSize) {
                app_log("OTA", "Write error at +%u bytes: %s",
                        (unsigned int)ota_manager_get_status()->received,
                        ota_manager_error_str());
            }
            break;
        case UPLOAD_FILE_END:
            app_log("OTA", "Upload complete, verifying image...");
            ota_manager_end();
            break;
        case UPLOAD_FILE_ABORTED:
            app_log("OTA", "Upload aborted by client");
            ota_manager_abort();
            break;
        default:
            break;
    }
}

static void handle_ota_done() {
    const ota_status_t* st = ota_manager_get_status();
    if (st->last_success) {
        s_server.sendHeader("Connection", "close");
        s_server.send(200, "application/json", "{\"success\":true,\"message\":\"固件升级成功，设备重启中，请稍候约 20~40 秒...\"}");
        delay(300);
        ESP.restart();
    } else {
        s_server.sendHeader("Connection", "close");
        s_server.send(500, "application/json",
                      String("{\"success\":false,\"message\":\"固件升级失败: ") + ota_manager_error_str() + "\"}");
    }
}

static void handle_config_export() {
    bool full = s_server.hasArg("mode") && s_server.arg("mode") == "full";
    s_server.sendHeader("Content-Type", "application/json");
    s_server.sendHeader("Content-Disposition",
                        String("attachment; filename=RemoteMapper_Config_") +
                        (full ? "full_" : "safe_") + FIRMWARE_VERSION + ".json");
    s_server.send(200, "application/json", config_backup_export(full));
}

static void handle_config_import() {
    String body = s_server.arg("plain");
    if (body.length() == 0) {
        if (s_server.hasArg("body")) body = s_server.arg("body");
    }
    if (body.length() == 0) {
        s_server.send(400, "application/json",
                      "{\"status\":\"error\",\"message\":\"缺少备份内容\"}");
        return;
    }
    String err = config_backup_import(body);
    if (err.length() == 0) {
        s_server.sendHeader("Connection", "close");
        s_server.send(200, "application/json",
                      "{\"status\":\"ok\",\"message\":\"配置已恢复，正在重启设备...\",\"reboot_required\":true}");
        delay(300);
        ESP.restart();
    } else {
        s_server.send(400, "application/json",
                      String("{\"status\":\"error\",\"message\":\"恢复失败: ") +
                      err + "\"}");
    }
}

static void handle_captive_portal() {
    String host = s_server.hostHeader();
    if (host != "192.168.4.1" && host != "remotemapper.local") {
        s_server.sendHeader("Location", "http://192.168.4.1/", true);
        s_server.send(302, "text/plain", "");
        return;
    }
    handle_root();
}

void web_server_init(void) {
    // API Routes
    s_server.on("/", HTTP_GET, handle_root);
    s_server.on("/api/status", HTTP_GET, handle_status);
    s_server.on("/api/logs", HTTP_GET, handle_logs);
    s_server.on("/api/logs/clear", HTTP_POST, handle_logs_clear);
    s_server.on("/api/wifi/scan", HTTP_GET, handle_wifi_scan);
    s_server.on("/api/wifi/config", HTTP_POST, handle_wifi_config);
    s_server.on("/api/wifi/ap", HTTP_POST, handle_wifi_ap_config);
    s_server.on("/api/keymap", HTTP_GET, handle_keymap_get);
    s_server.on("/api/keymap/save", HTTP_POST, handle_keymap_save);
    s_server.on("/api/keymap/reset", HTTP_POST, handle_keymap_reset);
    s_server.on("/api/keymap/telemetry", HTTP_GET, handle_keymap_telemetry);
    s_server.on("/api/wol/test", HTTP_POST, handle_wol_test);
    s_server.on("/api/ble/scan", HTTP_GET, handle_ble_scan);
    s_server.on("/api/ble/connect", HTTP_POST, handle_ble_connect);
    s_server.on("/api/ble/unpair", HTTP_POST, handle_ble_unpair);
    s_server.on("/api/ble/info", HTTP_GET, handle_ble_info);
    s_server.on("/api/ble/reconnect", HTTP_POST, handle_ble_reconnect);
    s_server.on("/api/nvs", HTTP_GET, handle_nvs_get);
    s_server.on("/api/nvs/save", HTTP_POST, handle_nvs_save);
    s_server.on("/api/nvs/reset", HTTP_POST, handle_nvs_reset);
    s_server.on("/api/power", HTTP_GET, handle_power_get);
    s_server.on("/api/power", HTTP_POST, handle_power_set);
    s_server.on("/api/system/restart", HTTP_POST, handle_system_restart);
    s_server.on("/api/config/export", HTTP_GET, handle_config_export);
    s_server.on("/api/config/import", HTTP_POST, handle_config_import);
    s_server.on("/api/audio", HTTP_GET, handle_audio);
    s_server.on("/api/audio/resample", HTTP_GET,  handle_audio_resample);
    s_server.on("/api/audio/resample", HTTP_POST, handle_audio_resample);
    s_server.on("/api/cores", HTTP_GET, handle_cores);
    s_server.on("/api/guard", HTTP_GET, handle_guard);
    s_server.on("/api/guard", HTTP_POST, handle_guard_set);
    s_server.on("/api/debug/gatt-dump", HTTP_GET, handle_gatt_dump_get);
    s_server.on("/api/debug/gatt-dump", HTTP_POST, handle_gatt_dump_set);

#if REMOTEMAPPER_OTA
    s_server.on("/api/ota/status", HTTP_GET, handle_ota_status);
    s_server.on("/api/ota/upload", HTTP_POST, handle_ota_done, handle_ota_upload);
#else
    s_server.on("/api/ota/status", HTTP_GET, []() {
        s_server.send(200, "application/json", "{\"supported\":false,\"message\":\"当前型号固件未启用在线升级\"}");
    });
    s_server.on("/api/ota/upload", HTTP_POST, []() {
        s_server.send(501, "application/json", "{\"supported\":false,\"message\":\"当前型号固件未启用在线升级\"}");
    });
#endif

    // Captive Portal probe redirects
    s_server.on("/generate_204", HTTP_GET, handle_captive_portal);
    s_server.on("/hotspot-detect.html", HTTP_GET, handle_captive_portal);
    s_server.on("/canonical.html", HTTP_GET, handle_captive_portal);
    s_server.on("/connecttest.txt", HTTP_GET, handle_captive_portal);
    s_server.onNotFound(handle_captive_portal);

    s_server.begin();
    app_log("WEB", "HTTP Web Server started on port 80");
}

void web_server_task(void) {
    s_server.handleClient();
}
