// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_SHELL_WRAP_H
#define EG_SHELL_WRAP_H

#include "eg_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts an executable asynchronously without passing through a command shell.
 * Policy is intentionally outside this wrapper: the Egret Shell.Application
 * method must re-check its runtime capability flag on every invocation.
 */
eg_string_t* eg_shell_execute(eg_string_t* file_name, eg_string_t* arguments, eg_string_t* directory);

#ifdef __cplusplus
}
#endif

#endif
