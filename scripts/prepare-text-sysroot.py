#!/usr/bin/env python3
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Build a local Egret system-library overlay without changing the installation."""

import pathlib
import sys

source = pathlib.Path(sys.argv[1]).resolve()
target = pathlib.Path(sys.argv[2]).resolve()
target.mkdir(parents=True, exist_ok=True)


def ensure_link(link: pathlib.Path, entry: pathlib.Path) -> None:
    """Create or repair one overlay symlink while preserving matching links."""
    if link.is_symlink() and link.resolve(strict=False) != entry.resolve():
        link.unlink()
    if not link.exists() and not link.is_symlink():
        link.symlink_to(entry, target_is_directory=entry.is_dir())


for entry in source.iterdir():
    if entry.name in ("system", "build"):
        continue
    link = target / entry.name
    ensure_link(link, entry)
system = target / "system"
system.mkdir(exist_ok=True)
for entry in (source / "system").iterdir():
    if entry.name == "strings":
        continue
    link = system / entry.name
    ensure_link(link, entry)
strings_dir = system / "strings"
strings_dir.mkdir(exist_ok=True)
for entry in (source / "system" / "strings").iterdir():
    if entry.name == "strings.eg":
        continue
    link = strings_dir / entry.name
    ensure_link(link, entry)
text = (source / "system" / "strings" / "strings.eg").read_text()
replacements = {
    "return std.view(s).starts_with(std.view(prefix));":
        "return eg_asp_text_starts_with(s, prefix) != 0;",
    "return std.view(s).ends_with(std.view(suffix));":
        "return eg_asp_text_ends_with(s, suffix) != 0;",
    """if std.len(a) != std.len(b) {
        return false;
    }
    return find_substr_ascii_case(a, b) == 0;""":
        "return eg_asp_text_equal_fold_ascii(a, b) != 0;",
}
for old, new in replacements.items():
    if text.count(old) != 1:
        raise SystemExit("Egret strings library changed; review the text overlay before rebuilding: " + old)
    text = text.replace(old, new)
text += """
// Allocation-free byte comparisons supplied by the ASP native library.
extern func eg_asp_text_starts_with(s: String, prefix: String) -> Int;
extern func eg_asp_text_ends_with(s: String, suffix: String) -> Int;
extern func eg_asp_text_equal_fold_ascii(a: String, b: String) -> Int;
"""
output = strings_dir / "strings.eg"
if not output.exists() or output.read_text() != text:
    output.write_text(text)
