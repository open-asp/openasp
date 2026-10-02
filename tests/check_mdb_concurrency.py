# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Multi-process, multi-thread, and lock tests for native MDB access."""

from __future__ import annotations

import csv
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def run(*args: str, check: bool = True, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    """Run one native test action with isolated, captured diagnostics."""
    return subprocess.run(args, check=check, capture_output=True, text=True, env=env)


def wait_all(processes: list[subprocess.Popen[str]], label: str) -> None:
    """Join a workload and retain each worker's output in assertion failures."""
    for process in processes:
        stdout, stderr = process.communicate(timeout=120)
        if process.returncode != 0:
            raise AssertionError(f"{label} failed ({process.returncode}):\n{stdout}\n{stderr}")


def export_rows(database: Path) -> list[list[str]]:
    """Read rows through an independent MDBTools process for external validation."""
    output = run("mdb-export", str(database), "concurrent_rows").stdout
    return list(csv.reader(io.StringIO(output)))


def main() -> int:
    """Verify process/thread/fork locking, AutoNumber uniqueness, and recovery."""
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <concurrency-test-binary> <jet4-fixture>", file=sys.stderr)
        return 2
    binary = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    if not binary.is_file() or not fixture.is_file():
        print("test binary and Jet 4 fixture must exist", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="egret-mdb-concurrency-") as directory:
        database = Path(directory) / "concurrent.mdb"
        shutil.copyfile(fixture, database)
        run(str(binary), "init", str(database))

        processes: list[subprocess.Popen[str]] = []
        for _reader in range(2):
            processes.append(
                subprocess.Popen(
                    (str(binary), "read", str(database), "30"),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
            )
        for writer in range(4):
            processes.append(
                subprocess.Popen(
                    (str(binary), "write", str(database), str(writer), "20"),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
            )
        wait_all(processes, "multi-process MDB workload")

        run(str(binary), "threads", str(database), "4", "10")
        run(str(binary), "fork", str(database), "3", "5")
        rows = export_rows(database)
        assert len(rows) == 136
        ids = [int(row[0]) for row in rows[1:]]
        assert ids == list(range(1, 136))
        pairs = {(int(row[1]), int(row[2])) for row in rows[1:]}
        expected = {(writer, sequence) for writer in range(4) for sequence in range(20)}
        expected.update((1000 + writer, sequence) for writer in range(4) for sequence in range(10))
        expected.update((2000 + writer, sequence) for writer in range(3) for sequence in range(5))
        assert pairs == expected

        holder = subprocess.Popen((str(binary), "hold-write", str(database), "700"))
        time.sleep(0.1)
        started = time.monotonic()
        run(str(binary), "read", str(database), "1")
        blocked_for = time.monotonic() - started
        assert holder.wait(timeout=5) == 0
        assert blocked_for >= 0.5

        holder = subprocess.Popen((str(binary), "hold-read", str(database), "700"))
        time.sleep(0.1)
        started = time.monotonic()
        run(str(binary), "read", str(database), "1")
        shared_elapsed = time.monotonic() - started
        assert shared_elapsed < 0.5
        assert holder.wait(timeout=5) == 0

    print("native MDB concurrency tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
