"""Run the production key engine through the production voice gate policy."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class VoiceActionGateTests(unittest.TestCase):
    def test_readiness_and_key_action_lifecycle(self):
        configured = os.environ.get("REMOTEMAPPER_HOST_CXX_JSON")
        compiler = json.loads(configured) if configured else [
            shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
        ]
        self.assertTrue(compiler and compiler[0], "No host C++ compiler available")
        stubs = ROOT / "tools/tests/keymap_stability_stubs"
        audio_stubs = ROOT / "tools/tests/audio_host_stubs"
        with tempfile.TemporaryDirectory(prefix="voice_action_gate_") as temporary:
            exe = Path(temporary) / ("voice_action_gate.exe" if os.name == "nt" else "voice_action_gate")
            compile_result = subprocess.run([
                *compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-missing-field-initializers",
                "-I", str(audio_stubs), "-I", str(stubs),
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src"),
                "-I", str(ROOT / "src/audio"),
                str(ROOT / "tools/tests/voice_action_gate_host.cpp"),
                "-x", "c++", str(ROOT / "src/keymap/key_state_machine.c"),
                "-o", str(exe),
            ], cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout + compile_result.stderr)
            run_result = subprocess.run([str(exe)], cwd=ROOT,
                                        capture_output=True, text=True, timeout=15)
            self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)


if __name__ == "__main__":
    unittest.main()
