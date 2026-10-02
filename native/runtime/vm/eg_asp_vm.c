// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_asp_vm.h"

#include "eg_asp_request_arena.h"
#include "eg_asp_vbc.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t len;
    char* data;
} eg_asp_string;

typedef struct {
    int64_t len;
    const char* data;
} eg_asp_native_slice;

/*
 * The C VM executes only the allocation-free scalar subset. Any instruction
 * requiring full Variant semantics returns a packed resume pc/reason to Egret,
 * which completes the operation and may re-enter native dispatch afterward.
 */
extern void* eg_string_from_bytes(const void* data, int64_t len);

enum {
    EG_ASP_VBC_HALT = 0,
    EG_ASP_VBC_JUMP = 2,
    EG_ASP_VBC_JUMP_IF_FALSE = 3,
    EG_ASP_VBC_POP = 4,
    EG_ASP_VBC_DUP = 5,
    EG_ASP_VBC_PUSH_EXPR_LEAF = 10,
    EG_ASP_VBC_NEG = 11,
    EG_ASP_VBC_CONCAT = 12,
    EG_ASP_VBC_EQ = 13,
    EG_ASP_VBC_NE = 14,
    EG_ASP_VBC_LT = 15,
    EG_ASP_VBC_LE = 16,
    EG_ASP_VBC_GT = 17,
    EG_ASP_VBC_GE = 18,
    EG_ASP_VBC_ADD = 19,
    EG_ASP_VBC_SUB = 20,
    EG_ASP_VBC_MUL = 21,
    EG_ASP_VBC_DIV = 22,
    EG_ASP_VBC_INT_DIV = 23,
    EG_ASP_VBC_MOD = 24,
    EG_ASP_VBC_NOT = 25,
    EG_ASP_VBC_AND = 26,
    EG_ASP_VBC_OR = 27,
    EG_ASP_VBC_XOR = 28,
    EG_ASP_VBC_PUSH_SCALAR_SLOT = 49,
    EG_ASP_VBC_ASSIGN_SCALAR_SLOT = 50,
    EG_ASP_VBC_POW = 53,
    EG_ASP_VBC_CONCAT_N = 55,
    EG_ASP_VM_RUN_BUDGET = 4096,
    EG_ASP_VM_POLL_MASK = 255,
    EG_ASP_VM_FRAME_CAPACITY = 1024,
    EG_ASP_VM_SLOT_CAPACITY = 65536,
    EG_ASP_VM_INITIAL_FRAME_CAPACITY = 128,
    EG_ASP_VM_INITIAL_SLOT_CAPACITY = 4096,
    EG_ASP_VM_DIRTY_CAPACITY = 8192,
    /* VariantEmpty is zero and therefore cannot double as the native
       uninitialized marker. Keep it tagged explicitly inside C frames. */
    EG_ASP_NATIVE_EMPTY = 10,
    EG_ASP_NATIVE_STRING_SLICE = 11
};

/*
 * Newer runtimes provide this latch strongly. The weak zero-initialized
 * definition keeps older runtimes compatible; generated/runtime code can
 * still set the same process-wide symbol when latch polling is enabled.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
int eg_gc_poll_requested;

static int eg_asp_vm_false_value(const char* value) {
    return value && (!strcmp(value, "0") || !strcmp(value, "off") ||
                     !strcmp(value, "false") || !strcmp(value, "no"));
}

int64_t eg_asp_vm_native_enabled(void) {
    static int enabled = -1;
    int cached = __atomic_load_n(&enabled, __ATOMIC_ACQUIRE);
    if (cached < 0) {
        const char* value = getenv("EGRET_ASP_C_VM");
        cached = value && *value && eg_asp_vm_false_value(value) ? 0 : 1;
        __atomic_store_n(&enabled, cached, __ATOMIC_RELEASE);
    }
    if (!cached) return 0;
    return 1;
}

int64_t eg_asp_vm_class_enabled(void) {
    static int enabled = -1;
    int cached = __atomic_load_n(&enabled, __ATOMIC_ACQUIRE);
    if (cached < 0) {
        const char* value = getenv("EGRET_ASP_C_VM_CLASS");
        cached = value && *value && eg_asp_vm_false_value(value) ? 0 : 1;
        __atomic_store_n(&enabled, cached, __ATOMIC_RELEASE);
    }
    return cached;
}

/*
 * Execute scalar-load/scalar-load/compare/branch without allocating managed
 * Variant objects. Return 2 when exact VBScript coercion needs managed code.
 * Variant ABI kinds are Bool=2, Int=3, Float=4.
 */
int64_t eg_asp_vm_fused_numeric_compare(int64_t op,
                                        int64_t left_kind,
                                        int64_t left_integer,
                                        double left_floating,
                                        int64_t right_kind,
                                        int64_t right_integer,
                                        double right_floating) {
    if (left_kind < 2 || left_kind > 4 ||
        right_kind < 2 || right_kind > 4 ||
        op < 13 || op > 18) {
        return 2;
    }
    double left = left_kind == 4
        ? left_floating
        : (double)(left_kind == 2 ? (left_integer ? -1 : 0) : left_integer);
    double right = right_kind == 4
        ? right_floating
        : (double)(right_kind == 2 ? (right_integer ? -1 : 0) : right_integer);
    int comparison = left < right ? -1 : (left > right ? 1 : 0);
    int result = 0;
    switch (op) {
        case 13: result = comparison == 0; break;
        case 14: result = comparison != 0; break;
        case 15: result = comparison < 0; break;
        case 16: result = comparison <= 0; break;
        case 17: result = comparison > 0; break;
        case 18: result = comparison >= 0; break;
        default: return 2;
    }
    return result;
}

