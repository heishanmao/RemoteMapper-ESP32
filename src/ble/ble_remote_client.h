#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <Arduino.h>
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_STATE_DISCONNECTED = 0,
    BLE_STATE_SCANNING,
    BLE_STATE_CONNECTING,
    BLE_STATE_CONNECTED,
    BLE_STATE_TALKING
} ble_remote_state_t;

/**
 * @brief Initialize NimBLE Client for Xiaomi Remote
 */
void ble_remote_init(void);

// Last complete ATVV frame received (independent of volume/USB backpressure).
uint32_t ble_remote_last_audio_frame_ms(void);
// Defer mic close to the BLE task; a new active session supersedes this request.
void ble_remote_request_mic_stop(void);


/**
 * @brief Main BLE task tick (handles state machine, watchdogs, and keep-alive packets)
 */
void ble_remote_task(void);

/**
 * @brief Emit the periodic audio RX accounting line (frame count, inter-arrival
 *        cadence, decode cost). Must be called from a normal task, never from a
 *        NimBLE callback: the host task dispatches every BLE notification on
 *        this connection, so formatting there kills key and audio delivery.
 */
void ble_audio_rx_diagnostics_tick(void);

/**
 * @brief Get current BLE connection state
 */
ble_remote_state_t ble_remote_get_state(void);

/**
 * @brief Trigger manual reconnect / re-scan
 */
void ble_remote_trigger_reconnect(void);

/**
 * @brief Scan for nearby BLE devices and return JSON list
 */
String ble_remote_scan_devices_json(void);

/**
 * @brief Ask the BLE task to run a short on-demand scan burst.
 *
 * While the remote is connected the continuous background scan is stopped
 * (radio power save); the WebUI calls this before reading the device list
 * so it gets a fresh burst instead of a stale cache.
 */
void ble_remote_request_scan_burst(void);

/**
 * @brief Manually connect and pair to specific BLE device by MAC address, address type, and optional name
 */
bool ble_remote_connect_target(const String& mac_str, uint8_t addr_type, const String& dev_name);

/**
 * @brief Manually connect and pair to specific BLE device by MAC address
 */
bool ble_remote_connect_mac(const String& mac_str);

/**
 * @brief Unpair and clear saved remote MAC address
 */
void ble_remote_unpair(void);

/**
 * @brief Get connected remote name and MAC
 */
String ble_remote_get_connected_info(void);

/**
 * @brief Get connected remote battery level percentage (0~100, or -1 if unknown/disconnected)
 */
int ble_remote_get_battery_pct(void);

/**
 * @brief Get connected remote device information (if available). All fields are optional.
 * Returned strings may be empty when not read yet.
 */
String ble_remote_get_device_info_model(void);
String ble_remote_get_device_info_manufacturer(void);
String ble_remote_get_device_info_serial(void);
String ble_remote_get_device_info_hw(void);
String ble_remote_get_device_info_fw(void);
String ble_remote_get_device_info_sw(void);

/**
 * @brief Notify the BLE layer that the Wi-Fi radio just woke up (ON_DEMAND wake).
 * A Wi-Fi re-init can starve BLE coexistence and drop the remote link; if that
 * happened the remote is likely advertising again, so drop the scan backoff to
 * tier-0 and rescan fast instead of slowly tiers'ing up.
 */
void ble_remote_notify_wifi_wake(void);

/**
 * @brief Whether GATT full-dump exploration mode is currently active
 */
bool ble_remote_gatt_dump_enabled(void);

/**
 * @brief Enable/disable GATT explorer.
 * While ON, every fresh remote connection triggers one full GATT enumeration
 * (services/chars/descriptors + readable values), covering 0x180A Device Info,
 * the HID Report Map (0x2A4A) and the unexplored vendor services 0xfe59 /
 * 0x01bf / 8a7a0001-2c42-c2a2-0f36-41928c259b78. Results go to the ring log
 * under the "GATTX" tag.
 */
void ble_remote_gatt_dump_request(bool on);

#ifdef __cplusplus
}
#endif
