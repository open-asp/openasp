// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#define PCRE2_CODE_UNIT_WIDTH 8

#include "eg_regexp_wrap.h"

#include <pcre2.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EG_REGEXP_CACHE_CAPACITY 128

/*
 * Process-wide immutable PCRE2 cache. Entries own normalized pattern bytes and
 * compiled code; the mutex covers lookup and insertion because PCRE2 match
 * contexts are allocated per call and do not need shared mutation.
 */
typedef struct {
    char* pattern;
    size_t pattern_len;
    uint32_t options;
    pcre2_code* code;
} eg_regexp_cache_entry_t;

static eg_regexp_cache_entry_t eg_regexp_cache[EG_REGEXP_CACHE_CAPACITY];
static size_t eg_regexp_cache_count = 0;
static pthread_mutex_t eg_regexp_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static int eg_regexp_is_hex(unsigned char ch) {
    return (ch >= '0' && ch <= '9')
        || (ch >= 'a' && ch <= 'f')
        || (ch >= 'A' && ch <= 'F');
}

/*
 * Translate VBScript \uFFFF escapes to PCRE2 \x{FFFF} only when the backslash
 * is not itself escaped. Other bytes are preserved exactly.
 */
static char* eg_regexp_normalize_pattern(const char* pattern, size_t pattern_len, size_t* normalized_len) {
    size_t i = 0;
    size_t out_len = 0;
    size_t slash_run = 0;
    char* normalized = (char*)malloc(pattern_len * 2 + 1);
    if (!normalized) {
        return NULL;
    }
    while (i < pattern_len) {
        if (i + 5 < pattern_len
            && pattern[i] == '\\'
            && slash_run % 2 == 0
            && (pattern[i + 1] == 'u' || pattern[i + 1] == 'U')
            && eg_regexp_is_hex((unsigned char)pattern[i + 2])
            && eg_regexp_is_hex((unsigned char)pattern[i + 3])
            && eg_regexp_is_hex((unsigned char)pattern[i + 4])
            && eg_regexp_is_hex((unsigned char)pattern[i + 5])) {
            normalized[out_len++] = '\\';
            normalized[out_len++] = 'x';
            normalized[out_len++] = '{';
            memcpy(normalized + out_len, pattern + i + 2, 4);
            out_len += 4;
            normalized[out_len++] = '}';
            i += 6;
            slash_run = 0;
            continue;
        }
        normalized[out_len++] = pattern[i];
        slash_run = pattern[i] == '\\' ? slash_run + 1 : 0;
        i += 1;
    }
    normalized[out_len] = '\0';
    *normalized_len = out_len;
    return normalized;
}

static uint32_t eg_regexp_options(int64_t flags) {
    uint32_t options = PCRE2_UTF | PCRE2_UCP;
    if ((flags & 2) != 0) {
        options |= PCRE2_CASELESS;
    }
    if ((flags & 4) != 0) {
        options |= PCRE2_MULTILINE;
    }
    return options;
}

/*
 * Return a borrowed cache entry when possible. On cache saturation the caller
 * receives an owned one-shot program and must free it after matching.
 */
