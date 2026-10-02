# Auto-applies required patches to the Arduino-ESP32 framework package at build
# time. Idempotent: patches already present are detected and skipped.
#
# Registered from platformio.ini via:
#   extra_scripts = pre:tools/patches/apply_patches.py
#
# Patch 1 (cores/esp32/USB.h):
#   ESPUSB event task stack 2048 -> 16384. The 2048-byte stack overflows
#   (stack canary "arduino_usb_events") on ESP32-S3 when several USB events
#   fire during enumeration, reboot-looping the firmware.
#
# Patch 2 (libraries/WebServer/src/WebServer.h):
#   Add hasUpload()/hasRaw() accessors. FunctionRequestHandler::raw() invokes
#   the upload callback for non-multipart POST bodies, at which point
#   _currentUpload is still null; without a null-guard, s_server.upload()
#   dereferences it and panics (LoadProhibited) during OTA uploads.

Import("env")

import os
import sys

FRAMEWORK = "framework-arduinoespressif32"

USB_ORIG = "ESPUSB(size_t event_task_stack_size=2048, uint8_t event_task_priority=5);"
USB_PATCHED = "ESPUSB(size_t event_task_stack_size=16384, uint8_t event_task_priority=5);"

WS_ANCHOR = "HTTPRaw& raw() { return *_currentRaw; }"
WS_ADDITION = (
    "HTTPRaw& raw() { return *_currentRaw; }\n"
    "  bool hasUpload() const { return _currentUpload != nullptr; }\n"
    "  bool hasRaw() const { return _currentRaw != nullptr; }"
)


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def _write(path, content):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(content)


def _framework_dir():
    try:
        d = env.PioPlatform().get_package_dir(FRAMEWORK)
        if d:
            return d
    except Exception:
        pass
    # Fallbacks for unusual setups
    candidates = [
        env.subst("$PROJECT_PACKAGES_DIR"),
        os.path.join(os.path.expanduser("~"), ".platformio", "packages"),
    ]
    for base in candidates:
        if not base:
            continue
        d = os.path.join(base, FRAMEWORK)
        if os.path.isdir(d):
            return d
    return None


def main():
    fw = _framework_dir()
    if not fw:
        sys.stderr.write("[patches] framework-arduinoespressif32 not found; skipping\n")
        return

    # --- Patch 1: USB event task stack ---
    usb_h = os.path.join(fw, "cores", "esp32", "USB.h")
    if os.path.isfile(usb_h):
        src = _read(usb_h)
        if USB_PATCHED in src:
            print("[patches] USB.h stack 16384: already patched")
        elif USB_ORIG in src:
            _write(usb_h, src.replace(USB_ORIG, USB_PATCHED))
            print("[patches] USB.h stack 2048 -> 16384: APPLIED")
        else:
            sys.stderr.write(
                "[patches] WARNING: USB.h matches neither original nor patched text\n")

    # --- Patch 2: WebServer hasUpload()/hasRaw() ---
    ws_h = os.path.join(fw, "libraries", "WebServer", "src", "WebServer.h")
    if os.path.isfile(ws_h):
        src = _read(ws_h)
        if "bool hasUpload() const" in src:
            print("[patches] WebServer.h hasUpload()/hasRaw(): already patched")
        elif WS_ANCHOR in src:
            _write(ws_h, src.replace(WS_ANCHOR, WS_ADDITION))
            print("[patches] WebServer.h hasUpload()/hasRaw(): APPLIED")
        else:
            sys.stderr.write(
                "[patches] WARNING: WebServer.h anchor line not found\n")

    # Patch 3: optional HID diagnostics. Preserve SendReport semantics and the
    # framework semaphore. Fail the build if its source changes underneath us.
    hid_cpp = os.path.join(fw, "libraries", "USB", "src", "USBHID.cpp")
    src = _read(hid_cpp)
    marker = "// REMOTEMAPPER_HID_TRACE_V1"
    if marker in src:
        print("[patches] USBHID diagnostics V1: already patched")
    else:
        edits = [
            ("bool USBHID::ready(void){", marker + '\nextern "C" void remotemapper_hid_trace(uint8_t, uint8_t) __attribute__((weak));\n'
             "static void rm_hid_trace(uint8_t stage, uint8_t report) {\n"
             "    if (remotemapper_hid_trace) remotemapper_hid_trace(stage, report);\n}\n\n"
             "bool USBHID::ready(void){"),
            ("    if (tinyusb_hid_device_input_sem) {\n        xSemaphoreGive(tinyusb_hid_device_input_sem);",
             "    rm_hid_trace(8, (report && len) ? report[0] : 0);\n"
             "    if (tinyusb_hid_device_input_sem) {\n        xSemaphoreGive(tinyusb_hid_device_input_sem);"),
            ("bool USBHID::SendReport(uint8_t id, const void* data, size_t len, uint32_t timeout_ms){",
             "bool USBHID::SendReport(uint8_t id, const void* data, size_t len, uint32_t timeout_ms){\n    rm_hid_trace(0, id);"),
            ('        log_e("TX Semaphore is NULL.', '        rm_hid_trace(1, id);\n        log_e("TX Semaphore is NULL.'),
            ('        log_e("report %u mutex failed", id);', '        rm_hid_trace(2, id);\n        log_e("report %u mutex failed", id);'),
            ('        log_e("not ready");', '        rm_hid_trace(3, id);\n        log_e("not ready");'),
            ("        res = tud_hid_n_report(0, id, data, len);", "        res = tud_hid_n_report(0, id, data, len);\n        rm_hid_trace(res ? 4 : 5, id);"),
            ('                log_e("report %u wait failed", id);', '                rm_hid_trace(6, id);\n                log_e("report %u wait failed", id);'),
            ("    xSemaphoreGive(tinyusb_hid_device_input_mutex);\n    return res;",
             "    if (res) rm_hid_trace(7, id);\n    xSemaphoreGive(tinyusb_hid_device_input_mutex);\n    return res;"),
        ]
        for old, new in edits:
            if src.count(old) != 1:
                raise RuntimeError("USBHID diagnostics anchor mismatch: " + old)
            src = src.replace(old, new)
        _write(hid_cpp, src)
        print("[patches] USBHID diagnostics V1: APPLIED")


main()