int64_t eg_asp_vm_fused_integer_binary(int64_t op,
                                       int64_t left,
                                       int64_t right) {
    uint64_t a = (uint64_t)left;
    uint64_t b = (uint64_t)right;
    int64_t result;
    switch (op) {
        case 19: result = (int64_t)(a + b); break;
        case 20: result = (int64_t)(a - b); break;
        case 21: result = (int64_t)(a * b); break;
        case 24:
            result = right == 0 ? 0
                : (left == INT64_MIN && right == -1 ? 0 : left % right);
            break;
        case 26: result = left & right; break;
        case 27: result = left | right; break;
        case 28: result = left ^ right; break;
        default: return 0;
    }
    return result;
}

/* Pack a resumable program counter and two-bit exit reason into one ABI word. */
static int64_t eg_asp_vm_result(int64_t pc, int reason) {
    if (pc < 0 || pc > (INT64_MAX - 3) / 4) return EG_ASP_VM_INVALID;
    return pc * 4 + reason;
}

/*
 * One run state lives inside each request arena. operands are transient stack
 * values, slots hold scalar locals, dirty_slots synchronize writes back to
 * managed Scope, and frames preserve nested routine state. The small object
 * call cache is direct-mapped by replacement order and validated on lookup.
 */
typedef struct eg_asp_vm_run_state {
    int64_t pc;
    int64_t limit_pc;
    eg_asp_native_value* operands;
    eg_asp_native_value* slots;
    int64_t* dirty_slots;
    eg_asp_native_call_frame* frames;
    int64_t operand_capacity;
    int64_t frame_capacity;
    int64_t frame_depth;
    int64_t operand_depth;
    int64_t slot_capacity;
    int64_t dirty_count;
    eg_asp_native_call_frame member_frame;
    struct {
        int64_t caller_program_id;
        int64_t callsite_pc;
        int64_t class_hash;
        int64_t member_id;
        int64_t callee_program_id;
        int64_t routine_id;
    } object_call_cache[32];
    int64_t object_call_cache_next;
} eg_asp_vm_run_state;

/* Lazily allocate VM arrays from the request arena; no pointer may escape it. */
static eg_asp_vm_run_state* eg_asp_vm_state(int64_t arena_handle) {
    eg_asp_vm_run_state* state = (eg_asp_vm_run_state*)
        eg_asp_request_arena_vm_state(arena_handle, sizeof(*state),
                                      _Alignof(eg_asp_vm_run_state));
    if (!state) return NULL;
    if (!state->operands) {
        state->operand_capacity = 64;
        state->frame_capacity = EG_ASP_VM_INITIAL_FRAME_CAPACITY;
        state->operands = (eg_asp_native_value*)(intptr_t)
            eg_asp_request_arena_alloc_operands(arena_handle,
                                                state->operand_capacity);
        state->frames = (eg_asp_native_call_frame*)(intptr_t)
            eg_asp_request_arena_alloc_call_frames(arena_handle,
                                                   state->frame_capacity);
        state->slots = (eg_asp_native_value*)(intptr_t)
            eg_asp_request_arena_alloc_native_values(
                arena_handle, EG_ASP_VM_INITIAL_SLOT_CAPACITY);
        state->dirty_slots = (int64_t*)(intptr_t)
            eg_asp_request_arena_alloc_scratch(
                arena_handle, EG_ASP_VM_DIRTY_CAPACITY * sizeof(int64_t),
                _Alignof(int64_t));
        state->slot_capacity = EG_ASP_VM_INITIAL_SLOT_CAPACITY;
        if (!state->operands || !state->frames || !state->slots ||
            !state->dirty_slots) return NULL;
    }
    return state;
}

static int eg_asp_vm_ensure_slot_capacity(int64_t arena_handle,
                                          eg_asp_vm_run_state* state,
                                          int64_t needed) {
    if (needed <= state->slot_capacity) return 1;
    if (needed <= 0 || needed > EG_ASP_VM_SLOT_CAPACITY) return 0;
    int64_t capacity = state->slot_capacity;
    while (capacity < needed) capacity *= 2;
    if (capacity > EG_ASP_VM_SLOT_CAPACITY) capacity = EG_ASP_VM_SLOT_CAPACITY;
    eg_asp_native_value* slots = (eg_asp_native_value*)(intptr_t)
        eg_asp_request_arena_alloc_native_values(arena_handle, capacity);
    if (!slots) return 0;
    memcpy(slots, state->slots,
           (size_t)state->slot_capacity * sizeof(*slots));
    state->slots = slots;
    state->slot_capacity = capacity;
    return 1;
}

