// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_asp_vbc.h"
#include "eg_asp_mmap.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define EG_ASP_VBC_MAGIC UINT64_C(0x4547425643303036) /* EGBVC006 */
#define EG_ASP_VBC_MAX_OPCODE 61

enum {
    EG_ASP_VBC_HALT = 0,
    EG_ASP_VBC_JUMP = 2,
    EG_ASP_VBC_JUMP_IF_FALSE = 3
};

/*
 * Native bytecode container. Each instruction is three int64 words. Owned
 * programs track initialization during construction; mapped programs borrow
 * immutable words from an AOT mapping that must outlive this handle. Validation
 * is the one-way transition that makes a program visible to the VM.
 */
struct eg_asp_vbc_program {
    uint64_t magic;
    int64_t instruction_count;
    int64_t* words;
    int owns_words;
    unsigned char* initialized;
    unsigned char* native_safe_start;
    struct {
        int64_t kind;
        int64_t integer;
        double floating;
    } *literals;
    int64_t literal_capacity;
    int validated;
};

static eg_asp_vbc_program* eg_asp_vbc_mutable(int64_t handle) {
    eg_asp_vbc_program* program = (eg_asp_vbc_program*)(intptr_t)handle;
    if (!program || program->magic != EG_ASP_VBC_MAGIC) return NULL;
    return program;
}

static int64_t eg_asp_vbc_word(const eg_asp_vbc_program* program,
                               size_t index) {
    int64_t value = 0;
    if (program->owns_words) return program->words[index];
    memcpy(&value, (const unsigned char*)program->words +
                   index * sizeof(value), sizeof(value));
    return value;
}

const eg_asp_vbc_program* eg_asp_vbc_from_handle(int64_t handle) {
    eg_asp_vbc_program* program = eg_asp_vbc_mutable(handle);
    return program && program->validated ? program : NULL;
}

int64_t eg_asp_vbc_create(int64_t instruction_count) {
    if (instruction_count < 0 ||
        (uint64_t)instruction_count > SIZE_MAX / (3 * sizeof(int64_t))) {
        return 0;
    }
    eg_asp_vbc_program* program = calloc(1, sizeof(*program));
    if (!program) return 0;
    size_t count = (size_t)instruction_count;
    if (count) {
        program->words = malloc(count * 3 * sizeof(*program->words));
        program->initialized = calloc(count, 1);
        if (!program->words || !program->initialized) {
            free(program->initialized);
            free(program->words);
            free(program);
            return 0;
        }
    }
    program->magic = EG_ASP_VBC_MAGIC;
    program->instruction_count = instruction_count;
    program->owns_words = 1;
    return (int64_t)(intptr_t)program;
}

/* Create a zero-copy view; destroy will not release the mapping or its words. */
int64_t eg_asp_vbc_create_mapped(int64_t mapping_handle,
                                 int64_t byte_offset,
                                 int64_t instruction_count) {
    if (instruction_count < 0 ||
        (uint64_t)instruction_count > SIZE_MAX / (3 * sizeof(int64_t))) {
        return 0;
    }
    int64_t bytes = instruction_count * 3 * (int64_t)sizeof(int64_t);
    int64_t* words = (int64_t*)(uintptr_t)eg_asp_mmap_data(
        mapping_handle, byte_offset, bytes);
    if (!words && instruction_count != 0) return 0;
    eg_asp_vbc_program* program = calloc(1, sizeof(*program));
    if (!program) return 0;
    program->magic = EG_ASP_VBC_MAGIC;
    program->instruction_count = instruction_count;
    program->words = words;
    program->owns_words = 0;
    return (int64_t)(intptr_t)program;
}

int64_t eg_asp_vbc_create_packed(const int64_t* words,
                                 int64_t instruction_count) {
    if (!words && instruction_count != 0) return 0;
    int64_t handle = eg_asp_vbc_create(instruction_count);
    eg_asp_vbc_program* program = eg_asp_vbc_mutable(handle);
    if (!program) return 0;
    if (instruction_count > 0) {
        memcpy(program->words, words,
               (size_t)instruction_count * 3 * sizeof(*program->words));
        memset(program->initialized, 1, (size_t)instruction_count);
    }
    return handle;
}

int64_t eg_asp_vbc_set(int64_t handle, int64_t pc, int64_t op,
                       int64_t arg0, int64_t arg1) {
    eg_asp_vbc_program* program = eg_asp_vbc_mutable(handle);
    if (!program || program->validated || pc < 0 ||
        pc >= program->instruction_count) {
        return 0;
    }
    size_t offset = (size_t)pc * 3;
    program->words[offset] = op;
    program->words[offset + 1] = arg0;
    program->words[offset + 2] = arg1;
    program->initialized[pc] = 1;
    return 1;
}

int64_t eg_asp_vbc_set_literal(int64_t handle, int64_t expr_id,
                               int64_t kind, int64_t integer,
                               double floating) {
    eg_asp_vbc_program* program = eg_asp_vbc_mutable(handle);
    if (!program || expr_id < 0 || expr_id >= 16777216)
        return 0;
    if (expr_id >= program->literal_capacity) {
        int64_t capacity = program->literal_capacity ? program->literal_capacity : 64;
        while (capacity <= expr_id) {
            if (capacity > INT64_MAX / 2) return 0;
            capacity *= 2;
        }
        void* grown = realloc(program->literals,
                              (size_t)capacity * sizeof(*program->literals));
        if (!grown) return 0;
        program->literals = grown;
        memset(program->literals + program->literal_capacity, 0,
               (size_t)(capacity - program->literal_capacity) *
                   sizeof(*program->literals));
        program->literal_capacity = capacity;
    }
    program->literals[expr_id].kind = kind;
    program->literals[expr_id].integer = integer;
    program->literals[expr_id].floating = floating;
    return 1;
}

