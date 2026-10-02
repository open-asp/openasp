// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#ifndef EG_MDBTOOLS_WRAP_H
#define EG_MDBTOOLS_WRAP_H

#include <stdint.h>
#include "eg_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Egret FFI for native MDB reads and transactional writes. Arguments are
 * borrowed managed strings; results are newly allocated managed strings using
 * the module's length-prefixed row/error protocol. No external mdb command is
 * executed. Write functions acquire the companion .eglock exclusively.
 */
eg_string_t* eg_mdb_table_dump(eg_string_t* db_path, eg_string_t* table_name);
eg_string_t* eg_mdb_query(eg_string_t* db_path, eg_string_t* table_name, eg_string_t* select_columns, int64_t where_kind, eg_string_t* where_column, eg_string_t* where_value, eg_string_t* order_column, int64_t order_desc, int64_t top_count, int64_t distinct, int64_t aggregate_kind);
eg_string_t* eg_mdb_sql_query(eg_string_t* db_path, eg_string_t* table_name, eg_string_t* select_columns, eg_string_t* where_sql, eg_string_t* order_column, int64_t order_desc, int64_t top_count, int64_t distinct, int64_t aggregate_kind);
eg_string_t* eg_mdb_join_query(eg_string_t* db_path, eg_string_t* left_table, eg_string_t* right_table, eg_string_t* select_columns, int64_t where_kind, eg_string_t* where_column, eg_string_t* where_value, eg_string_t* join_left_column, eg_string_t* join_right_column, int64_t top_count, int64_t distinct);
eg_string_t* eg_mdb_update(eg_string_t* db_path, eg_string_t* table_name, eg_string_t* where_column, eg_string_t* where_value, eg_string_t* set_columns, eg_string_t* set_values);
eg_string_t* eg_mdb_insert_row(eg_string_t* db_path, eg_string_t* table_name, eg_string_t* columns, eg_string_t* values);
eg_string_t* eg_mdb_delete(eg_string_t* db_path, eg_string_t* table_name, eg_string_t* where_column, eg_string_t* where_value);
eg_string_t* eg_mdb_create_table(eg_string_t* db_path, eg_string_t* sql);

/* Binary-safe transport helpers used by database and persistence formats. */
eg_string_t* eg_asp_hex_encode(eg_string_t* input);
eg_string_t* eg_asp_hex_decode(eg_string_t* input);

#ifdef __cplusplus
}
#endif

#endif
