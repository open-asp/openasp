# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Verify vbscript.bnf syntax behaves identically in the managed and C VMs."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/openasp"
FIXTURES = ROOT / "fixtures/pages"
EXPECTED = (
    "5|8|16|100|25|0|0|True|True|2020|7|1|2|item3|box"
    "|9|FalseTrueFalseTrue|TrueFalseTrueFalse|10|11"
)


def render(page: Path, root: Path, mode: str) -> str:
    """Render once in the selected VM and require a clean HTTP-style result."""
    result = subprocess.run(
        [str(BINARY), "render", "--file", str(page), "--root", str(root)],
        env=dict(os.environ, EGRET_ASP_C_VM=mode),
        capture_output=True,
        text=True,
        timeout=45,
    )
    body, separator, metadata = result.stdout.partition("----")
    if (
        result.returncode != 0
        or not separator
        or "status=200" not in metadata
        or "error_number=0" not in metadata
    ):
        raise SystemExit(
            f"FAIL: BNF page execution, C_VM={mode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return body.strip()


for vm_mode in ("off", "on"):
    actual = render(FIXTURES / "vbscript_bnf_coverage.asp", FIXTURES, vm_mode)
    if actual != EXPECTED:
        raise SystemExit(
            f"FAIL: BNF syntax coverage, C_VM={vm_mode}\n"
            f"expected: {EXPECTED}\nactual: {actual}"
        )

    with_actual = render(
        FIXTURES / "with_load_from_file_function.asp", FIXTURES, vm_mode
    )
    if with_actual != "1":
        raise SystemExit(
            f"FAIL: With member-call function-name conflict, C_VM={vm_mode}\n"
            f"expected: 1\nactual: {with_actual}"
        )

    with tempfile.TemporaryDirectory(prefix="vbscript-bnf-", dir=ROOT / "build") as temp:
        temp_root = Path(temp)
        cr_page = temp_root / "cr-only.asp"
        cr_page.write_bytes(
            b"<%\rDim value\rvalue = 40 + _\r 2\r' comment\rResponse.Write CStr(value)\r%>\r"
        )
        cr_actual = render(cr_page, temp_root, vm_mode)
        if cr_actual != "42":
            raise SystemExit(
                f"FAIL: CR-only newlines and continuation, C_VM={vm_mode}\n"
                f"expected: 42\nactual: {cr_actual}"
            )

    print(f"PASS: VBScript BNF syntax coverage, C_VM={vm_mode}")
