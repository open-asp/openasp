// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_mdb_ddl.h"
#include "eg_mdb_lock.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <mdbtools.h>

#define EG_JET4_PAGE_SIZE 4096
#define EG_JET4_MAX_COLUMNS 255
#define EG_JET4_TDEF_HEADER_SIZE 63
#define EG_JET4_COLUMN_SIZE 25
#define EG_JET4_USAGE_ROW_SIZE 69
#define EG_JET4_INDEX_ROW_COUNT_SIZE 12
#define EG_JET4_INDEX_COLUMN_BLOCK_SIZE 52
#define EG_JET4_INDEX_INFO_BLOCK_SIZE 28
#define EG_JET4_INDEX_ENTRY_MASK_SIZE 453
#define EG_JET4_TABLE_PARENT_ID 0x0f000001U
#define EG_JET4_TABLE_MAGIC 1625U
#define EG_JET4_INDEX_MAGIC 1923U

extern ssize_t mdb_write_pg(MdbHandle* mdb, unsigned long pg);

/*
 * This file implements the supported Jet 4 CREATE TABLE subset directly from
 * page-format structures. Parsing is intentionally strict: unsupported DDL is
 * rejected before an exclusive lock or journal is created.
 */
typedef struct EgDdlColumn {
    /* Parsed schema plus offsets assigned during the layout pass. */
    char* name;
    uint8_t type;
    uint16_t length;
    uint8_t flags;
    uint8_t ext_flags;
    uint16_t variable_index;
    uint16_t fixed_offset;
    int variable;
    int long_value;
    int required;
    int primary_key;
    char* default_literal;
    char* default_expression;
} EgDdlColumn;

typedef struct EgDdlTable {
    /* Fixed-size storage bounds the Jet format's 255-column maximum. */
    char* name;
    char* primary_key_name;
    EgDdlColumn columns[EG_JET4_MAX_COLUMNS];
    size_t column_count;
    size_t variable_count;
    size_t long_value_count;
    int primary_key_column;
} EgDdlTable;

typedef struct EgDdlBuffer {
    /* Growable encoder used for variable-length Jet property records. */
    unsigned char* data;
    size_t length;
    size_t capacity;
} EgDdlBuffer;

static void eg_ddl_error(char* error, size_t capacity, const char* message) {
    if (error && capacity) {
        snprintf(error, capacity, "%s", message ? message : "unknown error");
    }
}

static void eg_ddl_table_free(EgDdlTable* table) {
    size_t i = 0;
    if (!table) {
        return;
    }
    free(table->name);
    free(table->primary_key_name);
    while (i < table->column_count) {
        free(table->columns[i].name);
        free(table->columns[i].default_literal);
        free(table->columns[i].default_expression);
        i++;
    }
    memset(table, 0, sizeof(*table));
}

static const char* eg_ddl_skip_space(const char* cursor) {
    while (cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    return cursor;
}

static int eg_ddl_is_ident_char(unsigned char ch) {
    return isalnum(ch) || ch == '_' || ch == '$';
}

static int eg_ddl_keyword(const char* cursor, const char* keyword, const char** after) {
    size_t length = strlen(keyword);
    cursor = eg_ddl_skip_space(cursor);
    if (strncasecmp(cursor, keyword, length) != 0 || eg_ddl_is_ident_char((unsigned char)cursor[length])) {
        return 0;
    }
    if (after) {
        *after = cursor + length;
    }
    return 1;
}

static char* eg_ddl_parse_identifier(const char** cursor, char* error, size_t error_capacity) {
    const char* start;
    const char* end;
    char terminator = '\0';
    char* result;
    size_t length;
    *cursor = eg_ddl_skip_space(*cursor);
    start = *cursor;
    if (*start == '[') {
        terminator = ']';
        start++;
    } else if (*start == '"' || *start == '`') {
        terminator = *start;
        start++;
    }
    end = start;
    if (terminator) {
        while (*end && *end != terminator) {
            end++;
        }
        if (*end != terminator) {
            eg_ddl_error(error, error_capacity, "unterminated quoted identifier");
            return NULL;
        }
        *cursor = end + 1;
    } else {
        while (eg_ddl_is_ident_char((unsigned char)*end)) {
            end++;
        }
        *cursor = end;
    }
    length = (size_t)(end - start);
    if (length == 0 || length > 64 || !g_utf8_validate(start, (gssize)length, NULL)) {
        eg_ddl_error(error, error_capacity, "identifier must be valid UTF-8 and at most 64 characters");
        return NULL;
    }
    result = (char*)malloc(length + 1);
    if (!result) {
        eg_ddl_error(error, error_capacity, "out of memory");
        return NULL;
    }
    memcpy(result, start, length);
    result[length] = '\0';
    return result;
}

static int eg_ddl_parse_uint(const char** cursor, unsigned int* value) {
    unsigned long parsed;
    char* end;
    *cursor = eg_ddl_skip_space(*cursor);
    errno = 0;
    parsed = strtoul(*cursor, &end, 10);
    if (errno || end == *cursor || parsed > UINT16_MAX) {
        return 0;
    }
    *value = (unsigned int)parsed;
    *cursor = end;
    return 1;
}

static char* eg_ddl_copy_range(const char* start, const char* end) {
    size_t length = (size_t)(end - start);
    char* value = (char*)malloc(length + 1);
    if (!value) {
        return NULL;
    }
    memcpy(value, start, length);
    value[length] = '\0';
    return value;
}

static int eg_ddl_set_primary_key(EgDdlTable* table, const char* name, char* error, size_t error_capacity) {
    if (table->primary_key_name) {
        eg_ddl_error(error, error_capacity, "CREATE TABLE supports only one primary key");
        return 0;
    }
    table->primary_key_name = strdup(name);
    if (!table->primary_key_name) {
        eg_ddl_error(error, error_capacity, "out of memory");
        return 0;
    }
    return 1;
}

static int eg_ddl_parse_default(const char** cursor, const char* end, EgDdlColumn* column, char* error, size_t error_capacity) {
    const char* start;
    const char* value_start;
    const char* value_end;
    char* value;
    char quote;
    size_t length;
    size_t out_length = 0;
    *cursor = eg_ddl_skip_space(*cursor);
    start = *cursor;
    if (start >= end) {
        eg_ddl_error(error, error_capacity, "DEFAULT requires a literal value");
        return 0;
    }
    quote = *start;
    if (quote == '\'' || quote == '"') {
        const char* scan = start + 1;
        value_start = scan;
        while (scan < end) {
            if (*scan == quote) {
                if (scan + 1 < end && scan[1] == quote) {
                    scan += 2;
                    continue;
                }
                break;
            }
            scan++;
        }
        if (scan >= end || *scan != quote) {
            eg_ddl_error(error, error_capacity, "unterminated DEFAULT string");
            return 0;
        }
        value_end = scan;
        value = (char*)malloc((size_t)(value_end - value_start) + 2);
        if (!value) {
            eg_ddl_error(error, error_capacity, "out of memory");
            return 0;
        }
        value[out_length++] = 's';
        scan = value_start;
        while (scan < value_end) {
            value[out_length++] = *scan;
            if (*scan == quote && scan + 1 < value_end && scan[1] == quote) {
                scan++;
            }
            scan++;
        }
        value[out_length] = '\0';
        *cursor = value_end + 1;
    } else {
        const char* scan = start;
        int depth = 0;
        while (scan < end) {
            if (*scan == '(') {
                depth++;
            } else if (*scan == ')' && depth > 0) {
                depth--;
            } else if (depth == 0 && isspace((unsigned char)*scan)) {
                break;
            }
            scan++;
        }
        value_end = scan;
        length = (size_t)(value_end - start);
        if (length == 0) {
            eg_ddl_error(error, error_capacity, "DEFAULT requires a literal value");
            return 0;
        }
        value = (char*)malloc(length + 2);
        if (!value) {
            eg_ddl_error(error, error_capacity, "out of memory");
            return 0;
        }
        if ((length == 5 && !strncasecmp(start, "now()", length))
            || (length == 6 && !strncasecmp(start, "date()", length))
            || (length == 9 && !strncasecmp(start, "getdate()", length))) {
            value[0] = 'd';
        } else if ((length == 4 && !strncasecmp(start, "true", length))
                   || (length == 5 && !strncasecmp(start, "false", length))
                   || (length == 4 && !strncasecmp(start, "null", length))
                   || *start == '+' || *start == '-' || isdigit((unsigned char)*start)) {
            value[0] = (length == 4 && !strncasecmp(start, "null", length)) ? 'n' : 'v';
        } else {
            free(value);
            eg_ddl_error(error, error_capacity, "unsupported DEFAULT expression");
            return 0;
        }
        memcpy(value + 1, start, length);
        value[length + 1] = '\0';
        *cursor = value_end;
    }
    column->default_literal = value;
    column->default_expression = eg_ddl_copy_range(start, *cursor);
    if (!column->default_expression) {
        free(column->default_literal);
        column->default_literal = NULL;
        eg_ddl_error(error, error_capacity, "out of memory");
        return 0;
    }
    return 1;
}

static int eg_ddl_set_type(EgDdlColumn* column, const char* type, unsigned int text_length, char* error, size_t error_capacity) {
    int is_text_type = !strcasecmp(type, "text") || !strcasecmp(type, "varchar") || !strcasecmp(type, "char");
    if (text_length != 0 && !is_text_type) {
        eg_ddl_error(error, error_capacity, "column length is supported only for TEXT types");
        return 0;
    }
    column->flags = 0x02;
    if (!strcasecmp(type, "bool") || !strcasecmp(type, "boolean") || !strcasecmp(type, "bit") || !strcasecmp(type, "yesno")) {
        column->type = MDB_BOOL;
        column->length = 1;
    } else if (!strcasecmp(type, "byte") || !strcasecmp(type, "tinyint")) {
        column->type = MDB_BYTE;
        column->length = 1;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "short") || !strcasecmp(type, "smallint")) {
        column->type = MDB_INT;
        column->length = 2;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "int") || !strcasecmp(type, "integer") || !strcasecmp(type, "long")) {
        column->type = MDB_LONGINT;
        column->length = 4;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "counter") || !strcasecmp(type, "autoincrement")) {
        column->type = MDB_LONGINT;
        column->length = 4;
        column->flags |= 0x05;
    } else if (!strcasecmp(type, "single") || !strcasecmp(type, "real")) {
        column->type = MDB_FLOAT;
        column->length = 4;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "double") || !strcasecmp(type, "float")) {
        column->type = MDB_DOUBLE;
        column->length = 8;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "money") || !strcasecmp(type, "currency")) {
        column->type = MDB_MONEY;
        column->length = 8;
        column->flags |= 0x01;
    } else if (!strcasecmp(type, "date") || !strcasecmp(type, "time") || !strcasecmp(type, "datetime") || !strcasecmp(type, "timestamp")) {
        column->type = MDB_DATETIME;
        column->length = 8;
        column->flags |= 0x01;
    } else if (is_text_type) {
        if (text_length == 0) {
            text_length = 255;
        }
        if (text_length > 255) {
            eg_ddl_error(error, error_capacity, "Jet 4 TEXT length must be between 1 and 255");
            return 0;
        }
        column->type = MDB_TEXT;
        column->length = (uint16_t)(text_length * 2);
        column->variable = 1;
        column->ext_flags = 0x01;
    } else if (!strcasecmp(type, "memo") || !strcasecmp(type, "longtext")) {
        column->type = MDB_MEMO;
        column->variable = 1;
        column->long_value = 1;
    } else {
        eg_ddl_error(error, error_capacity, "unsupported Jet 4 column type");
        return 0;
    }
    return 1;
}

