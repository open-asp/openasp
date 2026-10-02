// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_asp_request_arena.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define EG_ASP_ARENA_MAGIC UINT64_C(0x45474152454e4131)
#define EG_ASP_ARENA_DEFAULT_CHUNK (16u * 1024u)
#define EG_ASP_ARENA_MAX_GROWTH_CHUNK (8u * 1024u * 1024u)
#define EG_ASP_MEMORY_TOKEN_DEFAULT_BUDGET (256ull * 1024ull * 1024ull)

#if defined(MAP_ANONYMOUS)
#define EG_ASP_MAP_ANONYMOUS MAP_ANONYMOUS
#else
#define EG_ASP_MAP_ANONYMOUS MAP_ANON
#endif

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void eg_gc_native_account_alloc(uint64_t bytes) { (void)bytes; }

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void eg_gc_native_account_free(uint64_t bytes) { (void)bytes; }

/*
 * Request arenas are append-only chains of anonymous mappings. Individual
 * allocations are never freed; reset keeps the first chunk and unmaps growth
 * chunks. generation invalidates every arena-backed handle after reset.
 */
typedef struct eg_asp_arena_chunk {
    struct eg_asp_arena_chunk* next;
    size_t capacity;
    size_t used;
    max_align_t data[];
} eg_asp_arena_chunk;

typedef struct eg_asp_request_arena {
    /* magic validates opaque integer handles crossing the Egret ABI. */
    uint64_t magic;
    /* Copied into dependent objects and changed on every reset. */
    uint64_t generation;
    size_t default_chunk;
    size_t used;
    size_t reserved;
    eg_asp_arena_chunk* first;
    eg_asp_arena_chunk* current;
    void* vm_state;
    size_t vm_state_bytes;
} eg_asp_request_arena;

static eg_asp_request_arena* arena_from_handle(int64_t handle);

/* Compact database cell stored outside the tracing GC heap. */
typedef struct {
    int64_t len;
    const char* data;
} eg_asp_string;

/*
 * current and original have identical dimensions, allowing dirty checks
 * without cloning managed Variants for every result cell.
 */
typedef struct {
    uint32_t kind;
    uint32_t flags;
    int64_t integer;
    double floating;
    const char* text;
    int64_t text_len;
} eg_asp_request_cell;

typedef struct {
    uint64_t magic;
    uint64_t generation;
    int64_t row_count;
    int64_t column_count;
    eg_asp_request_cell* current;
    eg_asp_request_cell* original;
} eg_asp_request_rows;