static int eg_asp_vm_ensure_frame_capacity(int64_t arena_handle,
                                           eg_asp_vm_run_state* state) {
    if (state->frame_depth < state->frame_capacity) return 1;
    if (state->frame_capacity >= EG_ASP_VM_FRAME_CAPACITY) return 0;
    int64_t capacity = state->frame_capacity * 2;
    if (capacity > EG_ASP_VM_FRAME_CAPACITY)
        capacity = EG_ASP_VM_FRAME_CAPACITY;
    eg_asp_native_call_frame* frames =
        (eg_asp_native_call_frame*)(intptr_t)
            eg_asp_request_arena_alloc_call_frames(arena_handle, capacity);
    if (!frames) return 0;
    memcpy(frames, state->frames,
           (size_t)state->frame_depth * sizeof(*frames));
    state->frames = frames;
    state->frame_capacity = capacity;
    return 1;
}

static int eg_asp_vm_scalar_kind(int64_t kind) {
    return kind == 1 || kind == 2 || kind == 3 || kind == 4 || kind == 5 ||
           kind == EG_ASP_NATIVE_EMPTY ||
           kind == EG_ASP_NATIVE_STRING_SLICE;
}

int64_t eg_asp_vm_slot_set(int64_t arena_handle, int64_t slot_index,
                           int64_t kind, int64_t integer, double floating) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state || slot_index < 0 ||
        !eg_asp_vm_ensure_slot_capacity(arena_handle, state, slot_index + 1))
        return 0;
    eg_asp_native_value* slot = &state->slots[slot_index];
    if (kind == 0) kind = EG_ASP_NATIVE_EMPTY;
    slot->kind = eg_asp_vm_scalar_kind(kind) ? (uint32_t)kind : 0;
    slot->flags = 0;
    if (kind == 4) slot->as.floating = floating;
    else slot->as.integer = integer;
    return slot->kind != 0;
}

int64_t eg_asp_vm_slot_set_string(int64_t arena_handle, int64_t slot_index,
                                  void* string_object) {
    return eg_asp_vm_slot_set(arena_handle, slot_index, 5,
                              (int64_t)(intptr_t)string_object, 0.0);
}

int64_t eg_asp_vm_take_dirty_slot(int64_t arena_handle) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state || state->dirty_count <= 0) return -1;
    int64_t index = state->dirty_slots[--state->dirty_count];
    if (index >= 0 && index < state->slot_capacity)
        state->slots[index].flags &= ~UINT32_C(1);
    return index;
}

int64_t eg_asp_vm_slot_kind(int64_t arena_handle, int64_t slot_index) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    return state && slot_index >= 0 && slot_index < state->slot_capacity
        ? state->slots[slot_index].kind : 0;
}

int64_t eg_asp_vm_slot_integer(int64_t arena_handle, int64_t slot_index) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    return state && slot_index >= 0 && slot_index < state->slot_capacity
        ? state->slots[slot_index].as.integer : 0;
}

double eg_asp_vm_slot_floating(int64_t arena_handle, int64_t slot_index) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    return state && slot_index >= 0 && slot_index < state->slot_capacity
        ? state->slots[slot_index].as.floating : 0.0;
}

void* eg_asp_vm_slot_string(int64_t arena_handle, int64_t slot_index) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state || slot_index < 0 || slot_index >= state->slot_capacity)
        return NULL;
    eg_asp_native_value* value = &state->slots[slot_index];
    if (value->kind == 5)
        return (void*)(intptr_t)value->as.integer;
    if (value->kind == EG_ASP_NATIVE_STRING_SLICE) {
        eg_asp_native_slice* slice =
            (eg_asp_native_slice*)(intptr_t)value->as.integer;
        return slice ? eg_string_from_bytes(slice->data, slice->len) : NULL;
    }
    return NULL;
}

static int eg_asp_vm_truthy(const eg_asp_native_value* value) {
    if (value->kind == 1 || value->kind == EG_ASP_NATIVE_EMPTY) return 0;
    if (value->kind == 4) return value->as.floating != 0.0;
    return value->as.integer != 0;
}

static double eg_asp_vm_number(const eg_asp_native_value* value) {
    if (value->kind == EG_ASP_NATIVE_EMPTY) return 0.0;
    if (value->kind == 4) return value->as.floating;
    if (value->kind == 2) return value->as.integer ? -1.0 : 0.0;
    return (double)value->as.integer;
}

static int64_t eg_asp_vm_integer(const eg_asp_native_value* value) {
    if (value->kind == EG_ASP_NATIVE_EMPTY) return 0;
    if (value->kind == 4) return (int64_t)value->as.floating;
    if (value->kind == 2) return value->as.integer ? -1 : 0;
    return value->as.integer;
}

static int eg_asp_vm_string_view(const eg_asp_native_value* value,
                                 const char** data, int64_t* len,
                                 char* scratch, size_t scratch_size) {
    if (value->kind == 5) {
        eg_asp_string* string =
            (eg_asp_string*)(intptr_t)value->as.integer;
        if (!string) return 0;
        *data = string->data;
        *len = string->len;
        return 1;
    }
    if (value->kind == EG_ASP_NATIVE_STRING_SLICE) {
        eg_asp_native_slice* slice =
            (eg_asp_native_slice*)(intptr_t)value->as.integer;
        if (!slice) return 0;
        *data = slice->data;
        *len = slice->len;
        return 1;
    }
    if (value->kind == EG_ASP_NATIVE_EMPTY) {
        *data = "";
        *len = 0;
        return 1;
    }
    int written;
    if (value->kind == 2) {
        const char* boolean = value->as.integer ? "True" : "False";
        *data = boolean;
        *len = value->as.integer ? 4 : 5;
        return 1;
    }
    if (value->kind == 3)
        written = snprintf(scratch, scratch_size, "%lld",
                           (long long)value->as.integer);
    else if (value->kind == 4)
        written = snprintf(scratch, scratch_size, "%.15g",
                           value->as.floating);
    else
        return 0;
    if (written < 0 || (size_t)written >= scratch_size) return 0;
    *data = scratch;
    *len = written;
    return 1;
}