static int eg_ddl_default_integer(const char* text, int64_t minimum, int64_t maximum) {
    char* end = NULL;
    long long value;
    errno = 0;
    value = strtoll(text, &end, 10);
    return errno == 0 && end != text && *end == '\0' && value >= minimum && value <= maximum;
}

static int eg_ddl_default_number(const char* text) {
    char* end = NULL;
    double value;
    errno = 0;
    value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0' && isfinite(value);
}

static int eg_ddl_validate_default(const EgDdlColumn* column, char* error, size_t error_capacity) {
    const char* value;
    int valid = 0;
    if (!column->default_literal || column->default_literal[0] == 'n') {
        return 1;
    }
    value = column->default_literal + 1;
    switch (column->type) {
        case MDB_BOOL:
            valid = column->default_literal[0] == 'v'
                && (!strcasecmp(value, "true") || !strcasecmp(value, "false")
                    || !strcmp(value, "0") || !strcmp(value, "1") || !strcmp(value, "-1"));
            break;
        case MDB_BYTE:
            valid = column->default_literal[0] == 'v' && eg_ddl_default_integer(value, 0, UINT8_MAX);
            break;
        case MDB_INT:
            valid = column->default_literal[0] == 'v' && eg_ddl_default_integer(value, INT16_MIN, INT16_MAX);
            break;
        case MDB_LONGINT:
            valid = column->default_literal[0] == 'v' && eg_ddl_default_integer(value, INT32_MIN, INT32_MAX);
            break;
        case MDB_FLOAT:
        case MDB_DOUBLE:
        case MDB_MONEY:
            valid = column->default_literal[0] == 'v' && eg_ddl_default_number(value);
            break;
        case MDB_DATETIME:
            valid = column->default_literal[0] == 'd';
            break;
        case MDB_TEXT:
        case MDB_MEMO:
            valid = column->default_literal[0] == 's' && g_utf8_validate(value, -1, NULL);
            break;
        default:
            break;
    }
    if (!valid) {
        eg_ddl_error(error, error_capacity, "DEFAULT value is incompatible with the Jet column type");
    }
    return valid;
}

