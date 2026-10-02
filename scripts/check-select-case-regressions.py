# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""验证多值 Case、闰年、嵌套分支及匹配表达式只求值到命中项。"""

import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/openasp"
expected = "31,28,31,30,31,30,31,31,30,31,30,31,|29,28,29|small:3|three:4|other:4|done"

for mode in ("off", "on"):
    result = subprocess.run(
        [str(binary), "render", "--file", str(root / "fixtures/pages/select_case_multi_value.asp"),
         "--root", str(root / "fixtures/pages")],
        env=dict(os.environ, EGRET_ASP_C_VM=mode),
        capture_output=True, text=True, timeout=45)
    body, _, metadata = result.stdout.partition("----")
    if result.returncode != 0 or body.strip() != expected or "status=200" not in metadata:
        raise SystemExit(
            f"失败：Select Case，C_VM={mode}\n预期：{expected}\n实际：{result.stdout}\n{result.stderr}")
    print(f"通过：Select Case，C_VM={mode}")
