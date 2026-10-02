# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Verify Response.Cookies assignments in both VMs and cache states."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/openasp"
expected = "value:0|path:0"

for mode in ("off", "on"):
    with tempfile.TemporaryDirectory(prefix="response-cookie-", dir=root / "build") as state:
        for cache in ("cold", "warm"):
            result = subprocess.run(
                [
                    str(binary),
                    "render",
                    "--file",
                    str(root / "fixtures/pages/response_cookie_assignment.asp"),
                    "--root",
                    str(root / "fixtures/pages"),
                ],
                env=dict(os.environ, EGRET_ASP_C_VM=mode, EGRET_ASP_STATE_DIR=state),
                capture_output=True,
                text=True,
                timeout=15,
            )
            body, _, metadata = result.stdout.partition("----")
            if (
                result.returncode != 0
                or body.strip() != expected
                or "error_number=0" not in metadata
                or "status=200" not in metadata
            ):
                raise SystemExit(
                    f"FAIL: Response.Cookies, C_VM={mode}, cache={cache}\n"
                    f"expected: {expected}\nactual: {result.stdout}\n{result.stderr}"
                )
            print(f"PASS: Response.Cookies, C_VM={mode}, cache={cache}")