static int eg_ddl_parse_column(const char* start, const char* end, EgDdlTable* table, char* error, size_t error_capacity) {
    EgDdlColumn* column;
    const char* cursor = start;
    const char* type_start;
    char type[32];
    size_t type_length;
    unsigned int text_length = 0;
    size_t i = 0;
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (table->column_count >= EG_JET4_MAX_COLUMNS) {
        eg_ddl_error(error, error_capacity, "Jet 4 tables support at most 255 columns");
        return 0;
    }
    column = &table->columns[table->column_count];
    column->name = eg_ddl_parse_identifier(&cursor, error, error_capacity);
    if (!column->name || cursor > end) {
        return 0;
    }
    while (i < table->column_count) {
        if (!strcasecmp(table->columns[i].name, column->name)) {
            eg_ddl_error(error, error_capacity, "duplicate CREATE TABLE column name");
            return 0;
        }
        i++;
    }
    cursor = eg_ddl_skip_space(cursor);
    type_start = cursor;
    while (cursor < end && (isalpha((unsigned char)*cursor) || *cursor == '_')) {
        cursor++;
    }
    type_length = (size_t)(cursor - type_start);
    if (type_length == 0 || type_length >= sizeof(type)) {
        eg_ddl_error(error, error_capacity, "missing or invalid CREATE TABLE column type");
        return 0;
    }
    memcpy(type, type_start, type_length);
    type[type_length] = '\0';
    cursor = eg_ddl_skip_space(cursor);
    if (cursor < end && *cursor == '(') {
        cursor++;
        if (!eg_ddl_parse_uint(&cursor, &text_length)) {
            eg_ddl_error(error, error_capacity, "invalid CREATE TABLE column length");
            return 0;
        }
        cursor = eg_ddl_skip_space(cursor);
        if (cursor >= end || *cursor != ')') {
            eg_ddl_error(error, error_capacity, "invalid CREATE TABLE column length");
            return 0;
        }
        cursor++;
    }
    if (!eg_ddl_set_type(column, type, text_length, error, error_capacity)) {
        return 0;
    }
    cursor = eg_ddl_skip_space(cursor);
    while (cursor < end) {
        const char* after;
        if (eg_ddl_keyword(cursor, "primary", &after) && eg_ddl_keyword(after, "key", &after)) {
            if (!eg_ddl_set_primary_key(table, column->name, error, error_capacity)) {
                return 0;
            }
            column->primary_key = 1;
            cursor = eg_ddl_skip_space(after);
        } else if (eg_ddl_keyword(cursor, "not", &after) && eg_ddl_keyword(after, "null", &after)) {
            column->required = 1;
            cursor = eg_ddl_skip_space(after);
        } else if (eg_ddl_keyword(cursor, "default", &after)) {
            if (column->default_literal) {
                eg_ddl_error(error, error_capacity, "duplicate DEFAULT clause");
                return 0;
            }
            cursor = after;
            if (!eg_ddl_parse_default(&cursor, end, column, error, error_capacity)) {
                return 0;
            }
            cursor = eg_ddl_skip_space(cursor);
        } else {
            eg_ddl_error(error, error_capacity, "unsupported CREATE TABLE column constraint or property");
            return 0;
        }
    }
    if (column->primary_key && (column->type != MDB_LONGINT || !(column->flags & 0x04))) {
        eg_ddl_error(error, error_capacity, "native Jet 4 primary keys currently require an AutoIncrement column");
        return 0;
    }
    if ((column->flags & 0x04) && column->default_literal) {
        eg_ddl_error(error, error_capacity, "AutoIncrement columns cannot have a DEFAULT value");
        return 0;
    }
    if (column->required && column->default_literal && column->default_literal[0] == 'n') {
        eg_ddl_error(error, error_capacity, "NOT NULL columns cannot default to NULL");
        return 0;
    }
    if (!eg_ddl_validate_default(column, error, error_capacity)) {
        return 0;
    }
    if (column->long_value) {
        table->long_value_count++;
    }
    table->column_count++;
    return 1;
}

static int eg_ddl_parse_table_constraint(const char* start, const char* end, EgDdlTable* table, char* error, size_t error_capacity) {
    const char* cursor = start;
    const char* after;
    char* column_name;
    if (eg_ddl_keyword(cursor, "constraint", &after)) {
        char* constraint_name;
        cursor = after;
        constraint_name = eg_ddl_parse_identifier(&cursor, error, error_capacity);
        if (!constraint_name || cursor > end) {
            free(constraint_name);
            return 0;
        }
        free(constraint_name);
    }
    if (!eg_ddl_keyword(cursor, "primary", &after) || !eg_ddl_keyword(after, "key", &after)) {
        eg_ddl_error(error, error_capacity, "only a single-column PRIMARY KEY table constraint is supported");
        return 0;
    }
    cursor = eg_ddl_skip_space(after);
    if (cursor >= end || *cursor++ != '(') {
        eg_ddl_error(error, error_capacity, "PRIMARY KEY requires one column");
        return 0;
    }
    column_name = eg_ddl_parse_identifier(&cursor, error, error_capacity);
    if (!column_name) {
        return 0;
    }
    cursor = eg_ddl_skip_space(cursor);
    if (cursor >= end || *cursor++ != ')' || eg_ddl_skip_space(cursor) != end) {
        free(column_name);
        eg_ddl_error(error, error_capacity, "only a single-column PRIMARY KEY is supported");
        return 0;
    }
    if (!eg_ddl_set_primary_key(table, column_name, error, error_capacity)) {
        free(column_name);
        return 0;
    }
    free(column_name);
    return 1;
}

static int eg_ddl_parse_item(const char* start, const char* end, EgDdlTable* table, char* error, size_t error_capacity) {
    const char* cursor = eg_ddl_skip_space(start);
    if (eg_ddl_keyword(cursor, "constraint", NULL) || eg_ddl_keyword(cursor, "primary", NULL)) {
        return eg_ddl_parse_table_constraint(cursor, end, table, error, error_capacity);
    }
    if (eg_ddl_keyword(cursor, "unique", NULL) || eg_ddl_keyword(cursor, "foreign", NULL)) {
        eg_ddl_error(error, error_capacity, "UNIQUE and FOREIGN KEY constraints are not supported");
        return 0;
    }
    return eg_ddl_parse_column(start, end, table, error, error_capacity);
}

/*
 * Split the top-level column list while honoring quotes, brackets, and nested
 * type/default parentheses. A second pass assigns fixed and variable slots,
 * which must match the order encoded into the table-definition page.
 */
static int eg_ddl_parse(const char* sql, EgDdlTable* table, char* error, size_t error_capacity) {
    const char* cursor;
    const char* item_start;
    const char* close;
    int depth = 0;
    char quote = '\0';
    if (!sql || !eg_ddl_keyword(sql, "create", &cursor) || !eg_ddl_keyword(cursor, "table", &cursor)) {
        eg_ddl_error(error, error_capacity, "expected CREATE TABLE");
        return 0;
    }
    table->name = eg_ddl_parse_identifier(&cursor, error, error_capacity);
    if (!table->name) {
        return 0;
    }
    cursor = eg_ddl_skip_space(cursor);
    if (*cursor != '(') {
        eg_ddl_error(error, error_capacity, "CREATE TABLE requires a column list");
        return 0;
    }
    item_start = ++cursor;
    close = NULL;
    while (*cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if (quote) {
            if (ch == (unsigned char)quote) {
                quote = '\0';
            }
        } else if (ch == '\'' || ch == '"' || ch == '`') {
            quote = (char)ch;
        } else if (ch == '[') {
            quote = ']';
        } else if (ch == '(') {
            depth++;
        } else if (ch == ')' && depth > 0) {
            depth--;
        } else if ((ch == ',' || ch == ')') && depth == 0) {
            if (!eg_ddl_parse_item(item_start, cursor, table, error, error_capacity)) {
                return 0;
            }
            if (ch == ')') {
                close = cursor;
                break;
            }
            item_start = cursor + 1;
        }
        cursor++;
    }
    if (!close || quote || table->column_count == 0) {
        eg_ddl_error(error, error_capacity, "invalid CREATE TABLE column list");
        return 0;
    }
    cursor = eg_ddl_skip_space(close + 1);
    if (*cursor == ';') {
        cursor = eg_ddl_skip_space(cursor + 1);
    }
    if (*cursor) {
        eg_ddl_error(error, error_capacity, "unexpected text after CREATE TABLE");
        return 0;
    }
    table->primary_key_column = -1;
    if (table->primary_key_name) {
        size_t primary_index = 0;
        while (primary_index < table->column_count
               && strcasecmp(table->columns[primary_index].name, table->primary_key_name)) {
            primary_index++;
        }
        if (primary_index == table->column_count) {
            eg_ddl_error(error, error_capacity, "PRIMARY KEY column was not found");
            return 0;
        }
        if (table->columns[primary_index].type != MDB_LONGINT || !(table->columns[primary_index].flags & 0x04)) {
            eg_ddl_error(error, error_capacity, "native Jet 4 primary keys currently require an AutoIncrement column");
            return 0;
        }
        table->columns[primary_index].primary_key = 1;
        table->primary_key_column = (int)primary_index;
    }
    {
        uint16_t fixed_offset = 0;
        uint16_t variable_index = 0;
        uint16_t long_variable_index;
        size_t i = 0;
        while (i < table->column_count) {
            EgDdlColumn* column = &table->columns[i];
            if (column->variable && !column->long_value) {
                column->variable_index = variable_index++;
            } else if (!column->long_value) {
                column->variable_index = variable_index;
            }
            i++;
        }
        long_variable_index = variable_index;
        i = 0;
        while (i < table->column_count) {
            EgDdlColumn* column = &table->columns[i];
            if (column->long_value) {
                column->variable_index = long_variable_index++;
            }
            if (!column->variable && column->type != MDB_BOOL) {
                column->fixed_offset = fixed_offset;
                fixed_offset = (uint16_t)(fixed_offset + column->length);
            }
            i++;
        }
        table->variable_count = long_variable_index;
    }
    return 1;
}

