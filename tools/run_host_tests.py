"""Run the complete host suite; missing runtime harnesses fail by default.

Supply REMOTEMAPPER_HOST_CXX_JSON as a compiler argv array, or install g++/
clang++. An existing local Zig validation installation is also recognized.
No compiler is downloaded and no device is accessed by this runner.
"""
import argparse
import json
import io
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-runtime-skips", action="store_true",
                        help="Explicitly allow missing host compiler harnesses")
    parser.add_argument("--log", type=Path, help="Save the validation transcript")
    args = parser.parse_args()
    os.chdir(ROOT)
    if not os.environ.get("REMOTEMAPPER_HOST_CXX_JSON"):
        cache = ROOT / ".cache/luna-hardening-2026-10-05"
        compiler_path = cache / "host-compiler-path.txt"
        if compiler_path.is_file():
            compiler = Path(compiler_path.read_text(encoding="utf-8").strip())
            if compiler.is_file():
                argv = [str(compiler), "c++"]
                if os.name == "nt":
                    argv += ["-target", "x86_64-windows-gnu"]
                os.environ["REMOTEMAPPER_HOST_CXX_JSON"] = json.dumps(argv)
                os.environ.setdefault("ZIG_GLOBAL_CACHE_DIR", str(cache / "zig-global-cache"))
                os.environ.setdefault("ZIG_LOCAL_CACHE_DIR", str(cache / "zig-local-cache"))
    # Retain the four legacy algorithm models as supplemental coverage.
    legacy = subprocess.run([sys.executable, str(ROOT / "test/native/test_suite.py")],
                            capture_output=True, text=True)
    transcript = io.StringIO()
    transcript.write(legacy.stdout + legacy.stderr)
    if legacy.returncode:
        print(transcript.getvalue())
        if args.log:
            args.log.parent.mkdir(parents=True, exist_ok=True)
            args.log.write_text(transcript.getvalue(), encoding="utf-8")
        return legacy.returncode
    suite = unittest.defaultTestLoader.discover(str(ROOT / "tools/tests"), pattern="test_*.py")
    result = unittest.TextTestRunner(stream=transcript, verbosity=2).run(suite)
    print(transcript.getvalue())
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text(transcript.getvalue(), encoding="utf-8")
    if not result.wasSuccessful():
        return 1
    if result.skipped and not args.allow_runtime_skips:
        print("ERROR: runtime tests were skipped. Configure REMOTEMAPPER_HOST_CXX_JSON "
              "or install a host C++ compiler; use --allow-runtime-skips only for partial checks.",
              file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
