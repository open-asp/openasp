# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Verify native date parsing, boundaries, trailing data, and full timestamps."""
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/openasp"
expected = (
    "True:2026-9-1|True:2026-9-1|True:2026-9-1|True:2026-9-1|True:2024-2-1|True:2026-12-1|"
    "False|False|False|False|False|False|False|True|12:34:56|2026-9-28"
)
for mode in ("off", "on"):
    result = subprocess.run(
        [str(binary), "render", "--file", str(root / "fixtures/pages/date_year_month.asp"),
         "--root", str(root / "fixtures/pages")],
        env=dict(os.environ, EGRET_ASP_C_VM=mode),
        capture_output=True, text=True, timeout=45)
    body, _, metadata = result.stdout.partition("----")
    if result.returncode != 0 or body.strip() != expected or "status=200" not in metadata:
        raise SystemExit(
            f"FAIL: date parsing, C_VM={mode}\n"
            f"expected: {expected}\nactual: {result.stdout}\n{result.stderr}")
    print(f"PASS: date parsing, C_VM={mode}")
