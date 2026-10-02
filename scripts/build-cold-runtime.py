#!/usr/bin/env python3
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Build a project-local native runtime overlay without modifying the SDK."""

import argparse
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("source", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("--finalizer-index", action="store_true")
parser.add_argument("--separate-hard-limit", action="store_true")
parser.add_argument("--preempt-depth-stores", action="store_true")
parser.add_argument("--idle-poll-throttle", action="store_true")
parser.add_argument("--lean-poll", action="store_true")
parser.add_argument("--omit-redundant-marking-poll", action="store_true")
parser.add_argument("--poll-latch", action="store_true")
parser.add_argument("--fast-objset-hash", action="store_true")
parser.add_argument("--quiescent-safety", action="store_true")
args = parser.parse_args()
source = args.source.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
shutil.copytree(source / "runtime", output / "runtime", dirs_exist_ok=True)
include_link = output / "include"
if not include_link.exists():
    include_link.symlink_to(source / "include", target_is_directory=True)

runtime_misc_path = output / "runtime/core/eg_runtime_misc.c"
runtime_misc = runtime_misc_path.read_text()
old_string_factory = """eg_string_t* eg_string_from_bytes(const void* data, int64_t len) {
    if (!data) return &eg_string_empty;
    if (len <= 0) return &eg_string_empty;
    uint64_t n = (uint64_t)len;
    uint64_t total = (uint64_t)sizeof(eg_string_t) + n + 1;
    eg_string_t* s = (eg_string_t*)eg_gc_alloc(total, 0, NULL);
    if (!s) return &eg_string_empty;
    char* buf = (char*)(s + 1);
    memcpy(buf, data, (size_t)n);
    buf[n] = '\\0';
    s->len = len;
    s->data = buf;
    return s;
}"""
new_string_factory = """#if defined(__GNUC__) || defined(__clang__)
extern void* eg_asp_request_string_alloc(int64_t len) __attribute__((weak));
extern int64_t eg_asp_request_string_is_owned(void* string_object) __attribute__((weak));
#else
extern void* eg_asp_request_string_alloc(int64_t len);
extern int64_t eg_asp_request_string_is_owned(void* string_object);
#endif

static int eg_string_request_owned(const eg_string_t* s) {
    return s && eg_asp_request_string_is_owned
        && eg_asp_request_string_is_owned((void*)s) != 0;
}

eg_string_t* eg_string_from_bytes_managed(const void* data, int64_t len) {
    if (!data) return &eg_string_empty;
    if (len <= 0) return &eg_string_empty;
    uint64_t n = (uint64_t)len;
    uint64_t total = (uint64_t)sizeof(eg_string_t) + n + 1;
    eg_string_t* s = (eg_string_t*)eg_gc_alloc(total, 0, NULL);
    if (!s) return &eg_string_empty;
    char* buf = (char*)(s + 1);
    memcpy(buf, data, (size_t)n);
    buf[n] = '\\0';
    s->len = len;
    s->data = buf;
    return s;
}

