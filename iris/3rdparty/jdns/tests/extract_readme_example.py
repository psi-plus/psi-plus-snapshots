#!/usr/bin/env python3
"""Extract the README's executable Qt quick start for installed-package CI.

The code is compiled but not run: it intentionally contacts a public DoH
resolver when executed by an application.
"""
from pathlib import Path
import re
import sys

readme = (Path(__file__).resolve().parents[1] / "README.md").read_text(encoding="utf-8")
blocks = re.findall(r"(?ms)^```(\w+)\n(.*?)^```\s*$", readme)

def unique(lang, marker):
    candidates = [body for mode, body in blocks if mode == lang and marker in body]
    if len(candidates) != 1:
        raise SystemExit(f"Expected exactly one {lang} README example with {marker!r}")
    return candidates[0]

target = Path(sys.argv[1])
target.mkdir(parents=True, exist_ok=True)
(target / "main.cpp").write_text(unique("cpp", "// main.cpp"), encoding="utf-8")
(target / "CMakeLists.txt").write_text(unique("cmake", "add_executable(dns_example"), encoding="utf-8")
print("Extracted README source and CMake configuration for compile-time validation.")
