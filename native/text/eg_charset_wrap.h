// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_CHARSET_WRAP_H
#define EG_CHARSET_WRAP_H

#include "eg_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Binary-safe iconv bridge. Unknown encodings and conversion failures are
 * reported through the wrapper's managed-string error convention. */
eg_string_t* eg_charset_convert(eg_string_t* input, eg_string_t* from_charset, eg_string_t* to_charset);

/* Detects BOM/legacy ASP source encodings and returns normalized source text. */
eg_string_t* eg_asp_decode_source(eg_string_t* input);

#ifdef __cplusplus
}
#endif

#endif
