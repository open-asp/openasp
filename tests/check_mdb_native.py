# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Regression tests for the native Jet 4 CREATE/INSERT/UPDATE/DELETE path."""

from __future__ import annotations

import csv
import hashlib
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

PAGE_SIZE = 4096
INDEX_DATA_OFFSET = 480
INDEX_MASK_OFFSET = 27
INDEX_MASK_SIZE = 453


def run(*args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    """Run one fixture tool with captured text output for deterministic assertions."""
    return subprocess.run(args, check=check, capture_output=True, text=True)


def digest(path: Path) -> str:
    """Hash the complete database to prove rejected DDL leaves no partial writes."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def index_entries(page: bytes) -> list[bytes]:
    """Decode Jet's bit-mask-delimited index entries from one 4 KiB page."""
    entries: list[bytes] = []
    previous = 0
    for boundary in range(1, INDEX_MASK_SIZE * 8):
        byte = page[INDEX_MASK_OFFSET + boundary // 8]
        if byte & (1 << (boundary % 8)):
            entries.append(page[INDEX_DATA_OFFSET + previous : INDEX_DATA_OFFSET + boundary])
            previous = boundary
    return entries


def verify_native_index(database: Path, root_page: int) -> None:
    """Validate root/leaf links, entry shape, sort order, and deleted keys."""
    data = database.read_bytes()

    def page(number: int) -> bytes:
        start = number * PAGE_SIZE
        return data[start : start + PAGE_SIZE]

    root = page(root_page)
    assert root[0] == 3
    root_entries = index_entries(root)
    assert root_entries and all(len(entry) == 13 for entry in root_entries)
    leaf_page = int.from_bytes(root_entries[0][-4:], "little")
    keys: list[int] = []
    leaf_count = 0
    while leaf_page:
        leaf = page(leaf_page)
        assert leaf[0] == 4
        assert int.from_bytes(leaf[12:16], "little") == (0 if leaf_count == 0 else previous_leaf)
        entries = index_entries(leaf)
        assert entries and all(len(entry) == 9 for entry in entries)
        keys.extend(
            ((entry[1] ^ 0x80) << 24) | (entry[2] << 16) | (entry[3] << 8) | entry[4]
            for entry in entries
        )
        previous_leaf = leaf_page
        leaf_page = int.from_bytes(leaf[16:20], "little")
        leaf_count += 1
    assert leaf_count >= 3
    assert keys == sorted(keys)
    assert len(keys) == 700
    assert keys[0] == 1 and keys[-1] == 702
    assert 2 not in keys and 350 not in keys


def main() -> int:
    """Exercise successful DDL/DML plus rollback for every rejected schema."""
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <native-test-binary> <jet4-fixture>", file=sys.stderr)
        return 2
    test_binary = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    if not test_binary.is_file() or not fixture.is_file():
        print("native test binary and Jet 4 fixture must exist", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="egret-mdb-native-") as directory:
        database = Path(directory) / "native.mdb"
        native_root_page = fixture.stat().st_size // PAGE_SIZE + 2
        shutil.copyfile(fixture, database)
        run(str(test_binary), str(database))

        schema = run("mdb-schema", "--indexes", "--default-values", "--not-null", str(database), "mysql").stdout
        properties = run("mdb-prop", str(database), "native_crud").stdout
        rows = list(csv.reader(io.StringIO(run("mdb-export", str(database), "native_crud").stdout)))
        assert "auto_increment unique" in schema
        assert "PRIMARY KEY (`id`)" in schema
        assert "`score`\t\t\tint DEFAULT 7" in schema
        assert "DefaultValue: now()" in properties
        assert "Required: yes" in properties
        assert len(rows) == 701
        assert rows[1][0:5] == ["1", "updated", "memo-value", "9", "address-value"]
        assert rows[1][5]
        assert rows[-1][0] == "702"
        assert all(row[0] not in {"2", "350"} for row in rows[1:])
        verify_native_index(database, native_root_page)
        indexed_query = subprocess.run(
            ("mdb-sql", "-H", str(database)),
            input="select id,name from native_crud where id = 700;\nquit\n",
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        assert "700" in indexed_query and "first" in indexed_query

        invalid_statements = (
            "CREATE TABLE bad_duplicate ([id] int, [ID] int)",
            "CREATE TABLE bad_number ([value] int DEFAULT 12abc)",
            "CREATE TABLE bad_string ([value] int DEFAULT '12')",
            "CREATE TABLE bad_date ([value] datetime DEFAULT 12)",
            "CREATE TABLE bad_length ([value] int(10))",
            "CREATE TABLE bad_null ([value] int DEFAULT NULL NOT NULL)",
            "CREATE TABLE bad_constraint ([id] AutoIncrement PRIMARY KEY, [name] text UNIQUE)",
        )
        for statement in invalid_statements:
            before = digest(database)
            failed = run(str(test_binary), str(database), statement, check=False)
            assert failed.returncode != 0
            assert digest(database) == before

        before = digest(database)
        columns = ",".join(f"[column_{index:03d}_{'x' * 50}] int" for index in range(80))
        failed = run(str(test_binary), str(database), f"CREATE TABLE oversized ({columns})", check=False)
        assert failed.returncode != 0
        assert "definition exceeds one page" in failed.stderr
        assert digest(database) == before

        run(
            str(test_binary),
            str(database),
            "CREATE TABLE [unicode_表] ([id] AutoIncrement, [标题] VARCHAR(40) DEFAULT '正常', "
            "CONSTRAINT [pk_unicode] PRIMARY KEY ([id]))",
        )
        tables = run("mdb-tables", "-1", str(database)).stdout
        assert "unicode_表" in tables
        before = digest(database)
        duplicate = run(
            str(test_binary),
            str(database),
            "CREATE TABLE [unicode_表] ([id] AutoIncrement PRIMARY KEY)",
            check=False,
        )
        assert duplicate.returncode != 0
        assert "already exists" in duplicate.stderr
        assert digest(database) == before

    print("native MDB DDL/DML regression tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
