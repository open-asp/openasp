// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_asp_mmap.h"
#include "eg_string.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Validate the mapped-AOT reader against a deliberately tiny binary image.
 * This test supplies only the Egret string constructor required by the native
 * module, so failures remain attributable to mapping and bounds logic. */
eg_string_t* eg_string_from_bytes(const void* data, int64_t len) {
    eg_string_t* value = calloc(1, sizeof(*value));
    assert(value);
    value->len = len;
    value->data = malloc((size_t)len + 1);
    assert(value->data);
    memcpy(value->data, data, (size_t)len);
    value->data[len] = '\0';
    return value;
}

int main(void) {
    char path[] = "/tmp/egret-asp-mmap-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    const unsigned char data[] = {
        'E', 'G', 'A', 'O', 'T', '0', '0', '1',
        6, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8
    };
    assert(write(fd, data, sizeof(data)) == (ssize_t)sizeof(data));
    close(fd);

    eg_string_t path_string = {(int64_t)strlen(path), path};
    int64_t mapping = eg_asp_mmap_open(&path_string);
    assert(mapping != 0);
    assert(eg_asp_mmap_length(mapping) == (int64_t)sizeof(data));
    eg_string_t magic = {8, "EGAOT001"};
    assert(eg_asp_mmap_matches(mapping, 0, &magic) == 1);
    assert(eg_asp_mmap_read_u32(mapping, 8) == 6);
    assert(eg_asp_mmap_read_i64(mapping, 12) ==
           INT64_C(0x0807060504030201));
    /* Reads ending exactly at EOF are valid; crossing it must fail without
     * dereferencing past the mapping. The data checks mirror that boundary. */
    assert(eg_asp_mmap_read_u32(mapping, 18) == -1);
    assert(eg_asp_mmap_data(mapping, 4, 16) != NULL);
    assert(eg_asp_mmap_data(mapping, 5, 16) == NULL);
    eg_asp_mmap_close(mapping);
    unlink(path);
    return 0;
}
