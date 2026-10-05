#pragma once
#include "esp_wifi.h"
#include <cstddef>
#include <cstdint>
using nvs_handle_t = uint32_t;
static constexpr int NVS_READONLY = 0;
static constexpr int NVS_READWRITE = 1;
static constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
inline esp_err_t nvs_open(const char*, int, nvs_handle_t* handle) {
    if (handle) *handle = 1;
    return handle ? ESP_OK : 3;
}
inline void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*);
esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*);
esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*);
esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*);
esp_err_t nvs_commit(nvs_handle_t);
