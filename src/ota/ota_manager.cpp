#include "ota_manager.h"
#include "log/app_log.h"
#include <Update.h>
#include <esp_ota_ops.h>
#include <Preferences.h>

#ifndef REMOTEMAPPER_OTA
#define REMOTEMAPPER_OTA 0
#endif

#define OTA_WATCHDOG_TRIES        3
#define OTA_WATCHDOG_CONFIRM_MS   (120000UL)
#define OTA_WATCHDOG_NS           "ota_boot"
#define OTA_WATCHDOG_KEY_PENDING  "pending"
#define OTA_WATCHDOG_KEY_FAIL     "fail"
#define OTA_WATCHDOG_CONFIRM_RETRY_MS 5000UL
#define OTA_WATCHDOG_CONFIRM_MAX_ATTEMPTS 3
#define OTA_ERROR_WATCHDOG_STORAGE 0xFFFF0001UL
#define OTA_ERROR_WATCHDOG_CONFIRM 0xFFFF0002UL
#define OTA_ERROR_WATCHDOG_PENDING 0xFFFF0003UL

static ota_status_t s_ota = {0};
static bool         s_watchdog_armed = false;
static bool         s_watchdog_boot_pending = false;
static uint8_t      s_watchdog_confirm_attempts = 0;
static uint32_t     s_watchdog_confirm_retry_at = 0;
// Partition the running OTA upload is written to; boot is switched to it on
// success. We use esp_ota_* directly: Arduino's Update library writes otadata
// with a broken CRC, which makes the bootloader treat it as invalid and never
// boot the freshly flashed slot.
static const esp_partition_t* s_ota_target = NULL;
static esp_ota_handle_t       s_ota_handle = 0;

static void ota_refresh_partition_info(void) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != NULL) {
        s_ota.running_addr = running->address;
        s_ota.running_label = running->label;
    }
    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
    if (next != NULL) {
        s_ota.target_addr = next->address;
        s_ota.target_label = next->label;
        s_ota.target_capacity = next->size;
    }
}

static bool ota_has_ota_slot(void) {
    return esp_ota_get_next_update_partition(NULL) != NULL;
}

static bool ota_watchdog_keys_absent(Preferences& p) {
    return !p.isKey(OTA_WATCHDOG_KEY_PENDING) && !p.isKey(OTA_WATCHDOG_KEY_FAIL);
}

static bool ota_clear_watchdog_keys(Preferences& p) {
    p.remove(OTA_WATCHDOG_KEY_PENDING);
    p.remove(OTA_WATCHDOG_KEY_FAIL);
    return ota_watchdog_keys_absent(p);
}

static bool ota_clear_watchdog_keys(void) {
    Preferences p;
    if (!p.begin(OTA_WATCHDOG_NS, false)) return false;
    const bool cleared = ota_clear_watchdog_keys(p);
    p.end();
    return cleared;
}

static bool ota_write_watchdog_pending(uint8_t fail_count) {
    Preferences p;
    if (!p.begin(OTA_WATCHDOG_NS, false)) {
        s_watchdog_armed = true;
        s_watchdog_boot_pending = true;
        s_ota.rollback_armed = true;
        return false;
    }

    // Keep the 1.2.50 keys and byte values. Set the fail counter first, then
    // publish pending last so a partial write cannot arm an uninitialized count.
    const bool writes_ok = p.putUChar(OTA_WATCHDOG_KEY_FAIL, fail_count) == sizeof(uint8_t) &&
                           p.putUChar(OTA_WATCHDOG_KEY_PENDING, 1) == sizeof(uint8_t);
    const bool verified = writes_ok &&
            p.getUChar(OTA_WATCHDOG_KEY_PENDING, 0xFF) == 1 &&
            p.getUChar(OTA_WATCHDOG_KEY_FAIL, 0xFF) == fail_count;
    bool cleared = false;
    if (!verified) cleared = ota_clear_watchdog_keys(p);
    p.end();

    if (verified) {
        s_watchdog_armed = true;
        s_ota.rollback_armed = true;
        s_ota.boot_fail_count = fail_count;
        s_watchdog_confirm_attempts = 0;
        s_watchdog_confirm_retry_at = 0;
        s_watchdog_boot_pending = true;
    } else if (cleared) {
        s_watchdog_armed = false;
        s_ota.rollback_armed = false;
        s_ota.boot_fail_count = 0;
        s_watchdog_boot_pending = false;
    } else {
        // If storage could not be opened or cleaned, retain a runtime guard so
        // the healthy current image gets a chance to clear a possibly stale key.
        s_watchdog_armed = true;
        s_ota.rollback_armed = true;
        s_watchdog_boot_pending = true;
    }
    return verified;
}

