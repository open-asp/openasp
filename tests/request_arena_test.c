// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_asp_request_arena.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int64_t len;
    const char* data;
} test_string;

/* Promotion leaves request storage and therefore uses this stand-in managed
 * allocator. The object and bytes share one allocation so the test can free
 * the promoted value without linking the full collector. */
void* eg_string_from_bytes_managed(const void* data, int64_t len) {
    if (!data || len <= 0) return NULL;
    test_string* string = (test_string*)malloc(
        sizeof(*string) + (size_t)len + 1u);
    assert(string != NULL);
    char* bytes = (char*)(string + 1);
    memcpy(bytes, data, (size_t)len);
    bytes[len] = '\0';
    string->len = len;
    string->data = bytes;
    return string;
}

int main(void) {
    int64_t arena = eg_asp_request_arena_create(128);
    assert(arena != 0);
    assert(eg_asp_request_arena_generation(arena) > 0);

    int64_t a = eg_asp_request_arena_alloc_scratch(arena, 17, 64);
    int64_t b = eg_asp_request_arena_alloc_operands(arena, 32);
    int64_t c = eg_asp_request_arena_alloc_call_frames(arena, 8);
    assert(a != 0 && ((uintptr_t)a % 64) == 0);
    assert(b != 0 && ((uintptr_t)b % _Alignof(eg_asp_native_value)) == 0);
    assert(c != 0 && ((uintptr_t)c % _Alignof(eg_asp_native_call_frame)) == 0);
    assert(eg_asp_request_arena_used_bytes(arena) > 0);
    assert(eg_asp_request_arena_reserved_bytes(arena) >= 128);

    /* Nested enter/leave calls model evaluator scopes. Ownership must survive
     * the inner leave, then disappear exactly when the outer scope closes. */
    assert(eg_asp_request_string_arena_enter(arena));
    assert(eg_asp_request_string_arena_enter(arena));
    test_string* arena_string = (test_string*)eg_asp_request_string_alloc(5);
    assert(arena_string != NULL);
    memcpy((char*)arena_string->data, "hello", 5);
    assert(arena_string->len == 5);
    assert(strcmp(arena_string->data, "hello") == 0);
    assert(eg_asp_request_string_is_owned(arena_string));
    test_string* promoted =
        (test_string*)eg_asp_request_string_promote(arena_string);
    assert(promoted != NULL && promoted != arena_string);
    assert(promoted->len == 5 && strcmp(promoted->data, "hello") == 0);
    free(promoted);
    eg_asp_request_string_arena_leave(arena);
    assert(eg_asp_request_string_is_owned(arena_string));
    eg_asp_request_string_arena_leave(arena);
    assert(!eg_asp_request_string_is_owned(arena_string));
    assert(eg_asp_request_string_alloc(1) == NULL);

    /* Row updates retain both current and original values so ADO-style dirty
     * tracking can compare or roll back cells without heap allocation. */
    int64_t generation = eg_asp_request_arena_generation(arena);
    int64_t rows = eg_asp_request_rows_create(arena, 2, 3);
    assert(rows != 0);
    assert(eg_asp_request_rows_generation(arena, rows) == generation);
    assert(eg_asp_request_rows_set_scalar(arena, rows, 0, 0, 3, 42, 0.0));
    assert(eg_asp_request_rows_cell_kind(arena, rows, 0, 0, 0) == 3);
    assert(eg_asp_request_rows_cell_integer(arena, rows, 0, 0, 0) == 42);
    assert(!eg_asp_request_rows_cell_changed(arena, rows, 0, 0));
    assert(eg_asp_request_rows_update_scalar(arena, rows, 0, 0, 3, 43, 0.0));
    assert(eg_asp_request_rows_cell_integer(arena, rows, 0, 0, 0) == 43);
    assert(eg_asp_request_rows_cell_integer(arena, rows, 0, 0, 1) == 42);
    assert(eg_asp_request_rows_cell_changed(arena, rows, 0, 0));
    test_string text = {5, "hello"};
    assert(eg_asp_request_rows_set_string(arena, rows, 1, 2, &text));
    assert(eg_asp_request_rows_cell_kind(arena, rows, 1, 2, 0) == 5);

    /* Reset invalidates generation-tagged handles and releases overflow
     * blocks, but keeps the initial reservation for reuse by the next request. */
    eg_asp_request_arena_reset(arena);
    assert(eg_asp_request_arena_used_bytes(arena) == 0);
    assert(eg_asp_request_arena_reserved_bytes(arena) == 128);
    assert(eg_asp_request_arena_generation(arena) != generation);
    assert(eg_asp_request_arena_escape_check(
        arena, eg_asp_request_arena_generation(arena)));
    assert(eg_asp_request_arena_escape_failures() == 0);
    eg_asp_request_arena_destroy(arena);

    /* Memory tokens enforce a process-wide admission budget. Failed attempts
     * reserve nothing, and releasing one request makes the full budget usable. */
    int64_t reservation = eg_asp_memory_token_try_acquire(64 * 1024 * 1024);
    assert(reservation == 64 * 1024 * 1024);
    assert(eg_asp_memory_token_try_acquire(256 * 1024 * 1024) == 0);
    eg_asp_memory_token_release(reservation);
    reservation = eg_asp_memory_token_try_acquire(256 * 1024 * 1024);
    assert(reservation == 256 * 1024 * 1024);
    eg_asp_memory_token_release(reservation);

    /* Filling an odd-sized first block forces the aligned allocation into an
     * overflow block and verifies that block growth preserves alignment. */
    int64_t alignment_arena = eg_asp_request_arena_create(33);
    assert(alignment_arena != 0);
    assert(eg_asp_request_arena_alloc_scratch(alignment_arena, 33, 1) != 0);
    int64_t aligned_after_full =
        eg_asp_request_arena_alloc_scratch(alignment_arena, 1, 16);
    assert(aligned_after_full != 0);
    assert(((uintptr_t)aligned_after_full % 16) == 0);
    assert(eg_asp_request_arena_reserved_bytes(alignment_arena) > 33);
    eg_asp_request_arena_destroy(alignment_arena);
    return 0;
}
