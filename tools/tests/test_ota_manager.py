"""Compile and exercise the production OTA manager against host mocks."""

from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
STUBS = ROOT / "tools/tests/ota_manager_stubs"


class OtaManagerTests(unittest.TestCase):
    def run_checked(self, command, env):
        result = subprocess.run(command, capture_output=True, text=True, env=env)
        self.assertEqual(result.returncode, 0,
                         "command failed:\n" + " ".join(command) +
                         "\nstdout:\n" + result.stdout + "\nstderr:\n" + result.stderr)

    def test_sentinel_activation_and_confirmation_failures(self):
        configured = os.environ.get("REMOTEMAPPER_HOST_CXX_JSON")
        if configured:
            compiler = json.loads(configured)
            if not isinstance(compiler, list) or not compiler or not all(isinstance(x, str) for x in compiler):
                self.fail("REMOTEMAPPER_HOST_CXX_JSON must be a non-empty JSON argv array")
        else:
            compiler_path = shutil.which("g++") or shutil.which("clang++")
            if compiler_path:
                compiler = [compiler_path]
            else:
                zig = ROOT / ".cache/luna-hardening-2026-10-05/host-compiler/zig-x86_64-windows-0.17.0/zig.exe"
                if not zig.is_file():
                    self.skipTest("a C++ compiler is required")
                compiler = [str(zig), "c++"]

        cache = ROOT / ".cache"
        cache.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="ota_manager_", dir=cache) as temp_dir:
            temp = Path(temp_dir)
            logger_obj = temp / "ota_manager.o"
            test_obj = temp / "ota_manager_test.o"
            executable = temp / ("ota_manager_test.exe" if os.name == "nt" else "ota_manager_test")
            common = compiler + ["-std=c++17", "-DREMOTEMAPPER_OTA=1", "-I", str(STUBS), "-I", str(ROOT / "src")]
            env = os.environ.copy()
            if Path(compiler[0]).name.lower() == "zig.exe":
                env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(cache / "zig-global"))
            self.run_checked(common + ["-c", str(ROOT / "src/ota/ota_manager.cpp"), "-o", str(logger_obj)], env)
            self.run_checked(common + ["-c", str(STUBS / "ota_manager_test.cpp"), "-o", str(test_obj)], env)
            self.run_checked(compiler + [str(logger_obj), str(test_obj), "-o", str(executable)], env)
            result = subprocess.run([str(executable)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