static bool ota_mark_watchdog_pending(void) {
    if (!ota_write_watchdog_pending(0)) return false;
    // The current image has not booted from this marker. It must not confirm
    // the next image merely because this upload took longer than the grace time.
    s_watchdog_boot_pending = false;
    return true;
}

static bool ota_mark_watchdog_confirmed(void) {
    if (!ota_clear_watchdog_keys()) return false;
    s_watchdog_armed = false;
    s_watchdog_boot_pending = false;
    s_ota.rollback_armed = false;
    s_ota.boot_fail_count = 0;
    s_watchdog_confirm_attempts = 0;
    s_watchdog_confirm_retry_at = 0;
    if (s_ota.last_error == OTA_ERROR_WATCHDOG_CONFIRM ||
            s_ota.last_error == OTA_ERROR_WATCHDOG_STORAGE ||
            s_ota.last_error == OTA_ERROR_WATCHDOG_PENDING) {
        s_ota.last_error = UPDATE_ERROR_OK;
    }
    app_log("OTA", "Safe-boot watchdog cleared: new firmware confirmed healthy");
    return true;
}

void ota_manager_init(void) {
    s_ota.in_progress = false;
    s_ota.last_success = false;
    s_ota.last_error = 0;
    s_ota.received = 0;
    s_ota.total = 0;
    s_ota.rollback_armed = false;
    s_ota.boot_fail_count = 0;
    s_watchdog_armed = false;
    s_watchdog_boot_pending = false;
    s_watchdog_confirm_attempts = 0;
    s_watchdog_confirm_retry_at = 0;
    s_ota_handle = 0;
    s_ota_target = NULL;
#if REMOTEMAPPER_OTA
    ota_refresh_partition_info();
    s_ota.enabled = (s_ota.target_capacity > 0);
    app_log("INIT", "OTA manager ready (%s@0x%x -> %s@0x%x, %u bytes)",
            s_ota.running_label ? s_ota.running_label : "-",
            s_ota.running_addr,
            s_ota.target_label ? s_ota.target_label : "-",
            s_ota.target_addr,
            s_ota.target_capacity);

    // Tell the IDF bootloader this image is healthy. When the bootloader is
    // built with app-rollback enabled, an OTA image stays "pending verify"
    // and boots get invalidated after a few tries unless the app marks itself
    // valid here; without this, every OTA reverts to the previous slot forever.
    esp_err_t mr = esp_ota_mark_app_valid_cancel_rollback();
    if (mr != ESP_OK) {
        app_log("OTA", "Warn: esp_ota_mark_app_valid_cancel_rollback failed: %s",
                esp_err_to_name(mr));
    }
#else
    s_ota.enabled = false;
    app_log("INIT", "OTA manager disabled in this build");
#endif

    // Safe-boot watchdog: if an OTA image is pending confirmation, count this
    // boot; roll back to the previous image after too many failed attempts.
    ota_watchdog_action_t act = ota_manager_watchdog_tick();
    if (act == OTA_WATCHDOG_ROLLBACK) {
        app_log("OTA", "Rolling back to previous firmware now...");
        delay(150);
        ESP.restart();
    }
}

bool ota_manager_is_supported(void) {
    return s_ota.enabled;
}

const ota_status_t* ota_manager_get_status(void) {
    return &s_ota;
}