eg_string_t* eg_string_from_bytes(const void* data, int64_t len) {
    if (!data) return &eg_string_empty;
    if (len <= 0) return &eg_string_empty;
    eg_string_t* s = eg_asp_request_string_alloc
        ? (eg_string_t*)eg_asp_request_string_alloc(len) : NULL;
    if (!s) return eg_string_from_bytes_managed(data, len);
    memcpy(s->data, data, (size_t)len);
    s->data[len] = '\\0';
    return s;
}"""
if runtime_misc.count(old_string_factory) != 1:
    raise SystemExit("String factory changed; review before rebuilding")
runtime_misc_path.write_text(runtime_misc.replace(
    old_string_factory, new_string_factory))

runtime_misc = runtime_misc_path.read_text()
old_string_cstr = """const char* eg_string_cstr(const eg_string_t* s) {
    if (!s) return "";
    eg_gc_obj_t* h = eg_string_heap_obj(s);"""
new_string_cstr = """const char* eg_string_cstr(const eg_string_t* s) {
    if (!s) return "";
    if (eg_string_request_owned(s)) return s->data ? s->data : "";
    eg_gc_obj_t* h = eg_string_heap_obj(s);"""
old_string_len = """int64_t eg_string_len(const eg_string_t* s) {
    if (!s) return 0;
    eg_gc_obj_t* live_string_obj = eg_string_heap_obj(s);"""
new_string_len = """int64_t eg_string_len(const eg_string_t* s) {
    if (!s) return 0;
    if (eg_string_request_owned(s)) return s->len;
    eg_gc_obj_t* live_string_obj = eg_string_heap_obj(s);"""
if runtime_misc.count(old_string_cstr) != 1:
    raise SystemExit("String cstr accessor changed; review before rebuilding")
if runtime_misc.count(old_string_len) != 1:
    raise SystemExit("String length accessor changed; review before rebuilding")
runtime_misc_path.write_text(runtime_misc.replace(
    old_string_cstr, new_string_cstr).replace(
    old_string_len, new_string_len))

std_common_path = output / "runtime/std/std_common.c"
std_common = std_common_path.read_text()
std_common_anchor = """static char* eg_std_empty_string(void) {"""
std_common_hook = """#if defined(__GNUC__) || defined(__clang__)
extern void* eg_asp_request_string_alloc(int64_t len) __attribute__((weak));
#else
extern void* eg_asp_request_string_alloc(int64_t len);
#endif

static char* eg_std_empty_string(void) {"""
if std_common.count(std_common_anchor) != 1:
    raise SystemExit("Standard runtime preamble changed; review before rebuilding")
std_common = std_common.replace(std_common_anchor, std_common_hook)
old_string_alloc = """    uint64_t total = (uint64_t)sizeof(eg_string_t) + n + 1;
    eg_string_t* s = (eg_string_t*)eg_gc_alloc(total, 0, NULL);
    if (!s) return eg_string_from_bytes("", 0);
    char* buf = (char*)(s + 1);
    buf[0] = '\\0';
    s->len = len;
    s->data = buf;
    return s;"""
new_string_alloc = """    eg_string_t* request_string = eg_asp_request_string_alloc
        ? (eg_string_t*)eg_asp_request_string_alloc(len) : NULL;
    if (request_string) return request_string;
    uint64_t total = (uint64_t)sizeof(eg_string_t) + n + 1;
    eg_string_t* s = (eg_string_t*)eg_gc_alloc(total, 0, NULL);
    if (!s) return eg_string_from_bytes("", 0);
    char* buf = (char*)(s + 1);
    buf[0] = '\\0';
    s->len = len;
    s->data = buf;
    return s;"""
if std_common.count(old_string_alloc) != 1:
    raise SystemExit("Standard String allocator changed; review before rebuilding")
std_common_path.write_text(std_common.replace(old_string_alloc, new_string_alloc))

std_abi_path = output / "runtime/std/std_abi.c"
std_abi = std_abi_path.read_text()
counter_block = re.compile(
    r"static uint64_t eg_std_string_alloc_count = 0;\n"
    r".*?"
    r"static void eg_std_string_count_alloc\(int64_t bytes\) \{\n"
    r".*?"
    r"\}\n\n",
    re.DOTALL,
)
if len(counter_block.findall(std_abi)) != 1:
    raise SystemExit("Standard String counters changed; review before rebuilding")
std_abi = counter_block.sub("", std_abi)
getter_block = re.compile(
    r"int64_t __std_string_alloc_count\(void\) \{.*?"
    r"int64_t __std_string_alloc_bucket_count\(int64_t bucket\) \{\n"
    r".*?"
    r"\}\n\n",
    re.DOTALL,
)
if len(getter_block.findall(std_abi)) != 1:
    raise SystemExit("Standard String counter API changed; review before rebuilding")
std_abi = getter_block.sub("", std_abi)
std_abi = re.sub(
    r"^[ \t]*__atomic_add_fetch\(&eg_std_string_(?:concat_count|substr_count|cstr_copy_count), 1, __ATOMIC_RELAXED\);\n",
    "",
    std_abi,
    flags=re.MULTILINE,
)
std_abi = re.sub(
    r"^[ \t]*eg_std_string_count_alloc\([^;\n]*\);\n",
    "",
    std_abi,
    flags=re.MULTILINE,
)
concat_debug = re.compile(
    r'    const char\* dbg_concat = getenv\("EG_DBG_CONCAT"\);\n'
    r".*?"
    r"        ;\n"
    r"    \}\n"
    r"(?=    if \(la < 0\) la = 0;)",
    re.DOTALL,
)
if len(concat_debug.findall(std_abi)) != 1:
    raise SystemExit("String concat debug probe changed; review before rebuilding")
std_abi = concat_debug.sub("", std_abi)
if "eg_std_string_count_" in std_abi or "eg_std_string_alloc_bucket" in std_abi:
    raise SystemExit("Standard String counter reference remains after cleanup")
if "EG_DBG_CONCAT" in std_abi or "trae-debug-log-concat" in std_abi:
    raise SystemExit("String concat debug probe remains after cleanup")
std_abi_path.write_text(std_abi)

std_container_path = output / "runtime/std/std_container.c"
std_container = std_container_path.read_text()
native_container_hook = """static void eg_std_native_container_change(const char* kind, size_t old_bytes, size_t new_bytes, size_t cap, size_t len) {
    eg_gc_debug_native_container_change(kind, (uint64_t)old_bytes, (uint64_t)new_bytes, (uint64_t)cap, (uint64_t)len);
}"""
if std_container.count(native_container_hook) != 1:
    raise SystemExit("Native container diagnostic hook changed; review before rebuilding")
std_container_path.write_text(std_container.replace(
    native_container_hook,
    "#define eg_std_native_container_change(kind, old_bytes, new_bytes, cap, len) "
    "do { (void)(kind); (void)(old_bytes); (void)(new_bytes); (void)(cap); (void)(len); } while (0)",
))
std_rt_path = output / "runtime/std/std_rt.c"
std_rt = std_rt_path.read_text()
std_rt_path.write_text(std_rt.replace(
    "void eg_gc_debug_native_container_change(const char* kind, uint64_t old_bytes, uint64_t new_bytes, uint64_t cap, uint64_t len);\n",
    "",
))

gc_path = output / "runtime/core/eg_gc.c"
code = gc_path.read_text()
code = code.replace('"../../include/eg_atomic_compat.h"', '"eg_atomic_compat.h"')
counter_declarations = """int64_t __std_string_alloc_count(void);
int64_t __std_string_alloc_bytes(void);
int64_t __std_string_concat_count(void);
int64_t __std_string_substr_count(void);
int64_t __std_string_cstr_copy_count(void);
int64_t __std_string_alloc_bucket_count(int64_t bucket);
"""
if code.count(counter_declarations) != 1:
    raise SystemExit("GC String counter declarations changed; review before rebuilding")
code = code.replace(counter_declarations, "")
counter_report = re.compile(
    r'    fprintf\(f, "\\n"\);\n'
    r'    fprintf\(f, "StdString\\n"\);\n'
    r".*?"
    r'    fprintf\(f, "\\n"\);\n'
    r'    fprintf\(f, "GC\\n"\);',
    re.DOTALL,
)
if len(counter_report.findall(code)) != 1:
    raise SystemExit("GC String counter report changed; review before rebuilding")
code = counter_report.sub(
    lambda _: '    fprintf(f, "\\n");\n    fprintf(f, "GC\\n");',
    code,
)
shutdown_report = re.compile(
    r"void eg_gc_shutdown_collect_for_report\(void\) \{\n"
    r".*?"
    r"\}\n\n(?=static void eg_gc_incremental_finish_mark)",
    re.DOTALL,
)
if len(shutdown_report.findall(code)) != 1:
    raise SystemExit("GC shutdown report hook changed; review before rebuilding")
code = shutdown_report.sub("", code)
perf_report = re.compile(
    r"static void eg_perf_collect_live\(.*?"
    r"\n\}\n\n(?=void\* eg_alloc)",
    re.DOTALL,
)
if len(perf_report.findall(code)) != 1:
    raise SystemExit("GC performance report changed; review before rebuilding")
code = perf_report.sub("", code)
debug_macros = """
#define eg_dbg_enabled() 0
#define eg_dbg_event(a, b, c, d) ((void)(a), (void)(b), (void)(c), (void)(d))
#define eg_dbg_uaf_on() 0
#define eg_dbg_uaf_event(a, b, c, d) ((void)(a), (void)(b), (void)(c), (void)(d))
#define eg_dbg_uaf_record_free(a, b, c, d, e) ((void)(a), (void)(b), (void)(c), (void)(d), (void)(e))
#define eg_dbg_uaf_find_recent(a, b) ((void)(a), (void)(b), 0)
#define eg_dbg_uaf_scan_heap_refs(a, b, c) ((void)(a), (void)(b), (void)(c))
#define eg_dbg_mem_growth_on() 0
#define eg_dbg_mem_growth_event(a, b, c, d) ((void)(a), (void)(b), (void)(c), (void)(d))
#define eg_dbg_mem_growth_gc_cycle(a, b, c, d, e, f) ((void)(a), (void)(b), (void)(c), (void)(d), (void)(e), (void)(f))
#define eg_dbg_segv_install_once() ((void)0)
#define eg_dbg_kept_begin_cycle() ((void)0)
#define eg_dbg_kept_push_source(a, b, c, d, e) ((void)(a), (void)(b), (void)(c), (void)(d), (void)(e), (eg_dbg_kept_source_ctx_t){0})
#define eg_dbg_kept_pop_source(a) ((void)(a))
#define eg_dbg_kept_record_mark(a, b) ((void)(a), (void)(b))
#define eg_dbg_kept_lookup_origin(a) ((void)(a), (eg_dbg_kept_origin_t){0})
#define eg_dbg_kept_report_cycle(a, b, c, d) ((void)(a), (void)(b), (void)(c), (void)(d))
#define eg_perf_record_gc_cycle(a, b) ((void)(a), (void)(b))
#define eg_perf_record_stw(a, b) ((void)(a), (void)(b))
#define eg_perf_record_finalizable_preserve(a, b, c, d, e, f) ((void)(a), (void)(b), (void)(c), (void)(d), (void)(e), (void)(f))
#define eg_perf_record_incremental_root_part(a, b, c, d) ((void)(a), (void)(b), (void)(c), (void)(d))
#define eg_perf_record_incremental_precise_stackmap_scan(a, b) ((void)(a), (void)(b))
#define eg_perf_record_incremental_conservative_scan(a, b, c) ((void)(a), (void)(b), (void)(c))
#define eg_perf_record_incremental_root_deferral(a) ((void)(a))
#define eg_perf_record_linux_preempt_sent() ((void)0)
#define eg_perf_record_linux_preempt_success(a) ((void)(a))
#define eg_perf_record_linux_preempt_failure() ((void)0)
#define eg_perf_record_linux_preempt_timeout() ((void)0)
"""
platform_include = '#include "eg_gc_platform.c"\n'
if code.count(platform_include) != 1:
    raise SystemExit("GC platform include changed; review before rebuilding")
code = code.replace(platform_include, platform_include + debug_macros)
code = re.sub(
    r"^([ \t]*)eg_perf_atomic_add_u64\(&eg_perf_[^,]+,\s*(.+)\);\n",
    r"\1(void)(\2);\n",
    code,
    flags=re.MULTILINE,
)
code = code.replace(
    "uint64_t cycle_start = __atomic_exchange_n(&eg_perf_gc_incremental_cycle_start_us, 0, __ATOMIC_ACQ_REL);",
    "uint64_t cycle_start = 0;",
)
gc_stw_path = output / "runtime/core/eg_gc_stw.c"
gc_stw = gc_stw_path.read_text()
perf_toggle = """void eg_perf_set_enabled(int64_t enabled) { eg_perf_enabled = enabled ? 1 : 0; }
static int eg_perf_is_enabled(void) { return eg_perf_enabled ? 1 : 0; }"""
if gc_stw.count(perf_toggle) != 1:
    raise SystemExit("GC performance toggle changed; review before rebuilding")
gc_stw_path.write_text(gc_stw.replace(
    perf_toggle, "static int eg_perf_is_enabled(void) { return 0; }"))
perf_path = output / "runtime/core/eg_perf.c"
perf = perf_path.read_text()
native_container_region = re.compile(
    r"// #region debug-point D:native-container-churn\n"
    r".*?"
    r"// #endregion\n",
    re.DOTALL,
)
if len(native_container_region.findall(perf)) != 1:
    raise SystemExit("Native container debug region changed; review before rebuilding")
perf = native_container_region.sub("", perf)
perf_enabled_declaration = "static int eg_perf_enabled = 0;\n"
if perf.count(perf_enabled_declaration) != 1:
    raise SystemExit("GC performance state changed; review before rebuilding")
perf_path.write_text(perf.replace(perf_enabled_declaration, ""))
perf_header_path = output / "runtime/core/eg_perf.h"
perf_header = perf_header_path.read_text()
perf_header_path.write_text(perf_header.replace(
    "void eg_perf_set_enabled(int64_t enabled);\n", "").replace(
    "void eg_perf_dump_mem(void);\n", ""))
gc_header_path = output / "runtime/core/eg_gc.h"
gc_header = gc_header_path.read_text()
gc_header_path.write_text(gc_header.replace(
    "void eg_gc_shutdown_collect_for_report(void);\n", "").replace(
    "void eg_gc_debug_native_container_change(const char* kind, uint64_t old_bytes, uint64_t new_bytes, uint64_t cap, uint64_t len);\n",
    "",
))
runtime_shutdown_path = output / "runtime/core/eg_runtime_shutdown.c"
runtime_shutdown = runtime_shutdown_path.read_text()
shutdown_report_declaration = "extern void eg_gc_shutdown_collect_for_report(void);\n"
shutdown_report_call = "    eg_gc_shutdown_collect_for_report();\n"
if runtime_shutdown.count(shutdown_report_declaration) != 1:
    raise SystemExit("Runtime shutdown report declaration changed; review before rebuilding")
if runtime_shutdown.count(shutdown_report_call) != 1:
    raise SystemExit("Runtime shutdown report call changed; review before rebuilding")
runtime_shutdown_path.write_text(runtime_shutdown.replace(
    shutdown_report_declaration, "").replace(shutdown_report_call, ""))
if args.finalizer_index:
    begin = code.index("static int eg_gc_preserve_finalizable_white_some_locked(void) {")
    end = code.index("\nstatic int eg_gc_hard_memory_pressure_full_collect_required", begin)
    replacement = pathlib.Path(__file__).resolve().parent.parent / "native/runtime/gc/eg_gc_finalizer_scan.c"
    code = code[:begin] + replacement.read_text() + "\n" + code[end:]
gc_path.write_text(code)
if args.preempt_depth_stores:
    platform_path = output / "runtime/core/eg_gc_platform.c"
    platform = platform_path.read_text()
    old = """static void eg_gc_preempt_critical_enter(void) {
    eg_gc_thread_t* t = eg_gc_tls_thread;
    if (t) __atomic_add_fetch(&t->preempt_critical, 1, __ATOMIC_ACQ_REL);
}

static void eg_gc_preempt_critical_leave(void) {
    eg_gc_thread_t* t = eg_gc_tls_thread;
    if (!t) return;
    int n = __atomic_load_n(&t->preempt_critical, __ATOMIC_ACQUIRE);
    if (n > 0) __atomic_sub_fetch(&t->preempt_critical, 1, __ATOMIC_ACQ_REL);
}"""
    new = """static void eg_gc_preempt_critical_enter(void) {
    eg_gc_thread_t* t = eg_gc_tls_thread;
    if (!t) return;
    /* Only the owning thread changes its nesting depth. Collectors and the
     * asynchronous stop handler only observe it, so an exclusive RMW adds
     * cache-line serialization without providing additional ownership. */
    int n = __atomic_load_n(&t->preempt_critical, __ATOMIC_RELAXED);
    __atomic_store_n(&t->preempt_critical, n + 1, __ATOMIC_RELEASE);
}

static void eg_gc_preempt_critical_leave(void) {
    eg_gc_thread_t* t = eg_gc_tls_thread;
    if (!t) return;
    int n = __atomic_load_n(&t->preempt_critical, __ATOMIC_RELAXED);
    if (n > 0) __atomic_store_n(&t->preempt_critical, n - 1, __ATOMIC_RELEASE);
}"""
    if platform.count(old) != 1:
        raise SystemExit("POSIX preempt critical-depth implementation changed; review before rebuilding")
    platform_path.write_text(platform.replace(old, new))
if sum((args.idle_poll_throttle, args.lean_poll, args.omit_redundant_marking_poll, args.poll_latch)) > 1:
    raise SystemExit("GC poll experiments are mutually exclusive")
if args.idle_poll_throttle:
    stw_path = output / "runtime/core/eg_gc_stw.c"
    stw = stw_path.read_text()
    old = """int eg_gc_should_poll(void) {
    if (!eg_gc_enabled) return 0;
    if (__atomic_load_n(&eg_gc_in_finalizer, __ATOMIC_ACQUIRE)) return 0;
    if (__atomic_load_n(&eg_gc_phase, __ATOMIC_ACQUIRE) != EG_GC_PHASE_IDLE) return 1;
    if (__atomic_load_n(&eg_gc_marking, __ATOMIC_ACQUIRE)) return 1;
    if (__atomic_load_n(&eg_gc_collect_pending, __ATOMIC_ACQUIRE)) return 1;
    if (eg_gc_bytes >= eg_gc_threshold) return 1;
    return __atomic_load_n(&eg_gc_world_stopping, __ATOMIC_ACQUIRE) ? 1 : 0;
}"""
    new = """static EG_GC_BARRIER_TLS uint32_t eg_gc_idle_poll_skip;
static uint32_t eg_gc_idle_poll_interval;

static uint32_t eg_gc_get_idle_poll_interval(void) {
    uint32_t interval = __atomic_load_n(&eg_gc_idle_poll_interval, __ATOMIC_ACQUIRE);
    if (interval) return interval;
    interval = 64;
    const char* value = getenv("EG_GC_IDLE_POLL_INTERVAL");
    if (value && value[0]) {
        unsigned long parsed = strtoul(value, NULL, 10);
        if (parsed >= 1 && parsed <= 4096) interval = (uint32_t)parsed;
    }
    __atomic_store_n(&eg_gc_idle_poll_interval, interval, __ATOMIC_RELEASE);
    return interval;
}

int eg_gc_should_poll(void) {
    /* Finalizers intentionally do not join a stop requested by their own
     * collection. Other mutators still observe every STW request immediately. */
    if (__atomic_load_n(&eg_gc_in_finalizer, __ATOMIC_ACQUIRE)) return 0;
    if (__atomic_load_n(&eg_gc_world_stopping, __ATOMIC_ACQUIRE)) return 1;

    uint32_t skip = eg_gc_idle_poll_skip;
    if (skip) {
        eg_gc_idle_poll_skip = skip - 1;
        return 0;
    }

    if (!eg_gc_enabled) return 0;
    if (__atomic_load_n(&eg_gc_phase, __ATOMIC_ACQUIRE) != EG_GC_PHASE_IDLE) return 1;
    if (__atomic_load_n(&eg_gc_marking, __ATOMIC_ACQUIRE)) return 1;
    if (__atomic_load_n(&eg_gc_collect_pending, __ATOMIC_ACQUIRE)) return 1;
    if (eg_gc_bytes >= eg_gc_threshold) return 1;
    eg_gc_idle_poll_skip = eg_gc_get_idle_poll_interval() - 1;
    return 0;
}"""
    if stw.count(old) != 2:
        raise SystemExit("GC poll implementations changed; review before rebuilding")
    stw_path.write_text(stw.replace(old, new))
if args.lean_poll:
    stw_path = output / "runtime/core/eg_gc_stw.c"
    stw = stw_path.read_text()
    old = """int eg_gc_should_poll(void) {
    if (!eg_gc_enabled) return 0;
    if (__atomic_load_n(&eg_gc_in_finalizer, __ATOMIC_ACQUIRE)) return 0;
    if (__atomic_load_n(&eg_gc_phase, __ATOMIC_ACQUIRE) != EG_GC_PHASE_IDLE) return 1;
    if (__atomic_load_n(&eg_gc_marking, __ATOMIC_ACQUIRE)) return 1;
    if (__atomic_load_n(&eg_gc_collect_pending, __ATOMIC_ACQUIRE)) return 1;
    if (eg_gc_bytes >= eg_gc_threshold) return 1;
    return __atomic_load_n(&eg_gc_world_stopping, __ATOMIC_ACQUIRE) ? 1 : 0;
}"""
    new = """int eg_gc_should_poll(void) {
    if (!eg_gc_enabled) return 0;
    if (__atomic_load_n(&eg_gc_in_finalizer, __ATOMIC_ACQUIRE)) return 0;
    /* A non-idle phase already covers every marking interval. Allocation and
     * native-memory accounting publish threshold crossings through pending,
     * so the hot poll path does not need duplicate marking and byte loads. */
    if (__atomic_load_n(&eg_gc_phase, __ATOMIC_ACQUIRE) != EG_GC_PHASE_IDLE) return 1;
    if (__atomic_load_n(&eg_gc_collect_pending, __ATOMIC_ACQUIRE)) return 1;
    return __atomic_load_n(&eg_gc_world_stopping, __ATOMIC_ACQUIRE) ? 1 : 0;
}"""
    if stw.count(old) != 2:
        raise SystemExit("GC poll implementations changed; review before rebuilding")
    stw_path.write_text(stw.replace(old, new))
if args.omit_redundant_marking_poll:
    stw_path = output / "runtime/core/eg_gc_stw.c"
    stw = stw_path.read_text()
    old = """    if (__atomic_load_n(&eg_gc_phase, __ATOMIC_ACQUIRE) != EG_GC_PHASE_IDLE) return 1;
    if (__atomic_load_n(&eg_gc_marking, __ATOMIC_ACQUIRE)) return 1;
    if (__atomic_load_n(&eg_gc_collect_pending, __ATOMIC_ACQUIRE)) return 1;"""
    new = """    /* Every concurrent marking interval is contained in a non-idle phase. */
    if (__atomic_load_n(&eg_gc_phase, __ATOMIC_ACQUIRE) != EG_GC_PHASE_IDLE) return 1;
    if (__atomic_load_n(&eg_gc_collect_pending, __ATOMIC_ACQUIRE)) return 1;"""
    if stw.count(old) != 2:
        raise SystemExit("GC poll state checks changed; review before rebuilding")
    stw_path.write_text(stw.replace(old, new))
if args.poll_latch:
    perf_path = output / "runtime/core/eg_perf.c"
    perf = perf_path.read_text()
    anchor = """static int eg_gc_collect_pending = 0;
static int eg_gc_in_finalizer = 0;
static int eg_gc_collecting = 0;"""
    replacement = """static int eg_gc_collect_pending = 0;
static int eg_gc_in_finalizer = 0;
int eg_gc_poll_requested = 0;
static void eg_gc_request_poll(void);
static int eg_gc_collecting = 0;"""
    if perf.count(anchor) != 1:
        raise SystemExit("GC poll-state declarations changed; review before rebuilding")
    perf_path.write_text(perf.replace(anchor, replacement))

    code = gc_path.read_text()
    anchor = '#include "eg_gc_platform.c"\n'
    helper = r'''
static void eg_gc_request_poll(void) {
    __atomic_store_n(&eg_gc_poll_requested, 1, __ATOMIC_RELEASE);
}

static int eg_gc_poll_trigger_active(void) {
    if (__atomic_load_n(&eg_gc_phase, __ATOMIC_ACQUIRE) != EG_GC_PHASE_IDLE) return 1;
    if (__atomic_load_n(&eg_gc_marking, __ATOMIC_ACQUIRE)) return 1;
    if (__atomic_load_n(&eg_gc_collect_pending, __ATOMIC_ACQUIRE)) return 1;
    if (__atomic_load_n(&eg_gc_world_stopping, __ATOMIC_ACQUIRE)) return 1;
    return eg_gc_bytes >= eg_gc_threshold;
}
'''
    if code.count(anchor) != 1:
        raise SystemExit("GC platform include changed; review before rebuilding")
    gc_path.write_text(code.replace(anchor, anchor + helper))

    stw_path = output / "runtime/core/eg_gc_stw.c"
    stw = stw_path.read_text()
    old = """int eg_gc_should_poll(void) {
    if (!eg_gc_enabled) return 0;
    if (__atomic_load_n(&eg_gc_in_finalizer, __ATOMIC_ACQUIRE)) return 0;
    if (__atomic_load_n(&eg_gc_phase, __ATOMIC_ACQUIRE) != EG_GC_PHASE_IDLE) return 1;
    if (__atomic_load_n(&eg_gc_marking, __ATOMIC_ACQUIRE)) return 1;
    if (__atomic_load_n(&eg_gc_collect_pending, __ATOMIC_ACQUIRE)) return 1;
    if (eg_gc_bytes >= eg_gc_threshold) return 1;
    return __atomic_load_n(&eg_gc_world_stopping, __ATOMIC_ACQUIRE) ? 1 : 0;
}"""
    new = """int eg_gc_should_poll(void) {
    if (!eg_gc_enabled) return 0;
    if (__atomic_load_n(&eg_gc_in_finalizer, __ATOMIC_ACQUIRE)) return 0;
    if (!__atomic_load_n(&eg_gc_poll_requested, __ATOMIC_ACQUIRE)) return 0;
    if (eg_gc_poll_trigger_active()) return 1;

    /* A trigger can race the clear after observing the old latch. Recheck
     * after clearing so its publication cannot be lost. */
    __atomic_store_n(&eg_gc_poll_requested, 0, __ATOMIC_RELEASE);
    if (eg_gc_poll_trigger_active()) {
        eg_gc_request_poll();
        return 1;
    }
    return 0;
}"""
    if stw.count(old) != 2:
        raise SystemExit("GC poll implementations changed; review before rebuilding")
    stw_path.write_text(stw.replace(old, new))

    code = gc_path.read_text()
    old = """void eg_gc_poll_c(uint64_t sp_id, const eg_gc_ctx_t* ctx) {
    eg_gc_refresh_disabled();
    if (!eg_gc_enabled) return;
    if (eg_gc_in_finalizer) return;
    if (eg_gc_poll_can_skip_full()) return;"""
    new = """void eg_gc_poll_c(uint64_t sp_id, const eg_gc_ctx_t* ctx) {
    eg_gc_refresh_disabled();
    /* Generated code may guard this call with the shared latch directly.
     * Revalidate all state here to clear stale requests and close races. */
    if (!eg_gc_should_poll()) return;
    if (eg_gc_poll_can_skip_full()) return;"""
    if code.count(old) != 1:
        raise SystemExit("GC poll entry changed; review before rebuilding")
    gc_path.write_text(code.replace(old, new))

    for relative in ("eg_perf.c", "eg_gc.c", "eg_gc_stw.c", "eg_alloc.c", "eg_finalizer.c", "eg_runtime_misc.c"):
        path = output / "runtime/core" / relative
        unit = path.read_text()
        unit = unit.replace(
            "__atomic_store_n(&eg_gc_collect_pending, 1, __ATOMIC_RELEASE);",
            "__atomic_store_n(&eg_gc_collect_pending, 1, __ATOMIC_RELEASE); eg_gc_request_poll();")
        unit = unit.replace(
            "__atomic_store_n(&eg_gc_collect_pending, eg_gc_multithread_full_fallback_needed(), __ATOMIC_RELEASE);",
            "__atomic_store_n(&eg_gc_collect_pending, eg_gc_multithread_full_fallback_needed(), __ATOMIC_RELEASE); "
            "if (__atomic_load_n(&eg_gc_collect_pending, __ATOMIC_ACQUIRE)) eg_gc_request_poll();")
        unit = unit.replace(
            "__atomic_store_n(&eg_gc_marking, 1, __ATOMIC_RELEASE);",
            "__atomic_store_n(&eg_gc_marking, 1, __ATOMIC_RELEASE); eg_gc_request_poll();")
        unit = unit.replace(
            "eg_gc_world_stopping = 1;",
            "eg_gc_world_stopping = 1; eg_gc_request_poll();")
        unit = unit.replace(
            "eg_gc_phase = EG_GC_PHASE_MARK;",
            "eg_gc_phase = EG_GC_PHASE_MARK; eg_gc_request_poll();")
        unit = unit.replace(
            "eg_gc_phase = EG_GC_PHASE_SWEEP;",
            "eg_gc_phase = EG_GC_PHASE_SWEEP; eg_gc_request_poll();")
        unit = re.sub(
            r"^([ \t]*eg_gc_threshold = [^;]+;)$",
            r"\1 eg_gc_request_poll();", unit, flags=re.MULTILINE)
        path.write_text(unit)

if args.fast_objset_hash:
    perf_path = output / "runtime/core/eg_perf.c"
    perf = perf_path.read_text()
    old = """static size_t eg_gc_objset_hash(uintptr_t x) {
    uint64_t z = (uint64_t)(x >> 4);
    z ^= z >> 33;
    z *= 0xff51afd7ed558ccdull;
    z ^= z >> 33;
    z *= 0xc4ceb9fe1a85ec53ull;
    z ^= z >> 33;
    return (size_t)z;
}"""
    new = """static size_t eg_gc_objset_hash(uintptr_t x) {
    /* Object headers are at least 16-byte aligned and the table capacity is a
     * power of two. Multiplication by an odd constant is bijective modulo
     * every table size, so sequential allocator addresses remain distinct
     * without paying for a general-purpose avalanche finalizer. */
    return (size_t)(((uint64_t)(x >> 4)) * 11400714819323198485ull);
}"""
    if perf.count(old) != 1:
        raise SystemExit("Object-set hash changed; review before rebuilding")
    perf_path.write_text(perf.replace(old, new))
if args.separate_hard_limit:
    perf_path = output / "runtime/core/eg_perf.c"
    perf = perf_path.read_text()
    begin = perf.index("static uint64_t eg_gc_hard_pressure_limit_cap_bytes(void) {")
    end = perf.index("\nstatic uint64_t eg_gc_percent(void)", begin)
    block = perf[begin:end]
    old = """        uint64_t pct = __atomic_load_n(&eg_gc_runtime_memory_limit_threshold_percent, __ATOMIC_ACQUIRE);
        if (pct == 0) pct = 80;
        if (pct > 100) pct = 100;
        return (runtime_limit / 100ull) * pct + ((runtime_limit % 100ull) * pct) / 100ull;"""
    assert block.count(old) == 1
    block = block.replace(old, """        /* The percentage is the collection trigger, not the hard limit.
         * Using it here makes emergency full collection compete with the first
         * incremental step at exactly the same heap size. */
        return runtime_limit;""")
    old = """                uint64_t pct = 80;
                const char* ps = getenv("EG_GC_MEMORY_LIMIT_THRESHOLD_PERCENT");
                if (ps && ps[0]) {
                    unsigned long long p = strtoull(ps, NULL, 10);
                    if (p > 0 && p <= 100) pct = (uint64_t)p;
                }
                v = ((uint64_t)n / 100ull) * pct + (((uint64_t)n % 100ull) * pct) / 100ull;"""
    assert block.count(old) == 1
    block = block.replace(old, "                v = (uint64_t)n;")
    perf_path.write_text(perf[:begin] + block + perf[end:])
if args.quiescent_safety:
    patch = pathlib.Path(__file__).resolve().parent.parent / "native/runtime/gc/eg_gc_quiescent.patch"
    subprocess.run(["patch", "--batch", "--fuzz=0", "-p1", "-d", str(output),
                    "-i", str(patch)], check=True)
    for artifact in output.rglob("*"):
        if artifact.is_file() and artifact.suffix in (".orig", ".rej"):
            artifact.unlink()
feature_flags = ["-D_GNU_SOURCE", "-D_POSIX_C_SOURCE=200809L"] if sys.platform.startswith("linux") else []
subprocess.run(["cc", "-O2", "-ffunction-sections", "-fdata-sections", "-std=c11", "-DEG_ENABLE_FUSE=0", *feature_flags,
                "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                "-Wno-unused-variable", "-Wno-unused-parameter",
                "-Wno-unused-but-set-variable",
                "-I" + str(source / "include"), "-c",
                str(output / "runtime/core/eg_rt.c"), "-o", str(output / "eg_runtime.o")],
               check=True)
openssl_cflags = subprocess.run(
    ["pkg-config", "--cflags", "openssl"], check=False, capture_output=True,
    text=True).stdout
std_cflags = shlex.split(os.environ.get("CPPFLAGS", "")) + shlex.split(openssl_cflags)
brew = shutil.which("brew")
if brew:
    brew_prefix = subprocess.run(
        [brew, "--prefix"], check=False, capture_output=True, text=True
    ).stdout.strip()
    if brew_prefix:
        std_cflags.append("-I" + str(pathlib.Path(brew_prefix) / "include"))
subprocess.run(["cc", "-O2", "-ffunction-sections", "-fdata-sections", "-std=c11", "-DEG_ENABLE_FUSE=0", *feature_flags,
                "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", *std_cflags,
                "-I" + str(source / "include"), "-c",
                str(output / "runtime/std/std_rt.c"), "-o",
                str(output / "eg_std_runtime.o")], check=True)