static void eg_ddl_put_u16(unsigned char* buffer, size_t offset, uint16_t value) {
    buffer[offset] = (unsigned char)(value & 0xff);
    buffer[offset + 1] = (unsigned char)((value >> 8) & 0xff);
}

static void eg_ddl_put_u24(unsigned char* buffer, size_t offset, uint32_t value) {
    buffer[offset] = (unsigned char)(value & 0xff);
    buffer[offset + 1] = (unsigned char)((value >> 8) & 0xff);
    buffer[offset + 2] = (unsigned char)((value >> 16) & 0xff);
}

static void eg_ddl_put_u32(unsigned char* buffer, size_t offset, uint32_t value) {
    eg_ddl_put_u16(buffer, offset, (uint16_t)(value & 0xffff));
    eg_ddl_put_u16(buffer, offset + 2, (uint16_t)(value >> 16));
}

static int eg_ddl_utf16le(const char* text, unsigned char** output, uint16_t* byte_length) {
    glong units = 0;
    gunichar2* utf16 = g_utf8_to_utf16(text, -1, NULL, &units, NULL);
    unsigned char* bytes;
    glong i = 0;
    if (!utf16 || units < 0 || units > UINT16_MAX / 2) {
        g_free(utf16);
        return 0;
    }
    bytes = (unsigned char*)malloc((size_t)units * 2);
    if (!bytes && units) {
        g_free(utf16);
        return 0;
    }
    while (i < units) {
        bytes[i * 2] = (unsigned char)(utf16[i] & 0xff);
        bytes[i * 2 + 1] = (unsigned char)((utf16[i] >> 8) & 0xff);
        i++;
    }
    g_free(utf16);
    *output = bytes;
    *byte_length = (uint16_t)(units * 2);
    return 1;
}

