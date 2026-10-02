#!/usr/bin/env python3
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Verify VBScript Empty comparison coercion in both VM modes."""

import os
from pathlib import Path
import subprocess
import sys


root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/openasp"
page = root / "fixtures/pages/empty_comparison.asp"
expected = "True|True|True|True|True|False|False|True|not-verified"

for mode in ("off", "on"):
    result = subprocess.run(
        [str(binary), "render", "--file", str(page), "--root", str(page.parent)],
        env=dict(os.environ, EGRET_ASP_C_VM=mode),
        capture_output=True,
        text=True,
        timeout=45,
    )
    body, _, metadata = result.stdout.partition("----")
    if result.returncode != 0 or body.strip() != expected or "status=200" not in metadata:
        raise SystemExit(
            f"失败：Empty 比较语义，C_VM={mode}\n"
            f"预期：{expected}\n实际：{result.stdout}\n{result.stderr}"
        )
    print(f"通过：Empty 比较语义，C_VM={mode}")
