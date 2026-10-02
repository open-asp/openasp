// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_VBSCRIPT_COMPAT_H
#define EG_VBSCRIPT_COMPAT_H

#include <stdint.h>
#include "eg_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Date/time values are Unix-epoch milliseconds in local-time VBScript
 * semantics. eg_vbs_prepare_timezone() must run once in each worker process. */
int64_t eg_vbs_now_millis(void);
void eg_vbs_prepare_timezone(void);
int64_t eg_vbs_make_date(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second);
int64_t eg_vbs_date_add(int64_t millis, int64_t interval, int64_t amount);
int64_t eg_vbs_date_part(int64_t millis, int64_t part);
int64_t eg_vbs_parse_date(eg_string_t* text);
eg_string_t* eg_vbs_format_date(int64_t millis, int64_t mode);
eg_string_t* eg_vbs_chr_w(int64_t codepoint);
int64_t eg_vbs_sgn(double value);
double eg_vbs_atn(double value);
double eg_vbs_sin(double value);
double eg_vbs_cos(double value);
double eg_vbs_tan(double value);
double eg_vbs_exp(double value);
double eg_vbs_log(double value);

/* Builtin classifiers let the compiler/runtime select fast paths without
 * duplicating the canonical case-insensitive name tables in Egret code. */
int64_t eg_vbs_is_compat_builtin(eg_string_t* name);
int64_t eg_vbs_is_values_only_builtin(eg_string_t* name);
int64_t eg_vbs_is_runtime_builtin(eg_string_t* name);
int64_t eg_vbs_string_to_bool(eg_string_t* text);
int64_t eg_asp_parse_decimal_range(eg_string_t* text, int64_t start, int64_t length);

/*
 * Explicit GC pins bridge native handles and moving managed objects. Pin
 * helpers encode known field layouts; every successful pin requires exactly
 * one eg_asp_gc_unpin_object call.
 */
int64_t eg_asp_gc_pin_object(void* object);
int64_t eg_asp_gc_pin_field(void* object, int64_t offset);
int64_t eg_asp_gc_pin_compiled_page(void* object);
int64_t eg_asp_gc_pin_page_nodes(void* object);
int64_t eg_asp_gc_pin_program_blobs(void* object);
int64_t eg_asp_gc_pin_programs(void* object);
int64_t eg_asp_gc_pin_program(void* object);
int64_t eg_asp_gc_pin_page_nodes_data(void* object);
int64_t eg_asp_gc_pin_programs_data(void* object);
void* eg_asp_gc_pinned_object(int64_t pin);
void eg_asp_gc_unpin_object(int64_t pin);

#ifdef __cplusplus
}
#endif

#endif
