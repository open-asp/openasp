# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Verify that Request.BinaryRead advances its per-request cursor."""

import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/openasp"
body = "A" * 102400 + "B" * 29519
expected = "102400:AAAA:A|29519:B:B|0|2:3"

for mode in ("off", "on"):
    result = subprocess.run(
        [
            str(binary),
            "render",
            "--file",
            str(root / "fixtures/pages/binary_read_cursor.asp"),
            "--root",
            str(root / "fixtures/pages"),
            "--method",
            "POST",
            "--body",
            body,
        ],
        env=dict(os.environ, EGRET_ASP_C_VM=mode),
        capture_output=True,
        text=True,
        timeout=45,
    )
    output, _, metadata = result.stdout.partition("----")
    if result.returncode != 0 or output.strip() != expected or "status=200" not in metadata:
        raise SystemExit(
            f"FAIL: Request.BinaryRead cursor, C_VM={mode}\n"
            f"expected: {expected}\nactual: {result.stdout}\n{result.stderr}"
        )
    print(f"PASS: Request.BinaryRead cursor, C_VM={mode}")
