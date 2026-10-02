# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""验证嵌套循环退出只结束所属过程，并恢复返回值和直接 ByRef 参数。"""

import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/openasp"
cases = {
    "nested_loop_routine_exit": "saved|deleted|True|end",
    "nested_loop_exit_copyback": "sub:10|for:11|selected|break:1|outer|True|end",
}

for mode in ("off", "on"):
    for name, expected in cases.items():
        result = subprocess.run(
            [str(binary), "render", "--file", str(root / "fixtures/pages" / (name + ".asp")),
             "--root", str(root / "fixtures/pages")],
            env=dict(os.environ, EGRET_ASP_C_VM=mode),
            capture_output=True, text=True, timeout=45)
        body, _, metadata = result.stdout.partition("----")
        if result.returncode != 0 or body.strip() != expected or "error_number=0" not in metadata or "status=200" not in metadata:
            raise SystemExit(
                f"失败：{name}，C_VM={mode}\n预期：{expected}\n实际：{result.stdout}\n{result.stderr}")
        print(f"通过：{name}，C_VM={mode}")
