#include "cli_manager.h"
#include "version.h"
#include "wifi/wifi_manager.h"
#include "log/app_log.h"
#include "ble/ble_remote_client.h"
#include "audio/audio_pipeline.h"
#include "keymap/key_state_machine.h"
#include "usb/usb_composite.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <USBCDC.h>

#if !ARDUINO_USB_CDC_ON_BOOT
extern USBCDC USBSerial;
#endif

extern key_mapper_engine_t g_key_engine;

static String s_uart_input_buffer = "";
static String s_cdc_input_buffer = "";
static app_log_output_t s_response_output = APP_LOG_OUTPUT_UART;

static void handle_command(const String& line);

static void cli_write_line(const String& line) {
    String response = line;
    response += "\r\n";
    app_log_queue_cli_text(s_response_output, response.c_str(), response.length());
}

static void cli_feed_char(char c, app_log_output_t source) {
    String& input_buffer = source == APP_LOG_OUTPUT_CDC
            ? s_cdc_input_buffer : s_uart_input_buffer;
    if (c == '\r' || c == '\n') {
        if (input_buffer.length() > 0) {
            s_response_output = source;
            handle_command(input_buffer);
            input_buffer = "";
        }
    } else {
        input_buffer += c;
        if (input_buffer.length() > 250) {
            input_buffer = "";
        }
    }
}

