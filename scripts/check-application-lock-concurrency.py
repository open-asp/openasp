#!/usr/bin/env python3
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Verify that Application.Lock serializes independent OpenASP processes."""

from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/openasp"
FIXTURES = ROOT / "fixtures/pages"
PAGE = FIXTURES / "application_lock_increment.asp"
WORKERS = 12


def render(environment: dict[str, str], query: str = "") -> str:
    command = [
        str(BINARY), "render", "--file", str(PAGE), "--root", str(FIXTURES),
    ]
    if query:
        command.extend(["--query", query])
    result = subprocess.run(
        command,
        env=environment,
        capture_output=True,
        text=True,
        timeout=120,
    )
    body, separator, metadata = result.stdout.partition("----")
    if (
        result.returncode != 0
        or not separator
        or "status=200" not in metadata
        or "error_number=0" not in metadata
    ):
        raise RuntimeError(
            f"Application.Lock request failed\nstdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    return body.strip()


with tempfile.TemporaryDirectory(prefix="openasp-application-lock-") as state:
    environment = dict(os.environ, EGRET_ASP_STATE_DIR=state)
    render(environment)
    for application_file in Path(state).glob("*/application.kv"):
        application_file.unlink()

    with ThreadPoolExecutor(max_workers=WORKERS) as executor:
        list(executor.map(lambda _: render(environment), range(WORKERS)))

    actual = render(environment, "read=1")
    if actual != str(WORKERS):
        raise SystemExit(
            f"FAIL: Application.Lock cross-process count\n"
            f"expected: {WORKERS}\nactual: {actual}"
        )

print(f"PASS: Application.Lock cross-process exclusion ({WORKERS} processes)")
