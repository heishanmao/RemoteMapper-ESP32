"""Compile-time behavior checks for production BLE audio diagnostic math."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def compiler_path():
    for name in ("g++", "clang++"):
        candidate = shutil.which(name)
        if candidate:
            return candidate
    candidate = Path(os.environ.get("USERPROFILE", "")) / (
        ".platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-g++.exe")
    return str(candidate) if candidate.is_file() else None


class BleAudioDiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = compiler_path()
        if not cls.compiler:
            raise unittest.SkipTest("No C++ compiler is installed for production helper assertions")

    def test_production_timing_helpers_compile_with_behavior_assertions(self):
        source = ROOT / "tools/tests/ble_audio_diagnostics_compile.cpp"
        with tempfile.TemporaryDirectory(prefix="ble_audio_diag_") as temporary:
            output = Path(temporary) / "ble_audio_diag.o"
            result = subprocess.run([
                self.compiler, "-std=gnu++11", "-Wall", "-Wextra", "-Werror", "-c",
                str(source), "-o", str(output), "-I", str(ROOT / "src/ble")],
                cwd=ROOT, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(output.is_file())


if __name__ == "__main__":
    unittest.main()
