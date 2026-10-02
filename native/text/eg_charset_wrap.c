// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_charset_wrap.h"

#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Validate UTF-8 locally instead of asking iconv to probe it. The source
 * loader needs a binary answer before choosing its GB18030 fallback, and this
 * routine rejects overlong encodings, UTF-16 surrogate values and code points
 * beyond U+10FFFF. The input buffer is borrowed and never modified. */
static int eg_source_valid_utf8(const unsigned char* data, size_t size) {
    size_t i = 0;
    while (i < size) {
        unsigned char first = data[i++];
        if (first < 0x80) continue;
        unsigned count = first >= 0xc2 && first <= 0xdf ? 1
            : (first >= 0xe0 && first <= 0xef ? 2 : (first >= 0xf0 && first <= 0xf4 ? 3 : 0));
        if (!count || size - i < count) return 0;
        unsigned char second = data[i];
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0)
            || (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return 0;
        for (unsigned j = 0; j < count; j++) {
            if ((data[i++] & 0xc0) != 0x80) return 0;
        }
    }
    return 1;
}

/* Match the source loader's strict utf-8-sig, then gb18030, then raw fallback.
 * UTF-8 input is borrowed directly; only BOM removal or transcoding allocates.
 * Do not use the response converter's lossy '?' replacement for source code. */
eg_string_t* eg_asp_decode_source(eg_string_t* input) {
    if (!input || input->len <= 0 || !input->data) return input;
    size_t size = (size_t)input->len;
    const unsigned char* data = (const unsigned char*)input->data;
    if (eg_source_valid_utf8(data, size)) {
        if (size >= 3 && memcmp(data, "\xef\xbb\xbf", 3) == 0) {
            return eg_string_from_bytes(data + 3, (int64_t)(size - 3));
        }
        return input;
    }
    if (size > (SIZE_MAX - 16) / 4) return input;
    iconv_t converter = iconv_open("UTF-8", "GB18030");
    if (converter == (iconv_t)-1) return input;
    size_t capacity = size * 4 + 16;
    char* output = malloc(capacity);
    if (!output) {
        iconv_close(converter);
        return input;
    }
    char* in_cursor = input->data;
    char* out_cursor = output;
    size_t in_left = size;
    size_t out_left = capacity;
    size_t result = iconv(converter, &in_cursor, &in_left, &out_cursor, &out_left);
    iconv_close(converter);
    eg_string_t* decoded = input;
    if (result != (size_t)-1 && in_left == 0) {
        decoded = eg_string_from_bytes(output, (int64_t)(out_cursor - output));
    }
    free(output);
    return decoded;
}

eg_string_t* eg_charset_convert(eg_string_t* input, eg_string_t* from_charset, eg_string_t* to_charset) {
    const char* input_bytes = eg_string_cstr(input);
    const char* from_name = eg_string_cstr(from_charset);
    const char* to_name = eg_string_cstr(to_charset);
    size_t input_len = (size_t)eg_string_len(input);
    size_t output_cap = 16;
    size_t input_left = input_len;
    size_t output_left = 0;
    char* input_cursor = (char*)input_bytes;
    char* output = NULL;
    char* output_cursor = NULL;
    char* next_output = NULL;
    size_t used = 0;
    size_t next_cap = 0;
    eg_string_t* result = NULL;
    if (input_len > (SIZE_MAX - 16) / 4) {
        return eg_string_from_bytes("", 0);
    }
    output_cap = input_len > 0 ? input_len * 4 + 16 : 16;
    output_left = output_cap;
    iconv_t converter = iconv_open(to_name, from_name);
    if (converter == (iconv_t)-1) {
        return eg_string_from_bytes("", 0);
    }
    output = (char*)malloc(output_cap);
    if (!output) {
        iconv_close(converter);
        return eg_string_from_bytes("", 0);
    }
    output_cursor = output;
    /* Response conversion is intentionally lossy: advance one source byte for
     * malformed input and emit '?', so a bad byte cannot discard the complete
     * response. E2BIG instead grows the native staging buffer while preserving
     * cursor offsets across realloc. */
    while (input_left > 0) {
        if (iconv(converter, &input_cursor, &input_left, &output_cursor, &output_left) != (size_t)-1) {
            continue;
        }
        if (errno == EILSEQ || errno == EINVAL) {
            if (output_left == 0) {
                errno = E2BIG;
            } else {
                *output_cursor++ = '?';
                output_left -= 1;
                input_cursor += 1;
                input_left -= 1;
                continue;
            }
        }
        if (errno != E2BIG || output_cap > SIZE_MAX / 2) {
            free(output);
            iconv_close(converter);
            return eg_string_from_bytes("", 0);
        }
        used = (size_t)(output_cursor - output);
        next_cap = output_cap * 2;
        next_output = (char*)realloc(output, next_cap);
        if (!next_output) {
            free(output);
            iconv_close(converter);
            return eg_string_from_bytes("", 0);
        }
        output = next_output;
        output_cap = next_cap;
        output_cursor = output + used;
        output_left = output_cap - used;
    }
    result = eg_string_from_bytes(output, (int64_t)(output_cursor - output));
    free(output);
    iconv_close(converter);
    return result;
}