bool ota_manager_begin(uint32_t image_size) {
    s_ota.last_success = false;
    s_ota.last_error = UPDATE_ERROR_OK;
    s_ota.received = 0;
    s_ota.total = 0;
    if (s_ota.in_progress) {
        s_ota.last_error = UPDATE_ERROR_BAD_ARGUMENT;
        app_log("OTA", "OTA begin rejected: another upload is in progress");
        return false;
    }
    if (s_watchdog_armed) {
        s_ota.last_error = OTA_ERROR_WATCHDOG_PENDING;
        app_log("OTA", "OTA begin rejected: the current firmware is still awaiting safe-boot confirmation");
        return false;
    }
    s_ota_target = NULL;
    s_ota_handle = 0;
    if (!s_ota.enabled) {
        s_ota.last_error = UPDATE_ERROR_NO_PARTITION;
        app_log("OTA", "OTA begin rejected: not supported");
        return false;
    }
    if (image_size == 0) {
        image_size = UPDATE_SIZE_UNKNOWN;
    }
    if (image_size != UPDATE_SIZE_UNKNOWN && image_size > s_ota.target_capacity) {
        s_ota.last_error = UPDATE_ERROR_SIZE;
        app_log("OTA", "OTA begin rejected: image %u > slot capacity %u", image_size, s_ota.target_capacity);
        return false;
    }
    s_ota_target = esp_ota_get_next_update_partition(NULL);
    if (s_ota_target == NULL) {
        s_ota.last_error = UPDATE_ERROR_NO_PARTITION;
        app_log("OTA", "OTA begin rejected: no OTA partition found");
        return false;
    }
    s_ota_handle = 0;
    esp_err_t eb = esp_ota_begin(
        s_ota_target,
        image_size == UPDATE_SIZE_UNKNOWN ? OTA_WITH_SEQUENTIAL_WRITES : image_size,
        &s_ota_handle);
    if (eb != ESP_OK) {
        s_ota.last_error = UPDATE_ERROR_WRITE;
        s_ota_target = NULL;
        app_log("OTA", "esp_ota_begin failed: %s", esp_err_to_name(eb));
        return false;
    }
    s_ota.in_progress = true;
    s_ota.last_success = false;
    s_ota.last_error = UPDATE_ERROR_OK;
    s_ota.received = 0;
    s_ota.total = image_size;
    app_log("OTA", "OTA upload started (size=%s, slot=%s)",
            image_size == UPDATE_SIZE_UNKNOWN ? "unknown" : String(image_size).c_str(),
            s_ota.target_label ? s_ota.target_label : "?");
    return true;
}

size_t ota_manager_write(const uint8_t* data, size_t len) {
    if (!s_ota.in_progress || s_ota_handle == 0 || len == 0 ||
            s_ota.last_error != UPDATE_ERROR_OK) {
        return 0;
    }
    if (!data) {
        s_ota.last_error = UPDATE_ERROR_BAD_ARGUMENT;
        app_log("OTA", "OTA write rejected: null data");
        return 0;
    }
    esp_err_t eb = esp_ota_write(s_ota_handle, data, len);
    if (eb != ESP_OK) {
        s_ota.last_error = UPDATE_ERROR_WRITE;
        app_log("OTA", "esp_ota_write failed at %u: %s", s_ota.received, esp_err_to_name(eb));
        return 0;
    }
    s_ota.received += len;
    return len;
}

bool ota_manager_end(void) {
    if (!s_ota.in_progress) {
        return false;
    }
    if (s_ota.last_error != UPDATE_ERROR_OK) {
        if (s_ota_handle != 0) esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
        s_ota_target = NULL;
        s_ota.last_success = false;
        s_ota.in_progress = false;
        app_log("OTA", "OTA finalization rejected after an earlier upload error");
        return false;
    }
    esp_err_t eb = esp_ota_end(s_ota_handle);
    s_ota_handle = 0;
    if (eb != ESP_OK) {
        s_ota.last_success = false;
        s_ota.in_progress = false;
        s_ota.last_error = UPDATE_ERROR_READ;
        s_ota_target = NULL;
        app_log("OTA", "esp_ota_end (image validation) failed: %s", esp_err_to_name(eb));
        return false;
    }
    // Persist and verify the rollback sentinel before making this image the
    // boot target. A valid image is not a successful OTA until both steps work.
    if (!ota_mark_watchdog_pending()) {
        s_ota.last_success = false;
        s_ota.in_progress = false;
        s_ota.last_error = OTA_ERROR_WATCHDOG_STORAGE;
        s_ota_target = NULL;
        app_log("OTA", "Failed to persist and verify safe-boot watchdog marker; image not activated");
        return false;
    }

    eb = esp_ota_set_boot_partition(s_ota_target);
    if (eb != ESP_OK) {
        s_ota.last_success = false;
        s_ota.in_progress = false;
        s_ota.last_error = UPDATE_ERROR_ACTIVATE;
        if (ota_clear_watchdog_keys()) {
            s_watchdog_armed = false;
            s_watchdog_boot_pending = false;
            s_ota.rollback_armed = false;
            s_ota.boot_fail_count = 0;
        } else {
            // Keep the runtime retry path armed if the marker could not be
            // removed; otherwise it could be left stale across a reboot.
            s_watchdog_armed = true;
            s_watchdog_boot_pending = true;
            s_ota.rollback_armed = true;
            app_log("OTA", "Activation failed and safe-boot marker cleanup could not be verified");
        }
        s_ota_target = NULL;
        app_log("OTA", "Failed to activate new boot partition: %s", esp_err_to_name(eb));
        return false;
    }
    s_ota.last_success = true;
    s_ota.in_progress = false;
    ota_refresh_partition_info();
    app_log("OTA", "OTA success: next boot from %s@0x%x, safe-boot watchdog armed",
            s_ota_target ? s_ota_target->label : "-",
            s_ota_target ? s_ota_target->address : 0U);
    return true;
}

