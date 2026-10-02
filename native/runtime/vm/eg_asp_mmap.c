// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_asp_mmap.h"

#include "eg_string.h"

#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define EG_ASP_MMAP_MAGIC UINT64_C(0x4547414d4d415031)

/*
 * Read-only AOT mapping exposed as an opaque integer handle. The engine owns
 * the handle for as long as any decoded page or bytecode program borrows spans
 * from data; close invalidates the magic before unmapping.
 */
typedef struct eg_asp_mmap {
    uint64_t magic;
    unsigned char* data;
    size_t length;
} eg_asp_mmap;

static eg_asp_mmap* mapping_from_handle(int64_t handle) {
    eg_asp_mmap* mapping = (eg_asp_mmap*)(intptr_t)handle;
    return mapping && mapping->magic == EG_ASP_MMAP_MAGIC ? mapping : NULL;
}

/* Overflow-safe half-open range validation shared by every accessor. */
static int mapping_range(const eg_asp_mmap* mapping, int64_t offset,
                         int64_t length) {
    if (!mapping || offset < 0 || length < 0) return 0;
    uint64_t start = (uint64_t)offset;
    uint64_t count = (uint64_t)length;
    return start <= mapping->length && count <= mapping->length - start;
}

int64_t eg_asp_mmap_open(eg_string_t* path) {
    if (!path || !path->data || path->len <= 0 || path->len >= PATH_MAX) {
        return 0;
    }
    char native_path[PATH_MAX];
    memcpy(native_path, path->data, (size_t)path->len);
    native_path[path->len] = '\0';
    int fd = open(native_path, O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (uint64_t)st.st_size > SIZE_MAX) {
        close(fd);
        return 0;
    }
    size_t length = (size_t)st.st_size;
    void* data = mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) return 0;
    eg_asp_mmap* mapping = (eg_asp_mmap*)calloc(1, sizeof(*mapping));
    if (!mapping) {
        munmap(data, length);
        return 0;
    }
    mapping->magic = EG_ASP_MMAP_MAGIC;
    mapping->data = (unsigned char*)data;
    mapping->length = length;
#if defined(MADV_RANDOM)
    (void)madvise(data, length, MADV_RANDOM);
#endif
    return (int64_t)(intptr_t)mapping;
}

void eg_asp_mmap_close(int64_t handle) {
    eg_asp_mmap* mapping = mapping_from_handle(handle);
    if (!mapping) return;
    mapping->magic = 0;
    munmap(mapping->data, mapping->length);
    free(mapping);
}

int64_t eg_asp_mmap_length(int64_t handle) {
    eg_asp_mmap* mapping = mapping_from_handle(handle);
    return mapping && mapping->length <= INT64_MAX
        ? (int64_t)mapping->length : -1;
}

int64_t eg_asp_mmap_matches(int64_t handle, int64_t offset,
                            eg_string_t* expected) {
    eg_asp_mmap* mapping = mapping_from_handle(handle);
    int64_t length = expected ? expected->len : 0;
    if (!mapping_range(mapping, offset, length)) return 0;
    if (length == 0) return 1;
    return expected->data &&
        memcmp(mapping->data + offset, expected->data, (size_t)length) == 0;
}

int64_t eg_asp_mmap_read_u32(int64_t handle, int64_t offset) {
    eg_asp_mmap* mapping = mapping_from_handle(handle);
    if (!mapping_range(mapping, offset, 4)) return -1;
    const unsigned char* p = mapping->data + offset;
    return (int64_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

int64_t eg_asp_mmap_read_i64(int64_t handle, int64_t offset) {
    eg_asp_mmap* mapping = mapping_from_handle(handle);
    if (!mapping_range(mapping, offset, 8)) return 0;
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) {
        value |= (uint64_t)mapping->data[offset + i] << (i * 8);
    }
    return (int64_t)value;
}

int64_t eg_asp_mmap_checksum(int64_t handle, int64_t offset, int64_t length) {
    eg_asp_mmap* mapping = mapping_from_handle(handle);
    if (!mapping_range(mapping, offset, length)) return -1;
    uint32_t hash = 216613626u;
    const unsigned char* data = mapping->data + offset;
    for (int64_t i = 0; i < length; i++) {
        hash = ((hash ^ data[i]) * 16777619u) & 2147483647u;
    }
    return hash;
}

eg_string_t* eg_asp_mmap_copy_string(int64_t handle, int64_t offset,
                                     int64_t length) {
    eg_asp_mmap* mapping = mapping_from_handle(handle);
    if (!mapping_range(mapping, offset, length)) {
        return eg_string_from_bytes("", 0);
    }
    return eg_string_from_bytes(mapping->data + offset, length);
}

/*
 * Return a borrowed span, never an owned allocation. Callers must retain the
 * mapping handle and must not write through the pointer.
 */
const void* eg_asp_mmap_data(int64_t handle, int64_t offset, int64_t length) {
    eg_asp_mmap* mapping = mapping_from_handle(handle);
    return mapping_range(mapping, offset, length)
        ? mapping->data + offset : NULL;
}
