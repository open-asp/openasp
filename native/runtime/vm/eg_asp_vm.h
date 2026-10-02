// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_ASP_VM_H
#define EG_ASP_VM_H

#include <stdint.h>

enum {
    EG_ASP_VM_HALT = -1,
    EG_ASP_VM_INVALID = -2,
    EG_ASP_VM_TRAP = 0,
    EG_ASP_VM_GC_POLL = 1,
    EG_ASP_VM_YIELD = 2
};

int64_t eg_asp_vm_native_enabled(void);
int64_t eg_asp_vm_class_enabled(void);
int64_t eg_asp_vm_fused_numeric_compare(int64_t op,
    int64_t left_kind, int64_t left_integer, double left_floating,
    int64_t right_kind, int64_t right_integer, double right_floating);
int64_t eg_asp_vm_fused_integer_binary(int64_t op, int64_t left,
    int64_t right);

/*
 * Non-negative results encode (pc * 4 + reason). TRAP asks Egret to execute
 * that opcode, GC_POLL returns at an Egret safepoint, and YIELD bounds native
 * runs. HALT and INVALID are negative terminal values.
 */
int64_t eg_asp_vm_dispatch(int64_t arena_handle, int64_t program_handle,
                           int64_t entry_pc, int64_t limit_pc);
int64_t eg_asp_vm_slot_set(int64_t arena_handle, int64_t slot_index,
                           int64_t kind, int64_t integer, double floating);
int64_t eg_asp_vm_slot_set_string(int64_t arena_handle, int64_t slot_index,
                                  void* string_object);
int64_t eg_asp_vm_take_dirty_slot(int64_t arena_handle);
int64_t eg_asp_vm_slot_kind(int64_t arena_handle, int64_t slot_index);
int64_t eg_asp_vm_slot_integer(int64_t arena_handle, int64_t slot_index);
double eg_asp_vm_slot_floating(int64_t arena_handle, int64_t slot_index);
void* eg_asp_vm_slot_string(int64_t arena_handle, int64_t slot_index);

/*
 * Fast scalar routine frames contain no managed references.  Managed Variant
 * slots stay in Egret's GC-visible reusable vector; C owns only control-flow
 * metadata and accounting.
 */
int64_t eg_asp_vm_routine_enter(int64_t arena_handle, int64_t routine_id,
                                int64_t return_pc,
                                int64_t limit_pc, int64_t stack_base,
                                int64_t slot_base, int64_t slot_count);
int64_t eg_asp_vm_routine_leave(int64_t arena_handle);
int64_t eg_asp_vm_routine_bind(int64_t arena_handle, int64_t me_object_id,
    int64_t return_dest, int64_t copyback_mask);
int64_t eg_asp_vm_routine_bind_program(int64_t arena_handle,
    int64_t caller_dynamic_active, int64_t caller_dynamic_index);
int64_t eg_asp_vm_routine_bind_error(int64_t arena_handle,
    int64_t error_mode, int64_t error_number, int64_t resume_target_pc);
int64_t eg_asp_vm_routine_caller_error_mode(int64_t arena_handle);
int64_t eg_asp_vm_routine_program_switched(int64_t arena_handle);
int64_t eg_asp_vm_routine_caller_dynamic_active(int64_t arena_handle);
int64_t eg_asp_vm_routine_caller_dynamic_index(int64_t arena_handle);

/*
 * Classifies an already compiler-bound object member and prepares scalar
 * frame metadata. The managed VM retains all GC-visible argument values.
 * Returns the selected routine/handler id, or -1 for the compatibility path.
 */
int64_t eg_asp_vm_member_select(int64_t arena_handle, int64_t member_id,
    int64_t object_kind, int64_t candidate_id, int64_t argument_count,
    int64_t member_flags);
int64_t eg_asp_vm_object_call_cache_lookup(int64_t arena_handle,
    int64_t caller_program_id, int64_t callsite_pc, int64_t class_hash,
    int64_t member_id);
int64_t eg_asp_vm_object_call_cache_bind(int64_t arena_handle,
    int64_t caller_program_id, int64_t callsite_pc, int64_t class_hash,
    int64_t member_id, int64_t callee_program_id, int64_t routine_id);

#endif
