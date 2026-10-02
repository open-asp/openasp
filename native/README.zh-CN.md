# Native 模块

[English](README.md)

原生 C 实现按职责划分，`native/` 根目录不直接放置源码文件。

| 目录 | 职责 |
| --- | --- |
| `components/` | OpenASP 通用组件，包括加密、JSON 和进程控制 |
| `database/mdb/` | MDB/Jet 数据访问、DDL、并发锁和崩溃恢复 |
| `database/sqlite/` | SQLite 数据访问适配 |
| `runtime/gc/` | Egret 编译器和运行时的 GC 扩展与补丁 |
| `runtime/vm/` | ASP 字节码、原生 VM、请求 arena 和 mmap |
| `system/` | 操作系统能力封装 |
| `text/` | 字符集、正则、文本扫描和 VBScript 文本兼容逻辑 |

新增源码应放入所属模块，并通过 `Makefile` 中对应的模块路径变量引用。模块内部头文件使用同目录短路径包含；测试通过模块 include 路径访问公开头文件。