void ota_manager_abort(void) {
    if (!s_ota.in_progress) {
        return;
    }
    if (s_ota_handle != 0) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }
    s_ota.last_error = UPDATE_ERROR_ABORT;
    s_ota.last_success = false;
    s_ota.in_progress = false;
    s_ota_target = NULL;
    app_log("OTA", "OTA upload aborted");
}

const char* ota_manager_error_str(void) {
    switch (s_ota.last_error) {
        case UPDATE_ERROR_OK:           return "OK";
        case UPDATE_ERROR_WRITE:        return "Flash 写入失败";
        case UPDATE_ERROR_ERASE:        return "Flash 擦除失败";
        case UPDATE_ERROR_READ:         return "新固件读取校验失败";
        case UPDATE_ERROR_SPACE:        return "Flash 空间不足";
        case UPDATE_ERROR_SIZE:         return "固件体积超过分区容量";
        case UPDATE_ERROR_STREAM:       return "固件流解析错误";
        case UPDATE_ERROR_MD5:          return "MD5 校验失败，固件已损坏";
        case UPDATE_ERROR_MAGIC_BYTE:   return "无效固件头，请确认选择正确型号的 .bin";
        case UPDATE_ERROR_ACTIVATE:     return "启动分区激活失败";
        case UPDATE_ERROR_NO_PARTITION: return "没有可用的 OTA 分区";
        case UPDATE_ERROR_BAD_ARGUMENT: return "参数错误";
        case UPDATE_ERROR_ABORT:        return "升级已中止（网络中断或手动取消）";
        case OTA_ERROR_WATCHDOG_STORAGE: return "安全启动回滚标记持久化失败，固件未激活";
        case OTA_ERROR_WATCHDOG_CONFIRM: return "安全启动确认状态清除失败";
        case OTA_ERROR_WATCHDOG_PENDING: return "当前固件仍待安全启动确认";
        default:                        return "未知错误";
    }
}

