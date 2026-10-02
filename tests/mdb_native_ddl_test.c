// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eg_mdb_ddl.h"
#include "eg_mdbtools_wrap.h"

/* End-to-end native Jet 4 DDL/DML harness. Local string constructors replace
 * the Egret runtime so this binary tests the MDB implementation directly and
 * treats an empty returned string as success, matching the language binding. */
eg_string_t* eg_string_from_bytes(const void* data, int64_t len) {
    eg_string_t* value = (eg_string_t*)calloc(1, sizeof(*value));
    if (!value) {
        return NULL;
    }
    value->data = (char*)malloc((size_t)len + 1);
    if (!value->data) {
        free(value);
        return NULL;
    }
    memcpy(value->data, data, (size_t)len);
    value->data[len] = '\0';
    value->len = len;
    return value;
}

eg_string_t* eg_string_from_cstr(const char* value) {
    return eg_string_from_bytes(value, (int64_t)strlen(value));
}

const char* eg_string_cstr(const eg_string_t* value) {
    return value ? value->data : "";
}

int64_t eg_string_len(const eg_string_t* value) {
    return value ? value->len : 0;
}

int64_t eg_string_byte_at(const eg_string_t* value, int64_t index) {
    return (value && index >= 0 && index < value->len) ? (unsigned char)value->data[index] : -1;
}

static eg_string_t eg_test_string(const char* value) {
    eg_string_t string;
    string.len = (int64_t)strlen(value);
    string.data = (char*)value;
    return string;
}

static int eg_test_result(const char* operation, eg_string_t* result) {
    int ok = result && result->len == 0;
    if (!ok) {
        fprintf(stderr, "%s: %s\n", operation, result ? result->data : "out of memory");
    }
    if (result) {
        free(result->data);
        free(result);
    }
    return ok;
}

static int eg_test_error(const char* operation, eg_string_t* result, const char* expected) {
    int ok = result && result->len > 0 && strstr(result->data, expected) != NULL;
    if (!ok) {
        fprintf(stderr, "%s: expected error containing \"%s\", got \"%s\"\n",
            operation, expected, result ? result->data : "out of memory");
    }
    if (result) {
        free(result->data);
        free(result);
    }
    return ok;
}

int main(int argc, char** argv) {
    eg_string_t database;
    eg_string_t sql = eg_test_string(
        "CREATE TABLE [native_crud] ("
        "[id] AutoIncrement primary key,"
        "[name] VARCHAR(50) default \"\" not null,"
        "[summary] MEMO default \"\","
        "[score] int default 7,"
        "[address] VARCHAR(50) default \"\","
        "[created] datetime default now())");
    eg_string_t table = eg_test_string("native_crud");
    eg_string_t insert_columns = eg_test_string("6e616d65");
    eg_string_t first_values = eg_test_string("736669727374");
    eg_string_t mixed_columns = eg_test_string(
        "6e616d65" "\x1f" "73756d6d617279" "\x1f" "61646472657373");
    eg_string_t mixed_values = eg_test_string(
        "736669727374" "\x1f" "736d656d6f2d76616c7565" "\x1f" "73616464726573732d76616c7565");
    eg_string_t second_values = eg_test_string("737365636f6e64");
    eg_string_t id_column = eg_test_string("6964");
    eg_string_t id_one = eg_test_string("7631");
    eg_string_t id_two = eg_test_string("7632");
    eg_string_t id_middle = eg_test_string("76333530");
    eg_string_t name_column = eg_test_string("6e616d65");
    eg_string_t updated_name = eg_test_string("7375706461746564");
    eg_string_t score_column = eg_test_string("73636f7265");
    eg_string_t score_nine = eg_test_string("7639");
    eg_string_t required_sql = eg_test_string(
        "CREATE TABLE [required_check] ("
        "[id] AutoIncrement primary key,"
        "[name] VARCHAR(20) not null,"
        "[score] int)");
    eg_string_t required_table = eg_test_string("required_check");
    eg_string_t score_value = eg_test_string("7631");
    eg_string_t auto_column = eg_test_string("6964");
    int bulk_index = 0;
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s <database> [create-table-sql]\n", argv[0]);
        return 2;
    }
    database = eg_test_string(argv[1]);
    if (argc == 3) {
        eg_string_t statement = eg_test_string(argv[2]);
        return eg_test_result("CREATE", eg_mdb_create_table(&database, &statement)) ? 0 : 1;
    }
    /* The first sequence covers ordinary CRUD and defaults. Seven hundred
     * inserts force a leaf split, after which deletion verifies that B-tree
     * traversal still reaches a row no longer stored in the original leaf. */
    if (!eg_test_result("CREATE", eg_mdb_create_table(&database, &sql))
        || !eg_test_result("first INSERT", eg_mdb_insert_row(&database, &table, &mixed_columns, &mixed_values))
        || !eg_test_result("second INSERT", eg_mdb_insert_row(&database, &table, &insert_columns, &second_values))
        || !eg_test_result("UPDATE", eg_mdb_update(&database, &table, &id_column, &id_one, &name_column, &updated_name))
        || !eg_test_result("fixed-column UPDATE", eg_mdb_update(&database, &table, &id_column, &id_one, &score_column, &score_nine))
        || !eg_test_result("DELETE", eg_mdb_delete(&database, &table, &id_column, &id_two))) {
        return 1;
    }
    while (bulk_index < 700) {
        if (!eg_test_result("bulk INSERT", eg_mdb_insert_row(&database, &table, &insert_columns, &first_values))) {
            return 1;
        }
        bulk_index++;
    }
    if (!eg_test_result("split-leaf DELETE", eg_mdb_delete(&database, &table, &id_column, &id_middle))) {
        return 1;
    }
    /* Required-column and AutoNumber failures are part of the public wrapper
     * contract; assert their diagnostics rather than accepting any failure. */
    if (!eg_test_result("required CREATE", eg_mdb_create_table(&database, &required_sql))
        || !eg_test_error("required INSERT",
            eg_mdb_insert_row(&database, &required_table, &score_column, &score_value),
            "required MDB column has no value")
        || !eg_test_error("explicit AutoNumber INSERT",
            eg_mdb_insert_row(&database, &required_table, &auto_column, &score_value),
            "explicit values for MDB AutoNumber columns are not supported")) {
        return 1;
    }
    return 0;
}
