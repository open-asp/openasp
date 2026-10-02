// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "eg_mdb_lock.h"
#include "eg_mdbtools_wrap.h"

/* Standalone harness for the native MDB lock protocol. It supplies the small
 * Egret string ABI expected by the production wrappers, then exposes modes
 * used by check_mdb_concurrency.py to combine threads, forked writers, readers
 * and deliberately held shared/exclusive locks against one database. */

/* Test-owned strings use malloc because this executable does not link the
 * Egret collector. Every wrapper result is released by eg_free_result. */
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

typedef struct EgWriterThread {
    const char* database;
    int writer;
    int count;
    int failed;
} EgWriterThread;

static int eg_run_reader(const char* database_path, int count);

static eg_string_t eg_test_string(const char* value) {
    eg_string_t string;
    string.len = (int64_t)strlen(value);
    string.data = (char*)value;
    return string;
}

static void eg_free_result(eg_string_t* result) {
    if (result) {
        free(result->data);
        free(result);
    }
}

static int eg_expect_success(const char* operation, eg_string_t* result) {
    int success = result && result->len == 0;
    if (!success) {
        fprintf(stderr, "%s failed: %s\n", operation, result ? result->data : "out of memory");
    }
    eg_free_result(result);
    return success;
}

static void eg_hex_encode(const char* source, char* destination, size_t capacity) {
    static const char digits[] = "0123456789abcdef";
    size_t index = 0;
    while (source[index] && index * 2 + 2 <= capacity) {
        unsigned char value = (unsigned char)source[index];
        destination[index * 2] = digits[value >> 4];
        destination[index * 2 + 1] = digits[value & 15];
        index++;
    }
    destination[index * 2] = '\0';
}

/* The native wrapper's wire format separates hex-encoded fields with 0x1f.
 * Encoding here exercises the same boundary as the Egret database driver
 * without making the C test depend on evaluator internals. */
static int eg_insert_rows(const char* database_path, int writer, int count) {
    eg_string_t database = eg_test_string(database_path);
    eg_string_t table = eg_test_string("concurrent_rows");
    eg_string_t columns = eg_test_string("777269746572\037736571\0377061796c6f6164");
    int index = 0;
    while (index < count) {
        char writer_literal[32];
        char sequence_literal[32];
        char payload_literal[64];
        char writer_hex[65];
        char sequence_hex[65];
        char payload_hex[129];
        char encoded_values[270];
        eg_string_t values;
        snprintf(writer_literal, sizeof(writer_literal), "v%d", writer);
        snprintf(sequence_literal, sizeof(sequence_literal), "v%d", index);
        snprintf(payload_literal, sizeof(payload_literal), "swriter-%d-row-%d", writer, index);
        eg_hex_encode(writer_literal, writer_hex, sizeof(writer_hex));
        eg_hex_encode(sequence_literal, sequence_hex, sizeof(sequence_hex));
        eg_hex_encode(payload_literal, payload_hex, sizeof(payload_hex));
        snprintf(encoded_values, sizeof(encoded_values), "%s\037%s\037%s", writer_hex, sequence_hex, payload_hex);
        values = eg_test_string(encoded_values);
        if (!eg_expect_success("INSERT", eg_mdb_insert_row(&database, &table, &columns, &values))) {
            return 0;
        }
        index++;
    }
    return 1;
}

static void* eg_writer_thread(void* argument) {
    EgWriterThread* writer = (EgWriterThread*)argument;
    writer->failed = !eg_insert_rows(writer->database, writer->writer, writer->count);
    return NULL;
}

static int eg_run_threads(const char* database, int thread_count, int row_count) {
    pthread_t* threads = (pthread_t*)calloc((size_t)thread_count, sizeof(*threads));
    EgWriterThread* writers = (EgWriterThread*)calloc((size_t)thread_count, sizeof(*writers));
    int started = 0;
    int result = 1;
    int index;
    if (!threads || !writers) {
        free(threads);
        free(writers);
        return 0;
    }
    while (started < thread_count) {
        writers[started].database = database;
        writers[started].writer = 1000 + started;
        writers[started].count = row_count;
        if (pthread_create(&threads[started], NULL, eg_writer_thread, &writers[started]) != 0) {
            result = 0;
            break;
        }
        started++;
    }
    index = 0;
    while (index < started) {
        pthread_join(threads[index], NULL);
        if (writers[index].failed) {
            result = 0;
        }
        index++;
    }
    free(threads);
    free(writers);
    return result;
}