static int eg_ddl_buffer_reserve(EgDdlBuffer* buffer, size_t extra) {
    size_t capacity;
    unsigned char* data;
    if (extra > SIZE_MAX - buffer->length) {
        return 0;
    }
    if (buffer->length + extra <= buffer->capacity) {
        return 1;
    }
    capacity = buffer->capacity ? buffer->capacity : 128;
    while (capacity < buffer->length + extra) {
        if (capacity > SIZE_MAX / 2) {
            return 0;
        }
        capacity *= 2;
    }
    data = (unsigned char*)realloc(buffer->data, capacity);
    if (!data) {
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int eg_ddl_buffer_append(EgDdlBuffer* buffer, const void* data, size_t length) {
    if (!eg_ddl_buffer_reserve(buffer, length)) {
        return 0;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return 1;
}

static int eg_ddl_buffer_u16(EgDdlBuffer* buffer, uint16_t value) {
    unsigned char bytes[2];
    eg_ddl_put_u16(bytes, 0, value);
    return eg_ddl_buffer_append(buffer, bytes, sizeof(bytes));
}

static int eg_ddl_buffer_u32(EgDdlBuffer* buffer, uint32_t value) {
    unsigned char bytes[4];
    eg_ddl_put_u32(bytes, 0, value);
    return eg_ddl_buffer_append(buffer, bytes, sizeof(bytes));
}

static int eg_ddl_buffer_name(EgDdlBuffer* buffer, const char* name) {
    unsigned char* encoded = NULL;
    uint16_t encoded_length = 0;
    int result;
    if (!eg_ddl_utf16le(name, &encoded, &encoded_length)) {
        return 0;
    }
    result = eg_ddl_buffer_u16(buffer, encoded_length)
        && eg_ddl_buffer_append(buffer, encoded, encoded_length);
    free(encoded);
    return result;
}

static int eg_ddl_buffer_patch_u32(EgDdlBuffer* buffer, size_t offset, uint32_t value) {
    if (offset + 4 > buffer->length) {
        return 0;
    }
    eg_ddl_put_u32(buffer->data, offset, value);
    return 1;
}

static int eg_ddl_property_value(EgDdlBuffer* buffer, uint8_t type, uint16_t name_index, const unsigned char* data, uint16_t data_length) {
    size_t start = buffer->length;
    unsigned char flags = 1;
    return eg_ddl_buffer_u16(buffer, 0)
        && eg_ddl_buffer_append(buffer, &flags, 1)
        && eg_ddl_buffer_append(buffer, &type, 1)
        && eg_ddl_buffer_u16(buffer, name_index)
        && eg_ddl_buffer_u16(buffer, data_length)
        && eg_ddl_buffer_append(buffer, data, data_length)
        && (eg_ddl_put_u16(buffer->data, start, (uint16_t)(buffer->length - start)), 1);
}

static int eg_ddl_build_properties(const EgDdlTable* table, unsigned char** output, size_t* output_length, char* error, size_t error_capacity) {
    static const unsigned char header[] = {'M', 'R', '2', '\0'};
    EgDdlBuffer buffer;
    int has_default = 0;
    int has_required = 0;
    size_t names_start;
    size_t i = 0;
    memset(&buffer, 0, sizeof(buffer));
    *output = NULL;
    *output_length = 0;
    while (i < table->column_count) {
        has_default |= table->columns[i].default_expression != NULL;
        has_required |= table->columns[i].required;
        i++;
    }
    if (!has_default && !has_required) {
        return 1;
    }
    if (!eg_ddl_buffer_append(&buffer, header, sizeof(header))) {
        goto oom;
    }
    names_start = buffer.length;
    if (!eg_ddl_buffer_u32(&buffer, 0) || !eg_ddl_buffer_u16(&buffer, 0x80)
        || (has_default && !eg_ddl_buffer_name(&buffer, "DefaultValue"))
        || (has_required && !eg_ddl_buffer_name(&buffer, "Required"))
        || !eg_ddl_buffer_patch_u32(&buffer, names_start, (uint32_t)(buffer.length - names_start))) {
        goto oom;
    }
    i = 0;
    while (i < table->column_count) {
        const EgDdlColumn* column = &table->columns[i];
        if (column->default_expression || column->required) {
            size_t block_start = buffer.length;
            size_t map_name_start;
            if (!eg_ddl_buffer_u32(&buffer, 0) || !eg_ddl_buffer_u16(&buffer, 1)) {
                goto oom;
            }
            map_name_start = buffer.length;
            if (!eg_ddl_buffer_u32(&buffer, 0) || !eg_ddl_buffer_name(&buffer, column->name)
                || !eg_ddl_buffer_patch_u32(&buffer, map_name_start, (uint32_t)(buffer.length - map_name_start))) {
                goto oom;
            }
            if (column->default_expression) {
                unsigned char* encoded = NULL;
                uint16_t encoded_length = 0;
                if (!eg_ddl_utf16le(column->default_expression, &encoded, &encoded_length)
                    || !eg_ddl_property_value(&buffer, MDB_MEMO, 0, encoded, encoded_length)) {
                    free(encoded);
                    goto oom;
                }
                free(encoded);
            }
            if (column->required) {
                unsigned char value = 0xff;
                uint16_t name_index = has_default ? 1 : 0;
                if (!eg_ddl_property_value(&buffer, MDB_BOOL, name_index, &value, 1)) {
                    goto oom;
                }
            }
            if (!eg_ddl_buffer_patch_u32(&buffer, block_start, (uint32_t)(buffer.length - block_start))) {
                goto oom;
            }
        }
        i++;
    }
    *output = (unsigned char*)calloc(buffer.length + 12, 1);
    if (!*output) {
        goto oom;
    }
    eg_ddl_put_u32(*output, 0, UINT32_C(0x80000000) | (uint32_t)buffer.length);
    memcpy(*output + 12, buffer.data, buffer.length);
    *output_length = buffer.length + 12;
    free(buffer.data);
    return 1;

oom:
    free(buffer.data);
    eg_ddl_error(error, error_capacity, "out of memory while building Jet column properties");
    return 0;
}

static int eg_ddl_write_page(MdbHandle* mdb, uint32_t page_number, const unsigned char* page) {
    memcpy(mdb->pg_buf, page, EG_JET4_PAGE_SIZE);
    mdb->cur_pg = page_number;
    return mdb_write_pg(mdb, page_number) == EG_JET4_PAGE_SIZE;
}

static int eg_ddl_mark_global_page_used(MdbHandle* mdb, uint32_t page_number, char* error, size_t error_capacity) {
    uint16_t row_start;
    uint8_t map_type;
    if (mdb_read_pg(mdb, 1) != EG_JET4_PAGE_SIZE) {
        eg_ddl_error(error, error_capacity, "unable to read Jet global usage map");
        return 0;
    }
    row_start = (uint16_t)mdb_get_int16(mdb->pg_buf, 14);
    map_type = mdb->pg_buf[row_start];
    if (map_type == 0) {
        uint32_t base = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1);
        uint32_t relative;
        if (page_number < base || page_number - base >= 512) {
            eg_ddl_error(error, error_capacity, "Jet global inline usage map capacity exceeded");
            return 0;
        }
        relative = page_number - base;
        mdb->pg_buf[row_start + 5 + relative / 8] &= (unsigned char)~(1U << (relative % 8));
        if (mdb_write_pg(mdb, 1) != EG_JET4_PAGE_SIZE) {
            eg_ddl_error(error, error_capacity, "unable to update Jet global usage map");
            return 0;
        }
        return 1;
    }
    if (map_type == 1) {
        uint32_t pages_per_map = (EG_JET4_PAGE_SIZE - 4) * 8;
        uint32_t map_index = page_number / pages_per_map;
        uint32_t relative = page_number % pages_per_map;
        uint32_t map_page;
        if (row_start + 1 + (map_index + 1) * 4 > EG_JET4_PAGE_SIZE) {
            eg_ddl_error(error, error_capacity, "Jet global reference usage map capacity exceeded");
            return 0;
        }
        map_page = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1 + map_index * 4);
        if (!map_page || mdb_read_pg(mdb, map_page) != EG_JET4_PAGE_SIZE || mdb->pg_buf[0] != 5) {
            eg_ddl_error(error, error_capacity, "missing Jet global reference usage map page");
            return 0;
        }
        mdb->pg_buf[4 + relative / 8] &= (unsigned char)~(1U << (relative % 8));
        if (mdb_write_pg(mdb, map_page) != EG_JET4_PAGE_SIZE) {
            eg_ddl_error(error, error_capacity, "unable to update Jet global reference usage map");
            return 0;
        }
        return 1;
    }
    eg_ddl_error(error, error_capacity, "unsupported Jet global usage map type");
    return 0;
}

static int eg_ddl_extend_file(MdbHandle* mdb, uint32_t first_page, uint32_t page_count, char* error, size_t error_capacity) {
    unsigned char zeros[EG_JET4_PAGE_SIZE];
    uint32_t i = 0;
    memset(zeros, 0, sizeof(zeros));
    if (fseeko(mdb->f->stream, 0, SEEK_END) != 0) {
        eg_ddl_error(error, error_capacity, "unable to seek to end of MDB file");
        return 0;
    }
    while (i < page_count) {
        if (fwrite(zeros, 1, sizeof(zeros), mdb->f->stream) != sizeof(zeros)) {
            eg_ddl_error(error, error_capacity, "unable to extend MDB file");
            return 0;
        }
        i++;
    }
    if (fflush(mdb->f->stream) != 0) {
        eg_ddl_error(error, error_capacity, "unable to flush extended MDB file");
        return 0;
    }
    i = 0;
    while (i < page_count) {
        if (!eg_ddl_mark_global_page_used(mdb, first_page + i, error, error_capacity)) {
            return 0;
        }
        i++;
    }
    return 1;
}

/*
 * Encode one Jet 4 table-definition page. All offsets are little-endian and
 * derived before writing so an oversized definition fails without touching
 * disk. Primary-key metadata is emitted only for the supported AutoNumber form.
 */
static int eg_ddl_write_tdef(MdbHandle* mdb, const EgDdlTable* table, uint32_t tdef_page, uint32_t usage_page, uint32_t root_page, char* error, size_t error_capacity) {
    unsigned char page[EG_JET4_PAGE_SIZE];
    size_t total_size = EG_JET4_TDEF_HEADER_SIZE + table->column_count * EG_JET4_COLUMN_SIZE + 2;
    size_t position = EG_JET4_TDEF_HEADER_SIZE
        + (table->primary_key_column >= 0 ? EG_JET4_INDEX_ROW_COUNT_SIZE : 0);
    size_t i = 0;
    memset(page, 0, sizeof(page));
    while (i < table->column_count) {
        unsigned char* name = NULL;
        uint16_t name_length = 0;
        if (!eg_ddl_utf16le(table->columns[i].name, &name, &name_length)) {
            eg_ddl_error(error, error_capacity, "unable to encode Jet column name");
            return 0;
        }
        total_size += 2 + name_length;
        free(name);
        i++;
    }
    total_size += table->long_value_count * 10;
    if (table->primary_key_column >= 0) {
        unsigned char* index_name = NULL;
        uint16_t index_name_length = 0;
        if (!eg_ddl_utf16le("PrimaryKey", &index_name, &index_name_length)) {
            eg_ddl_error(error, error_capacity, "unable to encode Jet primary key name");
            return 0;
        }
        total_size += EG_JET4_INDEX_ROW_COUNT_SIZE + EG_JET4_INDEX_COLUMN_BLOCK_SIZE
            + EG_JET4_INDEX_INFO_BLOCK_SIZE + 2 + index_name_length;
        free(index_name);
    }
    if (total_size > EG_JET4_PAGE_SIZE) {
        eg_ddl_error(error, error_capacity, "Jet table definition exceeds one page");
        return 0;
    }
    page[0] = 2;
    page[1] = 1;
    eg_ddl_put_u16(page, 2, (uint16_t)(EG_JET4_PAGE_SIZE - total_size - 8));
    eg_ddl_put_u32(page, 8, (uint32_t)total_size);
    eg_ddl_put_u32(page, 12, EG_JET4_TABLE_MAGIC);
    page[24] = 1;
    page[40] = 0x4e;
    eg_ddl_put_u16(page, 41, (uint16_t)table->column_count);
    eg_ddl_put_u16(page, 43, (uint16_t)table->variable_count);
    eg_ddl_put_u16(page, 45, (uint16_t)table->column_count);
    eg_ddl_put_u32(page, 47, table->primary_key_column >= 0 ? 1 : 0);
    eg_ddl_put_u32(page, 51, table->primary_key_column >= 0 ? 1 : 0);
    page[55] = 0;
    eg_ddl_put_u24(page, 56, usage_page);
    page[59] = 1;
    eg_ddl_put_u24(page, 60, usage_page);
    i = 0;
    while (i < table->column_count) {
        const EgDdlColumn* column = &table->columns[i];
        page[position] = column->type;
        eg_ddl_put_u32(page, position + 1, EG_JET4_TABLE_MAGIC);
        eg_ddl_put_u16(page, position + 5, (uint16_t)i);
        eg_ddl_put_u16(page, position + 7, column->variable_index);
        eg_ddl_put_u16(page, position + 9, (uint16_t)i);
        if (column->type == MDB_TEXT || column->type == MDB_MEMO) {
            eg_ddl_put_u16(page, position + 11, 0x0409);
        }
        page[position + 15] = column->flags;
        page[position + 16] = column->ext_flags;
        eg_ddl_put_u16(page, position + 21, column->fixed_offset);
        eg_ddl_put_u16(page, position + 23, column->long_value ? 0 : column->length);
        position += EG_JET4_COLUMN_SIZE;
        i++;
    }
    i = 0;
    while (i < table->column_count) {
        unsigned char* name = NULL;
        uint16_t name_length = 0;
        if (!eg_ddl_utf16le(table->columns[i].name, &name, &name_length)) {
            eg_ddl_error(error, error_capacity, "unable to encode Jet column name");
            return 0;
        }
        eg_ddl_put_u16(page, position, name_length);
        memcpy(page + position + 2, name, name_length);
        position += 2 + name_length;
        free(name);
        i++;
    }
    if (table->primary_key_column >= 0) {
        unsigned char* name = NULL;
        uint16_t name_length = 0;
        size_t key = 0;
        eg_ddl_put_u32(page, position, EG_JET4_INDEX_MAGIC);
        position += 4;
        while (key < 10) {
            eg_ddl_put_u16(page, position, key == 0 ? (uint16_t)table->primary_key_column : UINT16_MAX);
            page[position + 2] = key == 0 ? 1 : 0;
            position += 3;
            key++;
        }
        page[position++] = 2;
        eg_ddl_put_u24(page, position, usage_page);
        position += 3;
        eg_ddl_put_u32(page, position, root_page);
        position += 8;
        page[position++] = 0x89;
        position += 5;

        eg_ddl_put_u32(page, position, EG_JET4_TABLE_MAGIC);
        position += 4;
        eg_ddl_put_u32(page, position, 0);
        position += 4;
        eg_ddl_put_u32(page, position, 0);
        position += 4;
        page[position++] = 0;
        eg_ddl_put_u32(page, position, UINT32_MAX);
        position += 4;
        eg_ddl_put_u32(page, position, 0);
        position += 4;
        page[position++] = 0;
        page[position++] = 0;
        page[position++] = 1;
        position += 4;

        if (!eg_ddl_utf16le("PrimaryKey", &name, &name_length)) {
            eg_ddl_error(error, error_capacity, "unable to encode Jet primary key name");
            return 0;
        }
        eg_ddl_put_u16(page, position, name_length);
        memcpy(page + position + 2, name, name_length);
        position += 2 + name_length;
        free(name);
    }
    {
        uint8_t usage_row = table->primary_key_column >= 0 ? 3 : 2;
        i = 0;
        while (i < table->column_count) {
            if (table->columns[i].long_value) {
                eg_ddl_put_u16(page, position, (uint16_t)i);
                page[position + 2] = usage_row++;
                eg_ddl_put_u24(page, position + 3, usage_page);
                page[position + 6] = usage_row++;
                eg_ddl_put_u24(page, position + 7, usage_page);
                position += 10;
            }
            i++;
        }
    }
    page[position++] = 0xff;
    page[position++] = 0xff;
    if (position != total_size || !eg_ddl_write_page(mdb, tdef_page, page)) {
        eg_ddl_error(error, error_capacity, "unable to write Jet table definition");
        return 0;
    }
    return 1;
}

static int eg_ddl_write_usage_page(MdbHandle* mdb, const EgDdlTable* table, uint32_t usage_page, uint32_t root_page, uint32_t data_page, char* error, size_t error_capacity) {
    unsigned char page[EG_JET4_PAGE_SIZE];
    size_t row_count = 2 + (table->primary_key_column >= 0 ? 1 : 0) + table->long_value_count * 2;
    size_t row = 0;
    memset(page, 0, sizeof(page));
    page[0] = 1;
    page[1] = 1;
    eg_ddl_put_u16(page, 2, (uint16_t)(EG_JET4_PAGE_SIZE - 14 - row_count * (EG_JET4_USAGE_ROW_SIZE + 2)));
    eg_ddl_put_u16(page, 12, (uint16_t)row_count);
    while (row < row_count) {
        size_t start = EG_JET4_PAGE_SIZE - (row + 1) * EG_JET4_USAGE_ROW_SIZE;
        eg_ddl_put_u16(page, 14 + row * 2, (uint16_t)start);
        if (row < 2) {
            eg_ddl_put_u32(page, start + 1, data_page);
            page[start + 5] = 1;
        } else if (row == 2 && table->primary_key_column >= 0) {
            eg_ddl_put_u32(page, start + 1, root_page);
            page[start + 5] = 1;
        }
        row++;
    }
    if (!eg_ddl_write_page(mdb, usage_page, page)) {
        eg_ddl_error(error, error_capacity, "unable to write Jet table usage map");
        return 0;
    }
    return 1;
}

static int eg_ddl_write_index_root_page(MdbHandle* mdb, uint32_t root_page, uint32_t tdef_page, char* error, size_t error_capacity) {
    unsigned char page[EG_JET4_PAGE_SIZE];
    memset(page, 0, sizeof(page));
    page[0] = 4;
    page[1] = 1;
    eg_ddl_put_u16(page, 2, (uint16_t)(EG_JET4_PAGE_SIZE - 27 - EG_JET4_INDEX_ENTRY_MASK_SIZE));
    eg_ddl_put_u32(page, 4, tdef_page);
    if (!eg_ddl_write_page(mdb, root_page, page)) {
        eg_ddl_error(error, error_capacity, "unable to write Jet primary key root page");
        return 0;
    }
    return 1;
}

static int eg_ddl_write_data_page(MdbHandle* mdb, uint32_t data_page, uint32_t tdef_page, char* error, size_t error_capacity) {
    unsigned char page[EG_JET4_PAGE_SIZE];
    memset(page, 0, sizeof(page));
    page[0] = 1;
    page[1] = 1;
    eg_ddl_put_u16(page, 2, EG_JET4_PAGE_SIZE - 16);
    eg_ddl_put_u32(page, 4, tdef_page);
    if (!eg_ddl_write_page(mdb, data_page, page)) {
        eg_ddl_error(error, error_capacity, "unable to write initial Jet data page");
        return 0;
    }
    return 1;
}

static int eg_ddl_set_usage_map_bit(MdbHandle* mdb, uint32_t packed_page_row, uint32_t page_number, unsigned char* cached_map, size_t cached_size, char* error, size_t error_capacity) {
    uint32_t map_page_number = packed_page_row >> 8;
    uint32_t row_number = packed_page_row & 0xff;
    uint16_t row_count;
    uint16_t row_start;
    uint8_t map_type;
    if (!map_page_number || mdb_read_pg(mdb, map_page_number) != EG_JET4_PAGE_SIZE) {
        eg_ddl_error(error, error_capacity, "unable to read Jet table usage map");
        return 0;
    }
    row_count = (uint16_t)mdb_get_int16(mdb->pg_buf, 12);
    if (row_number >= row_count) {
        eg_ddl_error(error, error_capacity, "invalid Jet table usage map row");
        return 0;
    }
    row_start = (uint16_t)(mdb_get_int16(mdb->pg_buf, 14 + row_number * 2) & 0x1fff);
    map_type = mdb->pg_buf[row_start];
    if (map_type == 0) {
        uint32_t base = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1);
        uint32_t relative;
        if (page_number < base || page_number - base >= 512) {
            eg_ddl_error(error, error_capacity, "Jet table inline usage map capacity exceeded");
            return 0;
        }
        relative = page_number - base;
        mdb->pg_buf[row_start + 5 + relative / 8] |= (unsigned char)(1U << (relative % 8));
        if (cached_map && cached_size > 5 + relative / 8) {
            cached_map[5 + relative / 8] |= (unsigned char)(1U << (relative % 8));
        }
        if (mdb_write_pg(mdb, map_page_number) != EG_JET4_PAGE_SIZE) {
            eg_ddl_error(error, error_capacity, "unable to update Jet table inline usage map");
            return 0;
        }
        return 1;
    }
    if (map_type == 1) {
        uint32_t pages_per_map = (EG_JET4_PAGE_SIZE - 4) * 8;
        uint32_t map_index = page_number / pages_per_map;
        uint32_t relative = page_number % pages_per_map;
        uint32_t reference_page;
        if (row_start + 1 + (map_index + 1) * 4 > EG_JET4_PAGE_SIZE) {
            eg_ddl_error(error, error_capacity, "Jet table reference usage map capacity exceeded");
            return 0;
        }
        reference_page = (uint32_t)mdb_get_int32(mdb->pg_buf, row_start + 1 + map_index * 4);
        if (!reference_page || mdb_read_pg(mdb, reference_page) != EG_JET4_PAGE_SIZE || mdb->pg_buf[0] != 5) {
            eg_ddl_error(error, error_capacity, "missing Jet table reference usage map page");
            return 0;
        }
        mdb->pg_buf[4 + relative / 8] |= (unsigned char)(1U << (relative % 8));
        if (mdb_write_pg(mdb, reference_page) != EG_JET4_PAGE_SIZE) {
            eg_ddl_error(error, error_capacity, "unable to update Jet table reference usage map");
            return 0;
        }
        return 1;
    }
    eg_ddl_error(error, error_capacity, "unsupported Jet table usage map type");
    return 0;
}

