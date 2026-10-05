"""Compile and run the production MIC session-owner interleaving harness."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def host_compiler_argv():
    configured = os.environ.get("REMOTEMAPPER_HOST_CXX_JSON")
    if configured:
        argv = json.loads(configured)
        if not isinstance(argv, list) or not argv or not all(isinstance(x, str) for x in argv):
            raise ValueError("REMOTEMAPPER_HOST_CXX_JSON must be a non-empty JSON string array")
        return argv
    compiler = shutil.which("g++") or shutil.which("clang++") or shutil.which("c++")
    return [compiler] if compiler else None


class MicSessionOwnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = host_compiler_argv()
        if not cls.compiler:
            raise unittest.SkipTest("No host C++ compiler configured for MIC session tests")

    def test_new_open_supersedes_close_and_enqueue_failure_is_retryable(self):
        source = ROOT / "tools/tests/mic_session_owner_host.cpp"
        with tempfile.TemporaryDirectory(prefix="mic_session_owner_host_") as temporary:
            executable = Path(temporary) / (
                "mic_session_owner_test.exe" if os.name == "nt" else "mic_session_owner_test")
            compile_result = subprocess.run(
                self.compiler + [
                    "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "include"), str(source), "-o", str(executable),
                ],
                cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout + compile_result.stderr)
            run_result = subprocess.run([str(executable)], cwd=ROOT,
                                        capture_output=True, text=True, timeout=15)
            self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)


if __name__ == "__main__":
    unittest.main()