#define EG_ASP_ROWS_MAGIC UINT64_C(0x4547524f57533031)

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void* eg_string_from_bytes(const void* data, int64_t len) {
    (void)data;
    (void)len;
    return NULL;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void* eg_string_from_bytes_managed(const void* data, int64_t len) {
    return eg_string_from_bytes(data, len);
}

static _Atomic uint64_t arena_next_generation = 1;
static _Atomic uint64_t memory_token_used = 0;
static _Atomic uint64_t memory_token_budget = 0;
static _Atomic uint64_t arena_escape_failures = 0;
static _Thread_local eg_asp_request_arena* request_string_arena;
static _Thread_local uint32_t request_string_arena_depth;

static void account_alloc(size_t bytes) {
    eg_gc_native_account_alloc((uint64_t)bytes);
}

static void account_free(size_t bytes) {
    eg_gc_native_account_free((uint64_t)bytes);
}

static eg_asp_request_arena* arena_from_handle(int64_t handle) {
    eg_asp_request_arena* arena =
        (eg_asp_request_arena*)(intptr_t)handle;
    return arena && arena->magic == EG_ASP_ARENA_MAGIC ? arena : NULL;
}

static int valid_alignment(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

static eg_asp_arena_chunk* chunk_create(size_t capacity) {
    if (capacity > SIZE_MAX - sizeof(eg_asp_arena_chunk)) return NULL;
    size_t total = sizeof(eg_asp_arena_chunk) + capacity;
    eg_asp_arena_chunk* chunk = (eg_asp_arena_chunk*)mmap(
        NULL, total, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | EG_ASP_MAP_ANONYMOUS, -1, 0);
    if (chunk == MAP_FAILED) return NULL;
    chunk->next = NULL;
    chunk->capacity = capacity;
    chunk->used = 0;
    account_alloc(total);
    return chunk;
}

static void chunk_destroy(eg_asp_arena_chunk* chunk) {
    if (!chunk) return;
    size_t total = sizeof(*chunk) + chunk->capacity;
    account_free(total);
    (void)munmap(chunk, total);
}

int64_t eg_asp_request_arena_create(int64_t initial_bytes) {
    size_t capacity = EG_ASP_ARENA_DEFAULT_CHUNK;
    if (initial_bytes > 0) {
        if ((uint64_t)initial_bytes > SIZE_MAX) return 0;
        capacity = (size_t)initial_bytes;
    }
    eg_asp_request_arena* arena =
        (eg_asp_request_arena*)calloc(1, sizeof(*arena));
    if (!arena) return 0;
    account_alloc(sizeof(*arena));
    eg_asp_arena_chunk* first = chunk_create(capacity);
    if (!first) {
        account_free(sizeof(*arena));
        free(arena);
        return 0;
    }
    arena->magic = EG_ASP_ARENA_MAGIC;
    arena->generation = atomic_fetch_add_explicit(
        &arena_next_generation, 1, memory_order_relaxed);
    arena->default_chunk = capacity;
    arena->reserved = capacity;
    arena->first = first;
    arena->current = first;
    return (int64_t)(intptr_t)arena;
}

/*
 * Bump-allocate zeroed aligned storage. Chunks grow geometrically to the cap;
 * exceptionally large requests receive a dedicated larger chunk.
 */
static void* arena_alloc(eg_asp_request_arena* arena, size_t bytes,
                         size_t alignment) {
    if (!arena || bytes == 0 || !valid_alignment(alignment)) return NULL;
    if (alignment > (size_t)1 << 20) return NULL;
    eg_asp_arena_chunk* chunk = arena->current;
    uintptr_t base = (uintptr_t)chunk->data;
    uintptr_t cursor = base + chunk->used;
    uintptr_t aligned = (cursor + alignment - 1) & ~(uintptr_t)(alignment - 1);
    size_t offset = (size_t)(aligned - base);
    if (offset < chunk->used || offset > chunk->capacity ||
        bytes > chunk->capacity - offset) {
        if (bytes > SIZE_MAX - (alignment - 1)) return NULL;
        size_t needed = bytes + alignment - 1;
        size_t capacity = chunk->capacity;
        if (capacity < EG_ASP_ARENA_MAX_GROWTH_CHUNK) {
            capacity = capacity > EG_ASP_ARENA_MAX_GROWTH_CHUNK / 2
                ? EG_ASP_ARENA_MAX_GROWTH_CHUNK
                : capacity * 2;
        }
        if (capacity < arena->default_chunk) capacity = arena->default_chunk;
        if (capacity < needed) capacity = needed;
        chunk = chunk_create(capacity);
        if (!chunk) return NULL;
        if (arena->reserved > SIZE_MAX - capacity) {
            chunk_destroy(chunk);
            return NULL;
        }
        arena->current->next = chunk;
        arena->current = chunk;
        arena->reserved += capacity;
        base = (uintptr_t)chunk->data;
        aligned = (base + alignment - 1) & ~(uintptr_t)(alignment - 1);
        offset = (size_t)(aligned - base);
    }
    void* result = (unsigned char*)chunk->data + offset;
    chunk->used = offset + bytes;
    arena->used += bytes;
    memset(result, 0, bytes);
    return result;
}

int64_t eg_asp_request_arena_alloc(int64_t handle, int64_t bytes,
                                   int64_t alignment) {
    if (bytes <= 0 || alignment <= 0) return 0;
    void* result = arena_alloc(arena_from_handle(handle), (size_t)bytes,
                               (size_t)alignment);
    return (int64_t)(intptr_t)result;
}

static int64_t alloc_array(int64_t handle, int64_t count, size_t item_size,
                           size_t alignment) {
    if (count <= 0 || (uint64_t)count > SIZE_MAX / item_size) return 0;
    void* result = arena_alloc(arena_from_handle(handle),
                               (size_t)count * item_size, alignment);
    return (int64_t)(intptr_t)result;
}

int64_t eg_asp_request_arena_alloc_native_values(int64_t handle,
                                                 int64_t count) {
    return alloc_array(handle, count, sizeof(eg_asp_native_value),
                       _Alignof(eg_asp_native_value));
}

int64_t eg_asp_request_arena_alloc_operands(int64_t handle, int64_t count) {
    return eg_asp_request_arena_alloc_native_values(handle, count);
}

int64_t eg_asp_request_arena_alloc_call_frames(int64_t handle, int64_t count) {
    return alloc_array(handle, count, sizeof(eg_asp_native_call_frame),
                       _Alignof(eg_asp_native_call_frame));
}

int64_t eg_asp_request_arena_alloc_scratch(int64_t handle, int64_t bytes,
                                           int64_t alignment) {
    return eg_asp_request_arena_alloc(handle, bytes, alignment);
}

void* eg_asp_request_arena_vm_state(int64_t handle, size_t bytes,
                                    size_t alignment) {
    eg_asp_request_arena* arena = arena_from_handle(handle);
    if (!arena || bytes == 0) return NULL;
    if (arena->vm_state) {
        return arena->vm_state_bytes >= bytes ? arena->vm_state : NULL;
    }
    arena->vm_state = arena_alloc(arena, bytes, alignment);
    if (arena->vm_state) arena->vm_state_bytes = bytes;
    return arena->vm_state;
}

/*
 * Invalidate borrowed handles before reclaiming growth chunks. Retaining the
 * first mapping amortizes the next request's common working set.
 */
void eg_asp_request_arena_reset(int64_t handle) {
    eg_asp_request_arena* arena = arena_from_handle(handle);
    if (!arena) return;
    eg_asp_arena_chunk* chunk = arena->first->next;
    while (chunk) {
        eg_asp_arena_chunk* next = chunk->next;
        chunk_destroy(chunk);
        chunk = next;
    }
    arena->first->next = NULL;
    arena->first->used = 0;
    arena->current = arena->first;
    arena->used = 0;
    arena->reserved = arena->first->capacity;
    arena->vm_state = NULL;
    arena->vm_state_bytes = 0;
    arena->generation = atomic_fetch_add_explicit(
        &arena_next_generation, 1, memory_order_relaxed);
}

void eg_asp_request_arena_destroy(int64_t handle) {
    eg_asp_request_arena* arena = arena_from_handle(handle);
    if (!arena) return;
    if (request_string_arena == arena) {
        request_string_arena = NULL;
        request_string_arena_depth = 0;
    }
    arena->magic = 0;
    eg_asp_arena_chunk* chunk = arena->first;
    while (chunk) {
        eg_asp_arena_chunk* next = chunk->next;
        chunk_destroy(chunk);
        chunk = next;
    }
    account_free(sizeof(*arena));
    free(arena);
}

int64_t eg_asp_request_arena_used_bytes(int64_t handle) {
    eg_asp_request_arena* arena = arena_from_handle(handle);
    return arena && arena->used <= INT64_MAX ? (int64_t)arena->used : -1;
}

int64_t eg_asp_request_arena_reserved_bytes(int64_t handle) {
    eg_asp_request_arena* arena = arena_from_handle(handle);
    return arena && arena->reserved <= INT64_MAX ? (int64_t)arena->reserved : -1;
}

int64_t eg_asp_request_arena_generation(int64_t handle) {
    eg_asp_request_arena* arena = arena_from_handle(handle);
    return arena && arena->generation <= INT64_MAX
        ? (int64_t)arena->generation : 0;
}

/*
 * Install the thread-local string allocation target. Nesting is valid only for
 * the same arena and must be balanced by leave at the request boundary.
 */
int64_t eg_asp_request_string_arena_enter(int64_t handle) {
    eg_asp_request_arena* arena = arena_from_handle(handle);
    if (!arena || (request_string_arena && request_string_arena != arena)) {
        return 0;
    }
    request_string_arena = arena;
    request_string_arena_depth++;
    return 1;
}

void eg_asp_request_string_arena_leave(int64_t handle) {
    eg_asp_request_arena* arena = arena_from_handle(handle);
    if (request_string_arena != arena || request_string_arena_depth == 0) {
        return;
    }
    request_string_arena_depth--;
    if (request_string_arena_depth == 0) request_string_arena = NULL;
}

void* eg_asp_request_string_alloc(int64_t len) {
    eg_asp_request_arena* arena = request_string_arena;
    if (!arena || len <= 0) return NULL;
    uint64_t n = (uint64_t)len;
    if (n > SIZE_MAX - sizeof(eg_asp_string) - 1u) return NULL;
    size_t total = sizeof(eg_asp_string) + (size_t)n + 1u;
    eg_asp_string* string = (eg_asp_string*)arena_alloc(
        arena, total, _Alignof(eg_asp_string));
    if (!string) return NULL;
    string->len = len;
    string->data = (const char*)(string + 1);
    ((char*)string->data)[len] = '\0';
    return string;
}

static int arena_contains_string(const eg_asp_request_arena* arena,
                                 const eg_asp_string* string) {
    if (!arena || !string) return 0;
    uintptr_t address = (uintptr_t)string;
    for (const eg_asp_arena_chunk* chunk = arena->first; chunk;
         chunk = chunk->next) {
        uintptr_t begin = (uintptr_t)chunk->data;
        uintptr_t end = begin + chunk->used;
        if (address < begin || address > end ||
            end - address < sizeof(*string)) {
            continue;
        }
        if (string->len <= 0 ||
            (uint64_t)string->len > SIZE_MAX - sizeof(*string) - 1u) {
            return 0;
        }
        size_t total = sizeof(*string) + (size_t)string->len + 1u;
        return total <= end - address &&
            string->data == (const char*)(string + 1) &&
            string->data[string->len] == '\0';
    }
    return 0;
}

int64_t eg_asp_request_string_is_owned(void* string_object) {
    return arena_contains_string(
        request_string_arena, (const eg_asp_string*)string_object);
}

/* Copy an arena string to managed storage before it escapes request lifetime. */
void* eg_asp_request_string_promote(void* string_object) {
    const eg_asp_string* string = (const eg_asp_string*)string_object;
    if (!arena_contains_string(request_string_arena, string)) {
        return string_object;
    }
    return eg_string_from_bytes_managed(string->data, string->len);
}

static eg_asp_request_rows* request_rows_from_handle(
        eg_asp_request_arena* arena, int64_t rows_handle) {
    eg_asp_request_rows* rows =
        (eg_asp_request_rows*)(intptr_t)rows_handle;
    if (!arena || !rows || rows->magic != EG_ASP_ROWS_MAGIC ||
        rows->generation != arena->generation) {
        return NULL;
    }
    return rows;
}

static eg_asp_request_cell* request_rows_cell(
        eg_asp_request_arena* arena, int64_t rows_handle, int64_t row,
        int64_t column, int original) {
    eg_asp_request_rows* rows = request_rows_from_handle(arena, rows_handle);
    if (!rows || row < 0 || row >= rows->row_count || column < 0 ||
        column >= rows->column_count) {
        return NULL;
    }
    size_t index = (size_t)row * (size_t)rows->column_count + (size_t)column;
    return original ? &rows->original[index] : &rows->current[index];
}

int64_t eg_asp_request_rows_create(int64_t arena_handle, int64_t row_count,
                                   int64_t column_count) {
    eg_asp_request_arena* arena = arena_from_handle(arena_handle);
    if (!arena || row_count < 0 || column_count < 0) return 0;
    if (row_count != 0 &&
        (uint64_t)column_count > SIZE_MAX / (uint64_t)row_count) {
        return 0;
    }
    size_t count = (size_t)row_count * (size_t)column_count;
    if (count > SIZE_MAX / sizeof(eg_asp_request_cell)) return 0;
    eg_asp_request_rows* rows = (eg_asp_request_rows*)arena_alloc(
        arena, sizeof(*rows), _Alignof(eg_asp_request_rows));
    if (!rows) return 0;
    rows->magic = EG_ASP_ROWS_MAGIC;
    rows->generation = arena->generation;
    rows->row_count = row_count;
    rows->column_count = column_count;
    if (count != 0) {
        rows->current = (eg_asp_request_cell*)arena_alloc(
            arena, count * sizeof(*rows->current),
            _Alignof(eg_asp_request_cell));
        rows->original = (eg_asp_request_cell*)arena_alloc(
            arena, count * sizeof(*rows->original),
            _Alignof(eg_asp_request_cell));
        if (!rows->current || !rows->original) return 0;
    }
    return (int64_t)(intptr_t)rows;
}

int64_t eg_asp_request_rows_generation(int64_t arena_handle,
                                       int64_t rows_handle) {
    eg_asp_request_arena* arena = arena_from_handle(arena_handle);
    eg_asp_request_rows* rows = request_rows_from_handle(arena, rows_handle);
    return rows && rows->generation <= INT64_MAX
        ? (int64_t)rows->generation : 0;
}

static int request_rows_set_scalar(eg_asp_request_arena* arena,
                                   int64_t rows_handle, int64_t row,
                                   int64_t column, int64_t kind,
                                   int64_t integer, double floating,
                                   int initialize) {
    eg_asp_request_cell* cell = request_rows_cell(
        arena, rows_handle, row, column, 0);
    if (!cell || kind < 0 || kind > 9) return 0;
    cell->kind = (uint32_t)kind;
    cell->integer = integer;
    cell->floating = floating;
    cell->text = NULL;
    cell->text_len = 0;
    if (initialize) {
        eg_asp_request_cell* original = request_rows_cell(
            arena, rows_handle, row, column, 1);
        *original = *cell;
    } else {
        cell->flags |= UINT32_C(1);
    }
    return 1;
}

static int request_rows_set_string(eg_asp_request_arena* arena,
                                   int64_t rows_handle, int64_t row,
                                   int64_t column, void* string_object,
                                   int initialize) {
    eg_asp_string* string = (eg_asp_string*)string_object;
    if (!string || string->len < 0 ||
        (uint64_t)string->len > SIZE_MAX) {
        return 0;
    }
    eg_asp_request_cell* cell = request_rows_cell(
        arena, rows_handle, row, column, 0);
    if (!cell) return 0;
    char* copy = NULL;
    if (string->len != 0) {
        copy = (char*)arena_alloc(arena, (size_t)string->len, 1);
        if (!copy) return 0;
        memcpy(copy, string->data, (size_t)string->len);
    }
    cell->kind = 5;
    cell->integer = 0;
    cell->floating = 0.0;
    cell->text = copy;
    cell->text_len = string->len;
    if (initialize) {
        eg_asp_request_cell* original = request_rows_cell(
            arena, rows_handle, row, column, 1);
        *original = *cell;
    } else {
        cell->flags |= UINT32_C(1);
    }
    return 1;
}

int64_t eg_asp_request_rows_set_string(int64_t arena_handle,
                                       int64_t rows_handle, int64_t row,
                                       int64_t column, void* string_object) {
    return request_rows_set_string(arena_from_handle(arena_handle),
                                   rows_handle, row, column, string_object, 1);
}

int64_t eg_asp_request_rows_set_scalar(int64_t arena_handle,
                                       int64_t rows_handle, int64_t row,
                                       int64_t column, int64_t kind,
                                       int64_t integer, double floating) {
    return request_rows_set_scalar(arena_from_handle(arena_handle),
                                   rows_handle, row, column, kind, integer,
                                   floating, 1);
}

int64_t eg_asp_request_rows_update_string(int64_t arena_handle,
                                          int64_t rows_handle, int64_t row,
                                          int64_t column,
                                          void* string_object) {
    return request_rows_set_string(arena_from_handle(arena_handle),
                                   rows_handle, row, column, string_object, 0);
}

int64_t eg_asp_request_rows_update_scalar(int64_t arena_handle,
                                          int64_t rows_handle, int64_t row,
                                          int64_t column, int64_t kind,
                                          int64_t integer, double floating) {
    return request_rows_set_scalar(arena_from_handle(arena_handle),
                                   rows_handle, row, column, kind, integer,
                                   floating, 0);
}

int64_t eg_asp_request_rows_cell_kind(int64_t arena_handle,
                                      int64_t rows_handle, int64_t row,
                                      int64_t column, int64_t original) {
    eg_asp_request_cell* cell = request_rows_cell(
        arena_from_handle(arena_handle), rows_handle, row, column,
        original != 0);
    return cell ? (int64_t)cell->kind : -1;
}

int64_t eg_asp_request_rows_cell_integer(int64_t arena_handle,
                                         int64_t rows_handle, int64_t row,
                                         int64_t column, int64_t original) {
    eg_asp_request_cell* cell = request_rows_cell(
        arena_from_handle(arena_handle), rows_handle, row, column,
        original != 0);
    return cell ? cell->integer : 0;
}

double eg_asp_request_rows_cell_floating(int64_t arena_handle,
                                         int64_t rows_handle, int64_t row,
                                         int64_t column, int64_t original) {
    eg_asp_request_cell* cell = request_rows_cell(
        arena_from_handle(arena_handle), rows_handle, row, column,
        original != 0);
    return cell ? cell->floating : 0.0;
}

void* eg_asp_request_rows_cell_string(int64_t arena_handle,
                                      int64_t rows_handle, int64_t row,
                                      int64_t column, int64_t original) {
    eg_asp_request_cell* cell = request_rows_cell(
        arena_from_handle(arena_handle), rows_handle, row, column,
        original != 0);
    if (!cell || cell->kind != 5) return eg_string_from_bytes("", 0);
    return eg_string_from_bytes(cell->text ? cell->text : "", cell->text_len);
}

int64_t eg_asp_request_rows_cell_changed(int64_t arena_handle,
                                         int64_t rows_handle, int64_t row,
                                         int64_t column) {
    eg_asp_request_arena* arena = arena_from_handle(arena_handle);
    eg_asp_request_cell* current = request_rows_cell(
        arena, rows_handle, row, column, 0);
    eg_asp_request_cell* original = request_rows_cell(
        arena, rows_handle, row, column, 1);
    if (!current || !original) return 0;
    if (current->kind != original->kind) return 1;
    if (current->kind == 4)
        return current->floating != original->floating;
    if (current->kind == 5) {
        return current->text_len != original->text_len ||
            (current->text_len != 0 &&
             memcmp(current->text, original->text,
                    (size_t)current->text_len) != 0);
    }
    return current->integer != original->integer;
}

/* Reject stale generation tokens and count lifetime violations diagnostically. */
int64_t eg_asp_request_arena_escape_check(int64_t arena_handle,
                                          int64_t generation) {
    eg_asp_request_arena* arena = arena_from_handle(arena_handle);
    int valid = arena && generation > 0 &&
        arena->generation == (uint64_t)generation;
    if (!valid) {
        atomic_fetch_add_explicit(
            &arena_escape_failures, 1, memory_order_relaxed);
    }
    return valid;
}

int64_t eg_asp_request_arena_escape_failures(void) {
    uint64_t failures = atomic_load_explicit(
        &arena_escape_failures, memory_order_relaxed);
    return failures <= INT64_MAX ? (int64_t)failures : INT64_MAX;
}

static uint64_t configured_memory_token_budget(void) {
    uint64_t budget = atomic_load_explicit(
        &memory_token_budget, memory_order_acquire);
    if (budget) return budget;
    budget = EG_ASP_MEMORY_TOKEN_DEFAULT_BUDGET;
    const char* text = getenv("EGRET_ASP_REQUEST_MEMORY_MB");
    if (text && *text) {
        char* end = NULL;
        unsigned long long mb = strtoull(text, &end, 10);
        if (end != text && *end == '\0' && mb >= 32 && mb <= 4096) {
            budget = (uint64_t)mb * 1024ull * 1024ull;
        }
    }
    uint64_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &memory_token_budget, &expected, budget,
            memory_order_release, memory_order_acquire)) {
        budget = expected;
    }
    return budget;
}

/*
 * Reserve process-wide native-memory budget with an atomic CAS. This is
 * admission control; callers release the exact returned reservation.
 */
int64_t eg_asp_memory_token_try_acquire(int64_t predicted_bytes) {
    if (predicted_bytes <= 0) return 1;
    uint64_t requested = (uint64_t)predicted_bytes;
    uint64_t budget = configured_memory_token_budget();
    if (requested > budget) requested = budget;
    uint64_t used = atomic_load_explicit(
        &memory_token_used, memory_order_acquire);
    for (;;) {
        if (used > budget || requested > budget - used) return 0;
        if (atomic_compare_exchange_weak_explicit(
                &memory_token_used, &used, used + requested,
                memory_order_acq_rel, memory_order_acquire)) {
            return requested <= INT64_MAX ? (int64_t)requested : 0;
        }
    }
}

void eg_asp_memory_token_release(int64_t reservation) {
    if (reservation <= 1) return;
    uint64_t released = (uint64_t)reservation;
    uint64_t used = atomic_load_explicit(
        &memory_token_used, memory_order_acquire);
    for (;;) {
        uint64_t next = released < used ? used - released : 0;
        if (atomic_compare_exchange_weak_explicit(
                &memory_token_used, &used, next,
                memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
}
