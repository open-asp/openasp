// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_shell_wrap.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define EG_SHELL_MAX_ARGUMENT_BYTES 65536

/*
 * Shell.Application compatibility without a command shell. Arguments are
 * parsed into argv, execvp performs resolution, and a close-on-exec pipe reports
 * setup/exec failure to the parent without waiting for normal child completion.
 */
static eg_string_t* eg_shell_error(const char* prefix, int error_number) {
    char message[512];
    snprintf(message, sizeof(message), "%s: %s (errno=%d)", prefix, strerror(error_number), error_number);
    return eg_string_from_cstr(message);
}

static void eg_shell_write_error_number(int fd, int error_number) {
    const unsigned char* bytes = (const unsigned char*)&error_number;
    size_t written = 0;
    while (written < sizeof(error_number)) {
        ssize_t result = write(fd, bytes + written, sizeof(error_number) - written);
        if (result > 0) {
            written += (size_t)result;
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static void eg_shell_free_argv(char** argv, char* argument_buffer) {
    free(argv);
    free(argument_buffer);
}

/*
 * Implement Windows-style quote/backslash grouping closely enough for legacy
 * ASP callers. Returned argv elements borrow one contiguous argument buffer.
 */
static int eg_shell_parse_arguments(const char* file_name, const char* arguments, char*** out_argv, char** out_buffer) {
    size_t length = strlen(arguments);
    size_t capacity = length / 2 + 3;
    char** argv = (char**)calloc(capacity, sizeof(char*));
    char* buffer = (char*)malloc(length + 1);
    size_t input = 0;
    size_t output = 0;
    size_t count = 0;
    if (!argv || !buffer) {
        free(argv);
        free(buffer);
        return ENOMEM;
    }
    argv[count++] = (char*)file_name;
    while (input < length) {
        while (input < length && isspace((unsigned char)arguments[input])) {
            input += 1;
        }
        if (input >= length) {
            break;
        }
        if (count + 1 >= capacity) {
            eg_shell_free_argv(argv, buffer);
            return E2BIG;
        }
        argv[count++] = buffer + output;
        int quoted = 0;
        while (input < length) {
            char current = arguments[input];
            if (current == '"') {
                quoted = !quoted;
                input += 1;
                continue;
            }
            if (current == '\\') {
                size_t slash_count = 0;
                while (input < length && arguments[input] == '\\') {
                    slash_count += 1;
                    input += 1;
                }
                if (input < length && arguments[input] == '"') {
                    for (size_t i = 0; i < slash_count / 2; i += 1) {
                        buffer[output++] = '\\';
                    }
                    if ((slash_count & 1U) != 0) {
                        buffer[output++] = '"';
                    } else {
                        quoted = !quoted;
                    }
                    input += 1;
                    continue;
                }
                for (size_t i = 0; i < slash_count; i += 1) {
                    buffer[output++] = '\\';
                }
                continue;
            }
            if (!quoted && isspace((unsigned char)current)) {
                break;
            }
            buffer[output++] = current;
            input += 1;
        }
        if (quoted) {
            eg_shell_free_argv(argv, buffer);
            return EINVAL;
        }
        buffer[output++] = '\0';
    }
    argv[count] = NULL;
    *out_argv = argv;
    *out_buffer = buffer;
    return 0;
}

static void eg_shell_redirect_standard_streams(void) {
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0) {
        return;
    }
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd += 1) {
        if (null_fd != fd) {
            (void)dup2(null_fd, fd);
        }
    }
    if (null_fd > STDERR_FILENO) {
        close(null_fd);
    }
}

static void eg_shell_close_descriptors_except(int preserved_fd) {
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0 || maximum > 1048576) {
        maximum = 65536;
    }
    for (int fd = 3; fd < maximum; fd += 1) {
        if (fd != preserved_fd) {
            close(fd);
        }
    }
}

/*
 * Spawn asynchronously. The parent returns success only after EOF on the
 * close-on-exec pipe proves that execvp completed.
 */
eg_string_t* eg_shell_execute(eg_string_t* file_name_value, eg_string_t* arguments_value, eg_string_t* directory_value) {
    const char* file_name = eg_string_cstr(file_name_value);
    const char* arguments = eg_string_cstr(arguments_value);
    const char* directory = eg_string_cstr(directory_value);
    int64_t file_length = eg_string_len(file_name_value);
    int64_t argument_length = eg_string_len(arguments_value);
    int64_t directory_length = eg_string_len(directory_value);
    char** argv = NULL;
    char* argument_buffer = NULL;
    int error_number = 0;
    int error_pipe[2] = { -1, -1 };
    if (!file_name || file_length <= 0 || (int64_t)strlen(file_name) != file_length) {
        return eg_string_from_cstr("invalid executable path");
    }
    if (!arguments || argument_length < 0 || argument_length > EG_SHELL_MAX_ARGUMENT_BYTES || (int64_t)strlen(arguments) != argument_length) {
        return eg_string_from_cstr("invalid or oversized argument string");
    }
    if (!directory || directory_length < 0 || (int64_t)strlen(directory) != directory_length) {
        return eg_string_from_cstr("invalid working directory");
    }
    error_number = eg_shell_parse_arguments(file_name, arguments, &argv, &argument_buffer);
    if (error_number != 0) {
        return eg_shell_error("cannot parse arguments", error_number);
    }
    if (pipe(error_pipe) != 0) {
        eg_shell_free_argv(argv, argument_buffer);
        return eg_shell_error("cannot create launch status pipe", errno);
    }
    if (fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
        error_number = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        eg_shell_free_argv(argv, argument_buffer);
        return eg_shell_error("cannot configure launch status pipe", error_number);
    }
    pid_t first_child = fork();
    if (first_child < 0) {
        error_number = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        eg_shell_free_argv(argv, argument_buffer);
        return eg_shell_error("cannot fork launcher", error_number);
    }
    if (first_child == 0) {
        close(error_pipe[0]);
        if (setsid() < 0) {
            error_number = errno;
            eg_shell_write_error_number(error_pipe[1], error_number);
            _exit(127);
        }
        pid_t second_child = fork();
        if (second_child < 0) {
            error_number = errno;
            eg_shell_write_error_number(error_pipe[1], error_number);
            _exit(127);
        }
        if (second_child > 0) {
            _exit(0);
        }
        eg_shell_redirect_standard_streams();
        eg_shell_close_descriptors_except(error_pipe[1]);
        if (directory_length > 0 && chdir(directory) != 0) {
            error_number = errno;
            eg_shell_write_error_number(error_pipe[1], error_number);
            _exit(127);
        }
        execvp(file_name, argv);
        error_number = errno;
        eg_shell_write_error_number(error_pipe[1], error_number);
        _exit(127);
    }
    close(error_pipe[1]);
    while (waitpid(first_child, NULL, 0) < 0 && errno == EINTR) {
    }
    ssize_t received = 0;
    do {
        received = read(error_pipe[0], &error_number, sizeof(error_number));
    } while (received < 0 && errno == EINTR);
    close(error_pipe[0]);
    eg_shell_free_argv(argv, argument_buffer);
    if (received == 0) {
        return eg_string_from_bytes("", 0);
    }
    if (received == (ssize_t)sizeof(error_number)) {
        return eg_shell_error("cannot launch executable", error_number);
    }
    return eg_string_from_cstr("cannot read launch status");
}