static pcre2_code* eg_regexp_compile_cached(const char* pattern, size_t pattern_len, uint32_t options, int* owned) {
    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    pcre2_code* code = NULL;
    char* pattern_copy = NULL;
    char* normalized_pattern = NULL;
    size_t normalized_len = 0;
    size_t i = 0;
    *owned = 0;
    normalized_pattern = eg_regexp_normalize_pattern(pattern, pattern_len, &normalized_len);
    if (!normalized_pattern) {
        return NULL;
    }
    pthread_mutex_lock(&eg_regexp_cache_mutex);
    for (i = 0; i < eg_regexp_cache_count; i += 1) {
        eg_regexp_cache_entry_t* entry = &eg_regexp_cache[i];
        if (entry->options == options && entry->pattern_len == normalized_len && memcmp(entry->pattern, normalized_pattern, normalized_len) == 0) {
            code = entry->code;
            pthread_mutex_unlock(&eg_regexp_cache_mutex);
            free(normalized_pattern);
            return code;
        }
    }
    code = pcre2_compile((PCRE2_SPTR)normalized_pattern, normalized_len, options, &error_code, &error_offset, NULL);
    if (!code) {
        pthread_mutex_unlock(&eg_regexp_cache_mutex);
        free(normalized_pattern);
        return NULL;
    }
    if (eg_regexp_cache_count >= EG_REGEXP_CACHE_CAPACITY) {
        *owned = 1;
        pthread_mutex_unlock(&eg_regexp_cache_mutex);
        free(normalized_pattern);
        return code;
    }
    pattern_copy = (char*)malloc(normalized_len + 1);
    if (!pattern_copy) {
        *owned = 1;
        pthread_mutex_unlock(&eg_regexp_cache_mutex);
        free(normalized_pattern);
        return code;
    }
    memcpy(pattern_copy, normalized_pattern, normalized_len);
    pattern_copy[normalized_len] = '\0';
    eg_regexp_cache[eg_regexp_cache_count].pattern = pattern_copy;
    eg_regexp_cache[eg_regexp_cache_count].pattern_len = normalized_len;
    eg_regexp_cache[eg_regexp_cache_count].options = options;
    eg_regexp_cache[eg_regexp_cache_count].code = code;
    eg_regexp_cache_count += 1;
    pthread_mutex_unlock(&eg_regexp_cache_mutex);
    free(normalized_pattern);
    return code;
}

static eg_string_t* eg_regexp_copy_string(eg_string_t* value) {
    return eg_string_from_bytes(eg_string_cstr(value), eg_string_len(value));
}

int64_t eg_vbregexp_test(eg_string_t* pattern, eg_string_t* input, int64_t flags) {
    int owned = 0;
    int match_result = 0;
    pcre2_code* code = eg_regexp_compile_cached(eg_string_cstr(pattern), (size_t)eg_string_len(pattern), eg_regexp_options(flags), &owned);
    pcre2_match_data* match_data = NULL;
    if (!code) {
        return 0;
    }
    match_data = pcre2_match_data_create_from_pattern(code, NULL);
    if (match_data) {
        match_result = pcre2_match(code, (PCRE2_SPTR)eg_string_cstr(input), (PCRE2_SIZE)eg_string_len(input), 0, 0, match_data, NULL);
        pcre2_match_data_free(match_data);
    }
    if (owned) {
        pcre2_code_free(code);
    }
    return match_result >= 0 ? 1 : 0;
}

eg_string_t* eg_vbregexp_find(eg_string_t* pattern, eg_string_t* input, int64_t start_offset, int64_t flags) {
    int owned = 0;
    int match_result = 0;
    uint32_t capture_count = 0;
    size_t span_count = 0;
    size_t output_capacity = 0;
    size_t output_len = 0;
    char* output = NULL;
    PCRE2_SIZE* offsets = NULL;
    pcre2_code* code = NULL;
    pcre2_match_data* match_data = NULL;
    int64_t input_len = eg_string_len(input);
    if (start_offset < 0 || start_offset > input_len) {
        return eg_string_from_bytes("-1:-1", 5);
    }
    code = eg_regexp_compile_cached(eg_string_cstr(pattern), (size_t)eg_string_len(pattern), eg_regexp_options(flags), &owned);
    if (!code) {
        return eg_string_from_bytes("-1:-1", 5);
    }
    match_data = pcre2_match_data_create_from_pattern(code, NULL);
    if (match_data) {
        match_result = pcre2_match(code, (PCRE2_SPTR)eg_string_cstr(input), (PCRE2_SIZE)input_len, (PCRE2_SIZE)start_offset, 0, match_data, NULL);
    }
    if (match_result < 0) {
        if (match_data) {
            pcre2_match_data_free(match_data);
        }
        if (owned) {
            pcre2_code_free(code);
        }
        return eg_string_from_bytes("-1:-1", 5);
    }
    offsets = pcre2_get_ovector_pointer(match_data);
    if (pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &capture_count) != 0) {
        capture_count = match_result > 0 ? (uint32_t)(match_result - 1) : 0;
    }
    span_count = (size_t)capture_count + 1;
    output_capacity = span_count * 48 + 1;
    output = (char*)malloc(output_capacity);
    if (output) {
        size_t i = 0;
        for (i = 0; i < span_count; i += 1) {
            int written = 0;
            if (i > 0) {
                output[output_len] = ',';
                output_len += 1;
            }
            if (offsets[i * 2] == PCRE2_UNSET || offsets[i * 2 + 1] == PCRE2_UNSET) {
                written = snprintf(output + output_len, output_capacity - output_len, "-1:-1");
            } else {
                written = snprintf(output + output_len, output_capacity - output_len, "%llu:%llu", (unsigned long long)offsets[i * 2], (unsigned long long)offsets[i * 2 + 1]);
            }
            if (written < 0 || (size_t)written >= output_capacity - output_len) {
                free(output);
                output = NULL;
                break;
            }
            output_len += (size_t)written;
        }
    }
    pcre2_match_data_free(match_data);
    if (owned) {
        pcre2_code_free(code);
    }
    if (!output) {
        return eg_string_from_bytes("-1:-1", 5);
    }
    eg_string_t* result = eg_string_from_bytes(output, (int64_t)output_len);
    free(output);
    return result;
}

