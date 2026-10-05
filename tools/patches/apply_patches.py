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

def _read(path):
    with open(path, "r", encoding="utf-8") as f:
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
        raise RuntimeError("Required Arduino framework unavailable; cannot validate USB/OTA patches")

    # Patch 3: bounded, generation-checked HID completion. Upgrade stock and
    # TRACE_V1 reproducibly, and refresh the embedded V2 state machine on each
    # build. Validate both transformations before writing either SDK file.
    patch_dir = os.path.join(env.subst("$PROJECT_DIR"), "tools", "patches")
    sys.path.insert(0, patch_dir)
    from hid_tx_patch import patch_hid_source, patch_usb_source
    from framework_header_patches import patch_usb_header, patch_webserver_header

    edits = []
    patches = [
        ("cores/esp32/USB.h", patch_usb_header, "USB event stack"),
        ("libraries/WebServer/src/WebServer.h", patch_webserver_header, "WebServer null guards"),
        ("libraries/USB/src/USBHID.cpp", patch_hid_source, "HID completion guard V2"),
        ("cores/esp32/USB.cpp", patch_usb_source, "synchronous USB lifecycle"),
    ]
    # Validate every required adaptation before touching this shared SDK package.
    # Framework drift must fail the build, never silently omit a safety fix.
    for relative, patch, label in patches:
        path = os.path.join(fw, *relative.split("/"))
        source = _read(path)
        edits.append((path, source, patch(source), label))
    for path, source, patched, label in edits:
        if patched != source:
            _write(path, patched)
        print("[patches] %s: %s" % (label, "APPLIED/REFRESHED" if patched != source else "verified"))


main()