/*
 * Append and register a data page for an existing table. The caller must hold
 * the MDB write lock and an active journal transaction.
 */
int eg_mdb_append_table_data_page(MdbTableDef* table, char* error, size_t error_capacity) {
    MdbHandle* mdb;
    struct stat status;
    uint32_t page_number;
    uint32_t owned_map;
    uint32_t free_map;
    if (!table || !table->entry || !(mdb = table->entry->mdb) || !mdb->f || !mdb->f->writable) {
        eg_ddl_error(error, error_capacity, "invalid writable MDB table");
        return 0;
    }
    if (mdb->f->jet_version != MDB_VER_JET4 || fstat(fileno(mdb->f->stream), &status) != 0
        || status.st_size <= 0 || status.st_size % EG_JET4_PAGE_SIZE != 0) {
        eg_ddl_error(error, error_capacity, "native page allocation supports Jet 4 MDB files only");
        return 0;
    }
    page_number = (uint32_t)(status.st_size / EG_JET4_PAGE_SIZE);
    if (mdb_read_pg(mdb, table->entry->table_pg) != EG_JET4_PAGE_SIZE) {
        eg_ddl_error(error, error_capacity, "unable to read Jet table definition");
        return 0;
    }
    owned_map = (uint32_t)mdb_get_int32(mdb->pg_buf, 55);
    free_map = (uint32_t)mdb_get_int32(mdb->pg_buf, 59);
    if (!eg_ddl_extend_file(mdb, page_number, 1, error, error_capacity)
        || !eg_ddl_write_data_page(mdb, page_number, (uint32_t)table->entry->table_pg, error, error_capacity)
        || !eg_ddl_set_usage_map_bit(mdb, owned_map, page_number, table->usage_map, table->map_sz, error, error_capacity)
        || !eg_ddl_set_usage_map_bit(mdb, free_map, page_number, table->free_usage_map, table->freemap_sz, error, error_capacity)) {
        return 0;
    }
    return 1;
}

