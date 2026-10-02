// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_ASP_VBC_H
#define EG_ASP_VBC_H

#include <stdint.h>

typedef struct eg_asp_vbc_program eg_asp_vbc_program;

/*
 * Immutable bytecode view used by the C dispatcher. Constructors either own a
 * copied instruction array or borrow a validated mmap range. Literal tables
 * are populated before validate; mutation after publication is unsupported.
 * Opaque handles cross Egret's 64-bit Int FFI and require one destroy.
 */
int64_t eg_asp_vbc_create(int64_t instruction_count);
int64_t eg_asp_vbc_create_packed(const int64_t* words,
                                 int64_t instruction_count);
int64_t eg_asp_vbc_create_mapped(int64_t mapping_handle,
                                 int64_t byte_offset,
                                 int64_t instruction_count);
int64_t eg_asp_vbc_set(int64_t handle, int64_t pc, int64_t op,
                       int64_t arg0, int64_t arg1);
int64_t eg_asp_vbc_set_literal(int64_t handle, int64_t expr_id,
                               int64_t kind, int64_t integer,
                               double floating);
int64_t eg_asp_vbc_set_string_literal(int64_t handle, int64_t expr_id,
                                      void* string_object);
int64_t eg_asp_vbc_validate(int64_t handle);
void eg_asp_vbc_destroy(int64_t handle);

/* Internal checked accessors. Returned program/instruction data remains
 * borrowed from the handle and must not escape its lifetime. */
const eg_asp_vbc_program* eg_asp_vbc_from_handle(int64_t handle);
int64_t eg_asp_vbc_instruction_count(const eg_asp_vbc_program* program);
int eg_asp_vbc_instruction(const eg_asp_vbc_program* program, int64_t pc,
                           int64_t* op, int64_t* arg0, int64_t* arg1);
int eg_asp_vbc_literal(const eg_asp_vbc_program* program, int64_t expr_id,
                       int64_t* kind, int64_t* integer, double* floating);
int eg_asp_vbc_native_safe_start(const eg_asp_vbc_program* program,
                                 int64_t pc);

#endif
