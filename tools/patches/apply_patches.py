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


main()