int64_t eg_asp_vbc_set_string_literal(int64_t handle, int64_t expr_id,
                                      void* string_object) {
    return eg_asp_vbc_set_literal(handle, expr_id, 5,
                                  (int64_t)(intptr_t)string_object, 0.0);
}

static int eg_asp_vbc_stack_effect(int64_t op, int64_t arg0) {
    switch (op) {
        case 10: case 49: return 1; /* PushExprLeaf, PushScalarSlot */
        case 4: case 50: return -1; /* Pop, AssignScalarSlot */
        case 5: return 1;           /* Dup */
        case 11: case 25: return 0; /* Neg, Not */
        case 55: return arg0 > 0 && arg0 <= INT_MAX ? 1 - (int)arg0 : INT_MIN;
        case 12: case 13: case 14: case 15: case 16: case 17: case 18:
        case 19: case 20: case 21: case 22: case 23: case 24:
        case 26: case 27: case 28: case 53:
            return -1;
        case 3: return -1;          /* JumpIfFalse */
        default: return INT_MIN;
    }
}

static int eg_asp_vbc_is_sink(int64_t op) {
    return op == 3 || op == 4 || op == 50;
}

/*
 * Validate opcode bounds, branch targets, initialization, and conservative
 * stack effects. native_safe_start marks PCs from which the C VM can execute
 * without encountering an unsupported stack producer before a sink.
 */
int64_t eg_asp_vbc_validate(int64_t handle) {
    eg_asp_vbc_program* program = eg_asp_vbc_mutable(handle);
    if (!program) return 0;
    if (program->instruction_count < 0 ||
        (uint64_t)program->instruction_count > SIZE_MAX) {
        return 0;
    }
    size_t instruction_count = (size_t)program->instruction_count;
    for (int64_t pc = 0; pc < program->instruction_count; pc++) {
        if (program->owns_words && !program->initialized[pc]) return 0;
        size_t offset = (size_t)pc * 3;
        int64_t op = eg_asp_vbc_word(program, offset);
        int64_t arg0 = eg_asp_vbc_word(program, offset + 1);
        int64_t arg1 = eg_asp_vbc_word(program, offset + 2);
        if (op < EG_ASP_VBC_HALT || op > EG_ASP_VBC_MAX_OPCODE) return 0;
        if (op == EG_ASP_VBC_JUMP &&
            (arg0 < 0 || arg0 > program->instruction_count)) {
            return 0;
        }
        if (op == EG_ASP_VBC_JUMP_IF_FALSE &&
            (arg1 < 0 || arg1 > program->instruction_count)) {
            return 0;
        }
    }
    if (instruction_count) {
        program->native_safe_start = calloc(instruction_count, 1);
        if (!program->native_safe_start) return 0;
        /*
         * Pre-analyze stack-neutral scalar regions. Native execution starts
         * only where an empty operand stack reaches a store/pop/branch with
         * no unsupported opcode or underflow, so rollback never has to
         * reconstruct a partially materialized managed stack.
         */
        for (int64_t start = 0; start < program->instruction_count; start++) {
            int depth = 0;
            int max_depth = 0;
            for (int64_t pc = start;
                 pc < program->instruction_count && pc < start + 256; pc++) {
                int64_t op = eg_asp_vbc_word(program, (size_t)pc * 3);
                int effect = eg_asp_vbc_stack_effect(
                    op, eg_asp_vbc_word(program, (size_t)pc * 3 + 1));
                if (effect == INT_MIN) break;
                if (effect < 0 && depth < -effect) break;
                depth += effect;
                if (depth > max_depth) max_depth = depth;
                if (max_depth > 64) break;
                if (eg_asp_vbc_is_sink(op)) {
                    if (depth == 0)
                        program->native_safe_start[start] = 1;
                    break;
                }
            }
        }
    }
    program->validated = 1;
    return 1;
}

void eg_asp_vbc_destroy(int64_t handle) {
    eg_asp_vbc_program* program = eg_asp_vbc_mutable(handle);
    if (!program) return;
    program->magic = 0;
    free(program->initialized);
    free(program->native_safe_start);
    free(program->literals);
    if (program->owns_words) free((void*)program->words);
    free(program);
}

int eg_asp_vbc_literal(const eg_asp_vbc_program* program, int64_t expr_id,
                       int64_t* kind, int64_t* integer, double* floating) {
    if (!program || !program->validated || expr_id < 0 ||
        expr_id >= program->literal_capacity ||
        program->literals[expr_id].kind == 0) return 0;
    *kind = program->literals[expr_id].kind;
    *integer = program->literals[expr_id].integer;
    *floating = program->literals[expr_id].floating;
    return 1;
}

int eg_asp_vbc_native_safe_start(const eg_asp_vbc_program* program,
                                 int64_t pc) {
    return program && program->validated && pc >= 0 &&
           pc < program->instruction_count && program->native_safe_start &&
           program->native_safe_start[pc] != 0;
}

int64_t eg_asp_vbc_instruction_count(const eg_asp_vbc_program* program) {
    return program ? program->instruction_count : 0;
}

int eg_asp_vbc_instruction(const eg_asp_vbc_program* program, int64_t pc,
                           int64_t* op, int64_t* arg0, int64_t* arg1) {
    if (!program || !program->validated || pc < 0 ||
        pc >= program->instruction_count) {
        return 0;
    }
    size_t offset = (size_t)pc * 3;
    *op = eg_asp_vbc_word(program, offset);
    *arg0 = eg_asp_vbc_word(program, offset + 1);
    *arg1 = eg_asp_vbc_word(program, offset + 2);
    return 1;
}
