#include "ota/ota_manager.h"
#include "Arduino.h"
#include "Preferences.h"
#include "esp_ota_ops.h"
#include <cassert>
#include <cstdarg>
#include <cstring>
#include <string>

TestEsp ESP;
static uint32_t now_ms = 0;
static esp_partition_t running_partition = {0x10000, 0x200000, "ota_0"};
static esp_partition_t target_partition = {0x210000, 0x200000, "ota_1"};
static const esp_partition_t* selected_boot_partition = &running_partition;
static bool fail_ota_begin = false;
static bool fail_ota_write = false;
static bool fail_ota_end = false;
static bool fail_activate = false;
static uint32_t ota_end_calls = 0;
static uint32_t ota_abort_calls = 0;
static uint32_t confirmed_log_count = 0;

uint32_t millis(void) { return now_ms; }
void delay(uint32_t) {}

void app_log(const char*, const char* format, ...) {
    if (std::strstr(format, "confirmed healthy")) confirmed_log_count++;
}

extern "C" const esp_partition_t* esp_ota_get_running_partition(void) {
    return &running_partition;
}

extern "C" const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {
    return &target_partition;
}

extern "C" esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t* handle) {
    if (fail_ota_begin) return ESP_FAIL;
    *handle = 7;
    return ESP_OK;
}

extern "C" esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t) {
    return fail_ota_write ? ESP_FAIL : ESP_OK;
}

extern "C" esp_err_t esp_ota_end(esp_ota_handle_t) {
    ota_end_calls++;
    return fail_ota_end ? ESP_FAIL : ESP_OK;
}

extern "C" esp_err_t esp_ota_abort(esp_ota_handle_t) {
    ota_abort_calls++;
    return ESP_OK;
}

extern "C" esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) {
    if (fail_activate) return ESP_FAIL;
    selected_boot_partition = partition;
    return ESP_OK;
}

extern "C" esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }

