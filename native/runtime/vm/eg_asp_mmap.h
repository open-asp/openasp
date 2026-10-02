// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_ASP_MMAP_H
#define EG_ASP_MMAP_H

#include <stdint.h>

typedef struct eg_string eg_string_t;

/*
 * Read-only mapping API exposed through opaque 64-bit handles. Every range
 * operation validates offset and length; eg_asp_mmap_data returns a borrowed
 * pointer valid only until the matching handle is closed.
 */
int64_t eg_asp_mmap_open(eg_string_t* path);
void eg_asp_mmap_close(int64_t handle);
int64_t eg_asp_mmap_length(int64_t handle);
int64_t eg_asp_mmap_matches(int64_t handle, int64_t offset,
                            eg_string_t* expected);
int64_t eg_asp_mmap_read_u32(int64_t handle, int64_t offset);
int64_t eg_asp_mmap_read_i64(int64_t handle, int64_t offset);
int64_t eg_asp_mmap_checksum(int64_t handle, int64_t offset, int64_t length);
eg_string_t* eg_asp_mmap_copy_string(int64_t handle, int64_t offset,
                                     int64_t length);
const void* eg_asp_mmap_data(int64_t handle, int64_t offset, int64_t length);

#endif
