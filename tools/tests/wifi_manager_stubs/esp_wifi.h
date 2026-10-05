#pragma once
using esp_err_t = int;
static constexpr int ESP_OK = 0;
static constexpr int ESP_ERR_WIFI_NOT_STARTED = 1;
static constexpr int ESP_ERR_WIFI_NOT_INIT = 2;
inline esp_err_t esp_wifi_scan_stop() { return ESP_OK; }
inline esp_err_t esp_wifi_deinit_internal() { return ESP_OK; }
