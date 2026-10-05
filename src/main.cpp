#include <Arduino.h>
#include "app_config.h"
#include "version.h"
#include "log/app_log.h"
#include "wifi/wifi_manager.h"
#include "web/web_server.h"
#include "audio/audio_pipeline.h"
#include "keymap/key_state_machine.h"
#include "keymap/key_config_storage.h"
#include "core_diagnostics.h"
#include "runtime_diagnostics.h"
#include "usb/usb_composite.h"
#include "usb/uac_microphone.h"
#include "ble/ble_remote_client.h"
#include "cli/cli_manager.h"
#include "led_indicator.h"
#include "config/config_manager.h"
#include "ota/ota_manager.h"

#include <nvs_flash.h>
#include <nvs.h>

key_mapper_engine_t g_key_engine;
static bool s_critical_startup_ok = false;

// Core placement contract. The DWC2 driver protects DIEPEMPMSK with a shared
// spinlock because HID submissions and the USB ISR run on different cores; if
// both ends ever land on the same core that protection stops being a
// cross-core guarantee, so fail the build instead of losing it silently.
static_assert(TASK_CORE_BLE != TASK_CORE_USB, "BLE and USB tasks must stay on separate cores");
static_assert(TASK_CORE_LED == TASK_CORE_USB, "LED task follows the USB core");
static_assert(PRIO_TASK_USB > PRIO_TASK_BLE, "USB outranks BLE");
static_assert(PRIO_TASK_BLE > PRIO_TASK_HID_STRESS, "stress sender must not outrank BLE");

// loopTask carries USB recovery, Wi-Fi, the web server and the CLI. Its core comes
// from the framework's generated sdkconfig (CONFIG_ARDUINO_RUNNING_CORE) rather
// than this project, so check it here: a framework-side default flip would
// otherwise move all of that traffic onto the BLE core unnoticed.
static_assert(ARDUINO_RUNNING_CORE == TASK_CORE_USB, "Arduino loopTask must run on the USB core");

