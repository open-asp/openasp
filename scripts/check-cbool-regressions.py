# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""验证布尔转换、失败赋值恢复及原生过程返回；二进制路径可由第一个参数指定。"""

import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/openasp"
cases = {
    "cbool_conversion": "checks=46;failures=0",
    "native_routine_end_branch": "95|96|caller-resumed",
    "resume_next_failed_assignment": (
        "global:13|array:13|dictionary:13|field:13|local:global:94|session:13|13"
    ),
}

for mode in ("off", "on"):
    for name, expected in cases.items():
        result = subprocess.run(
            [str(binary), "render", "--file", str(root / "fixtures/pages" / (name + ".asp")),
             "--root", str(root / "fixtures/pages")],
            env=dict(os.environ, EGRET_ASP_C_VM=mode), capture_output=True, text=True, timeout=45)
        body, _, metadata = result.stdout.partition("----")
        if result.returncode != 0 or body.strip() != expected or "status=200" not in metadata:
            raise SystemExit(
                f"失败：{name}，C_VM={mode}\n预期：{expected}\n实际：{result.stdout}\n{result.stderr}"
            )
        if name == "resume_next_failed_assignment" and "content_type=text/plain" not in metadata:
            raise SystemExit("失败：转换异常覆盖了 Response.ContentType")
        print(f"通过：{name}，C_VM={mode}")
