#!/usr/bin/env python3
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Verify that File.DateLastModified is a VBScript Date, not Unix seconds."""

import datetime
import os
from pathlib import Path
import subprocess
import sys


root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/openasp"
page = root / "fixtures/pages/fso_date_last_modified.asp"
modified = datetime.datetime.fromtimestamp(page.stat().st_mtime)
expected = f"Date|{modified.year}-{modified.month}-{modified.day} {modified.hour}:{modified.minute}:{modified.second}"

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
            f"FAIL: File.DateLastModified, C_VM={mode}\n"
            f"expected: {expected}\nactual: {result.stdout}\n{result.stderr}"
        )
    print(f"PASS: File.DateLastModified date type, C_VM={mode}")
