# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""验证对象条件与只读属性 ByRef 语义在冷、热字节码中一致。"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build/openasp"
expected = "if:0:91|loop:0:91|branches:0:91|post:1:91|empty:kept:424"
for mode in ("off", "on"):
    with tempfile.TemporaryDirectory(prefix="object-condition-", dir=root / "build") as state:
        for cache in ("cold", "warm"):
            run = subprocess.run(
                [str(binary), "render", "--file",
                 str(root / "fixtures/pages/resume_next_missing_object_condition.asp"),
                 "--root", str(root / "fixtures/pages")],
                env=dict(os.environ, EGRET_ASP_C_VM=mode, EGRET_ASP_STATE_DIR=state),
                capture_output=True, text=True, timeout=15)
            body, _, metadata = run.stdout.partition("----")
            if run.returncode != 0 or body.strip() != expected or "status=200" not in metadata:
                raise SystemExit(f"失败：C_VM={mode}，缓存={cache}\n{run.stdout}\n{run.stderr}")
            print(f"通过：缺失对象条件，C_VM={mode}，缓存={cache}")
            readonly = subprocess.run(
                [str(binary), "render", "--file",
                 str(root / "fixtures/pages/class_readonly_property_byref.asp"),
                 "--root", str(root / "fixtures/pages")],
                env=dict(os.environ, EGRET_ASP_C_VM=mode, EGRET_ASP_STATE_DIR=state),
                capture_output=True, text=True, timeout=15)
            readonly_body, _, readonly_metadata = readonly.stdout.partition("----")
            if readonly.returncode != 0 or readonly_body.strip() != "stable|0|stable" or "status=200" not in readonly_metadata:
                raise SystemExit(
                    f"失败：只读属性 ByRef，C_VM={mode}，缓存={cache}\n"
                    f"{readonly.stdout}\n{readonly.stderr}"
                )
            print(f"通过：只读属性 ByRef，C_VM={mode}，缓存={cache}")