static int eg_asp_vm_concat(int64_t arena_handle,
                            eg_asp_native_value* left,
                            const eg_asp_native_value* right) {
    const char* a;
    const char* b;
    int64_t a_len;
    int64_t b_len;
    char a_scratch[64];
    char b_scratch[64];
    if (!eg_asp_vm_string_view(left, &a, &a_len, a_scratch,
                               sizeof(a_scratch)) ||
        !eg_asp_vm_string_view(right, &b, &b_len, b_scratch,
                               sizeof(b_scratch)) ||
        a_len < 0 || b_len < 0 || a_len > INT64_MAX - b_len)
        return 0;
    int64_t total = a_len + b_len;
    eg_asp_native_slice* slice =
        (eg_asp_native_slice*)(intptr_t)eg_asp_request_arena_alloc(
            arena_handle, sizeof(*slice), _Alignof(eg_asp_native_slice));
    char* bytes = total > 0
        ? (char*)(intptr_t)eg_asp_request_arena_alloc(
            arena_handle, total, _Alignof(char))
        : NULL;
    if (!slice || (total > 0 && !bytes)) return 0;
    if (a_len > 0) memcpy(bytes, a, (size_t)a_len);
    if (b_len > 0) memcpy(bytes + a_len, b, (size_t)b_len);
    slice->data = total > 0 ? bytes : "";
    slice->len = total;
    left->kind = EG_ASP_NATIVE_STRING_SLICE;
    left->flags = 0;
    left->as.integer = (int64_t)(intptr_t)slice;
    return 1;
}

static int eg_asp_vm_stringish(const eg_asp_native_value* value) {
    return value->kind == 5 || value->kind == EG_ASP_NATIVE_STRING_SLICE ||
           value->kind == EG_ASP_NATIVE_EMPTY;
}

static int eg_asp_vm_string_compare(const eg_asp_native_value* left,
                                    const eg_asp_native_value* right,
                                    int* comparison) {
    const char* a;
    const char* b;
    int64_t a_len;
    int64_t b_len;
    char a_scratch[64];
    char b_scratch[64];
    if (!eg_asp_vm_stringish(left) || !eg_asp_vm_stringish(right) ||
        !eg_asp_vm_string_view(left, &a, &a_len, a_scratch,
                               sizeof(a_scratch)) ||
        !eg_asp_vm_string_view(right, &b, &b_len, b_scratch,
                               sizeof(b_scratch)))
        return 0;
    int64_t common = a_len < b_len ? a_len : b_len;
    int compared = common > 0 ? memcmp(a, b, (size_t)common) : 0;
    *comparison = compared < 0 ? -1 : (compared > 0 ? 1 :
        (a_len < b_len ? -1 : (a_len > b_len ? 1 : 0)));
    return 1;
}

static int eg_asp_vm_push(eg_asp_vm_run_state* state,
                          const eg_asp_native_value* value) {
    if (state->operand_depth >= state->operand_capacity) return 0;
    state->operands[state->operand_depth++] = *value;
    state->operands[state->operand_depth - 1].flags = 0;
    return 1;
}

static int eg_asp_vm_mark_dirty(eg_asp_vm_run_state* state, int64_t index) {
    if (index < 0 || index >= state->slot_capacity) return 0;
    if (state->slots[index].flags & UINT32_C(1)) return 1;
    if (state->dirty_count >= EG_ASP_VM_DIRTY_CAPACITY) return 0;
    state->slots[index].flags |= UINT32_C(1);
    state->dirty_slots[state->dirty_count++] = index;
    return 1;
}

/*
 * Execute at most EG_ASP_VM_RUN_BUDGET instructions per call. Polling the GC
 * latch and yielding on unsupported values bounds latency and keeps managed GC
 * coordination outside this allocation-free loop.
 */
