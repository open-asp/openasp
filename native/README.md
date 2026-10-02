# Native Modules

[中文](README.zh-CN.md)

Native C implementations are organized by responsibility. Source files do not
live directly in the `native/` root.

| Directory | Responsibility |
| --- | --- |
| `components/` | General OpenASP components, including cryptography, JSON, and process control |
| `database/mdb/` | MDB/Jet data access, DDL, concurrent locking, and crash recovery |
| `database/sqlite/` | SQLite data-access adapter |
| `runtime/gc/` | GC extensions and patches for the Egret compiler and runtime |
| `runtime/vm/` | ASP bytecode, native VM, request arenas, and mmap support |
| `system/` | Operating-system capability wrappers |
| `text/` | Character sets, regular expressions, text scanning, and VBScript text compatibility |

Place new source files in the module that owns their functionality and
reference them through the corresponding module path variable in `Makefile`.
Headers internal to a module use short same-directory include paths. Tests
access public headers through module include paths.