ota_watchdog_action_t ota_manager_watchdog_tick(void) {
    Preferences p;
    if (!p.begin(OTA_WATCHDOG_NS, false)) {
        s_watchdog_armed = true;
        s_watchdog_boot_pending = true;
        s_ota.rollback_armed = true;
        s_ota.last_error = OTA_ERROR_WATCHDOG_STORAGE;
        app_log("OTA", "Safe-boot marker could not be opened; OTA remains blocked until confirmation");
        return OTA_WATCHDOG_CONTINUE;
    }

    if (!p.isKey(OTA_WATCHDOG_KEY_PENDING)) {
        p.end();
        s_watchdog_armed = false;
        s_watchdog_boot_pending = false;
        s_ota.rollback_armed = false;
        return OTA_WATCHDOG_CONTINUE;
    }

    const uint8_t pending = p.getUChar(OTA_WATCHDOG_KEY_PENDING, 0xFF);
    if (pending != 1) {
        p.end();
        s_watchdog_armed = true;
        s_watchdog_boot_pending = true;
        s_ota.rollback_armed = true;
        s_ota.last_error = OTA_ERROR_WATCHDOG_STORAGE;
        app_log("OTA", "Safe-boot marker is unreadable; OTA remains blocked until confirmation");
        return OTA_WATCHDOG_CONTINUE;
    }

    const uint8_t previous_fail = p.isKey(OTA_WATCHDOG_KEY_FAIL)
            ? p.getUChar(OTA_WATCHDOG_KEY_FAIL, 0xFF) : 0;
    const uint8_t fail = previous_fail == 0xFF ? 0xFF : (uint8_t)(previous_fail + 1);
    if (p.putUChar(OTA_WATCHDOG_KEY_FAIL, fail) != sizeof(uint8_t) ||
            p.getUChar(OTA_WATCHDOG_KEY_FAIL, 0xFF) != fail) {
        s_watchdog_armed = true;
        s_watchdog_boot_pending = true;
        s_ota.rollback_armed = true;
        s_ota.last_error = OTA_ERROR_WATCHDOG_STORAGE;
        p.end();
        app_log("OTA", "Safe-boot failure counter could not be persisted and verified");
        return OTA_WATCHDOG_CONTINUE;
    }
    s_ota.boot_fail_count = fail;
    s_watchdog_armed = true;
    s_watchdog_boot_pending = true;
    s_ota.rollback_armed = true;

    if (fail < OTA_WATCHDOG_TRIES) {
        app_log("OTA", "Safe-boot watchdog: boot %u/%u awaiting confirmation "
                "(new firmware seems unstable)", (unsigned int)fail,
                (unsigned int)OTA_WATCHDOG_TRIES);
        p.end();
        return OTA_WATCHDOG_CONTINUE;
    }

    const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
    if (!other) {
        s_ota.last_error = OTA_ERROR_WATCHDOG_STORAGE;
        app_log("OTA", "Safe-boot rollback impossible: no OTA partition found");
        p.end();
        return OTA_WATCHDOG_CONTINUE;
    }

    // Clear and verify the sentinel before changing boot selection. If NVS
    // cannot clear it, keep this image selected and let healthy runtime retry.
    if (!ota_clear_watchdog_keys(p)) {
        s_ota.last_error = OTA_ERROR_WATCHDOG_STORAGE;
        app_log("OTA", "Safe-boot rollback marker could not be cleared; boot selection unchanged");
        p.end();
        return OTA_WATCHDOG_CONTINUE;
    }
    p.end();

    const esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        const bool restored = ota_write_watchdog_pending(fail);
        s_watchdog_armed = true;
        s_watchdog_boot_pending = true;
        s_ota.rollback_armed = true;
        s_ota.boot_fail_count = fail;
        s_ota.last_error = restored ? UPDATE_ERROR_ACTIVATE : OTA_ERROR_WATCHDOG_STORAGE;
        app_log("OTA", "Safe-boot rollback FAILED to set partition: %s; marker %s",
                esp_err_to_name(err), restored ? "restored" : "restore unverified");
        return OTA_WATCHDOG_CONTINUE;
    }

    app_log("OTA", "SAFE-BOOT ROLLBACK: %u consecutive failures, switching boot to %s@0x%x",
            (unsigned int)fail, other->label, (unsigned int)other->address);
    s_watchdog_armed = false;
    s_watchdog_boot_pending = false;
    s_ota.rollback_armed = false;
    s_ota.boot_fail_count = 0;
    if (s_ota.last_error == OTA_ERROR_WATCHDOG_STORAGE) s_ota.last_error = UPDATE_ERROR_OK;
    return OTA_WATCHDOG_ROLLBACK;
}
void ota_manager_watchdog_confirm(void) {
    if (!s_watchdog_armed || !s_watchdog_boot_pending) {
        return;
    }
    const uint32_t now = millis();
    if (now < OTA_WATCHDOG_CONFIRM_MS) {
        return;
    }
    if (s_watchdog_confirm_attempts >= OTA_WATCHDOG_CONFIRM_MAX_ATTEMPTS) return;
    if (s_watchdog_confirm_attempts && (int32_t)(now - s_watchdog_confirm_retry_at) < 0) return;

    s_watchdog_confirm_attempts++;
    if (ota_mark_watchdog_confirmed()) return;

    s_ota.last_error = OTA_ERROR_WATCHDOG_CONFIRM;
    s_watchdog_confirm_retry_at = now + OTA_WATCHDOG_CONFIRM_RETRY_MS;
    app_log("OTA", "Safe-boot confirmation marker cleanup failed (attempt %u/%u)",
            (unsigned int)s_watchdog_confirm_attempts,
            (unsigned int)OTA_WATCHDOG_CONFIRM_MAX_ATTEMPTS);
}
