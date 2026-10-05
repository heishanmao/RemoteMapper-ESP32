"""Host regression coverage for keymap parse, save, and live swap boundaries."""
import os
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class KeymapStabilityTests(unittest.TestCase):
    def test_candidate_validation_persistence_failure_and_feed_interleaving(self):
        configured = os.environ.get("REMOTEMAPPER_HOST_CXX_JSON")
        compiler = json.loads(configured) if configured else [
            shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        ]
        self.assertTrue(compiler and compiler[0], "No host C++ compiler available")
        arduino_json = ROOT / ".pio/libdeps/esp32s3_n16r8/ArduinoJson/src"
        self.assertTrue(arduino_json.exists(), "PlatformIO ArduinoJson headers are not present")
        stubs = ROOT / "tools/tests/keymap_stability_stubs"
        with tempfile.TemporaryDirectory(prefix="keymap_stability_") as temp:
            exe = Path(temp) / ("keymap_stability.exe" if os.name == "nt" else "keymap_stability")
            compile_result = subprocess.run([
                *compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-Wno-missing-field-initializers", "-DARDUINO=1", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
                "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
                "-DARDUINOJSON_ENABLE_PROGMEM=0",
                "-I", str(stubs), "-I", str(arduino_json),
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src"), "-I", str(ROOT / "src/keymap"),
                "-I", str(ROOT / "src/config"),
                str(ROOT / "tools/tests/keymap_stability_host.cpp"),
                "-x", "c++", str(ROOT / "src/keymap/key_state_machine.c"),
                str(ROOT / "src/keymap/key_config_storage.cpp"),
                str(ROOT / "src/config/config_backup.cpp"), "-o", str(exe),
            ], cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout + compile_result.stderr)
            run_result = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=15)
            self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)


if __name__ == "__main__":
    unittest.main()