int eg_mdb_append_index_page(MdbTableDef* table, MdbIndex* index, unsigned int* page_number, char* error, size_t error_capacity) {
    MdbHandle* mdb;
    struct stat status;
    uint32_t allocated_page;
    uint32_t index_map;
    size_t index_position;
    if (!table || !index || !page_number || !table->entry || !(mdb = table->entry->mdb)
        || !mdb->f || !mdb->f->writable || index->index_num < 0) {
        eg_ddl_error(error, error_capacity, "invalid writable MDB index");
        return 0;
    }
    if (mdb->f->jet_version != MDB_VER_JET4 || fstat(fileno(mdb->f->stream), &status) != 0
        || status.st_size <= 0 || status.st_size % EG_JET4_PAGE_SIZE != 0) {
        eg_ddl_error(error, error_capacity, "native index allocation supports Jet 4 MDB files only");
        return 0;
    }
    index_position = (size_t)table->index_start + (size_t)index->index_num * EG_JET4_INDEX_COLUMN_BLOCK_SIZE;
    if (index_position + EG_JET4_INDEX_COLUMN_BLOCK_SIZE > EG_JET4_PAGE_SIZE
        || mdb_read_pg(mdb, table->entry->table_pg) != EG_JET4_PAGE_SIZE) {
        eg_ddl_error(error, error_capacity, "unable to read Jet index definition");
        return 0;
    }
    index_map = (uint32_t)mdb_get_int32(mdb->pg_buf, (unsigned int)index_position + 34);
    allocated_page = (uint32_t)(status.st_size / EG_JET4_PAGE_SIZE);
    if (!eg_ddl_extend_file(mdb, allocated_page, 1, error, error_capacity)
        || !eg_ddl_set_usage_map_bit(mdb, index_map, allocated_page, NULL, 0, error, error_capacity)) {
        return 0;
    }
    *page_number = allocated_page;
    return 1;
}