eg_string_t* eg_vbregexp_replace(eg_string_t* pattern, eg_string_t* input, eg_string_t* replacement, int64_t flags) {
    int owned = 0;
    int substitute_result = 0;
    uint32_t substitute_options = PCRE2_SUBSTITUTE_OVERFLOW_LENGTH | PCRE2_SUBSTITUTE_UNSET_EMPTY;
    PCRE2_SIZE output_len = 0;
    PCRE2_SIZE output_capacity = 0;
    unsigned char* output = NULL;
    unsigned char* resized = NULL;
    pcre2_code* code = eg_regexp_compile_cached(eg_string_cstr(pattern), (size_t)eg_string_len(pattern), eg_regexp_options(flags), &owned);
    if (!code) {
        return eg_regexp_copy_string(input);
    }
    if ((flags & 1) != 0) {
        substitute_options |= PCRE2_SUBSTITUTE_GLOBAL;
    }
    output_capacity = (PCRE2_SIZE)eg_string_len(input) + (PCRE2_SIZE)eg_string_len(replacement) + 64;
    if (output_capacity < 256) {
        output_capacity = 256;
    }
    output = (unsigned char*)malloc((size_t)output_capacity);
    if (!output) {
        if (owned) {
            pcre2_code_free(code);
        }
        return eg_regexp_copy_string(input);
    }
    output_len = output_capacity;
    substitute_result = pcre2_substitute(code, (PCRE2_SPTR)eg_string_cstr(input), (PCRE2_SIZE)eg_string_len(input), 0, substitute_options, NULL, NULL, (PCRE2_SPTR)eg_string_cstr(replacement), (PCRE2_SIZE)eg_string_len(replacement), output, &output_len);
    if (substitute_result == PCRE2_ERROR_NOMEMORY && output_len > output_capacity) {
        resized = (unsigned char*)realloc(output, (size_t)output_len);
        if (resized) {
            output = resized;
            output_capacity = output_len;
            output_len = output_capacity;
            substitute_result = pcre2_substitute(code, (PCRE2_SPTR)eg_string_cstr(input), (PCRE2_SIZE)eg_string_len(input), 0, substitute_options, NULL, NULL, (PCRE2_SPTR)eg_string_cstr(replacement), (PCRE2_SIZE)eg_string_len(replacement), output, &output_len);
        }
    }
    if (owned) {
        pcre2_code_free(code);
    }
    if (substitute_result < 0) {
        free(output);
        return eg_regexp_copy_string(input);
    }
    eg_string_t* result = eg_string_from_bytes((const char*)output, (int64_t)output_len);
    free(output);
    return result;
}
