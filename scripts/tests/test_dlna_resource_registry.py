#!/usr/bin/env python3
"""Compile and run the DLNA resource registry ownership tests on the host."""

import os
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
DLNA_DIR = ROOT / "main" / "audio" / "dlna"
STUB_DIR = DLNA_DIR / "tests" / "stubs"
TEST_SOURCE = DLNA_DIR / "tests" / "test_resource_registry.cc"
REGISTRY_SOURCE = DLNA_DIR / "dlna_resource_registry.cc"


def main() -> int:
    compiler = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory(prefix="echoear-dlna-test-") as temp:
        executable = pathlib.Path(temp) / "test_resource_registry"
        command = [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(STUB_DIR),
            "-I",
            str(DLNA_DIR),
            str(TEST_SOURCE),
            str(REGISTRY_SOURCE),
            "-o",
            str(executable),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)
    print("DLNA resource registry host tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