int64_t eg_asp_vm_dispatch(int64_t arena_handle, int64_t program_handle,
                           int64_t entry_pc, int64_t limit_pc) {
    const eg_asp_vbc_program* program =
        eg_asp_vbc_from_handle(program_handle);
    if (!program) return EG_ASP_VM_INVALID;
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state) return EG_ASP_VM_INVALID;
    int64_t count = eg_asp_vbc_instruction_count(program);
    if (limit_pc < 0 || limit_pc > count) limit_pc = count;
    if (entry_pc < 0 || entry_pc >= limit_pc) return EG_ASP_VM_HALT;
    if (!eg_asp_vbc_native_safe_start(program, entry_pc))
        return eg_asp_vm_result(entry_pc, EG_ASP_VM_TRAP);

    state->pc = entry_pc;
    state->limit_pc = limit_pc;
    state->operand_depth = 0;
    int64_t pc = state->pc;
    int64_t region_start = pc;
    for (int steps = 0; steps < EG_ASP_VM_RUN_BUDGET; steps++) {
        state->pc = pc;
        /*
         * Native runs are bounded to 4096 scalar instructions and contain no
         * managed pointers. Poll at the managed trap/yield boundary; returning
         * the same PC for a sticky latch can otherwise livelock a request.
         */
        int64_t op;
        int64_t arg0;
        int64_t arg1;
        if (!eg_asp_vbc_instruction(program, pc, &op, &arg0, &arg1)) {
            return EG_ASP_VM_INVALID;
        }
        switch (op) {
            case EG_ASP_VBC_HALT:
                return EG_ASP_VM_HALT;
            case EG_ASP_VBC_JUMP:
                pc = arg0;
                state->pc = pc;
                if (pc < 0) return EG_ASP_VM_INVALID;
                if (pc >= limit_pc)
                    return eg_asp_vm_result(pc, EG_ASP_VM_TRAP);
                break;
            case EG_ASP_VBC_PUSH_EXPR_LEAF: {
                eg_asp_native_value value = {0};
                int64_t kind, integer;
                double floating;
                if (!eg_asp_vbc_literal(program, arg0, &kind, &integer,
                                        &floating) ||
                    !eg_asp_vm_scalar_kind(kind)) {
                    state->operand_depth = 0;
                    return eg_asp_vm_result(region_start, EG_ASP_VM_TRAP);
                }
                value.kind = (uint32_t)kind;
                if (kind == 4) value.as.floating = floating;
                else value.as.integer = integer;
                if (!eg_asp_vm_push(state, &value))
                    return EG_ASP_VM_INVALID;
                pc++;
                break;
            }
            case EG_ASP_VBC_PUSH_SCALAR_SLOT: {
                if (state->frame_depth <= 0) {
                    state->operand_depth = 0;
                    return eg_asp_vm_result(region_start, EG_ASP_VM_TRAP);
                }
                eg_asp_native_call_frame* frame =
                    &state->frames[state->frame_depth - 1];
                int64_t index = frame->slot_base + arg0;
                if (arg0 < 0 || arg0 >= frame->slot_count || index < 0 ||
                    index >= state->slot_capacity ||
                    !eg_asp_vm_scalar_kind(state->slots[index].kind)) {
                    state->operand_depth = 0;
                    return eg_asp_vm_result(region_start, EG_ASP_VM_TRAP);
                }
                if (!eg_asp_vm_push(state, &state->slots[index]))
                    return EG_ASP_VM_INVALID;
                pc++;
                break;
            }
            case EG_ASP_VBC_DUP:
                if (state->operand_depth <= 0 ||
                    !eg_asp_vm_push(
                        state, &state->operands[state->operand_depth - 1]))
                    return EG_ASP_VM_INVALID;
                pc++;
                break;
            case EG_ASP_VBC_NEG:
            case EG_ASP_VBC_NOT: {
                if (state->operand_depth <= 0) return EG_ASP_VM_INVALID;
                eg_asp_native_value* value =
                    &state->operands[state->operand_depth - 1];
                if (value->kind == 1) {
                    state->operand_depth = 0;
                    return eg_asp_vm_result(region_start, EG_ASP_VM_TRAP);
                }
                if (op == EG_ASP_VBC_NEG) {
                    if (value->kind == 4)
                        value->as.floating = -value->as.floating;
                    else {
                        value->kind = 3;
                        value->as.integer = -eg_asp_vm_integer(value);
                    }
                } else if (value->kind == 2) {
                    value->as.integer = !value->as.integer;
                } else {
                    value->kind = 3;
                    value->as.integer = ~eg_asp_vm_integer(value);
                }
                pc++;
                break;
            }
            case EG_ASP_VBC_CONCAT: {
                if (state->operand_depth < 2) return EG_ASP_VM_INVALID;
                eg_asp_native_value right =
                    state->operands[state->operand_depth - 1];
                eg_asp_native_value* left =
                    &state->operands[state->operand_depth - 2];
                if (left->kind == 1 || right.kind == 1 ||
                    !eg_asp_vm_concat(arena_handle, left, &right)) {
                    state->operand_depth = 0;
                    return eg_asp_vm_result(region_start, EG_ASP_VM_TRAP);
                }
                state->operand_depth--;
                pc++;
                break;
            }
            case EG_ASP_VBC_CONCAT_N: {
                if (arg0 <= 0 || arg0 > state->operand_depth)
                    return EG_ASP_VM_INVALID;
                int64_t part_start = state->operand_depth - arg0;
                eg_asp_native_value* combined = &state->operands[part_start];
                for (int64_t i = part_start + 1;
                     i < state->operand_depth; i++) {
                    if (combined->kind == 1 ||
                        state->operands[i].kind == 1 ||
                        !eg_asp_vm_concat(arena_handle, combined,
                                          &state->operands[i])) {
                        state->operand_depth = 0;
                        return eg_asp_vm_result(region_start,
                                                EG_ASP_VM_TRAP);
                    }
                }
                state->operand_depth = part_start + 1;
                pc++;
                break;
            }
            case EG_ASP_VBC_EQ: case EG_ASP_VBC_NE:
            case EG_ASP_VBC_LT: case EG_ASP_VBC_LE:
            case EG_ASP_VBC_GT: case EG_ASP_VBC_GE:
            case EG_ASP_VBC_ADD: case EG_ASP_VBC_SUB:
            case EG_ASP_VBC_MUL: case EG_ASP_VBC_DIV:
            case EG_ASP_VBC_INT_DIV: case EG_ASP_VBC_MOD:
            case EG_ASP_VBC_AND: case EG_ASP_VBC_OR:
            case EG_ASP_VBC_XOR: case EG_ASP_VBC_POW: {
                if (state->operand_depth < 2) return EG_ASP_VM_INVALID;
                eg_asp_native_value right =
                    state->operands[state->operand_depth - 1];
                eg_asp_native_value* left =
                    &state->operands[state->operand_depth - 2];
                if (left->kind == 1 || right.kind == 1) {
                    state->operand_depth = 0;
                    return eg_asp_vm_result(region_start, EG_ASP_VM_TRAP);
                }
                int compare = 0;
                if (op >= EG_ASP_VBC_EQ && op <= EG_ASP_VBC_GE) {
                    if (eg_asp_vm_stringish(left) ||
                        eg_asp_vm_stringish(&right)) {
                        if (!eg_asp_vm_string_compare(left, &right, &compare)) {
                            state->operand_depth = 0;
                            return eg_asp_vm_result(region_start,
                                                    EG_ASP_VM_TRAP);
                        }
                    } else {
                        double a = eg_asp_vm_number(left);
                        double b = eg_asp_vm_number(&right);
                        compare = a < b ? -1 : (a > b ? 1 : 0);
                    }
                    left->kind = 2;
                    left->as.integer =
                        op == EG_ASP_VBC_EQ ? compare == 0 :
                        op == EG_ASP_VBC_NE ? compare != 0 :
                        op == EG_ASP_VBC_LT ? compare < 0 :
                        op == EG_ASP_VBC_LE ? compare <= 0 :
                        op == EG_ASP_VBC_GT ? compare > 0 : compare >= 0;
                } else if (op == EG_ASP_VBC_DIV ||
                           op == EG_ASP_VBC_POW) {
                    if (eg_asp_vm_stringish(left) ||
                        eg_asp_vm_stringish(&right)) {
                        state->operand_depth = 0;
                        return eg_asp_vm_result(region_start,
                                                EG_ASP_VM_TRAP);
                    }
                    double dividend = eg_asp_vm_number(left);
                    double divisor = eg_asp_vm_number(&right);
                    if (divisor == 0.0) {
                        if (op == EG_ASP_VBC_POW) {
                            left->kind = 4;
                            left->as.floating = pow(dividend, divisor);
                            state->operand_depth--;
                            pc++;
                            break;
                        }
                        state->operand_depth = 0;
                        return eg_asp_vm_result(region_start,
                                                EG_ASP_VM_TRAP);
                    }
                    left->kind = 4;
                    left->as.floating = op == EG_ASP_VBC_DIV
                        ? dividend / divisor
                        : pow(dividend, divisor);
                } else {
                    if (eg_asp_vm_stringish(left) ||
                        eg_asp_vm_stringish(&right)) {
                        state->operand_depth = 0;
                        return eg_asp_vm_result(region_start,
                                                EG_ASP_VM_TRAP);
                    }
                    int64_t a = eg_asp_vm_integer(left);
                    int64_t b = eg_asp_vm_integer(&right);
                    if ((op == EG_ASP_VBC_INT_DIV ||
                         op == EG_ASP_VBC_MOD) && b == 0) {
                        state->operand_depth = 0;
                        return eg_asp_vm_result(region_start,
                                                EG_ASP_VM_TRAP);
                    }
                    if ((op == EG_ASP_VBC_ADD ||
                         op == EG_ASP_VBC_SUB ||
                         op == EG_ASP_VBC_MUL) &&
                        (left->kind == 4 || right.kind == 4)) {
                        double a_number = eg_asp_vm_number(left);
                        double b_number = eg_asp_vm_number(&right);
                        left->kind = 4;
                        left->as.floating =
                            op == EG_ASP_VBC_ADD ? a_number + b_number :
                            (op == EG_ASP_VBC_SUB ? a_number - b_number :
                             a_number * b_number);
                    } else {
                        left->kind = 3;
                        switch (op) {
                            case EG_ASP_VBC_ADD:
                                left->as.integer =
                                    (int64_t)((uint64_t)a + (uint64_t)b);
                                break;
                            case EG_ASP_VBC_SUB:
                                left->as.integer =
                                    (int64_t)((uint64_t)a - (uint64_t)b);
                                break;
                            case EG_ASP_VBC_MUL:
                                left->as.integer =
                                    (int64_t)((uint64_t)a * (uint64_t)b);
                                break;
                            case EG_ASP_VBC_INT_DIV:
                                left->as.integer =
                                    a == INT64_MIN && b == -1 ? 0 : a / b;
                                break;
                            case EG_ASP_VBC_MOD:
                                left->as.integer =
                                    a == INT64_MIN && b == -1 ? 0 : a % b;
                                break;
                            case EG_ASP_VBC_AND: left->as.integer = a & b; break;
                            case EG_ASP_VBC_OR: left->as.integer = a | b; break;
                            case EG_ASP_VBC_XOR: left->as.integer = a ^ b; break;
                            default:
                                state->operand_depth = 0;
                                return eg_asp_vm_result(
                                    region_start, EG_ASP_VM_TRAP);
                        }
                    }
                }
                state->operand_depth--;
                pc++;
                break;
            }
            case EG_ASP_VBC_ASSIGN_SCALAR_SLOT: {
                if (arg1 != 0 || state->frame_depth <= 0 ||
                    state->operand_depth <= 0) {
                    state->operand_depth = 0;
                    return eg_asp_vm_result(region_start, EG_ASP_VM_TRAP);
                }
                eg_asp_native_call_frame* frame =
                    &state->frames[state->frame_depth - 1];
                int64_t index = frame->slot_base + arg0;
                if (arg0 < 0 || arg0 >= frame->slot_count || index < 0 ||
                    index >= state->slot_capacity) {
                    return EG_ASP_VM_INVALID;
                }
                state->slots[index] =
                    state->operands[--state->operand_depth];
                state->slots[index].flags = 0;
                if (!eg_asp_vm_mark_dirty(state, index))
                    return EG_ASP_VM_INVALID;
                pc++;
                region_start = pc;
                if (pc >= limit_pc)
                    return eg_asp_vm_result(pc, EG_ASP_VM_TRAP);
                if (!eg_asp_vbc_native_safe_start(program, pc))
                    return eg_asp_vm_result(pc, EG_ASP_VM_TRAP);
                break;
            }
            case EG_ASP_VBC_POP:
                if (state->operand_depth <= 0) return EG_ASP_VM_INVALID;
                state->operand_depth--;
                pc++;
                region_start = pc;
                if (pc >= limit_pc)
                    return eg_asp_vm_result(pc, EG_ASP_VM_TRAP);
                if (!eg_asp_vbc_native_safe_start(program, pc))
                    return eg_asp_vm_result(pc, EG_ASP_VM_TRAP);
                break;
            case EG_ASP_VBC_JUMP_IF_FALSE: {
                if (state->operand_depth <= 0) return EG_ASP_VM_INVALID;
                eg_asp_native_value value =
                    state->operands[--state->operand_depth];
                pc = eg_asp_vm_truthy(&value) ? pc + 1 : arg1;
                region_start = pc;
                if (pc < 0) return EG_ASP_VM_INVALID;
                if (pc >= limit_pc)
                    return eg_asp_vm_result(pc, EG_ASP_VM_TRAP);
                if (!eg_asp_vbc_native_safe_start(program, pc))
                    return eg_asp_vm_result(pc, EG_ASP_VM_TRAP);
                break;
            }
            default:
                state->operand_depth = 0;
                return eg_asp_vm_result(region_start, EG_ASP_VM_TRAP);
        }
    }
    return eg_asp_vm_result(pc, EG_ASP_VM_YIELD);
}

