"""Compile and exercise production Wi-Fi persistence and request validation on the host."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_audio_pipeline_concurrency import host_compiler_argv


ROOT = Path(__file__).resolve().parents[2]
STUBS = ROOT / "tools/tests/wifi_manager_stubs"
ARDUINO_JSON = ROOT / ".pio/libdeps/esp32s3_n16r8/ArduinoJson/src"


class WifiManagerPersistenceTests(unittest.TestCase):
    def test_production_nvs_paths_and_json_request_validation(self):
        self.assertTrue(ARDUINO_JSON.is_dir(), "PlatformIO ArduinoJson headers are required")
        compiler = host_compiler_argv()
        self.assertIsNotNone(compiler, "REMOTEMAPPER_HOST_CXX_JSON or a host C++ compiler is required")

        with tempfile.TemporaryDirectory(prefix="wifi_manager_host_") as temp:
            executable = Path(temp) / ("wifi_manager_test.exe" if os.name == "nt" else "wifi_manager_test")
            command = compiler + [
                "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-return-type-c-linkage",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
                "-I", str(STUBS),
                "-I", str(ROOT / "include"),
                "-I", str(ROOT / "src/config"),
                "-I", str(ROOT / "src/wifi"),
                "-I", str(ROOT / "src/web"),
                "-I", str(ROOT / "src"),
                "-I", str(ARDUINO_JSON),
                str(ROOT / "src/wifi/wifi_manager.cpp"),
                str(STUBS / "wifi_test_stubs.cpp"),
                str(ROOT / "tools/tests/wifi_manager_host.cpp"),
                "-o", str(executable),
            ]
            env = os.environ.copy()
            result = subprocess.run(command, cwd=ROOT, env=env,
                                    capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run = subprocess.run([str(executable)], cwd=ROOT, env=env,
                                 capture_output=True, text=True, timeout=20)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            failed_begin = subprocess.run([str(executable), "begin-fail"], cwd=ROOT, env=env,
                                          capture_output=True, text=True, timeout=20)
            self.assertEqual(failed_begin.returncode, 0,
                             failed_begin.stdout + failed_begin.stderr)

if __name__ == "__main__":
    unittest.main()