// Task running on Core 0: BLE Central & Audio Decoding
static void ble_task_core0(void* param) {
    app_log("SYSTEM", "BLE & Audio Task started on Core %d", xPortGetCoreID());
    ble_remote_init();

    while (true) {
        ble_remote_task();
        key_engine_tick(&g_key_engine, millis());
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void setup() {
    // 0. Lower CPU clock to cut dynamic power/heat. Must run before peripheral
    //    init; the S3 keeps USB and the radios valid at 160 MHz.
    setCpuFrequencyMhz(160);

    // 0.1 Initialize Flash NVS with auto-recovery for corrupted partitions
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 1. Initialize Log System first
    app_log_init();
    core_diagnostics_register("loopTask", xTaskGetCurrentTaskHandle(), TASK_CORE_USB);
    
    // 1.5. Initialize LED Indicator
    if (!led_indicator_init()) {
        app_log("INIT", "LED indicator task unavailable");
    }

    // 2. Initialize USB Composite Stack (UAC Mic + HID Keyboard + Consumer + CDC)
    usb_composite_init();
    Serial.begin(115200);
    delay(200);

    app_log("SYSTEM", "==================================================");
    app_log("SYSTEM", " %s v%s (%s)", FIRMWARE_NAME, FIRMWARE_VERSION, HARDWARE_TARGET);
    app_log("SYSTEM", " Xiaomi Remote Hardware Bridge (BLE -> USB + Web)");
    app_log("SYSTEM", "==================================================");
    app_log("SYSTEM", "CPU frequency: %u MHz", (unsigned)(getCpuFrequencyMhz()));

    // UAC initialization owns the shared audio pipeline and initializes it
    // once before creating the USB TX worker.

    // 4. Initialize Key Engine with USB HID dispatcher callback and restore NVS mappings
    key_engine_init(&g_key_engine, usb_hid_dispatch_action);
    key_config_storage_init(&g_key_engine);
    app_log("INIT", "Key Engine active with %u layers (Layer 0 has %u mappings)", 
            (unsigned int)g_key_engine.layer_count, (unsigned int)g_key_engine.layers[0].binding_count);

    // 5. Initialize Serial / CDC CLI Manager
    cli_manager_init();

    // 5.5. Config schema versioning & migration (must run before Wi-Fi reads policy)
    config_manager_init();

    // 5.6. OTA manager (partition info & online firmware update support)
    ota_manager_init();

    // 6. Initialize Wi-Fi AP + STA & Captive Portal
    wifi_manager_init();

    // 7. Initialize Embedded Web Server & REST APIs (Wi-Fi only).
    //    lwIP/esp_netif are initialized inside the Wi-Fi library; starting a TCP
    //    server while Wi-Fi is disabled triggers a lwIP assert and reboots the chip.
    if (wifi_manager_get_enabled()) {
        web_server_init();
    } else {
        app_log("SYSTEM", "Wi-Fi disabled: web server not started");
    }

    // 8. Launch BLE Central Task pinned to Core 0
    TaskHandle_t ble_task = nullptr;
    const BaseType_t ble_task_created = xTaskCreatePinnedToCore(
        ble_task_core0,
        "ble_audio_task",
        8192,
        NULL,
        PRIO_TASK_BLE,
        &ble_task,
        TASK_CORE_BLE
    );
    if (ble_task_created == pdPASS) {
        core_diagnostics_register("ble_audio_task", ble_task, TASK_CORE_BLE);
    } else {
        app_log("INIT", "Failed to spawn BLE task");
    }

    s_critical_startup_ok = ble_task_created == pdPASS && usb_composite_is_initialized() &&
            uac_microphone_is_ready();
    runtime_diagnostics_set_startup_health(s_critical_startup_ok);
    if (!s_critical_startup_ok) {
        app_log("INIT", "Critical startup incomplete; OTA safe-boot confirmation withheld");
    }

    if (wifi_manager_get_enabled()) {
        app_log("SYSTEM", "System initialization complete. Web available at http://192.168.4.1 or http://remotemapper.local");
    } else {
        app_log("SYSTEM", "System initialization complete. Wi-Fi radio off (use CDC/UART: 'wifi on')");
    }

    // Mute routine UART console logs now that bring-up is done: saves power and
    // stops the USB-UART bridge activity LED from blinking. The web /api/logs
    // ring buffer keeps recording; re-enable with the CLI command
    // 'log console on' (or 'log on' to mirror to USB CDC).
    app_log("SYSTEM", "Console UART logs muted (web /api/logs + 'log console on' still work)");
    app_log_set_console_enabled(false);
}

void loop() {
    const uint32_t loop_started_us = runtime_diagnostics_loop_begin();

    // 1. Service application-level USB recovery and audio state. TinyUSB
    //    transfer callbacks run from the framework's usbd task.
    usb_composite_task();

    // 2. Service Wi-Fi & DNS tasks
    wifi_manager_task();

    // 3. Service HTTP Web Server (only when Wi-Fi is enabled)
    if (wifi_manager_get_enabled()) {
        web_server_task();
    }

    // 4. Service Serial / WebSerial CLI commands
    cli_manager_task();

    // 5. Confirm safe-boot watchdog once the new firmware has run stably
    if (s_critical_startup_ok && usb_composite_is_initialized() && uac_microphone_is_ready()) {
        ota_manager_watchdog_confirm();
    }

    // 6. Flush audio RX accounting from the loop task. This must never run in
    // the NimBLE host task, which is the single dispatcher for every BLE
    // notification (key reports included) and has a shallow stack.
    ble_audio_rx_diagnostics_tick();
    core_diagnostics_discover_framework_tasks();
    app_log_task();
    runtime_diagnostics_loop_end(loop_started_us);

    // USB audio task handles its own timing via vTaskDelayUntil. Keep this a
    // few ms so the loop does not spin (Core 1 wakeups) while still servicing
    // the web server promptly.
    vTaskDelay(pdMS_TO_TICKS(3)); // yield to let higher-priority tasks run (USB, BLE)
}
