"""Strict, independently testable Arduino HID completion guard patch.

Accepts the stock source and this repository's TRACE_V1 source. Unknown source
layouts fail closed instead of silently building without the transport guard.
"""

import hashlib
from pathlib import Path

V1_MARKER = "// REMOTEMAPPER_HID_TRACE_V1"
V2_MARKER = "// REMOTEMAPPER_HID_TX_GUARD_V2"
V2_END_MARKER = "// REMOTEMAPPER_HID_TX_GUARD_V2_END"
LIFECYCLE_MARKER = "// REMOTEMAPPER_USB_LIFECYCLE_V1"
KNOWN_HID_REGIONS = {
    "560841ea2fa7b9c52ed24086660fa4436bbbcb6a9384780efda4587f616773e9",  # stock
    "2fb012b3181b6c9cc02bb6fd886ff8c4accd1353fc7e2685c8d8bf8477593389",  # TRACE_V1
}

GUARD_ADAPTER = r'''// REMOTEMAPPER_HID_TX_GUARD_V2
// REMOTEMAPPER_EMBEDDED_HID_GUARD
extern "C" void remotemapper_hid_trace(uint8_t, uint8_t) __attribute__((weak));
static void rm_hid_trace(uint8_t stage, uint8_t report) {
    if (remotemapper_hid_trace) remotemapper_hid_trace(stage, report);
}
static portMUX_TYPE rm_hid_tx_mux = portMUX_INITIALIZER_UNLOCKED;
static remotemapper::HidTxGuard rm_hid_tx;

// Called on the TinyUSB task before the Arduino event is queued. Never take
// the sender mutex here: its owner can be waiting for this task's callback.
extern "C" void remotemapper_hid_tx_lifecycle(bool mounted) {
    portENTER_CRITICAL(&rm_hid_tx_mux);
    const bool wake = rm_hid_tx.lifecycle(mounted);
    portEXIT_CRITICAL(&rm_hid_tx_mux);
    if (wake && tinyusb_hid_device_input_sem)
        xSemaphoreGive(tinyusb_hid_device_input_sem);
}

bool USBHID::ready(void){
    return tud_hid_n_ready(0);
}

// Preserve the framework's TinyUSB callback signature compatibility.
template <class F> struct ArgType;
template <class R, class T1, class T2, class T3>
struct ArgType<R(*)(T1, T2, T3)> {
    typedef T1 type1;
    typedef T2 type2;
    typedef T3 type3;
};
typedef ArgType<decltype(&tud_hid_report_complete_cb)>::type3 tud_hid_report_complete_cb_len_t;

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, tud_hid_report_complete_cb_len_t len){
    rm_hid_trace(8, (report && len) ? report[0] : 0);
    portENTER_CRITICAL(&rm_hid_tx_mux);
    const bool matched = rm_hid_tx.complete(instance, report, len);
    portEXIT_CRITICAL(&rm_hid_tx_mux);
    if (!matched) {
        rm_hid_trace(10, (report && len) ? report[0] : 0);
    } else if (tinyusb_hid_device_input_sem) {
        xSemaphoreGive(tinyusb_hid_device_input_sem);
    }
}

bool USBHID::SendReport(uint8_t id, const void* data, size_t len, uint32_t timeout_ms){
    rm_hid_trace(0, id);
    if(!tinyusb_hid_device_input_sem || !tinyusb_hid_device_input_mutex){
        rm_hid_trace(1, id);
        log_e("TX Semaphore is NULL. You must call USBHID::begin() before you can send reports");
        return false;
    }
    const TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    if(xSemaphoreTake(tinyusb_hid_device_input_mutex, budget) != pdTRUE){
        rm_hid_trace(2, id);
        log_e("report %u mutex failed", id);
        return false;
    }

    bool res = false;
    if (!ready()) {
        rm_hid_trace(3, id);
    } else {
        uint32_t generation = 0;
        portENTER_CRITICAL(&rm_hid_tx_mux);
        const auto started = rm_hid_tx.begin(id, static_cast<const uint8_t*>(data), len, generation);
        portEXIT_CRITICAL(&rm_hid_tx_mux);
        if (started != remotemapper::HidTxGuard::Start::Started) {
            rm_hid_trace(started == remotemapper::HidTxGuard::Start::PreviousPending ? 9 : 3, id);
        } else {
            // Clearing an old wakeup is only an optimization. A wakeup can
            // never acknowledge a report without the matching generation.
            xSemaphoreTake(tinyusb_hid_device_input_sem, 0);
            const bool accepted = tud_hid_n_report(0, id, data, len);
            rm_hid_trace(accepted ? 4 : 5, id);
            portENTER_CRITICAL(&rm_hid_tx_mux);
            rm_hid_tx.submitted(generation, accepted);
            portEXIT_CRITICAL(&rm_hid_tx_mux);
            if (accepted) {
                const TickType_t started_at = xTaskGetTickCount();
                for (;;) {
                    portENTER_CRITICAL(&rm_hid_tx_mux);
                    const auto result = rm_hid_tx.result(generation);
                    portEXIT_CRITICAL(&rm_hid_tx_mux);
                    if (result == remotemapper::HidTxGuard::Result::Success) {
                        res = true;
                        break;
                    }
                    if (result != remotemapper::HidTxGuard::Result::Pending) {
                        rm_hid_trace(11, id);
                        break;
                    }
                    const TickType_t elapsed = xTaskGetTickCount() - started_at;
                    if (elapsed >= budget) {
                        // Keep the slot pending after timeout. No subsequent
                        // report may be armed before this callback retires.
                        rm_hid_trace(6, id);
                        log_e("report %u wait failed", id);
                        break;
                    }
                    xSemaphoreTake(tinyusb_hid_device_input_sem, budget - elapsed);
                }
            }
        }
    }
    if (res) rm_hid_trace(7, id);
    xSemaphoreGive(tinyusb_hid_device_input_mutex);
    return res;
}

// REMOTEMAPPER_HID_TX_GUARD_V2_END
'''

