// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_OPENASP_COMPONENTS_H
#define EG_OPENASP_COMPONENTS_H

#include <stdint.h>
#include "eg_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure in-process crypto and JSON helpers. Returned strings are managed Egret
 * objects; input pointers are borrowed for the duration of each call. */
eg_string_t* eg_openasp_sha256(eg_string_t* input);
eg_string_t* eg_openasp_hmac_sha256(eg_string_t* key, eg_string_t* input);
eg_string_t* eg_openasp_base64_encode(eg_string_t* input, int64_t url_safe);
eg_string_t* eg_openasp_base64_decode(eg_string_t* input, int64_t url_safe);
eg_string_t* eg_openasp_random_bytes(int64_t count);
int64_t eg_openasp_constant_time_equals(eg_string_t* left, eg_string_t* right);
int64_t eg_openasp_json_validate(eg_string_t* input);
eg_string_t* eg_openasp_json_minify(eg_string_t* input);
eg_string_t* eg_openasp_json_escape(eg_string_t* input);

/*
 * Process APIs never invoke a command shell: executable and parsed arguments
 * are passed to exec directly. Runtime capability checks occur in the Egret
 * object methods on every call; these C functions provide no policy boundary.
 */
eg_string_t* eg_openasp_process_run(eg_string_t* executable, eg_string_t* arguments, eg_string_t* directory, eg_string_t* stdin_data, int64_t timeout_ms, int64_t max_output);
int64_t eg_openasp_process_start(eg_string_t* executable, eg_string_t* arguments, eg_string_t* directory);
int64_t eg_openasp_process_wait(int64_t pid, int64_t timeout_ms);
int64_t eg_openasp_process_terminate(int64_t pid, int64_t signal_number);
int64_t eg_openasp_process_fork(void);
eg_string_t* eg_openasp_process_waitpid(int64_t pid, int64_t timeout_ms);
int64_t eg_openasp_process_signal(int64_t pid, int64_t signal_number);
int64_t eg_openasp_process_exit(int64_t exit_code);

#ifdef __cplusplus
}
#endif

#endif
