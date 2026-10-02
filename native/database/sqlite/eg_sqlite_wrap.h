// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_SQLITE_WRAP_H
#define EG_SQLITE_WRAP_H

#include "eg_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Executes one SQL statement through the in-process SQLite library. The return
 * value contains the wrapper's binary-safe result/error encoding and is owned
 * by the Egret runtime.
 */
eg_string_t* eg_sqlite_execute(eg_string_t* db_path, eg_string_t* sql);

#ifdef __cplusplus
}
#endif

#endif
