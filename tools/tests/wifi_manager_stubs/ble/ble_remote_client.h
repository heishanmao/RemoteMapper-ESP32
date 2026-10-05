#pragma once
enum ble_remote_state_t { BLE_STATE_DISCONNECTED, BLE_STATE_SCANNING, BLE_STATE_CONNECTING, BLE_STATE_CONNECTED, BLE_STATE_TALKING };
inline ble_remote_state_t ble_remote_get_state() { return BLE_STATE_CONNECTED; }
inline void ble_remote_notify_wifi_wake() {}
