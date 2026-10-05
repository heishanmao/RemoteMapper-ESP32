"""Executable host tests for the production HID queue/epoch state machine."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tools/tests/hid_command_queue_runtime.cpp"


def compiler_argv():
    configured = os.environ.get("REMOTEMAPPER_HOST_CXX_JSON")
    if configured:
        parsed = json.loads(configured)
        if isinstance(parsed, list) and parsed and all(isinstance(item, str) for item in parsed):
            return parsed
        raise ValueError("REMOTEMAPPER_HOST_CXX_JSON must be a non-empty JSON argv array")
    for name in ("g++", "clang++", "c++"):
        path = shutil.which(name)
        if path:
            return [path]
    return None


class HidCommandQueueRuntimeTests(unittest.TestCase):
    def test_real_queue_state_machine_scenarios(self):
        compiler = compiler_argv()
        if not compiler:
            self.skipTest("No host C++ compiler configured; set REMOTEMAPPER_HOST_CXX_JSON")
        cache = ROOT / ".cache"
        cache.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="hid_queue_", dir=cache) as temp:
            executable = Path(temp) / "hid_queue_test.exe"
            result = subprocess.run(
                [*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(HARNESS), "-o", str(executable)],
                cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