# Embed the exact tested implementation. Shared Arduino packages must not
# depend on an include directory belonging to this particular checkout.
GUARD_ADAPTER = GUARD_ADAPTER.replace(
    "// REMOTEMAPPER_EMBEDDED_HID_GUARD",
    Path(__file__).with_name("hid_tx_guard.h").read_text(encoding="utf-8")
        .replace("#pragma once\n", "", 1).rstrip(),
)

LIFECYCLE_DECLARATIONS = r'''// REMOTEMAPPER_USB_LIFECYCLE_V1
extern "C" void remotemapper_hid_tx_lifecycle(bool) __attribute__((weak));
extern "C" void remotemapper_usb_lifecycle(bool) __attribute__((weak));
static void rm_usb_lifecycle(bool mounted) {
    if (remotemapper_hid_tx_lifecycle) remotemapper_hid_tx_lifecycle(mounted);
    if (remotemapper_usb_lifecycle) remotemapper_usb_lifecycle(mounted);
}

'''


def _once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError("Arduino USB HID guard anchor mismatch: " + old)
    return source.replace(old, new, 1)


def patch_hid_source(source):
    if V2_MARKER in source:
        if source.count(V2_MARKER + "\n") != 1 or source.count(V2_END_MARKER) != 1:
            raise RuntimeError("Arduino USB HID V2 guard boundary mismatch")
        start = source.index(V2_MARKER + "\n")
        end = source.index(V2_END_MARKER, start) + len(V2_END_MARKER)
        # Refresh the controlled block when the repository's state machine is
        # updated, instead of trusting a version marker with stale contents.
        return source[:start] + GUARD_ADAPTER.rstrip() + source[end:]
    if V1_MARKER in source:
        start = source.index(V1_MARKER)
    else:
        anchor = "bool USBHID::ready(void){"
        if source.count(anchor) != 1:
            raise RuntimeError("Arduino USB HID ready() anchor mismatch")
        start = source.index(anchor)
    end_anchor = "bool USBHID::addDevice(USBHIDDevice * device, uint16_t descriptor_len){"
    if source.count(end_anchor) != 1:
        raise RuntimeError("Arduino USB HID addDevice() anchor mismatch")
    end = source.index(end_anchor)
    old = source[start:end]
    if hashlib.sha256(old.encode("utf-8")).hexdigest() not in KNOWN_HID_REGIONS:
        raise RuntimeError("Arduino USB HID source differs from stock and TRACE_V1")
    # Validate the upstream shape rather than overwrite an unrelated version.
    required = (
        "return tud_hid_n_ready(0);",
        "void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, tud_hid_report_complete_cb_len_t len){",
        "bool USBHID::SendReport(uint8_t id, const void* data, size_t len, uint32_t timeout_ms){",
        "xSemaphoreGive(tinyusb_hid_device_input_sem);",
        "xSemaphoreTake(tinyusb_hid_device_input_mutex, timeout_ms / portTICK_PERIOD_MS)",
        "res = tud_hid_n_report(0, id, data, len);",
        "xSemaphoreTake(tinyusb_hid_device_input_sem, timeout_ms / portTICK_PERIOD_MS)",
        "xSemaphoreGive(tinyusb_hid_device_input_mutex);",
    )
    for anchor in required:
        if old.count(anchor) != 1:
            raise RuntimeError("Arduino USB HID guard source shape mismatch: " + anchor)
    return source[:start] + GUARD_ADAPTER + source[end:]


def patch_usb_source(source):
    if LIFECYCLE_MARKER in source:
        if source.count(LIFECYCLE_MARKER) != 1 or LIFECYCLE_DECLARATIONS not in source:
            raise RuntimeError("Arduino USB lifecycle hook was modified")
        for mounted in ("true", "false"):
            if source.count("    rm_usb_lifecycle(" + mounted + ");") != 1:
                raise RuntimeError("Arduino USB lifecycle hook call mismatch")
        return source
    source = _once(source, "static bool tinyusb_device_mounted = false;",
                   LIFECYCLE_DECLARATIONS + "static bool tinyusb_device_mounted = false;")
    source = _once(source, "void tud_mount_cb(void){\n    tinyusb_device_mounted = true;",
                   "void tud_mount_cb(void){\n    rm_usb_lifecycle(true);\n    tinyusb_device_mounted = true;")
    return _once(source, "void tud_umount_cb(void){\n    tinyusb_device_mounted = false;",
                 "void tud_umount_cb(void){\n    rm_usb_lifecycle(false);\n    tinyusb_device_mounted = false;")
