# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""End-to-end checks for ASP optimization, semantics, and cache isolation."""

from __future__ import annotations

import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
OPENASP = BUILD / "openasp"

OPTIMIZATION_PAGE = """<%
Const SCALE = 4

Function InlineAdd(ByVal value)
    InlineAdd = value + SCALE
End Function

Dim i, total, dynamicValue
total = 0

If (2 + 3) = 5 Then
    total = total + 0
Else
    total = 100000
End If

If False Then
    total = total + 200000
End If

While False
    total = total + 400000
Wend

For i = 1 To 3
    total = total + InlineAdd(i)
    total = total + ((i + 1) * (i + 1))
Next

total = total + (2 ^ 3)
dynamicValue = 1
Execute "dynamicValue = dynamicValue + 1"
Response.Write total & "|" & dynamicValue & "|" & Eval("20 + 22") & "|x" & Nothing & "y"
%>
"""

ERROR_PAGE = """<%
On Error Resume Next
Dim value
value = 1 / 0
Response.Write "err=" & Err.Number
%>
"""

EXPECTED_OUTPUT = "55|2|42|xy\n"


def run(
    argv: list[str],
    *,
    env: dict[str, str] | None = None,
    expected_returncode: int = 0,
) -> subprocess.CompletedProcess[str]:
    """Run one command and retain both streams in assertion diagnostics."""
    result = subprocess.run(
        argv,
        capture_output=True,
        text=True,
        timeout=45,
        env=env,
    )
    assert result.returncode == expected_returncode, (
        f"unexpected return code for {' '.join(argv)}: "
        f"{result.returncode} != {expected_returncode}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    return result


def command_env(state_dir: Path, c_vm: str) -> dict[str, str]:
    """Create an isolated process environment for one VM/cache combination."""
    return {
        **os.environ,
        "EGRET_ASP_C_VM": c_vm,
        "EGRET_ASP_STATE_DIR": str(state_dir),
    }


def compile_page(page: Path, optimize: str | None, state_dir: Path) -> tuple[int, str]:
    """Compile a page and return its instruction count and metadata output."""
    argv = [
        str(OPENASP),
        "compile",
        "--file",
        str(page),
        "--root",
        str(page.parent),
        "--log-file",
        "off",
    ]
    if optimize is not None:
        argv.append(f"--optimize={optimize}")
    result = run(argv, env=command_env(state_dir, "off"))
    match = re.search(r"^instruction_count=(\d+)$", result.stdout, re.MULTILINE)
    assert match is not None, result.stdout
    return int(match.group(1)), result.stdout


def render_page(page: Path, optimize: str, state_dir: Path, c_vm: str) -> str:
    """Render through the selected optimizer and VM modes."""
    result = run(
        [
            str(OPENASP),
            str(page),
            "--root",
            str(page.parent),
            f"--optimize={optimize}",
            "--log-file",
            "off",
        ],
        env=command_env(state_dir, c_vm),
    )
    return result.stdout


def cache_optimizer_mode(state_dir: Path) -> int:
    """Read the optimizer discriminator from the v13 page-cache payload."""
    cache_files = list(state_dir.rglob("*.cache"))
    assert len(cache_files) == 1, cache_files
    data = cache_files[0].read_bytes()
    assert data[:8] == b"EGBPC001" and len(data) >= 24, cache_files[0]
    assert struct.unpack_from("<I", data, 8)[0] == 13
    return struct.unpack_from("<I", data, 20)[0]


def check_command_contract(page: Path, state_dir: Path) -> None:
    """Verify default-on behavior, help text, and invalid-value rejection."""
    for binary in ("openasp", "openasp-cli", "openasp-fpm"):
        help_result = run([str(BUILD / binary), "--help"])
        assert "--optimize=on|off (default: on)" in help_result.stdout, (
            binary,
            help_result.stdout,
        )

    default_count, default_metadata = compile_page(page, None, state_dir)
    on_count, on_metadata = compile_page(page, "on", state_dir)
    off_count, off_metadata = compile_page(page, "off", state_dir)
    assert "optimize=on" in default_metadata
    assert "optimize=on" in on_metadata
    assert "optimize=off" in off_metadata
    assert default_count == on_count
    assert on_count < off_count, (on_count, off_count)

    for binary in ("openasp", "openasp-cli", "openasp-fpm"):
        argv = [str(BUILD / binary), "--optimize=invalid"]
        if binary == "openasp":
            argv.insert(1, str(page))
        result = run(
            argv,
            expected_returncode=2,
        )
        assert "invalid --optimize value; expected on or off" in result.stdout


def check_semantics(page: Path, error_page: Path, state_dir: Path) -> None:
    """Compare optimized/unoptimized execution on both VM implementations."""
    for c_vm in ("off", "on"):
        baseline = render_page(page, "off", state_dir / f"semantic-{c_vm}", c_vm)
        optimized = render_page(page, "on", state_dir / f"semantic-{c_vm}", c_vm)
        assert baseline == EXPECTED_OUTPUT, (c_vm, baseline)
        assert optimized == baseline, (c_vm, optimized, baseline)

        error_baseline = render_page(
            error_page,
            "off",
            state_dir / f"error-{c_vm}",
            c_vm,
        )
        error_optimized = render_page(
            error_page,
            "on",
            state_dir / f"error-{c_vm}",
            c_vm,
        )
        assert error_optimized == error_baseline, (
            c_vm,
            error_optimized,
            error_baseline,
        )
        assert re.fullmatch(r"err=-?\d+\n", error_optimized)
        assert error_optimized != "err=0\n"


def check_cache_isolation(page: Path, state_dir: Path) -> None:
    """Ensure alternating optimizer modes cannot reuse incompatible bytecode."""
    cache_state = state_dir / "shared-cache"
    assert render_page(page, "on", cache_state, "on") == EXPECTED_OUTPUT
    assert cache_optimizer_mode(cache_state) == 1
    assert render_page(page, "off", cache_state, "on") == EXPECTED_OUTPUT
    assert cache_optimizer_mode(cache_state) == 0
    assert render_page(page, "on", cache_state, "on") == EXPECTED_OUTPUT
    assert cache_optimizer_mode(cache_state) == 1


def main() -> None:
    """Run compiler-shape checks followed by runtime equivalence checks."""
    with tempfile.TemporaryDirectory(prefix="openasp-optimizer-") as temp_dir:
        root = Path(temp_dir)
        page = root / "optimizer.asp"
        error_page = root / "optimizer-error.asp"
        page.write_text(OPTIMIZATION_PAGE, encoding="ascii")
        error_page.write_text(ERROR_PAGE, encoding="ascii")
        state_dir = root / "state"

        check_command_contract(page, state_dir / "contract")
        check_semantics(page, error_page, state_dir)
        check_cache_isolation(page, state_dir)

    print("PASS: ASP optimization, semantic equivalence, errors, and cache isolation")


if __name__ == "__main__":
    main()
