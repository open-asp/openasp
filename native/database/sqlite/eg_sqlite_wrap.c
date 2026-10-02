// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_sqlite_wrap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

/*
 * SQLite adapter emits the same tab-separated, typed-header format as MDB.
 * The builder owns malloc storage until a final eg_string_t copy is created;
 * embedded row/column separators are sanitized to preserve framing.
 */
typedef struct EgSqliteStringBuilder {
    char* data;
    size_t len;
    size_t cap;
} EgSqliteStringBuilder;

static int eg_sqlite_sb_reserve(EgSqliteStringBuilder* sb, size_t extra) {
    size_t need = 0;
    size_t next_cap = 0;
    char* next_data = NULL;
    if (extra > SIZE_MAX - sb->len - 1) {
        return 0;
    }
    need = sb->len + extra + 1;
    if (need <= sb->cap) {
        return 1;
    }
    next_cap = sb->cap ? sb->cap : 256;
    while (next_cap < need) {
        if (next_cap > SIZE_MAX / 2) {
            next_cap = need;
            break;
        }
        next_cap *= 2;
    }
    next_data = (char*)realloc(sb->data, next_cap);
    if (!next_data) {
        return 0;
    }
    sb->data = next_data;
    sb->cap = next_cap;
    return 1;
}

static int eg_sqlite_sb_append_bytes(EgSqliteStringBuilder* sb, const void* data, size_t len) {
    if (!eg_sqlite_sb_reserve(sb, len)) {
        return 0;
    }
    if (len) {
        memcpy(sb->data + sb->len, data, len);
        sb->len += len;
    }
    sb->data[sb->len] = '\0';
    return 1;
}

static int eg_sqlite_sb_append_cstr(EgSqliteStringBuilder* sb, const char* text) {
    return eg_sqlite_sb_append_bytes(sb, text ? text : "", text ? strlen(text) : 0);
}

static int eg_sqlite_sb_append_char(EgSqliteStringBuilder* sb, char ch) {
    return eg_sqlite_sb_append_bytes(sb, &ch, 1);
}

static int eg_sqlite_sb_append_cell(EgSqliteStringBuilder* sb, const unsigned char* value, int value_len) {
    int i = 0;
    if (!value || value_len <= 0) {
        return 1;
    }
    /* The shared ADO parser consumes tab-separated rows, so embedded separators must not alter the result shape. */
    while (i < value_len) {
        unsigned char ch = value[i++];
        if (ch == '\t' || ch == '\r' || ch == '\n') {
            ch = ' ';
        }
        if (!eg_sqlite_sb_append_bytes(sb, &ch, 1)) {
            return 0;
        }
    }
    return 1;
}

static eg_string_t* eg_sqlite_error_result(const char* message) {
    EgSqliteStringBuilder sb = {0};
    eg_string_t* result = NULL;
    if (!eg_sqlite_sb_append_cstr(&sb, "Error: ") || !eg_sqlite_sb_append_cstr(&sb, message ? message : "unknown SQLite error")) {
        free(sb.data);
        return eg_string_from_cstr("Error: out of memory");
    }
    result = eg_string_from_bytes(sb.data, (int64_t)sb.len);
    free(sb.data);
    return result;
}

static int eg_sqlite_append_statement(EgSqliteStringBuilder* sb, sqlite3_stmt* statement) {
    int column_count = sqlite3_column_count(statement);
    int column_index = 0;
    int step_result = SQLITE_OK;
    if (column_count > 0) {
        while (column_index < column_count) {
            if (column_index > 0 && !eg_sqlite_sb_append_char(sb, '\t')) {
                return SQLITE_NOMEM;
            }
            if (!eg_sqlite_sb_append_cstr(sb, sqlite3_column_name(statement, column_index))) {
                return SQLITE_NOMEM;
            }
            column_index++;
        }
        if (!eg_sqlite_sb_append_char(sb, '\n')) {
            return SQLITE_NOMEM;
        }
    }
    while ((step_result = sqlite3_step(statement)) == SQLITE_ROW) {
        column_index = 0;
        while (column_index < column_count) {
            const unsigned char* value = NULL;
            int value_len = 0;
            if (column_index > 0 && !eg_sqlite_sb_append_char(sb, '\t')) {
                return SQLITE_NOMEM;
            }
            if (sqlite3_column_type(statement, column_index) != SQLITE_NULL) {
                value = sqlite3_column_text(statement, column_index);
                value_len = sqlite3_column_bytes(statement, column_index);
                if (!value && value_len > 0) {
                    return SQLITE_NOMEM;
                }
                if (!eg_sqlite_sb_append_cell(sb, value, value_len)) {
                    return SQLITE_NOMEM;
                }
            }
            column_index++;
        }
        if (!eg_sqlite_sb_append_char(sb, '\n')) {
            return SQLITE_NOMEM;
        }
    }
    return step_result;
}

/*
 * Prepare and step every statement in the input. Result-producing statements
 * append one table, while errors abort immediately with a stable "Error:"
 * prefix consumed by the Egret ADO layer.
 */
eg_string_t* eg_sqlite_execute(eg_string_t* db_path, eg_string_t* sql) {
    sqlite3* db = NULL;
    sqlite3_stmt* statement = NULL;
    EgSqliteStringBuilder output = {0};
    const char* sql_cursor = eg_string_cstr(sql);
    const char* sql_end = sql_cursor + eg_string_len(sql);
    int result = sqlite3_open_v2(eg_string_cstr(db_path), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (result != SQLITE_OK) {
        eg_string_t* error = eg_sqlite_error_result(db ? sqlite3_errmsg(db) : sqlite3_errstr(result));
        sqlite3_close(db);
        return error;
    }
    sqlite3_busy_timeout(db, 5000);
    /* Advance through every prepared statement so ADO Execute also supports transactional setup batches. */
    while (sql_cursor < sql_end) {
        const char* tail = NULL;
        while (sql_cursor < sql_end && (*sql_cursor == ' ' || *sql_cursor == '\t' || *sql_cursor == '\r' || *sql_cursor == '\n' || *sql_cursor == ';')) {
            sql_cursor++;
        }
        if (sql_cursor >= sql_end) {
            break;
        }
        result = sqlite3_prepare_v2(db, sql_cursor, (int)(sql_end - sql_cursor), &statement, &tail);
        if (result != SQLITE_OK) {
            eg_string_t* error = eg_sqlite_error_result(sqlite3_errmsg(db));
            free(output.data);
            sqlite3_finalize(statement);
            sqlite3_close(db);
            return error;
        }
        if (!statement) {
            if (!tail || tail <= sql_cursor) {
                break;
            }
            sql_cursor = tail;
            continue;
        }
        result = eg_sqlite_append_statement(&output, statement);
        sqlite3_finalize(statement);
        statement = NULL;
        if (result != SQLITE_DONE) {
            eg_string_t* error = eg_sqlite_error_result(result == SQLITE_NOMEM ? "out of memory" : sqlite3_errmsg(db));
            free(output.data);
            sqlite3_close(db);
            return error;
        }
        if (!tail || tail <= sql_cursor) {
            break;
        }
        sql_cursor = tail;
    }
    sqlite3_close(db);
    eg_string_t* result_string = eg_string_from_bytes(output.data ? output.data : "", (int64_t)output.len);
    free(output.data);
    return result_string;
}
