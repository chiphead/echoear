#!/usr/bin/env python3
"""Compile and run the DLNA HTTP Range parser tests on the host."""

import os
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
DLNA_DIR = ROOT / "main" / "audio" / "dlna"
TEST_SOURCE = DLNA_DIR / "tests" / "test_http_range.cc"


def main() -> int:
    compiler = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory(prefix="echoear-dlna-range-test-") as temp:
        executable = pathlib.Path(temp) / "test_http_range"
        command = [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-I",
            str(DLNA_DIR),
            str(TEST_SOURCE),
            "-o",
            str(executable),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)
    print("DLNA HTTP Range host tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