static void handle_wifi_command(const String& arg) {
    String a = arg;
    a.trim();
    String low = a;
    low.toLowerCase();

    if (low.startsWith("policy")) {
        String p = a.substring(6);
        p.trim();
        if (p.length() == 0) {
            JsonDocument doc;
            doc["policy"] = (int)wifi_manager_get_policy();
            doc["policy_str"] = wifi_manager_policy_str(wifi_manager_get_policy());
            doc["timeout_min"] = wifi_manager_get_timeout_min();
            doc["timeout_enabled"] = wifi_manager_get_timeout_enabled();
            doc["radio_state"] = (int)wifi_manager_get_radio_state();
            doc["radio_state_str"] = wifi_manager_state_str(wifi_manager_get_radio_state());
            String out;
            serializeJson(doc, out);
            cli_write_line(out);
            return;
        }
        wifi_policy_t pol;
        if (p.equalsIgnoreCase("always_on"))      pol = WIFI_POLICY_ALWAYS_ON;
        else if (p.equalsIgnoreCase("on_demand")) pol = WIFI_POLICY_ON_DEMAND;
        else if (p.equalsIgnoreCase("disabled"))  pol = WIFI_POLICY_DISABLED;
        else {
            cli_write_line("{\"error\":\"invalid_policy\",\"hint\":\"always_on|on_demand|disabled\"}");
            return;
        }

        bool changed = (wifi_manager_get_policy() != pol);
        wifi_manager_set_policy(pol);
        JsonDocument doc;
        doc["status"] = "ok";
        doc["policy_str"] = wifi_manager_policy_str(wifi_manager_get_policy());
        doc["rebooting"] = true;
        String out;
        serializeJson(doc, out);
        cli_write_line(out);
        if (changed) {
            cli_write_line(pol == WIFI_POLICY_DISABLED
                ? "Wi-Fi 将在重启后彻底关闭（USB CDC/UART 可用 'wifi on' 或 'wifi policy on_demand' 恢复）"
                : "Wi-Fi 策略将在重启后生效...");
        }
        return;
    }

    if (low.startsWith("timeout")) {
        String t = a.substring(7);
        t.trim();
        if (t.length() == 0) {
            uint32_t tm = wifi_manager_get_timeout_min();
            bool en = wifi_manager_get_timeout_enabled();
            String out = "{\"timeout_min\":" + String(tm) + ",\"never\":" +
                         String(tm == 0 ? "true" : "false") +
                         ",\"enabled\":" + String(en ? "true" : "false") + "}";
            cli_write_line(out);
            if (!en) {
                cli_write_line("Wi-Fi 空闲自动关闭当前为关闭状态（超时已保存但不生效；"
                               "'wifi timeout on' 开启，'wifi timeout off' 关闭）");
            }
            return;
        }
        if (t.equalsIgnoreCase("on") || t.equalsIgnoreCase("enable") ||
            t.equalsIgnoreCase("enabled") || t.equalsIgnoreCase("off") ||
            t.equalsIgnoreCase("disable") || t.equalsIgnoreCase("disabled")) {
            bool en = t.equalsIgnoreCase("on") || t.equalsIgnoreCase("enable") ||
                      t.equalsIgnoreCase("enabled");
            wifi_manager_set_timeout_enabled(en);
            cli_write_line("{\"status\":\"ok\",\"enabled\":" +
                           String(wifi_manager_get_timeout_enabled() ? "true" : "false") +
                           ",\"timeout_min\":" + String(wifi_manager_get_timeout_min()) + "}");
            cli_write_line(wifi_manager_get_timeout_enabled()
                ? "Wi-Fi 空闲自动关闭已开启（" + String(wifi_manager_get_timeout_min()) +
                  " 分钟无需操作）"
                : "Wi-Fi 空闲自动关闭已关闭（不再因空闲自动断网）");
            return;
        }
        uint32_t minutes = 0;
        bool digits_only = true;
        for (size_t i = 0; i < t.length(); i++) {
            if (!isdigit((unsigned char)t[i])) { digits_only = false; break; }
        }
        if (t.equalsIgnoreCase("never")) {
            minutes = 0;
        } else if (digits_only) {
            minutes = t.toInt();
        } else {
            cli_write_line("{\"error\":\"invalid_timeout\",\"hint\":\"on|off|1|5|10|30|never\"}");
            return;
        }
        if (wifi_manager_set_timeout_min(minutes)) {
            cli_write_line("{\"status\":\"ok\",\"timeout_min\":" + String(wifi_manager_get_timeout_min()) + "}");
            cli_write_line(minutes == 0
                ? "Wi-Fi 空闲自动关闭超时已设为 永不过期"
                : "Wi-Fi 空闲自动关闭超时已设为 " + String(minutes) + " 分钟");
            if (!wifi_manager_get_timeout_enabled()) {
                cli_write_line("注意：空闲自动关闭开关为关闭状态，该超时暂不生效（'wifi timeout on' 开启）");
            }
        } else {
            cli_write_line("{\"error\":\"invalid_timeout\",\"hint\":\"on|off|1|5|10|30|never\"}");
        }
        return;
    }

    bool want_on = arg.equalsIgnoreCase("on");
    bool want_off = arg.equalsIgnoreCase("off");
    if (arg.equalsIgnoreCase("wake")) {
        bool woke = wifi_manager_request_wifi(WIFI_WAKE_MANUAL);
        cli_write_line(woke
            ? "{\"status\":\"ok\",\"wake\":true,\"radio_state\":" + String((int)wifi_manager_get_radio_state()) + "}"
            : "{\"status\":\"ignored\",\"wake\":false,\"reason\":\"policy_disabled_or_already_on\"}");
        return;
    }
    if (want_on || want_off) {
        if (wifi_manager_get_enabled() == want_on) {
            cli_write_line(want_on
                ? "{\"status\":\"nochange\",\"wifi_enabled\":true}"
                : "{\"status\":\"nochange\",\"wifi_enabled\":false}");
            return;
        }
        wifi_manager_set_enabled(want_on);
        cli_write_line(want_on
            ? "{\"status\":\"ok\",\"wifi_enabled\":true,\"rebooting\":true}"
            : "{\"status\":\"ok\",\"wifi_enabled\":false,\"rebooting\":true}");
        cli_write_line(want_on
            ? "Wi-Fi 将在重启后开启（AP 热点 RemoteMapper-AP 恢复）..."
            : "Wi-Fi 将在重启后关闭（后台改用 USB CDC/UART 的 'wifi on' 恢复）...");
        delay(300);
        ESP.restart();
    } else {
        JsonDocument doc;
        doc["wifi_enabled"] = wifi_manager_get_enabled();
        doc["wifi_policy"] = (int)wifi_manager_get_policy();
        doc["wifi_policy_str"] = wifi_manager_policy_str(wifi_manager_get_policy());
        doc["wifi_timeout_min"] = wifi_manager_get_timeout_min();
        doc["wifi_timeout_enabled"] = wifi_manager_get_timeout_enabled();
        doc["wifi_radio_state"] = (int)wifi_manager_get_radio_state();
        doc["ap_running"] = wifi_manager_is_ap_running();
        doc["ap_ip"] = wifi_manager_get_ap_ip();
        doc["sta_connected"] = wifi_manager_is_sta_connected();
        doc["sta_ip"] = wifi_manager_get_sta_ip();
        String out;
        serializeJson(doc, out);
        cli_write_line(out);
    }
}

