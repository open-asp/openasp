// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_string.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Serialize scalar AST fields without temporary managed strings or vectors.
 * Borrowed inputs are copied before the single managed allocation at the end;
 * no Egret object layout or moving-GC assumptions cross this boundary. */
typedef struct {
    const char* data;
    size_t length;
    char decimal[21];
} eg_pack_field;

/* ABI shared with collections.Vector<String>. The managed wrapper contains
 * only this native handle; native strings are owned by the vector and remain
 * stable for the duration of this non-mutating call. */
typedef struct {
    char** data;
    size_t* lengths;
    size_t length;
    size_t capacity;
    size_t accounted_bytes;
    size_t string_bytes;
} eg_string_vector;

typedef struct {
    void* type_descriptor;
    int64_t handle;
} eg_string_vector_wrapper;

static eg_pack_field eg_pack_integer(int64_t value) {
    eg_pack_field field = {0};
    uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    char reverse[20];
    size_t count = 0;
    do {
        reverse[count++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (value < 0) field.decimal[field.length++] = '-';
    while (count) field.decimal[field.length++] = reverse[--count];
    return field;
}

static eg_pack_field eg_pack_string(eg_string_t* value) {
    eg_pack_field field = {0};
    field.data = value && value->data ? value->data : "";
    field.length = value && value->len > 0 ? (size_t)value->len : 0;
    return field;
}

static size_t eg_pack_digits(size_t value) {
    size_t length = 1;
    while (value >= 10) {
        length++;
        value /= 10;
    }
    return length;
}

static char* eg_pack_length(char* out, size_t length) {
    size_t digits = eg_pack_digits(length);
    char* end = out + digits;
    char* cursor = end;
    do {
        *--cursor = (char)('0' + length % 10);
        length /= 10;
    } while (length);
    *end++ = ':';
    return end;
}

static eg_string_t* eg_pack_fields(eg_pack_field* fields, size_t count) {
    size_t size = eg_pack_digits(count) + 1;
    for (size_t i = 0; i < count; i++) {
        size_t prefix = eg_pack_digits(fields[i].length) + 1;
        if (fields[i].length > SIZE_MAX - prefix || size > SIZE_MAX - prefix - fields[i].length) abort();
        size += prefix + fields[i].length;
    }
    if (size > INT64_MAX) abort();
    char* buffer = malloc(size);
    if (!buffer) abort();
    char* out = eg_pack_length(buffer, count);
    for (size_t i = 0; i < count; i++) {
        out = eg_pack_length(out, fields[i].length);
        memcpy(out, fields[i].data ? fields[i].data : fields[i].decimal, fields[i].length);
        out += fields[i].length;
    }
    eg_string_t* result = eg_string_from_bytes(buffer, (int64_t)size);
    free(buffer);
    return result;
}

eg_string_t* eg_asp_pack_string_parts(eg_string_vector_wrapper* wrapper) {
    eg_string_vector* vector = wrapper ? (eg_string_vector*)(uintptr_t)wrapper->handle : NULL;
    if (!vector || vector->length == 0) return eg_string_from_bytes("0:", 2);
    size_t size = eg_pack_digits(vector->length) + 1;
    for (size_t i = 0; i < vector->length; i++) {
        size_t length = vector->lengths[i];
        size_t prefix = eg_pack_digits(length) + 1;
        if (length > SIZE_MAX - prefix || size > SIZE_MAX - prefix - length) abort();
        size += prefix + length;
    }
    if (size > INT64_MAX) abort();
    char* buffer = malloc(size);
    if (!buffer) abort();
    char* out = eg_pack_length(buffer, vector->length);
    for (size_t i = 0; i < vector->length; i++) {
        size_t length = vector->lengths[i];
        out = eg_pack_length(out, length);
        if (length) memcpy(out, vector->data[i], length);
        out += length;
    }
    eg_string_t* result = eg_string_from_bytes(buffer, (int64_t)size);
    free(buffer);
    return result;
}

eg_string_t* eg_asp_pack_expr(int64_t kind, eg_string_t* text, int64_t value, int64_t boolean, int64_t left, int64_t right, eg_string_t* args) {
    eg_pack_field fields[] = {
        eg_pack_integer(kind), eg_pack_string(text), eg_pack_integer(value),
        eg_pack_integer(boolean != 0), eg_pack_integer(left), eg_pack_integer(right), eg_pack_string(args)
    };
    return eg_pack_fields(fields, sizeof(fields) / sizeof(fields[0]));
}

eg_string_t* eg_asp_pack_stmt(int64_t kind, eg_string_t* names, eg_string_t* exprs, int64_t target, eg_string_t* target_name, eg_string_t* target_args, int64_t value, int64_t extra, int64_t extra2, eg_string_t* call_name, eg_string_t* call_args, int64_t condition, int64_t then_block, int64_t else_block, int64_t body, eg_string_t* cases, int64_t routine, int64_t cls, int64_t boolean) {
    eg_pack_field fields[] = {
        eg_pack_integer(kind), eg_pack_string(names), eg_pack_string(exprs), eg_pack_integer(target),
        eg_pack_string(target_name), eg_pack_string(target_args), eg_pack_integer(value),
        eg_pack_integer(extra), eg_pack_integer(extra2), eg_pack_string(call_name), eg_pack_string(call_args),
        eg_pack_integer(condition), eg_pack_integer(then_block), eg_pack_integer(else_block),
        eg_pack_integer(body), eg_pack_string(cases), eg_pack_integer(routine), eg_pack_integer(cls),
        eg_pack_integer(boolean != 0)
    };
    return eg_pack_fields(fields, sizeof(fields) / sizeof(fields[0]));
}
