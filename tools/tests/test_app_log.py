"""Compile and exercise the real logger implementation against small host stubs."""

from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
STUBS = ROOT / "tools/tests/app_log_stubs"


class AppLogTests(unittest.TestCase):
    def test_bounded_logging_queue_and_snapshot(self):
        configured = os.environ.get("REMOTEMAPPER_HOST_CXX_JSON")
        if configured:
            compiler = json.loads(configured)
            if not isinstance(compiler, list) or not compiler or not all(isinstance(x, str) for x in compiler):
                self.fail("REMOTEMAPPER_HOST_CXX_JSON must be a non-empty JSON argv array")
        else:
            compiler_path = shutil.which("g++") or shutil.which("clang++")
            if not compiler_path:
                self.skipTest("a C++ compiler is required")
            compiler = [compiler_path]

        if not compiler:
            self.skipTest("a C++ compiler is required")

        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            logger_obj = temp / "app_log.o"
            test_obj = temp / "app_log_test.o"
            executable = temp / ("app_log_test.exe" if os.name == "nt" else "app_log_test")
            common = compiler + ["-std=c++17", "-I", str(STUBS), "-I", str(ROOT / "src")]
            subprocess.run(common + ["-DARDUINO_USB_CDC_ON_BOOT=0", "-include", str(STUBS / "free_hook.h"),
                                    "-c", str(ROOT / "src/log/app_log.cpp"), "-o", str(logger_obj)],
                           check=True, capture_output=True, text=True)
            subprocess.run(common + ["-c", str(STUBS / "app_log_test.cpp"), "-o", str(test_obj)],
                           check=True, capture_output=True, text=True)
            subprocess.run(compiler + [str(logger_obj), str(test_obj), "-o", str(executable)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
