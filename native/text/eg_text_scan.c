// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_string.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

/* Return contract: a nonnegative value is an owned descriptor holding the
 * lock, -2 means another builder currently owns it, and -1 is an I/O or
 * allocation failure. Keeping the descriptor open is what keeps flock alive. */
int64_t eg_asp_aot_build_try_lock(eg_string_t* path) {
    if (!path || !path->data || path->len == 0) return -1;
    char* native_path = (char*)malloc(path->len + 1);
    if (!native_path) return -1;
    memcpy(native_path, path->data, path->len);
    native_path[path->len] = '\0';
    int fd = open(native_path, O_CREAT | O_RDWR, 0600);
    free(native_path);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int lock_error = errno;
        if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
            close(fd);
            return -2;
        }
        close(fd);
        return -1;
    }
    return (int64_t)fd;
}

void eg_asp_aot_build_unlock(int64_t handle) {
    if (handle < 0) return;
    int fd = (int)handle;
    (void)flock(fd, LOCK_UN);
    close(fd);
}

/* Application.Lock must serialize all FPM workers, not only threads in one
 * worker. The caller keeps this descriptor for the complete load-modify-save
 * transaction and releases it after atomically publishing application.kv. */
int64_t eg_asp_application_state_lock(eg_string_t* path) {
    if (!path || !path->data || path->len == 0) return -1;
    char* native_path = (char*)malloc(path->len + 1);
    if (!native_path) return -1;
    memcpy(native_path, path->data, path->len);
    native_path[path->len] = '\0';
    int fd = open(native_path, O_CREAT | O_RDWR, 0600);
    free(native_path);
    if (fd < 0) return -1;
    while (flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            close(fd);
            return -1;
        }
    }
    return (int64_t)fd;
}

void eg_asp_application_state_unlock(int64_t handle) {
    eg_asp_aot_build_unlock(handle);
}


/* These leaf operations neither allocate nor retain their borrowed strings.
 * Explicit lengths preserve embedded NUL bytes and UTF-8 byte semantics. */
int64_t eg_asp_text_starts_with(eg_string_t* text, eg_string_t* prefix) {
    int64_t size = text ? text->len : 0;
    int64_t length = prefix ? prefix->len : 0;
    if (length == 0) return 1;
    if (length < 0 || size < length || !text || !text->data || !prefix->data) return 0;
    return memcmp(text->data, prefix->data, (size_t)length) == 0;
}

int64_t eg_asp_text_ends_with(eg_string_t* text, eg_string_t* suffix) {
    int64_t size = text ? text->len : 0;
    int64_t length = suffix ? suffix->len : 0;
    if (length == 0) return 1;
    if (length < 0 || size < length || !text || !text->data || !suffix->data) return 0;
    return memcmp(text->data + size - length, suffix->data, (size_t)length) == 0;
}

int64_t eg_asp_text_equal_fold_ascii(eg_string_t* left, eg_string_t* right) {
    int64_t size = left ? left->len : 0;
    if (size != (right ? right->len : 0)) return 0;
    if (size == 0 || left == right) return 1;
    if (size < 0 || !left->data || !right->data) return 0;
    for (int64_t i = 0; i < size; i++) {
        unsigned char a = (unsigned char)left->data[i];
        unsigned char b = (unsigned char)right->data[i];
        if (a == b) continue;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

/* Preserve the existing binary format's 31-bit checksum exactly. Unsigned
 * arithmetic avoids C signed-overflow UB for long inputs. */
int64_t eg_asp_text_checksum(eg_string_t* text) {
    uint32_t hash = 216613626u;
    if (!text || !text->data) return hash;
    for (int64_t i = 0; i < text->len; i++) {
        hash = ((hash ^ (unsigned char)text->data[i]) * 16777619u) & 2147483647u;
    }
    return hash;
}

int64_t eg_asp_text_checksum_range(eg_string_t* text, int64_t start, int64_t length) {
    uint32_t hash = 216613626u;
    if (!text || !text->data || start < 0 || length < 0 ||
        start > text->len || length > text->len - start) {
        return hash;
    }
    const unsigned char* data = (const unsigned char*)text->data + start;
    for (int64_t i = 0; i < length; i++) {
        hash = ((hash ^ data[i]) * 16777619u) & 2147483647u;
    }
    return hash;
}

/* Return the beginning of the next line that may contain template markup.
 * Plain source spans can then be appended once, without per-line strings,
 * lowercase copies, or character-by-character Egret calls. */
int64_t eg_asp_next_markup_line(eg_string_t* text, int64_t start) {
    if (!text || !text->data || start < 0 || start >= text->len) return text ? text->len : 0;
    const char* begin = text->data + start;
    const char* marker = memchr(begin, '<', (size_t)(text->len - start));
    if (!marker) return text->len;
    while (marker > begin && marker[-1] != '\n') marker--;
    return (int64_t)(marker - text->data);
}

int64_t eg_asp_count_newlines(eg_string_t* text, int64_t start, int64_t end) {
    if (!text || !text->data || start < 0 || end <= start) return 0;
    if (end > text->len) end = text->len;
    int64_t count = 0;
    while (start < end) {
        const char* found = memchr(text->data + start, '\n', (size_t)(end - start));
        if (!found) break;
        count++;
        start = (int64_t)(found - text->data) + 1;
    }
    return count;
}

/* The scanner starts immediately after the opening quote. VBScript escapes
 * quotes by doubling them; an unterminated literal consumes the remaining
 * source, matching the existing lexer. Neither scan allocates managed data. */
int64_t eg_asp_vbs_string_end(eg_string_t* text, int64_t start) {
    if (!text || !text->data || start < 0) return start;
    int64_t pos = start;
    while (pos < text->len) {
        const char* found = memchr(text->data + pos, '"', (size_t)(text->len - pos));
        if (!found) return text->len;
        pos = (int64_t)(found - text->data) + 1;
        if (pos == text->len || text->data[pos] != '"') return pos;
        pos++;
    }
    return pos;
}

eg_string_t* eg_asp_vbs_string_value(eg_string_t* text, int64_t start, int64_t end) {
    if (!text || !text->data || start < 0 || start > text->len || end < start) return eg_string_from_bytes("", 0);
    if (end > text->len) end = text->len;
    char* buffer = malloc((size_t)(end - start) + 1);
    if (!buffer) abort();
    size_t length = 0;
    for (int64_t pos = start; pos < end; pos++) {
        char ch = text->data[pos];
        if (ch == '"') {
            if (pos + 1 == end || text->data[pos + 1] != '"') break;
            pos++;
        }
        buffer[length++] = ch;
    }
    eg_string_t* result = eg_string_from_bytes(buffer, (int64_t)length);
    free(buffer);
    return result;
}

void* eg_asp_variant_or_fallback(void* value, void* fallback) {
    /* This is a pointer-selection primitive for generated code. Both values
     * are borrowed managed references; no ownership transfer occurs. */
    return value != NULL ? value : fallback;
}

int64_t eg_asp_packed_i64_at(eg_string_t* data, int64_t index, int64_t fallback) {
    if (!data || !data->data || index < 0 || index > (INT64_MAX / 8)) return fallback;
    int64_t offset = index * 8;
    if (offset < 0 || offset + 8 > data->len) return fallback;
    /* Packed runtime metadata is explicitly little-endian. Assemble bytes
     * instead of casting so unaligned mappings work on every target. */
    const unsigned char* p = (const unsigned char*)data->data + offset;
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value |= ((uint64_t)p[i]) << (i * 8);
    return (int64_t)value;
}