/*
 * Push a routine frame and carve its scalar slots from the shared slot stack.
 * Failure leaves the previous frame/depth untouched so the caller can fall back
 * to managed execution safely.
 */
int64_t eg_asp_vm_routine_enter(int64_t arena_handle, int64_t routine_id,
                                int64_t return_pc,
                                int64_t limit_pc, int64_t stack_base,
                                int64_t slot_base, int64_t slot_count) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state || routine_id < 0 || return_pc < 0 || limit_pc < 0 || stack_base < 0 ||
        slot_base < 0 || slot_count <= 0 ||
        slot_base > EG_ASP_VM_SLOT_CAPACITY - slot_count ||
        !eg_asp_vm_ensure_slot_capacity(arena_handle, state,
                                        slot_base + slot_count) ||
        !eg_asp_vm_ensure_frame_capacity(arena_handle, state)) {
        return 0;
    }
    int64_t inherited_error_mode = state->frame_depth > 0
        ? state->frames[state->frame_depth - 1].error_mode : 0;
    eg_asp_native_call_frame* frame = &state->frames[state->frame_depth++];
    frame->return_pc = return_pc;
    frame->limit_pc = limit_pc;
    frame->stack_base = stack_base;
    frame->slot_base = slot_base;
    frame->slot_count = slot_count;
    frame->member_id = routine_id;
    frame->handler_id = -1;
    frame->argument_count = 0;
    frame->object_kind = 0;
    frame->member_flags = 0;
    frame->me_object_id = 0;
    frame->return_dest = 0;
    frame->copyback_mask = 0;
    frame->program_switched = 0;
    frame->caller_dynamic_active = 0;
    frame->caller_dynamic_index = -1;
    frame->error_mode = inherited_error_mode;
    frame->caller_error_mode = inherited_error_mode;
    frame->resume_target_pc = -1;
    frame->error_number = 0;
    return 1;
}

