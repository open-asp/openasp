# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Reject source imports and generated adapters that depend on host paths."""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SKIP_PARTS = {".run", "build", "logs", "release", ".git", "__pycache__"}
SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".eg", ".h", ".hpp", ".mk", ".py", ".sh",
}
SOURCE_NAMES = {"Makefile", "Dockerfile", "Dockerfile.linux-x64", "make-release.sh"}
ABSOLUTE_INCLUDE = re.compile(
    r"""^\s*(?:\#\s*include|include)\s*[<"](?:/|[A-Za-z]:[\\/])""",
    re.MULTILINE,
)
MACOS_HOME_ROOT = "/" + "Users" + "/"
LINUX_HOME_ROOT = "/" + "home" + "/"
WINDOWS_HOME_ROOT = "Users"
PERSONAL_HOME = re.compile(
    rf"""(?:{re.escape(MACOS_HOME_ROOT)}[^/"'\s]+/|"""
    rf"""{re.escape(LINUX_HOME_ROOT)}[^/"'\s]+/|"""
    rf"""[A-Za-z]:[\\/]{WINDOWS_HOME_ROOT}[\\/][^\\/"'\s]+[\\/])"""
)


def source_files():
    """Yield maintained source files while excluding generated output trees."""
    for path in ROOT.rglob("*"):
        if not path.is_file() or any(part in SKIP_PARTS for part in path.parts):
            continue
        if path.suffix.lower() in SOURCE_SUFFIXES or path.name in SOURCE_NAMES:
            yield path


def main() -> None:
    """Report every non-portable source import with an actionable location."""
    violations: list[str] = []
    for path in source_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern, label in (
            (ABSOLUTE_INCLUDE, "absolute include"),
            (PERSONAL_HOME, "personal home path"),
        ):
            for match in pattern.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                violations.append(f"{path.relative_to(ROOT)}:{line}: {label}: {match.group(0)}")

    generated_adapter = ROOT / "build/cold-compiler/codegen_opt.c"
    if generated_adapter.is_file():
        text = generated_adapter.read_text(encoding="utf-8", errors="replace")
        for match in ABSOLUTE_INCLUDE.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            violations.append(
                f"{generated_adapter.relative_to(ROOT)}:{line}: absolute include: {match.group(0)}"
            )

    if violations:
        raise SystemExit(
            "禁止在源码或生成适配器中引入宿主绝对路径：\n" + "\n".join(violations)
        )
    print("通过：源码和生成适配器未使用宿主绝对路径引入")


if __name__ == "__main__":
    main()
