// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

/* Compact only the ordinary shadow-root array emitted by eg_gc_begin_fn.
 * Keep every slot referenced anywhere in the completed function, including
 * temporary slots in branches and loop bodies. This is a layout correction,
 * independent of LLVM's O1/O2 optimization pipelines. */

#include "codegen_internal.h"
#include "eg_compact_gc_roots.h"

#include <llvm/Config/llvm-config.h>

static int eg_root_name_starts(LLVMValueRef value, const char* prefix) {
    size_t length = 0;
    const char* name = LLVMGetValueName2(value, &length);
    size_t prefix_length = strlen(prefix);
    return length >= prefix_length && memcmp(name, prefix, prefix_length) == 0;
}

static void eg_compact_one_root_array(eg_codegen_t* cg, LLVMValueRef slots) {
    LLVMTypeRef old_type = LLVMGetAllocatedType(slots);
    if (LLVMGetTypeKind(old_type) != LLVMArrayTypeKind) return;
    unsigned capacity = LLVMGetArrayLength(old_type);
    unsigned used = 0;
    LLVMValueRef push = NULL;
    LLVMValueRef* initializers = calloc(capacity, sizeof(*initializers));
    if (!initializers) return;
    int valid = 1;
    for (LLVMUseRef use = LLVMGetFirstUse(slots); use; use = LLVMGetNextUse(use)) {
        LLVMValueRef gep = LLVMGetUser(use);
        if (!LLVMIsAGetElementPtrInst(gep) || LLVMGetNumOperands(gep) != 3
            || LLVMGetOperand(gep, 0) != slots || !LLVMIsAConstantInt(LLVMGetOperand(gep, 1))
            || LLVMConstIntGetZExtValue(LLVMGetOperand(gep, 1)) != 0
            || !LLVMIsAConstantInt(LLVMGetOperand(gep, 2))) {
            valid = 0;
            break;
        }
        uint64_t index = LLVMConstIntGetZExtValue(LLVMGetOperand(gep, 2));
        if (index >= capacity) {
            valid = 0;
            break;
        }
        if (eg_root_name_starts(gep, "gc.slot.init.ptr")) {
            LLVMUseRef init_use = LLVMGetFirstUse(gep);
            LLVMValueRef store = init_use ? LLVMGetUser(init_use) : NULL;
            if (!store || LLVMGetNextUse(init_use) || !LLVMIsAStoreInst(store)
                || LLVMGetOperand(store, 1) != gep || initializers[index]) {
                valid = 0;
                break;
            }
            initializers[index] = gep;
        } else if (eg_root_name_starts(gep, "gc.slots.ptr")) {
            LLVMUseRef call_use = LLVMGetFirstUse(gep);
            LLVMValueRef call = call_use ? LLVMGetUser(call_use) : NULL;
            if (push || index != 0 || !call || LLVMGetNextUse(call_use)
                || !LLVMIsACallInst(call) || LLVMGetCalledValue(call) != cg->rt_gc_push_frame
                || LLVMGetNumArgOperands(call) != 3 || LLVMGetOperand(call, 2) != gep) {
                valid = 0;
                break;
            }
            push = call;
        } else {
            if (index + 1 > used) used = (unsigned)index + 1;
        }
    }
    /* Keep one physical slot for the base GEP even in a pointer-free function. */
    unsigned physical = used ? used : 1;
    if (!push || physical >= capacity) valid = 0;
    for (unsigned i = 0; valid && i < capacity; i++) {
        if (!initializers[i]) valid = 0;
    }
    if (valid) {
        for (unsigned i = used; i < capacity; i++) {
            LLVMValueRef gep = initializers[i];
            LLVMInstructionEraseFromParent(LLVMGetUser(LLVMGetFirstUse(gep)));
            LLVMInstructionEraseFromParent(gep);
        }
        if (used == 0 && !getenv("EGRET_KEEP_EMPTY_ROOT_FRAMES")) {
            LLVMValueRef frame = LLVMGetOperand(push, 0);
            int only_frame_calls = LLVMIsAAllocaInst(frame) != NULL;
            for (LLVMUseRef use = LLVMGetFirstUse(frame); use; use = LLVMGetNextUse(use)) {
                LLVMValueRef call = LLVMGetUser(use);
                if (call != push && (!LLVMIsACallInst(call)
                    || LLVMGetCalledValue(call) != cg->rt_gc_pop_frame
                    || LLVMGetNumArgOperands(call) != 1)) {
                    only_frame_calls = 0;
                    break;
                }
            }
            /* An empty frame contributes no roots. Keep all explicit polls and
             * every frame with a live slot; only unlink this empty node. */
            if (only_frame_calls) {
                while (LLVMGetFirstUse(frame)) {
                    LLVMInstructionEraseFromParent(LLVMGetUser(LLVMGetFirstUse(frame)));
                }
                free(initializers);
                return;
            }
        }
        LLVMSetOperand(push, 1, LLVMConstInt(LLVMTypeOf(LLVMGetOperand(push, 1)), used, 0));
        LLVMBuilderRef builder = LLVMCreateBuilderInContext(cg->ctx);
        LLVMPositionBuilderBefore(builder, slots);
        LLVMValueRef compact = LLVMBuildAlloca(builder, LLVMArrayType(LLVMGetElementType(old_type), physical), "gc.compact.slots");
        LLVMSetAlignment(compact, LLVMGetAlignment(slots));
        LLVMReplaceAllUsesWith(slots, compact);
        LLVMInstructionEraseFromParent(slots);
        LLVMDisposeBuilder(builder);
    }
    free(initializers);
}