int64_t eg_asp_vm_routine_bind(int64_t arena_handle, int64_t me_object_id,
                               int64_t return_dest,
                               int64_t copyback_mask) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state || state->frame_depth <= 0 || me_object_id < 0 ||
        return_dest < 0 || return_dest > 1 || copyback_mask < 0) {
        return 0;
    }
    eg_asp_native_call_frame* frame = &state->frames[state->frame_depth - 1];
    frame->me_object_id = me_object_id;
    frame->return_dest = return_dest;
    frame->copyback_mask = copyback_mask;
    return 1;
}

int64_t eg_asp_vm_routine_bind_program(int64_t arena_handle,
                                       int64_t caller_dynamic_active,
                                       int64_t caller_dynamic_index) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state || state->frame_depth <= 0 ||
        (caller_dynamic_active != 0 && caller_dynamic_active != 1) ||
        (caller_dynamic_active && caller_dynamic_index < 0)) {
        return 0;
    }
    eg_asp_native_call_frame* frame = &state->frames[state->frame_depth - 1];
    frame->program_switched = 1;
    frame->caller_dynamic_active = caller_dynamic_active;
    frame->caller_dynamic_index = caller_dynamic_index;
    return 1;
}

int64_t eg_asp_vm_routine_bind_error(int64_t arena_handle,
                                     int64_t error_mode,
                                     int64_t error_number,
                                     int64_t resume_target_pc) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state || state->frame_depth <= 0 ||
        (error_mode != 0 && error_mode != 1) || resume_target_pc < 0) {
        return 0;
    }
    eg_asp_native_call_frame* frame = &state->frames[state->frame_depth - 1];
    frame->caller_error_mode = frame->error_mode;
    frame->error_mode = error_mode;
    frame->error_number = error_number;
    frame->resume_target_pc = resume_target_pc;
    return 1;
}

