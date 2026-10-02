# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""验证 Response.Cookies 成员赋值在原生 VM 和缓存冷热路径下不产生错误。"""

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
                    f"失败：Response.Cookies，C_VM={mode}，缓存={cache}\n"
                    f"预期：{expected}\n实际：{result.stdout}\n{result.stderr}"
                )
            print(f"通过：Response.Cookies，C_VM={mode}，缓存={cache}")