static int eg_private_shadow_root(eg_codegen_t* cg, LLVMValueRef slot) {
    int registered = 0;
    for (LLVMUseRef use = LLVMGetFirstUse(slot); use; use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        if (LLVMIsALoadInst(user) && LLVMGetOperand(user, 0) == slot) continue;
        if (LLVMIsAStoreInst(user)) {
            if (LLVMGetOperand(user, 1) == slot) continue;
            LLVMValueRef dst = LLVMGetOperand(user, 1);
            if (LLVMGetOperand(user, 0) == slot && LLVMIsAGetElementPtrInst(dst)
                && eg_root_name_starts(dst, "gc.slot.ptr")) {
                registered = 1;
                continue;
            }
        }
        if (LLVMIsACallInst(user) && LLVMGetCalledValue(user) == cg->rt_gc_store_ptr
            && LLVMGetNumArgOperands(user) == 2 && LLVMGetOperand(user, 0) == slot
            && LLVMGetOperand(user, 1) != slot) continue;
        /* ByRef addresses, async frame fields and every unknown escape retain
         * the runtime insertion barrier. Only private native stack slots qualify. */
        return 0;
    }
    return registered;
}

static void eg_lower_null_root_stores(eg_codegen_t* cg, LLVMValueRef fn) {
    if (getenv("EGRET_KEEP_NULL_ROOT_BARRIERS")) return;
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(cg->ctx);
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(fn); block; block = LLVMGetNextBasicBlock(block)) {
        LLVMValueRef instruction = LLVMGetFirstInstruction(block);
        while (instruction) {
            LLVMValueRef next = LLVMGetNextInstruction(instruction);
            if (LLVMIsACallInst(instruction) && LLVMGetCalledValue(instruction) == cg->rt_gc_store_ptr
                && LLVMGetNumArgOperands(instruction) == 2) {
                LLVMValueRef dst = LLVMGetOperand(instruction, 0);
                LLVMValueRef value = LLVMGetOperand(instruction, 1);
                /* Roots are rescanned at the final stop-the-world remark.
                 * Private registered stack slots need atomic publication but
                 * cannot mutate a black heap object. No safepoint is removed;
                 * heap stores and escaping addresses keep their insertion barrier. */
                if (LLVMIsAAllocaInst(dst)
                    && LLVMGetTypeKind(LLVMGetAllocatedType(dst)) == LLVMPointerTypeKind
                    && (LLVMIsNull(value) || (getenv("EGRET_LOCAL_ROOT_STORES")
                        && eg_private_shadow_root(cg, dst)))) {
                    LLVMPositionBuilderBefore(builder, instruction);
                    LLVMValueRef store = LLVMBuildStore(builder, value, dst);
                    LLVMSetOrdering(store, LLVMAtomicOrderingRelease);
                    LLVMSetAlignment(store, (unsigned)LLVMABISizeOfType(LLVMGetModuleDataLayout(cg->module), LLVMTypeOf(value)));
                    LLVMInstructionEraseFromParent(instruction);
                }
            }
            instruction = next;
        }
    }
    LLVMDisposeBuilder(builder);
}