static void handle_log_command(const String& arg) {
    if (arg.equalsIgnoreCase("on")) {
        app_log_set_cdc_enabled(true);
        cli_write_line("{\"status\":\"ok\",\"cdc_log_enabled\":true}");
        cli_write_line("CDC 日志镜像已开启（注意：语音会话期间日志较密集）");
    } else if (arg.equalsIgnoreCase("off")) {
        app_log_set_cdc_enabled(false);
        cli_write_line("{\"status\":\"ok\",\"cdc_log_enabled\":false}");
        cli_write_line("CDC 日志镜像已关闭（UART 日志不受影响）");
    } else if (arg.equalsIgnoreCase("console on")) {
        app_log_set_console_enabled(true);
        cli_write_line("{\"status\":\"ok\",\"console_log_enabled\":true}");
        cli_write_line("UART 控制台日志已开启");
    } else if (arg.equalsIgnoreCase("console off")) {
        app_log_set_console_enabled(false);
        cli_write_line("{\"status\":\"ok\",\"console_log_enabled\":false}");
        cli_write_line("UART 控制台日志已关闭（仅影响 UART 输出，网页日志保留）");
    } else {
        JsonDocument doc;
        doc["cdc_log_enabled"] = app_log_get_cdc_enabled();
        doc["console_log_enabled"] = app_log_get_console_enabled();
        doc["uart_cli_dropped"] = app_log_get_cli_dropped(APP_LOG_OUTPUT_UART);
        doc["cdc_cli_dropped"] = app_log_get_cli_dropped(APP_LOG_OUTPUT_CDC);
        doc["log_mirror_dropped"] = app_log_get_mirror_dropped();
        String out;
        serializeJson(doc, out);
        cli_write_line(out);
    }
}

#if defined(REMOTEMAPPER_DWC2_DRIVER)
static void add_usb_stress_json(JsonObject out) {
    usb_hid_stress_stats_t stats = {};
    usb_hid_stress_get(&stats);
    out["active"] = stats.active;
    out["user_aborted"] = stats.user_aborted;
    out["test_id"] = stats.test_id;
    out["duration_ms"] = stats.duration_ms;
    out["started_ms"] = stats.started_ms;
    out["ended_ms"] = stats.ended_ms;
    out["attempted"] = stats.attempted;
    out["completed"] = stats.completed;
    out["failed"] = stats.failed;
    out["stop_reason"] = stats.stop_reason;
}

static void handle_usb_stress_command(const String& arg) {
    String command = arg;
    command.trim();
    if (command != "stress" && !command.startsWith("stress ")) {
        cli_write_line("{\"error\":\"unknown_usb_command\",\"hint\":\"usb stress <1..300>|stop|status\"}");
        return;
    }
    String value = command.substring(6);
    value.trim();
    if (value.equalsIgnoreCase("stop")) {
        usb_hid_stress_stop();
    } else if (value.length() && !value.equalsIgnoreCase("status")) {
        bool valid = value.length() <= 3;
        for (size_t i = 0; i < value.length(); i++)
            if (!isdigit((unsigned char)value[i])) valid = false;
        const uint32_t seconds = valid ? value.toInt() : 0;
        if (seconds < 1 || seconds > 300) {
            cli_write_line("{\"error\":\"invalid_duration\",\"hint\":\"usb stress <1..300>|stop|status\"}");
            return;
        }
        if (!usb_hid_stress_start(seconds)) {
            cli_write_line("{\"error\":\"stress_unavailable\",\"hint\":\"USB must be mounted and idle; only one test may run\"}");
            return;
        }
    }
    JsonDocument doc;
    add_usb_stress_json(doc["usb_stress"].to<JsonObject>());
    String out;
    serializeJson(doc, out);
    cli_write_line(out);
}
#endif

