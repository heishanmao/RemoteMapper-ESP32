"""C++ state-machine compilation and reproducible framework patch checks.

No hardware is opened. Compile-time assertions use the production guard, while
the adapter is cross-compiled against declarations of its RTOS/USB interfaces.
"""

import importlib.util
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "hid_tx_patch", ROOT / "tools/patches/hid_tx_patch.py")
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)
BASELINE = (ROOT / "tools/tests/fixtures/hid_stock_transport.inc").read_text(encoding="utf-8").rstrip() + "\n\n"
PREFIX = "// Copyright Espressif Systems; Apache-2.0 transport fixture.\n"
SUFFIX = "bool USBHID::addDevice(USBHIDDevice * device, uint16_t descriptor_len){\n}\n"
STOCK = PREFIX + BASELINE + SUFFIX
USB_STOCK = """static bool tinyusb_device_mounted = false;
static bool tinyusb_device_suspended = false;
void tud_mount_cb(void){
    tinyusb_device_mounted = true;
    arduino_usb_event_post(STARTED);
}
void tud_umount_cb(void){
    tinyusb_device_mounted = false;
    arduino_usb_event_post(STOPPED);
}
"""


def trace_v1(source):
    edits = [
        ("bool USBHID::ready(void){", PATCH.V1_MARKER + '\nextern "C" void remotemapper_hid_trace(uint8_t, uint8_t) __attribute__((weak));\n'
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
        source = source.replace(old, new)
    return source


def compiler_path():
    for name in ("g++", "clang++"):
        candidate = shutil.which(name)
        if candidate:
            return candidate
    candidate = Path(os.environ.get("USERPROFILE", "")) / (
        ".platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-g++.exe")
    return str(candidate) if candidate.is_file() else None


class FrameworkPatchTests(unittest.TestCase):
    def test_stock_and_v1_upgrade_to_same_guard(self):
        self.assertEqual(PATCH.patch_hid_source(STOCK), PATCH.patch_hid_source(trace_v1(STOCK)))

    def test_v2_patch_is_idempotent(self):
        patched = PATCH.patch_hid_source(STOCK)
        self.assertEqual(PATCH.patch_hid_source(patched), patched)

    def test_v2_refreshes_embedded_guard(self):
        patched = PATCH.patch_hid_source(STOCK)
        stale = patched.replace("MAX_REPORT_BYTES = 64", "MAX_REPORT_BYTES = 63")
        self.assertEqual(PATCH.patch_hid_source(stale), patched)

    def test_unknown_baseline_fails_closed(self):
        with self.assertRaises(RuntimeError):
            PATCH.patch_hid_source(STOCK.replace("return tud_hid_n_ready(0);", "return true;"))

    def test_v2_bad_boundary_fails_closed(self):
        patched = PATCH.patch_hid_source(STOCK)
        with self.assertRaises(RuntimeError):
            PATCH.patch_hid_source(patched.replace(PATCH.V2_END_MARKER, "// deleted"))

    def test_guard_is_embedded_without_project_include_dependency(self):
        patched = PATCH.patch_hid_source(STOCK)
        self.assertIn("struct HidTxGuard", patched)
        self.assertNotIn('#include "remotemapper_hid_tx_guard.h"', patched)
        self.assertTrue(patched.startswith(PREFIX))
        self.assertTrue(patched.endswith(SUFFIX))

    def test_lifecycle_hooks_are_synchronous_optional_and_idempotent(self):
        patched = PATCH.patch_usb_source(USB_STOCK)
        self.assertEqual(PATCH.patch_usb_source(patched), patched)
        self.assertIn('extern "C" void remotemapper_usb_lifecycle(bool) __attribute__((weak));', patched)
        self.assertIn('extern "C" void remotemapper_hid_tx_lifecycle(bool) __attribute__((weak));', patched)
        self.assertLess(patched.index("rm_usb_lifecycle(true);"), patched.index("arduino_usb_event_post(STARTED);"))
        self.assertLess(patched.index("rm_usb_lifecycle(false);"), patched.index("arduino_usb_event_post(STOPPED);"))
        self.assertNotIn("xSemaphoreTake", patched)

    def test_lifecycle_source_drift_fails_closed(self):
        with self.assertRaises(RuntimeError):
            PATCH.patch_usb_source(USB_STOCK.replace("void tud_mount_cb(void){", "void tud_mount_cb(){"))


class CppGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = compiler_path()
        if not cls.compiler:
            raise unittest.SkipTest("No installed C++ compiler; production guard compilation unavailable")

    def compile(self, source, extra=()):
        cache = ROOT / ".cache"
        cache.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="hid_guard_", dir=cache) as temporary:
            output = Path(temporary) / "guard.o"
            result = subprocess.run([
                self.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-c",
                str(source), "-o", str(output), *extra],
                cwd=ROOT, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(output.is_file())

    def test_production_state_machine_ten_compile_time_scenarios(self):
        self.compile(ROOT / "tools/tests/hid_tx_guard_compile.cpp")

    def test_generated_framework_adapter_compiles(self):
        self.compile_generated_adapter("-std=c++17")

    def test_generated_framework_adapter_compiles_arduino_gnu11(self):
        self.compile_generated_adapter("-std=gnu++11")

    def compile_generated_adapter(self, standard):
        declarations = r'''
#include <stddef.h>
#include <stdint.h>
using TickType_t = uint32_t;
using portMUX_TYPE = int;
static void* tinyusb_hid_device_input_sem = nullptr;
static void* tinyusb_hid_device_input_mutex = nullptr;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define pdTRUE 1
#define pdMS_TO_TICKS(value) (value)
#define log_e(...) ((void)0)
extern int xSemaphoreTake(void*, TickType_t);
extern void xSemaphoreGive(void*);
extern TickType_t xTaskGetTickCount();
extern bool tud_hid_n_ready(uint8_t);
extern bool tud_hid_n_report(uint8_t, uint8_t, const void*, uint16_t);
void tud_hid_report_complete_cb(uint8_t, const uint8_t*, uint16_t);
class USBHID {
public:
    bool ready();
    bool SendReport(uint8_t, const void*, size_t, uint32_t);
};
'''
        cache = ROOT / ".cache"
        cache.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="hid_adapter_", dir=cache) as temporary:
            source = Path(temporary) / "adapter.cpp"
            source.write_text(declarations + PATCH.GUARD_ADAPTER, encoding="utf-8")
            self.compile(source, extra=(standard,))


if __name__ == "__main__":
    unittest.main()