void eg_compact_gc_roots(eg_codegen_t* cg) {
    if (!cg || !cg->gc_enabled || getenv("EGRET_KEEP_ROOT_RESERVE")) return;
    for (LLVMValueRef fn = LLVMGetFirstFunction(cg->module); fn; fn = LLVMGetNextFunction(fn)) {
        LLVMBasicBlockRef entry = LLVMGetFirstBasicBlock(fn);
        if (!entry) continue;
        LLVMValueRef instruction = LLVMGetFirstInstruction(entry);
        while (instruction) {
            LLVMValueRef next = LLVMGetNextInstruction(instruction);
            if (LLVMIsAAllocaInst(instruction) && strcmp(LLVMGetValueName(instruction), "gc.slots") == 0) {
                eg_compact_one_root_array(cg, instruction);
            }
            instruction = next;
        }
        eg_lower_null_root_stores(cg, fn);
    }
}

int eg_optimize_scalar_ir(eg_codegen_t* cg, LLVMTargetMachineRef tm) {
    int scalar_all = getenv("EGRET_SCALAR_IR_OPT") != NULL;
    int hot_vm = getenv("EGRET_HOT_VM_O1") != NULL;
    if (!scalar_all && !hot_vm) return 1;
#if !EG_LLVM_HAS_PASS_BUILDER
    (void)cg;
    (void)tm;
    return 1;
#else
    /* Keep O0 target code generation and avoid inlining/global transforms.
     * These bounded scalar passes remove frontend temporaries inside each
     * function, which is especially valuable in the VM dispatch loop. */
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    if (!options) return 0;
    LLVMPassBuilderOptionsSetVerifyEach(options, 0);
    LLVMErrorRef error = NULL;
    if (hot_vm) {
#if LLVM_VERSION_MAJOR >= 19
        static const char* suffixes[] = {
            "VbsBytecodeRunner__execute_pc",
            "VbsBytecodeRunner__execute_block",
            "evaluator_calls_call_fast_scalar_values",
            "evaluator_calls_call_user_routine_values"
        };
        for (LLVMValueRef fn = LLVMGetFirstFunction(cg->module); fn && !error; fn = LLVMGetNextFunction(fn)) {
            size_t length = 0;
            const char* name = LLVMGetValueName2(fn, &length);
            int selected = 0;
            for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
                size_t suffix_length = strlen(suffixes[i]);
                if (length >= suffix_length && memcmp(name + length - suffix_length, suffixes[i], suffix_length) == 0) {
                    selected = 1;
                    break;
                }
            }
            if (selected) error = LLVMRunPassesOnFunction(fn, "default<O1>", tm, options);
        }
#endif
    } else {
        error = LLVMRunPasses(
            cg->module, "function(mem2reg,sroa,instcombine,simplifycfg)", tm, options);
    }
    LLVMDisposePassBuilderOptions(options);
    if (!error) return 1;
    char* message = LLVMGetErrorMessage(error);
    fprintf(stderr, "egret scalar IR optimization failed: %s\n", message ? message : "<unknown>");
    if (message) LLVMDisposeErrorMessage(message);
    return 0;
#endif
}
