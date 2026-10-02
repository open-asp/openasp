#!/usr/bin/env python3
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT

"""Link a local compiler with compact GC root layouts, using existing objects."""

import argparse
import os
import pathlib
import re
import shlex
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("source", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
parser.add_argument("--omit-call-entry-poll", action="store_true")
parser.add_argument("--coalesce-adjacent-polls", action="store_true")
parser.add_argument("--direct-poll-latch", action="store_true")
parser.add_argument("--hot-direct-poll-latch", action="store_true")
args = parser.parse_args()
source = args.source.resolve()
output = args.output.resolve()
project = pathlib.Path(__file__).resolve().parent.parent
output.mkdir(parents=True, exist_ok=True)


def find_llvm_config():
    """Locate llvm-config without binding the build to one package-manager prefix."""
    configured = os.environ.get("LLVM_CONFIG")
    if configured:
        return configured
    discovered = shutil.which("llvm-config")
    if discovered:
        return discovered
    brew = shutil.which("brew")
    if brew:
        prefix = subprocess.run(
            [brew, "--prefix", "llvm"], check=False, capture_output=True, text=True
        ).stdout.strip()
        candidate = pathlib.Path(prefix) / "bin/llvm-config"
        if prefix and candidate.is_file():
            return str(candidate)
    raise SystemExit("llvm-config not found; set LLVM_CONFIG or add LLVM to PATH")


llvm = find_llvm_config()
compact_source = project / "native/runtime/gc/eg_compact_gc_roots.c"
compact_include = compact_source.parent
prefix_map_flags = [
    "-ffile-prefix-map=" + str(project) + "=.",
    "-fdebug-prefix-map=" + str(project) + "=.",
    "-ffile-prefix-map=" + str(source) + "=egret",
    "-fdebug-prefix-map=" + str(source) + "=egret",
]

def flags(*args):
    """Return llvm-config output as argv tokens without shell interpretation."""
    return shlex.split(subprocess.check_output([llvm, *args], text=True))

code = (source / "compiler/backend/codegen_opt.c").read_text()
anchor = "int eg_run_llvm_optimization_pipeline(eg_codegen_t* cg, LLVMTargetMachineRef tm) {"
if code.count(anchor) != 1:
    raise SystemExit("Compiler hook changed; review codegen_opt.c before rebuilding")
code = code.replace(anchor, '#include "eg_compact_gc_roots.h"\n\n' + anchor + "\n    eg_compact_gc_roots(cg);\n    if (!eg_optimize_scalar_ir(cg, tm)) return 0;")
patched = output / "codegen_opt.c"
patched.write_text(code)
obj = output / "codegen_opt.o"
subprocess.run(["cc", "-O2", "-std=c11", "-D_GNU_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                "-Wall", "-Wextra", "-Werror",
                "-I" + str(source / "include"), "-I" + str(source / "compiler/backend"),
                "-I" + str(compact_include), *prefix_map_flags,
                *flags("--cflags"), "-c", str(patched), "-o", str(obj)], check=True)
compact_obj = output / "eg_compact_gc_roots.o"
subprocess.run(["cc", "-O2", "-std=c11", "-D_GNU_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                "-Wall", "-Wextra", "-Werror",
                "-I" + str(source / "include"), "-I" + str(source / "compiler/backend"),
                "-I" + str(compact_include), *prefix_map_flags,
                *flags("--cflags"), "-c", str(compact_source), "-o", str(compact_obj)], check=True)
replacement_objects = {str(source / "build/backend/codegen_opt.o"): obj}
if args.coalesce_adjacent_polls or args.direct_poll_latch or args.hot_direct_poll_latch:
    gc_code = (source / "compiler/backend/codegen_gc.c").read_text()
    if args.coalesce_adjacent_polls:
        old = """void eg_gc_poll_impl(eg_fn_ctx_t* fcx, int force) {
    if (!fcx || !fcx->cg || !fcx->cg->gc_enabled) return;
    if (fcx->suppress_gc_poll) return;
    eg_codegen_t* cg = fcx->cg;"""
        new = """void eg_gc_poll_impl(eg_fn_ctx_t* fcx, int force) {
    if (!fcx || !fcx->cg || !fcx->cg->gc_enabled) return;
    if (fcx->suppress_gc_poll) return;
    if (!force) {
        LLVMBasicBlockRef current = LLVMGetInsertBlock(fcx->cg->builder);
        if (current && !LLVMGetFirstInstruction(current)) {
            LLVMValueRef block_value = LLVMBasicBlockAsValue(current);
            size_t name_length = 0;
            const char* name = LLVMGetValueName2(block_value, &name_length);
            static const char prefix[] = "gc.poll.end";
            if (name_length >= sizeof(prefix) - 1
                && memcmp(name, prefix, sizeof(prefix) - 1) == 0) return;
        }
    }
    eg_codegen_t* cg = fcx->cg;"""
        if gc_code.count(old) != 1:
            raise SystemExit("GC poll emitter changed; review before rebuilding")
        gc_code = gc_code.replace(old, new)
    if args.direct_poll_latch or args.hot_direct_poll_latch:
        old = """        LLVMValueRef should = eg_dbg_inst(fcx, LLVMBuildCall2(cg->builder, cg->rt_gc_should_poll_ty, cg->rt_gc_should_poll, NULL, 0, "gc.poll.should"));
        LLVMValueRef active = eg_dbg_inst(fcx, LLVMBuildICmp(cg->builder, LLVMIntNE, should, LLVMConstInt(i32, 0, 0), "gc.poll.active"));"""
        direct = """        LLVMValueRef poll_latch = LLVMGetNamedGlobal(cg->module, "eg_gc_poll_requested");
        if (!poll_latch) {
            poll_latch = LLVMAddGlobal(cg->module, i32, "eg_gc_poll_requested");
            LLVMSetLinkage(poll_latch, LLVMExternalLinkage);
        }
        LLVMValueRef should = eg_dbg_inst(fcx, LLVMBuildLoad2(cg->builder, i32, poll_latch, "gc.poll.requested"));
        LLVMSetOrdering(should, LLVMAtomicOrderingAcquire);
        LLVMValueRef active = eg_dbg_inst(fcx, LLVMBuildICmp(cg->builder, LLVMIntNE, should, LLVMConstInt(i32, 0, 0), "gc.poll.active"));"""
        if args.direct_poll_latch:
            new = direct
        else:
            new = """        size_t function_name_length = 0;
        const char* function_name = LLVMGetValueName2(fcx->fn, &function_name_length);
        static const char execute_pc_suffix[] = "__VbsBytecodeRunner__execute_pc";
        static const char execute_block_suffix[] = "__VbsBytecodeRunner__execute_block";
        int direct_poll = (function_name_length >= sizeof(execute_pc_suffix) - 1
                && memcmp(function_name + function_name_length - (sizeof(execute_pc_suffix) - 1),
                    execute_pc_suffix, sizeof(execute_pc_suffix) - 1) == 0)
            || (function_name_length >= sizeof(execute_block_suffix) - 1
                && memcmp(function_name + function_name_length - (sizeof(execute_block_suffix) - 1),
                    execute_block_suffix, sizeof(execute_block_suffix) - 1) == 0);
        LLVMValueRef should;
        if (direct_poll) {
            LLVMValueRef poll_latch = LLVMGetNamedGlobal(cg->module, "eg_gc_poll_requested");
            if (!poll_latch) {
                poll_latch = LLVMAddGlobal(cg->module, i32, "eg_gc_poll_requested");
                LLVMSetLinkage(poll_latch, LLVMExternalLinkage);
            }
            should = eg_dbg_inst(fcx, LLVMBuildLoad2(cg->builder, i32, poll_latch, "gc.poll.requested"));
            LLVMSetOrdering(should, LLVMAtomicOrderingAcquire);
        } else {
            should = eg_dbg_inst(fcx, LLVMBuildCall2(cg->builder, cg->rt_gc_should_poll_ty,
                cg->rt_gc_should_poll, NULL, 0, "gc.poll.should"));
        }
        LLVMValueRef active = eg_dbg_inst(fcx, LLVMBuildICmp(cg->builder, LLVMIntNE, should, LLVMConstInt(i32, 0, 0), "gc.poll.active"));"""
        if gc_code.count(old) != 1:
            raise SystemExit("GC poll guard changed; review before rebuilding")
        gc_code = gc_code.replace(old, new)
    gc_patched = output / "codegen_gc.c"
    gc_patched.write_text(gc_code)
    gc_obj = output / "codegen_gc.o"
    subprocess.run(["cc", "-O2", "-std=c11", "-D_GNU_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                    "-Wall", "-Wextra", "-Werror",
                    "-I" + str(source / "include"), "-I" + str(source / "compiler/backend"),
                    *flags("--cflags"), "-c", str(gc_patched), "-o", str(gc_obj)], check=True)
    replacement_objects[str(source / "build/backend/codegen_gc.o")] = gc_obj
if args.omit_call_entry_poll:
    expr_code = (source / "compiler/backend/codegen_expr.c").read_text()
    old = """    case EG_EXPR_CALL: {
            LLVMValueRef trusted_vec_out = NULL;
            if (eg_emit_trusted_vec_method_call(fcx, e, &trusted_vec_out)) return trusted_vec_out;
            LLVMValueRef fast_map_out = NULL;
            if (eg_emit_fast_int_map_method_call(fcx, e, &fast_map_out)) return fast_map_out;
            LLVMValueRef std_string_out = NULL;
            if (eg_try_emit_std_string_intrinsic(fcx, e, &std_string_out)) return std_string_out;
            eg_gc_poll(fcx);"""
    new = """    case EG_EXPR_CALL: {
            LLVMValueRef trusted_vec_out = NULL;
            if (eg_emit_trusted_vec_method_call(fcx, e, &trusted_vec_out)) return trusted_vec_out;
            LLVMValueRef fast_map_out = NULL;
            if (eg_emit_fast_int_map_method_call(fcx, e, &fast_map_out)) return fast_map_out;
            LLVMValueRef std_string_out = NULL;
            if (eg_try_emit_std_string_intrinsic(fcx, e, &std_string_out)) return std_string_out;
            /* Callees retain their own statement and loop safepoints, and the
             * containing statement retains its completion poll. Avoid a second
             * poll before evaluating arguments at every call boundary. */"""
    if expr_code.count(old) != 1:
        raise SystemExit("Call-expression poll site changed; review before rebuilding")
    expr_patched = output / "codegen_expr.c"
    expr_patched.write_text(expr_code.replace(old, new))
    expr_obj = output / "codegen_expr.o"
    subprocess.run(["cc", "-O2", "-std=c11", "-D_GNU_SOURCE", "-D_POSIX_C_SOURCE=200809L",
                    "-Wall", "-Wextra", "-Werror",
                    "-I" + str(source / "include"), "-I" + str(source / "compiler/backend"),
                    *flags("--cflags"), "-c", str(expr_patched), "-o", str(expr_obj)], check=True)
    replacement_objects[str(source / "build/backend/codegen_expr.o")] = expr_obj
makefile = (source / "Makefile").read_text()
block = makefile.split("COMPILER_LIB_OBJS := \\", 1)[1].split("\n\n", 1)[0]
objects = [source / "build" / name for name in re.findall(r"\$\(BUILD_DIR\)/([^\s\\]+)", block)]
objects = [replacement_objects.get(str(p), p) for p in objects]
objects.insert(0, source / "build/bin/egret/main.o")
objects.append(compact_obj)
missing = [str(p) for p in objects if not p.is_file()]
if missing:
    raise SystemExit("Build the base compiler first; missing objects: " + ", ".join(missing))
subprocess.run(["c++", *map(str, objects), *flags("--ldflags", "--link-shared", "--libs", "--system-libs"),
                "-lpthread", "-lm", "-o", str(output / "egret")], check=True)
