# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""验证 vbscript.bnf 补齐语法在 managed VM 与 C VM 下行为一致。"""

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
            f"失败：BNF 页面执行，C_VM={mode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return body.strip()


for vm_mode in ("off", "on"):
    actual = render(FIXTURES / "vbscript_bnf_coverage.asp", FIXTURES, vm_mode)
    if actual != EXPECTED:
        raise SystemExit(
            f"失败：BNF 语法覆盖，C_VM={vm_mode}\n预期：{EXPECTED}\n实际：{actual}"
        )

    with_actual = render(
        FIXTURES / "with_load_from_file_function.asp", FIXTURES, vm_mode
    )
    if with_actual != "1":
        raise SystemExit(
            f"失败：With 成员调用的函数名冲突，C_VM={vm_mode}\n"
            f"预期：1\n实际：{with_actual}"
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
                f"失败：CR-only 换行及续行，C_VM={vm_mode}\n"
                f"预期：42\n实际：{cr_actual}"
            )

    print(f"通过：VBScript BNF 语法覆盖，C_VM={vm_mode}")
