// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_REGEXP_WRAP_H
#define EG_REGEXP_WRAP_H

#include <stdint.h>

#include "eg_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PCRE2-backed VBScript.RegExp primitives. `flags` carries IgnoreCase,
 * Global, and Multiline bits. Offsets and match metadata are encoded in
 * managed strings so no native match-data pointer crosses the FFI boundary.
 */
int64_t eg_vbregexp_test(eg_string_t* pattern, eg_string_t* input, int64_t flags);
eg_string_t* eg_vbregexp_find(eg_string_t* pattern, eg_string_t* input, int64_t start_offset, int64_t flags);
eg_string_t* eg_vbregexp_replace(eg_string_t* pattern, eg_string_t* input, eg_string_t* replacement, int64_t flags);

#ifdef __cplusplus
}
#endif

#endif
