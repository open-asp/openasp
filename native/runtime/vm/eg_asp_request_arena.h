// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_ASP_REQUEST_ARENA_H
#define EG_ASP_REQUEST_ARENA_H

#include <stddef.h>
#include <stdint.h>

/*
 * Arena values are deliberately scalar-only. Managed Egret references must
 * remain in GC-visible managed containers (or a future registered root table);
 * they must never be stored in these slots as raw pointers.
 */
typedef struct eg_asp_native_value {
    uint32_t kind;
    uint32_t flags;
    union {
        int64_t integer;
        double floating;
        uint64_t bits;
    } as;
} eg_asp_native_value;

typedef struct eg_asp_native_call_frame {
    int64_t return_pc;
    int64_t limit_pc;
    int64_t stack_base;
    int64_t slot_base;
    int64_t slot_count;
    int64_t member_id;
    int64_t handler_id;
    int64_t argument_count;
    int64_t object_kind;
    int64_t member_flags;
    int64_t me_object_id;
    int64_t return_dest;
    int64_t copyback_mask;
    int64_t program_switched;
    int64_t caller_dynamic_active;
    int64_t caller_dynamic_index;
    int64_t error_mode;
    int64_t caller_error_mode;
    int64_t resume_target_pc;
    int64_t error_number;
} eg_asp_native_call_frame;

/* Opaque handles cross Egret's 64-bit Int FFI. */
int64_t eg_asp_request_arena_create(int64_t initial_bytes);
void eg_asp_request_arena_reset(int64_t handle);
void eg_asp_request_arena_destroy(int64_t handle);

int64_t eg_asp_request_arena_alloc(int64_t handle, int64_t bytes,
                                   int64_t alignment);
int64_t eg_asp_request_arena_alloc_native_values(int64_t handle,
                                                 int64_t count);
int64_t eg_asp_request_arena_alloc_operands(int64_t handle, int64_t count);
int64_t eg_asp_request_arena_alloc_call_frames(int64_t handle, int64_t count);
int64_t eg_asp_request_arena_alloc_scratch(int64_t handle, int64_t bytes,
                                           int64_t alignment);

int64_t eg_asp_request_arena_used_bytes(int64_t handle);
int64_t eg_asp_request_arena_reserved_bytes(int64_t handle);
int64_t eg_asp_request_arena_generation(int64_t handle);

/*
 * Bind String allocations on the current worker thread to a request arena.
 * The binding must be left before an await can transfer the thread to another
 * request. String objects retain the runtime ABI and own inline NUL-terminated
 * bytes, but are reclaimed in bulk with the arena.
 */
int64_t eg_asp_request_string_arena_enter(int64_t handle);
void eg_asp_request_string_arena_leave(int64_t handle);
void* eg_asp_request_string_alloc(int64_t len);
int64_t eg_asp_request_string_is_owned(void* string_object);
void* eg_asp_request_string_promote(void* string_object);

/*
 * Request-owned tabular storage. Strings are copied into the arena and cells
 * contain no managed references, so an entire Recordset can be discarded at
 * the request boundary without tracing each row.
 */
int64_t eg_asp_request_rows_create(int64_t arena_handle, int64_t row_count,
                                  int64_t column_count);
int64_t eg_asp_request_rows_generation(int64_t arena_handle,
                                      int64_t rows_handle);
int64_t eg_asp_request_rows_set_string(int64_t arena_handle,
                                      int64_t rows_handle, int64_t row,
                                      int64_t column, void* string_object);
int64_t eg_asp_request_rows_set_scalar(int64_t arena_handle,
                                      int64_t rows_handle, int64_t row,
                                      int64_t column, int64_t kind,
                                      int64_t integer, double floating);
int64_t eg_asp_request_rows_update_string(int64_t arena_handle,
                                         int64_t rows_handle, int64_t row,
                                         int64_t column, void* string_object);
int64_t eg_asp_request_rows_update_scalar(int64_t arena_handle,
                                         int64_t rows_handle, int64_t row,
                                         int64_t column, int64_t kind,
                                         int64_t integer, double floating);
int64_t eg_asp_request_rows_cell_kind(int64_t arena_handle,
                                     int64_t rows_handle, int64_t row,
                                     int64_t column, int64_t original);
int64_t eg_asp_request_rows_cell_integer(int64_t arena_handle,
                                        int64_t rows_handle, int64_t row,
                                        int64_t column, int64_t original);
double eg_asp_request_rows_cell_floating(int64_t arena_handle,
                                        int64_t rows_handle, int64_t row,
                                        int64_t column, int64_t original);
void* eg_asp_request_rows_cell_string(int64_t arena_handle,
                                     int64_t rows_handle, int64_t row,
                                     int64_t column, int64_t original);
int64_t eg_asp_request_rows_cell_changed(int64_t arena_handle,
                                        int64_t rows_handle, int64_t row,
                                        int64_t column);
int64_t eg_asp_request_arena_escape_check(int64_t arena_handle,
                                         int64_t generation);
int64_t eg_asp_request_arena_escape_failures(void);

/*
 * Per-worker admission budget for requests that can transiently duplicate
 * large entity bodies. A successful reservation is the opaque value returned
 * by try_acquire and must be released exactly once.
 */
int64_t eg_asp_memory_token_try_acquire(int64_t predicted_bytes);
void eg_asp_memory_token_release(int64_t reservation);

/* One request-owned native VM run-state, allocated lazily from this arena. */
void* eg_asp_request_arena_vm_state(int64_t handle, size_t bytes,
                                    size_t alignment);

#endif
