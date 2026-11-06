#!/usr/bin/env python3
"""Run the focused host PMIC contract test against an applied ATF source tree."""
from pathlib import Path
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]).resolve() / "plat/nvidia/tegra/soc/t210/t214/pmic_power.c"
tests = repo / "patches/atf/tests"
with tempfile.TemporaryDirectory(prefix="lite-pmic-") as directory:
    binary = Path(directory) / "test-pmic"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(tests / "shim"),
                    f'-DPOWER_SOURCE="{source}"', str(tests / "test-pmic.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