/* Children exit through _exit so they cannot flush or tear down inherited
 * parent runtime state. Their independent descriptors exercise process-level
 * locking in addition to the in-process path mutex covered by threads. */
static int eg_run_forked(const char* database, int process_count, int row_count) {
    int started = 0;
    int result = 1;
    if (!eg_run_reader(database, 1)) {
        return 0;
    }
    while (started < process_count) {
        pid_t child = fork();
        if (child < 0) {
            result = 0;
            break;
        }
        if (child == 0) {
            _exit(eg_insert_rows(database, 2000 + started, row_count) ? 0 : 1);
        }
        started++;
    }
    while (started > 0) {
        int status;
        if (wait(&status) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            result = 0;
        }
        started--;
    }
    return result;
}

static int eg_run_reader(const char* database_path, int count) {
    eg_string_t database = eg_test_string(database_path);
    eg_string_t table = eg_test_string("concurrent_rows");
    eg_string_t empty = eg_test_string("");
    int index = 0;
    while (index < count) {
        eg_string_t* result = eg_mdb_query(&database, &table, &empty, 0, &empty, &empty, &empty, 0, 0, 0, 0);
        if (!result || result->len == 0 || strstr(result->data, "Error:") != NULL) {
            fprintf(stderr, "reader observed an invalid query result: %s\n", result ? result->data : "out of memory");
            eg_free_result(result);
            return 0;
        }
        eg_free_result(result);
        index++;
    }
    return 1;
}

static int eg_hold_lock(const char* database, int exclusive, long milliseconds) {
    EgMdbLock lock;
    struct timespec duration;
    char error[512];
    if (!eg_mdb_lock_acquire(database, exclusive, &lock, error, sizeof(error))) {
        fprintf(stderr, "lock failed: %s\n", error);
        return 0;
    }
    duration.tv_sec = milliseconds / 1000;
    duration.tv_nsec = (milliseconds % 1000) * 1000000L;
    while (nanosleep(&duration, &duration) != 0) {
    }
    eg_mdb_lock_release(&lock);
    return 1;
}

/* Keep this executable mode-driven: the Python coordinator controls timing,
 * validates blocking behavior and inspects final row/AutoNumber integrity. */
int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <mode> <database> [arguments]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "init")) {
        eg_string_t database = eg_test_string(argv[2]);
        eg_string_t sql = eg_test_string(
            "CREATE TABLE [concurrent_rows] ("
            "[id] AutoIncrement primary key,"
            "[writer] int not null,"
            "[seq] int not null,"
            "[payload] VARCHAR(80) not null)");
        return eg_expect_success("CREATE", eg_mdb_create_table(&database, &sql)) ? 0 : 1;
    }
    if (!strcmp(argv[1], "write") && argc == 5) {
        return eg_insert_rows(argv[2], atoi(argv[3]), atoi(argv[4])) ? 0 : 1;
    }
    if (!strcmp(argv[1], "read") && argc == 4) {
        return eg_run_reader(argv[2], atoi(argv[3])) ? 0 : 1;
    }
    if (!strcmp(argv[1], "threads") && argc == 5) {
        return eg_run_threads(argv[2], atoi(argv[3]), atoi(argv[4])) ? 0 : 1;
    }
    if (!strcmp(argv[1], "fork") && argc == 5) {
        return eg_run_forked(argv[2], atoi(argv[3]), atoi(argv[4])) ? 0 : 1;
    }
    if (!strcmp(argv[1], "hold-read") && argc == 4) {
        return eg_hold_lock(argv[2], 0, atol(argv[3])) ? 0 : 1;
    }
    if (!strcmp(argv[1], "hold-write") && argc == 4) {
        return eg_hold_lock(argv[2], 1, atol(argv[3])) ? 0 : 1;
    }
    fprintf(stderr, "invalid mode or arguments\n");
    return 2;
}
