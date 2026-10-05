#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
typedef uint32_t esp_ota_handle_t;
static const esp_err_t ESP_OK = 0;
static const esp_err_t ESP_FAIL = -1;
static const uint32_t OTA_WITH_SEQUENTIAL_WRITES = 0xFFFFFFFFu;

typedef struct esp_partition_t {
    uint32_t address;
    uint32_t size;
    const char* label;
} esp_partition_t;

typedef enum {
    ESP_OTA_IMG_ABORTED = 0,
    ESP_OTA_IMG_UNDEFINED = 1,
    ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3,
    ESP_OTA_IMG_PENDING_VERIFY = 4
} esp_ota_img_states_t;

extern "C" {
const esp_partition_t* esp_ota_get_running_partition(void);
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* start_from);
esp_err_t esp_ota_begin(const esp_partition_t* partition, size_t image_size, esp_ota_handle_t* handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
const char* esp_err_to_name(esp_err_t err);
}