static void handle_command(const String& line) {
    String cmd = line;
    cmd.trim();
    if (cmd.length() == 0) return;

    // Split into "head [tail]" so wifi on/off/status can share one dispatcher
    int sp = cmd.indexOf(' ');
    String head = (sp < 0) ? cmd : cmd.substring(0, sp);
    String tail = (sp < 0) ? String("") : cmd.substring(sp + 1);
    tail.trim();

    if (head.equalsIgnoreCase("wifi")) {
        handle_wifi_command(tail);
        return;
    }

    if (head.equalsIgnoreCase("log")) {
        handle_log_command(tail);
        return;
    }

#if defined(REMOTEMAPPER_DWC2_DRIVER)
    if (head.equalsIgnoreCase("usb")) {
        handle_usb_stress_command(tail);
        return;
    }
#endif

    if (cmd.equalsIgnoreCase("status") || cmd.equalsIgnoreCase("info")) {
        JsonDocument doc;
        doc["firmware"] = FIRMWARE_NAME;
        doc["version"] = FIRMWARE_VERSION;
        doc["target"] = HARDWARE_TARGET;
        doc["uptime_sec"] = millis() / 1000;
        doc["ble_state"] = (int)ble_remote_get_state();
        doc["frames_decoded"] = g_audio_pipeline.total_frames_decoded;
        doc["samples_pushed"] = g_audio_pipeline.total_samples_pushed;
        doc["free_heap"] = ESP.getFreeHeap();
        doc["free_psram"] = ESP.getFreePsram();
        doc["wifi_enabled"] = wifi_manager_get_enabled();
#if defined(REMOTEMAPPER_DWC2_DRIVER)
        add_usb_stress_json(doc["usb_stress"].to<JsonObject>());
#endif

        String out;
        serializeJson(doc, out);
        cli_write_line(out);
    }
    else if (cmd.equalsIgnoreCase("reconnect")) {
        cli_write_line("{\"status\":\"reconnecting\"}");
        ble_remote_trigger_reconnect();
    }
    else if (cmd.equalsIgnoreCase("reset_keys")) {
        key_engine_load_defaults(&g_key_engine);
        cli_write_line("{\"status\":\"keymap_reset_to_defaults\"}");
    }
    else if (head.equalsIgnoreCase("gattdump")) {
        tail.toLowerCase();
        if (tail == "on" || tail == "1" || tail == "true") {
            ble_remote_gatt_dump_request(true);
            cli_write_line("{\"status\":\"ok\",\"gatt_dump\":true,\"hint\":\"next remote connection triggers a full GATT enumeration\"}");
        } else if (tail == "off" || tail == "0" || tail == "false") {
            ble_remote_gatt_dump_request(false);
            cli_write_line("{\"status\":\"ok\",\"gatt_dump\":false}");
        } else {
            cli_write_line("{\"status\":\"ok\",\"gatt_dump\":" + String(ble_remote_gatt_dump_enabled() ? "true" : "false") + "}");
        }
    }
    else if (cmd.equalsIgnoreCase("help")) {
        cli_write_line("Commands:");
        cli_write_line("  status        - Display system info & runtime statistics (JSON)");
        cli_write_line("  reconnect     - Trigger BLE remote re-scan");
        cli_write_line("  reset_keys    - Reset key bindings to factory defaults");
        cli_write_line("  gattdump on|off - Toggle full GATT enumeration per remote connection (GATTX log)");
        cli_write_line("  wifi on|off   - Enable/disable the whole Wi-Fi radio (persisted, reboots)");
        cli_write_line("  wifi policy [always_on|on_demand|disabled] - Get/set Wi-Fi power policy");
        cli_write_line("  wifi timeout [on|off] - Enable/disable ON_DEMAND idle auto-shutdown (default off)");
    cli_write_line("  wifi timeout [1|5|10|30|never] - Get/set ON_DEMAND idle timeout (min)");
        cli_write_line("  wifi status   - Show Wi-Fi radio & connection status (JSON)");
        cli_write_line("  wifi wake     - Wake the radio from ON_DEMAND sleep (same path as USB/key gesture)");
        cli_write_line("  log on|off    - Mirror full logs to USB CDC (default: off)");
        cli_write_line("  log console on|off - Mute/enable routine UART console logs (default: muted after boot)");
        cli_write_line("  log status    - Show log mirror/console state (JSON)");
#if defined(REMOTEMAPPER_DWC2_DRIVER)
        cli_write_line("  usb stress <1..300>|stop|status - Bounded idle-only zero-report HID test");
#endif
        cli_write_line("  help          - Show available commands");
    }
    else {
        cli_write_line("{\"error\":\"unknown_command\",\"hint\":\"type help\"}");
    }
}

extern "C" {

void cli_manager_init(void) {
    s_uart_input_buffer.reserve(256);
    s_cdc_input_buffer.reserve(256);
}

void cli_manager_task(void) {
    // A stream of console input must not monopolize USB recovery maintenance.
    // Preserve the partial command and finish it over subsequent loop turns.
    static const unsigned RX_BUDGET = 64;
    unsigned uart_read = 0;
    while (uart_read++ < RX_BUDGET && Serial.available() > 0) {
        char c = (char)Serial.read();
        cli_feed_char(c, APP_LOG_OUTPUT_UART);
    }
#if !ARDUINO_USB_CDC_ON_BOOT
    // Received bytes are valid even when the host hasn't asserted both
    // control lines required by USBCDC::operator bool(). Always drain RX;
    // otherwise commands accumulate until a later terminal handshake.
    unsigned cdc_read = 0;
    while (cdc_read++ < RX_BUDGET && USBSerial.available() > 0) {
        char c = (char)USBSerial.read();
        cli_feed_char(c, APP_LOG_OUTPUT_CDC);
    }
#endif
}

} // extern "C"
