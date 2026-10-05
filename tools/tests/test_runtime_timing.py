"""Execute the production loop-stage accounting helper, including clock wrap."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_audio_pipeline_concurrency import ROOT, host_compiler_argv


class RuntimeTimingTests(unittest.TestCase):
    def test_stage_isolation_budget_and_clock_wrap(self):
        compiler = host_compiler_argv()
        if not compiler:
            self.skipTest("No host C++ compiler configured")
        with tempfile.TemporaryDirectory(prefix="runtime_timing_") as directory:
            executable = Path(directory) / ("timing.exe" if os.name == "nt" else "timing")
            result = subprocess.run(compiler + ["-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src"), str(ROOT / "tools/tests/runtime_timing_host.cpp"),
                "-o", str(executable)], cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
