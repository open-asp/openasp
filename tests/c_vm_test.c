// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_asp_request_arena.h"
#include "eg_asp_vbc.h"
#include "eg_asp_vm.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    int64_t len;
    char* data;
} test_string;

/* The VM materializes only the final concatenation through this ABI in the
 * scenarios below. A fixed buffer is sufficient because tests consume the
 * value immediately and never exercise managed-string lifetime behavior. */
void* eg_string_from_bytes(const void* data, int64_t len) {
    static char bytes[64];
    static test_string value;
    assert(len >= 0 && len <= (int64_t)sizeof(bytes));
    if (len > 0) memcpy(bytes, data, (size_t)len);
    value.len = len;
    value.data = bytes;
    return &value;
}

int main(void) {
    enum {
        VBC_PUSH_EXPR_LEAF = 10,
        VBC_DIV = 22,
        VBC_ASSIGN_SCALAR_SLOT = 50,
        VBC_POW = 53,
        VBC_CONCAT_N = 55,
        VARIANT_INT = 3,
        VARIANT_FLOAT = 4,
        NATIVE_STRING_SLICE = 11
    };
    int64_t program = eg_asp_vbc_create(4);
    /* A power expression checks integer operands, floating-point result
     * promotion, bytecode validation and scalar-slot assignment together. */
    assert(program != 0);
    assert(eg_asp_vbc_set(program, 0, VBC_PUSH_EXPR_LEAF, 0, 0));
    assert(eg_asp_vbc_set(program, 1, VBC_PUSH_EXPR_LEAF, 1, 0));
    assert(eg_asp_vbc_set(program, 2, VBC_POW, -1, -1));
    assert(eg_asp_vbc_set(program, 3, VBC_ASSIGN_SCALAR_SLOT, 0, 0));
    assert(eg_asp_vbc_set_literal(program, 0, VARIANT_INT, 2, 0.0));
    assert(eg_asp_vbc_set_literal(program, 1, VARIANT_INT, 10, 0.0));
    assert(eg_asp_vbc_validate(program));
    assert(eg_asp_vbc_native_safe_start(
        eg_asp_vbc_from_handle(program), 0));

    int64_t arena = eg_asp_request_arena_create(4096);
    assert(arena != 0);
    assert(eg_asp_vm_routine_enter(arena, 1, 0, 4, 0, 0, 1));
    assert(eg_asp_vm_dispatch(arena, program, 0, 4) >= 0);
    assert(eg_asp_vm_slot_kind(arena, 0) == VARIANT_FLOAT);
    assert(fabs(eg_asp_vm_slot_floating(arena, 0) - 1024.0) < 1e-12);
    eg_asp_vm_routine_leave(arena);
    eg_asp_vbc_destroy(program);

    /* Division follows the same dispatch path but guards the distinct numeric
     * opcode and its floating-point semantics. */
    program = eg_asp_vbc_create(4);
    assert(eg_asp_vbc_set(program, 0, VBC_PUSH_EXPR_LEAF, 0, 0));
    assert(eg_asp_vbc_set(program, 1, VBC_PUSH_EXPR_LEAF, 1, 0));
    assert(eg_asp_vbc_set(program, 2, VBC_DIV, -1, -1));
    assert(eg_asp_vbc_set(program, 3, VBC_ASSIGN_SCALAR_SLOT, 0, 0));
    assert(eg_asp_vbc_set_literal(program, 0, VARIANT_INT, 8, 0.0));
    assert(eg_asp_vbc_set_literal(program, 1, VARIANT_INT, 2, 0.0));
    assert(eg_asp_vbc_validate(program));
    assert(eg_asp_vm_routine_enter(arena, 2, 0, 4, 0, 0, 1));
    assert(eg_asp_vm_dispatch(arena, program, 0, 4) >= 0);
    assert(fabs(eg_asp_vm_slot_floating(arena, 0) - 4.0) < 1e-12);
    eg_asp_vm_routine_leave(arena);
    eg_asp_vbc_destroy(program);

    /* CONCAT_N is the ownership-sensitive path: native operand slices are
     * collapsed into one string that remains readable through the slot API. */
    program = eg_asp_vbc_create(5);
    assert(eg_asp_vbc_set(program, 0, VBC_PUSH_EXPR_LEAF, 0, 0));
    assert(eg_asp_vbc_set(program, 1, VBC_PUSH_EXPR_LEAF, 1, 0));
    assert(eg_asp_vbc_set(program, 2, VBC_PUSH_EXPR_LEAF, 2, 0));
    assert(eg_asp_vbc_set(program, 3, VBC_CONCAT_N, 3, 0));
    assert(eg_asp_vbc_set(program, 4, VBC_ASSIGN_SCALAR_SLOT, 0, 0));
    assert(eg_asp_vbc_set_literal(program, 0, VARIANT_INT, 1, 0.0));
    assert(eg_asp_vbc_set_literal(program, 1, VARIANT_INT, 2, 0.0));
    assert(eg_asp_vbc_set_literal(program, 2, VARIANT_INT, 3, 0.0));
    assert(eg_asp_vbc_validate(program));
    assert(eg_asp_vbc_native_safe_start(
        eg_asp_vbc_from_handle(program), 0));
    assert(eg_asp_vm_routine_enter(arena, 3, 0, 5, 0, 0, 1));
    assert(eg_asp_vm_dispatch(arena, program, 0, 5) >= 0);
    assert(eg_asp_vm_slot_kind(arena, 0) == NATIVE_STRING_SLICE);
    test_string* concatenated =
        (test_string*)eg_asp_vm_slot_string(arena, 0);
    assert(concatenated != NULL);
    assert(concatenated->len == 3);
    assert(memcmp(concatenated->data, "123", 3) == 0);
    eg_asp_vm_routine_leave(arena);

    eg_asp_request_arena_destroy(arena);
    eg_asp_vbc_destroy(program);
    return 0;
}