int64_t eg_asp_vm_routine_caller_error_mode(int64_t arena_handle) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    return state && state->frame_depth > 0
        ? state->frames[state->frame_depth - 1].caller_error_mode : 0;
}

int64_t eg_asp_vm_routine_program_switched(int64_t arena_handle) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    return state && state->frame_depth > 0
        ? state->frames[state->frame_depth - 1].program_switched : 0;
}

int64_t eg_asp_vm_routine_caller_dynamic_active(int64_t arena_handle) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    return state && state->frame_depth > 0
        ? state->frames[state->frame_depth - 1].caller_dynamic_active : 0;
}

int64_t eg_asp_vm_routine_caller_dynamic_index(int64_t arena_handle) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    return state && state->frame_depth > 0
        ? state->frames[state->frame_depth - 1].caller_dynamic_index : -1;
}

int64_t eg_asp_vm_routine_leave(int64_t arena_handle) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state || state->frame_depth <= 0) return 0;
    int64_t result = 1 + state->frames[state->frame_depth - 1].caller_error_mode;
    state->frame_depth--;
    return result;
}

/*
 * Validate a compile-time member candidate against runtime receiver kind,
 * arity, and ByRef flags. A negative result requests name-based dispatch.
 */
int64_t eg_asp_vm_member_select(int64_t arena_handle, int64_t member_id,
                                int64_t object_kind, int64_t candidate_id,
                                int64_t argument_count,
                                int64_t member_flags) {
    const char* value = getenv("EGRET_ASP_MEMBER_FASTPATH");
    if (value && *value && eg_asp_vm_false_value(value)) return -1;
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state) return -1;

    /*
     * RuntimeObjectUserClass is ABI value 4; 100 is the compiler-known ASP
     * intrinsic object class. Flags reserve bits for SET, default-member,
     * late-bound and writable-ByRef respectively.
     */
    if (member_id <= 0 || (object_kind != 4 && object_kind != 100) ||
        candidate_id < 0 ||
        argument_count < 0 || (member_flags & 15) != 0) {
        return -1;
    }
    state->member_frame.return_pc = -1;
    state->member_frame.limit_pc = -1;
    state->member_frame.stack_base = 0;
    state->member_frame.slot_base = 0;
    state->member_frame.slot_count = argument_count + 1;
    state->member_frame.member_id = member_id;
    state->member_frame.handler_id = candidate_id;
    state->member_frame.argument_count = argument_count;
    state->member_frame.object_kind = object_kind;
    state->member_frame.member_flags = member_flags;
    return candidate_id;
}

int64_t eg_asp_vm_object_call_cache_lookup(int64_t arena_handle,
                                           int64_t caller_program_id,
                                           int64_t callsite_pc,
                                           int64_t class_hash,
                                           int64_t member_id) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state) return -1;
    for (int64_t i = 0; i < 32; i++) {
        if (state->object_call_cache[i].caller_program_id == caller_program_id &&
            state->object_call_cache[i].callsite_pc == callsite_pc &&
            state->object_call_cache[i].class_hash == class_hash &&
            state->object_call_cache[i].member_id == member_id &&
            state->object_call_cache[i].routine_id >= 0) {
            return (state->object_call_cache[i].callee_program_id + 1) * 1048576 +
                   state->object_call_cache[i].routine_id;
        }
    }
    return -1;
}

int64_t eg_asp_vm_object_call_cache_bind(int64_t arena_handle,
                                         int64_t caller_program_id,
                                         int64_t callsite_pc,
                                         int64_t class_hash,
                                         int64_t member_id,
                                         int64_t callee_program_id,
                                         int64_t routine_id) {
    eg_asp_vm_run_state* state = eg_asp_vm_state(arena_handle);
    if (!state || caller_program_id < 0 || callsite_pc < 0 || member_id < 0 ||
        callee_program_id < 0 || routine_id < 0 || routine_id >= 1048576) {
        return 0;
    }
    int64_t slot = state->object_call_cache_next++ & 31;
    state->object_call_cache[slot].caller_program_id = caller_program_id;
    state->object_call_cache[slot].callsite_pc = callsite_pc;
    state->object_call_cache[slot].class_hash = class_hash;
    state->object_call_cache[slot].member_id = member_id;
    state->object_call_cache[slot].callee_program_id = callee_program_id;
    state->object_call_cache[slot].routine_id = routine_id;
    return 1;
}