extern "C" const char* esp_err_to_name(esp_err_t err) {
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

static void reset_case() {
    Preferences::values.clear();
    Preferences::fail_begin = false;
    Preferences::fail_put = false;
    Preferences::fail_readback = false;
    Preferences::fail_remove = false;
    Preferences::begin_calls = 0;
    fail_ota_begin = false;
    fail_ota_write = false;
    fail_ota_end = false;
    fail_activate = false;
    ota_end_calls = 0;
    ota_abort_calls = 0;
    confirmed_log_count = 0;
    now_ms = 0;
    selected_boot_partition = &running_partition;
    ota_manager_init();
}

static bool start_upload() {
    static const uint8_t bytes[] = {1, 2, 3, 4};
    return ota_manager_begin(sizeof(bytes)) &&
           ota_manager_write(bytes, sizeof(bytes)) == sizeof(bytes);
}

static bool has_key(const char* key) {
    return Preferences::values.find(key) != Preferences::values.end();
}

int main() {
    // A Preferences begin failure prevents boot-partition activation.
    reset_case();
    assert(start_upload());
    Preferences::fail_begin = true;
    assert(!ota_manager_end());
    assert(selected_boot_partition == &running_partition);
    assert(!ota_manager_get_status()->last_success);
    assert(std::strstr(ota_manager_error_str(), "回滚标记") != nullptr);

    // Failed byte writes and unreadable readback both reject the image.
    reset_case();
    assert(start_upload());
    Preferences::fail_put = true;
    assert(!ota_manager_end());
    assert(selected_boot_partition == &running_partition);
    assert(!ota_manager_get_status()->last_success);
    assert(!has_key("pending") && !has_key("fail"));

    reset_case();
    assert(start_upload());
    Preferences::fail_readback = true;
    assert(!ota_manager_end());
    assert(selected_boot_partition == &running_partition);
    assert(!ota_manager_get_status()->last_success);
    assert(!has_key("pending") && !has_key("fail"));

    // If activation fails after marker verification, remove both old-format
    // keys and leave the running firmware unarmed.
    reset_case();
    assert(start_upload());
    fail_activate = true;
    assert(!ota_manager_end());
    assert(selected_boot_partition == &running_partition);
    assert(!ota_manager_get_status()->last_success);
    assert(!ota_manager_get_status()->rollback_armed);
    assert(!has_key("pending") && !has_key("fail"));

    // Confirmation removal failures remain armed and are retried at most
    // three times, with a five-second minimum interval.
    reset_case();
    assert(start_upload());
    assert(ota_manager_end());
    assert(Preferences::values["pending"] == 1);
    assert(Preferences::values["fail"] == 0);
    // The upload's current (old) image cannot confirm the next boot marker.
    now_ms = 120000;
    ota_manager_watchdog_confirm();
    assert(ota_manager_get_status()->rollback_armed);
    assert(Preferences::values["pending"] == 1);
    assert(Preferences::values["fail"] == 0);
    assert(ota_manager_get_status()->last_success);
    assert(ota_manager_get_status()->rollback_armed);
    Preferences::fail_remove = true;
    // Simulate reboot: only a marker loaded by this boot may be confirmed.
    now_ms = 0;
    ota_manager_init();
    assert(Preferences::values["fail"] == 1);
    now_ms = 120000;
    ota_manager_watchdog_confirm();
    assert(ota_manager_get_status()->rollback_armed);
    assert(std::strstr(ota_manager_error_str(), "确认状态清除失败") != nullptr);
    const uint32_t first_attempt_calls = Preferences::begin_calls;
    for (int i = 0; i < 100; ++i) ota_manager_watchdog_confirm();
    assert(Preferences::begin_calls == first_attempt_calls);
    now_ms = 125000;
    ota_manager_watchdog_confirm();
    now_ms = 130000;
    ota_manager_watchdog_confirm();
    const uint32_t third_attempt_calls = Preferences::begin_calls;
    now_ms = 135000;
    for (int i = 0; i < 100; ++i) ota_manager_watchdog_confirm();
    assert(Preferences::begin_calls == third_attempt_calls);
    assert(confirmed_log_count == 0);
    assert(ota_manager_get_status()->rollback_armed);

    // Normal completion keeps the compatible sentinel and activates only after
    // readback has verified pending=1 and fail=0.
    reset_case();
    assert(start_upload());
    assert(ota_manager_end());
    assert(selected_boot_partition == &target_partition);
    assert(ota_manager_get_status()->last_success);
    assert(ota_manager_get_status()->rollback_armed);
    assert(Preferences::values["pending"] == 1);
    assert(Preferences::values["fail"] == 0);
    assert(!ota_manager_begin(4));
    assert(std::strstr(ota_manager_error_str(), "仍待安全启动确认") != nullptr);
    assert(Preferences::values["pending"] == 1);
    assert(Preferences::values["fail"] == 0);
    now_ms = 0;
    ota_manager_init();
    assert(Preferences::values["fail"] == 1);
    now_ms = 120000;
    ota_manager_watchdog_confirm();
    assert(!ota_manager_get_status()->rollback_armed);
    assert(!has_key("pending") && !has_key("fail"));

    // A failed flash write is sticky: end aborts the handle and cannot activate.
    reset_case();
    assert(ota_manager_begin(4));
    fail_ota_write = true;
    static const uint8_t bytes[] = {1, 2, 3, 4};
    assert(ota_manager_write(bytes, sizeof(bytes)) == 0);
    fail_ota_write = false;
    assert(!ota_manager_end());
    assert(ota_abort_calls == 1);
    assert(ota_end_calls == 0);
    assert(selected_boot_partition == &running_partition);
    assert(!ota_manager_get_status()->last_success);
    assert(!has_key("pending") && !has_key("fail"));

    // A rejected new begin clears stale success state and reports its current error.
    reset_case();
    assert(start_upload());
    assert(ota_manager_end());
    now_ms = 0;
    ota_manager_init();
    now_ms = 120000;
    ota_manager_watchdog_confirm();
    assert(!ota_manager_begin(target_partition.size + 1));
    assert(!ota_manager_get_status()->last_success);
    assert(std::strstr(ota_manager_error_str(), "超过分区容量") != nullptr);

    // Rollback must not switch partitions if removing the sentinel fails.
    reset_case();
    Preferences::values["pending"] = 1;
    Preferences::values["fail"] = 2;
    Preferences::fail_remove = true;
    ota_manager_init();
    assert(selected_boot_partition == &running_partition);
    assert(ota_manager_get_status()->rollback_armed);
    assert(ota_manager_get_status()->boot_fail_count == 3);
    assert(Preferences::values["pending"] == 1);
    assert(Preferences::values["fail"] == 3);
    assert(std::strstr(ota_manager_error_str(), "回滚标记") != nullptr);
    Preferences::fail_remove = false;
    now_ms = 120000;
    ota_manager_watchdog_confirm();
    assert(!ota_manager_get_status()->rollback_armed);
    assert(!has_key("pending") && !has_key("fail"));

    // If partition activation fails after clearing, restore pending/fail before
    // reporting the failed rollback so the next boot can retry the same decision.
    reset_case();
    Preferences::values["pending"] = 1;
    Preferences::values["fail"] = 2;
    fail_activate = true;
    ota_manager_init();
    assert(selected_boot_partition == &running_partition);
    assert(ota_manager_get_status()->rollback_armed);
    assert(Preferences::values["pending"] == 1);
    assert(Preferences::values["fail"] == 3);
    assert(std::strstr(ota_manager_error_str(), "激活失败") != nullptr);

    // If NVS cannot be opened at boot, fail closed: block new OTA until a
    // bounded healthy-runtime cleanup succeeds.
    reset_case();
    Preferences::fail_begin = true;
    ota_manager_init();
    assert(ota_manager_get_status()->rollback_armed);
    assert(std::strstr(ota_manager_error_str(), "回滚标记") != nullptr);
    assert(!ota_manager_begin(4));
    Preferences::fail_begin = false;
    now_ms = 120000;
    ota_manager_watchdog_confirm();
    assert(!ota_manager_get_status()->rollback_armed);
    assert(!has_key("pending") && !has_key("fail"));
    return 0;
}