static int eg_ddl_catalog_insert(MdbHandle* mdb, const EgDdlTable* ddl, uint32_t tdef_page, char* error, size_t error_capacity) {
    MdbTableDef* table = mdb_read_table_by_name(mdb, (char*)"MSysObjects", MDB_TABLE);
    MdbField fields[EG_JET4_MAX_COLUMNS];
    void* values[EG_JET4_MAX_COLUMNS];
    unsigned int i = 0;
    int result = 0;
    unsigned char* properties = NULL;
    size_t properties_length = 0;
    memset(fields, 0, sizeof(fields));
    memset(values, 0, sizeof(values));
    if (!table || !mdb_read_columns(table) || table->num_cols > EG_JET4_MAX_COLUMNS) {
        eg_ddl_error(error, error_capacity, "unable to read MSysObjects metadata");
        goto cleanup;
    }
    if (!mdb_read_indices(table)) {
        table->num_idxs = 0;
        table->num_real_idxs = 0;
        table->indices = g_ptr_array_new();
    }
    if (!eg_ddl_build_properties(ddl, &properties, &properties_length, error, error_capacity)) {
        goto cleanup;
    }
    while (i < table->num_cols) {
        MdbColumn* column = g_ptr_array_index(table->columns, i);
        fields[i].colnum = (int)i;
        fields[i].is_fixed = column->is_fixed;
        fields[i].is_null = 1;
        if (!strcasecmp(column->name, "Id") || !strcasecmp(column->name, "ParentId") || !strcasecmp(column->name, "Flags")) {
            uint32_t value = !strcasecmp(column->name, "Id") ? tdef_page : (!strcasecmp(column->name, "ParentId") ? EG_JET4_TABLE_PARENT_ID : 0);
            values[i] = calloc(1, 4);
            if (values[i]) {
                eg_ddl_put_u32(values[i], 0, value);
                fields[i].value = values[i];
                fields[i].siz = 4;
                fields[i].is_null = 0;
            }
        } else if (!strcasecmp(column->name, "Type")) {
            values[i] = calloc(1, 2);
            if (values[i]) {
                eg_ddl_put_u16(values[i], 0, 1);
                fields[i].value = values[i];
                fields[i].siz = 2;
                fields[i].is_null = 0;
            }
        } else if (!strcasecmp(column->name, "Name")) {
            unsigned char* encoded = NULL;
            uint16_t encoded_length = 0;
            if (eg_ddl_utf16le(ddl->name, &encoded, &encoded_length)) {
                values[i] = encoded;
                fields[i].value = encoded;
                fields[i].siz = encoded_length;
                fields[i].is_null = 0;
            }
        } else if (!strcasecmp(column->name, "DateCreate") || !strcasecmp(column->name, "DateUpdate")) {
            double date_value;
            struct tm local;
            time_t now = time(NULL);
            localtime_r(&now, &local);
            mdb_tm_to_date(&local, &date_value);
            values[i] = malloc(sizeof(date_value));
            if (values[i]) {
                memcpy(values[i], &date_value, sizeof(date_value));
                fields[i].value = values[i];
                fields[i].siz = sizeof(date_value);
                fields[i].is_null = 0;
            }
        } else if (!strcasecmp(column->name, "LvProp") && properties) {
            values[i] = properties;
            properties = NULL;
            fields[i].value = values[i];
            fields[i].siz = (int)properties_length;
            fields[i].is_null = 0;
        }
        if (!fields[i].is_null && !values[i]) {
            eg_ddl_error(error, error_capacity, "out of memory while building MSysObjects row");
            goto cleanup;
        }
        i++;
    }
    result = mdb_insert_row(table, (int)table->num_cols, fields);
    if (result != 1) {
        eg_ddl_error(error, error_capacity, "unable to insert Jet system catalog row");
        result = 0;
    }

cleanup:
    free(properties);
    i = 0;
    while (i < EG_JET4_MAX_COLUMNS) {
        free(values[i]);
        i++;
    }
    if (table) {
        mdb_free_tabledef(table);
    }
    return result;
}

/*
 * Top-level DDL transaction. Parse first, then lock, verify Jet 4 geometry,
 * journal, append all pages/catalog rows, flush, and commit. Every failure after
 * begin rolls back to the byte-for-byte before-image.
 */
int eg_mdb_create_table_file(const char* db_path, const char* sql, char* error, size_t error_capacity) {
    EgDdlTable ddl;
    MdbHandle* mdb = NULL;
    MdbTableDef* existing = NULL;
    struct stat status;
    uint32_t tdef_page;
    uint32_t usage_page;
    uint32_t root_page;
    uint32_t data_page;
    uint32_t page_count;
    EgMdbLock lock;
    int result = 0;
    memset(&ddl, 0, sizeof(ddl));
    memset(&lock, 0, sizeof(lock));
    if (!db_path || !*db_path || !sql || !*sql) {
        eg_ddl_error(error, error_capacity, "invalid MDB CREATE TABLE arguments");
        return 0;
    }
    if (!eg_ddl_parse(sql, &ddl, error, error_capacity)) {
        goto cleanup;
    }
    if (!eg_mdb_lock_acquire(db_path, 1, &lock, error, error_capacity)) {
        goto cleanup;
    }
    if (stat(db_path, &status) != 0 || status.st_size <= 0 || status.st_size % EG_JET4_PAGE_SIZE != 0) {
        eg_ddl_error(error, error_capacity, "MDB file size is not Jet 4 page aligned");
        goto cleanup;
    }
    mdb = mdb_open(db_path, MDB_WRITABLE);
    if (!mdb) {
        eg_ddl_error(error, error_capacity, "unable to open MDB file for writing");
        goto cleanup;
    }
    if (mdb->f->jet_version != MDB_VER_JET4 || mdb->fmt->pg_size != EG_JET4_PAGE_SIZE) {
        eg_ddl_error(error, error_capacity, "native CREATE TABLE supports Jet 4 MDB files only");
        goto cleanup;
    }
    existing = mdb_read_table_by_name(mdb, ddl.name, MDB_TABLE);
    if (existing) {
        eg_ddl_error(error, error_capacity, "MDB table already exists");
        goto cleanup;
    }
    if (!eg_mdb_transaction_begin(&lock, error, error_capacity)) {
        goto cleanup;
    }
    tdef_page = (uint32_t)(status.st_size / EG_JET4_PAGE_SIZE);
    usage_page = tdef_page + 1;
    root_page = ddl.primary_key_column >= 0 ? tdef_page + 2 : 0;
    data_page = tdef_page + (ddl.primary_key_column >= 0 ? 3 : 2);
    page_count = ddl.primary_key_column >= 0 ? 4 : 3;
    if (!eg_ddl_extend_file(mdb, tdef_page, page_count, error, error_capacity)
        || !eg_ddl_write_tdef(mdb, &ddl, tdef_page, usage_page, root_page, error, error_capacity)
        || !eg_ddl_write_usage_page(mdb, &ddl, usage_page, root_page, data_page, error, error_capacity)
        || (root_page && !eg_ddl_write_index_root_page(mdb, root_page, tdef_page, error, error_capacity))
        || !eg_ddl_write_data_page(mdb, data_page, tdef_page, error, error_capacity)
        || !eg_ddl_catalog_insert(mdb, &ddl, tdef_page, error, error_capacity)) {
        goto cleanup;
    }
    if (fflush(mdb->f->stream) != 0 || fsync(fileno(mdb->f->stream)) != 0) {
        eg_ddl_error(error, error_capacity, "unable to durably flush CREATE TABLE changes");
        goto cleanup;
    }
    mdb_close(mdb);
    mdb = mdb_open(db_path, MDB_NOFLAGS);
    if (!mdb) {
        eg_ddl_error(error, error_capacity, "unable to reopen MDB after CREATE TABLE");
        goto cleanup;
    }
    existing = mdb_read_table_by_name(mdb, ddl.name, MDB_TABLE);
    if (!existing || !mdb_read_columns(existing) || existing->num_cols != ddl.column_count) {
        eg_ddl_error(error, error_capacity, "Jet table verification failed after CREATE TABLE");
        goto cleanup;
    }
    if (ddl.primary_key_column >= 0) {
        (void)mdb_read_indices(existing);
        if (!existing->indices || existing->num_idxs != 1) {
            eg_ddl_error(error, error_capacity, "Jet primary key verification failed after CREATE TABLE");
            goto cleanup;
        }
    }
    result = 1;

cleanup:
    if (existing) {
        mdb_free_tabledef(existing);
    }
    if (mdb) {
        mdb_close(mdb);
    }
    if (result) {
        if (!eg_mdb_transaction_commit(&lock, error, error_capacity)) {
            result = 0;
            (void)eg_mdb_transaction_rollback(&lock, error, error_capacity);
        }
    } else {
        (void)eg_mdb_transaction_rollback(&lock, error, error_capacity);
    }
    eg_mdb_lock_release(&lock);
    eg_ddl_table_free(&ddl);
    return result;
}
