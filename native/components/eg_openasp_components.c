// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_openasp_components.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define EG_COMPONENT_MAX_ARGUMENT_BYTES 65536
#define EG_COMPONENT_DEFAULT_OUTPUT (4 * 1024 * 1024)

/*
 * Native implementations for components that require bounded byte processing,
 * cryptographic libraries, or process control. All returned eg_string_t values
 * copy temporary buffers before those buffers are freed.
 */
typedef struct {
    const unsigned char* data;
    size_t length;
    size_t offset;
} eg_json_parser_t;

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
} eg_buffer_t;

static void eg_process_write_error_number(int fd, int error_number) {
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

static int eg_buffer_reserve(eg_buffer_t* buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length - 1) {
        return 0;
    }
    size_t required = buffer->length + extra + 1;
    if (required <= buffer->capacity) {
        return 1;
    }
    size_t capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char* grown = (char*)realloc(buffer->data, capacity);
    if (!grown) {
        return 0;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int eg_buffer_append(eg_buffer_t* buffer, const void* data, size_t length) {
    if (!eg_buffer_reserve(buffer, length)) {
        return 0;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static eg_string_t* eg_empty_string(void) {
    return eg_string_from_bytes("", 0);
}

static eg_string_t* eg_hex_digest(const unsigned char* digest, unsigned int length) {
    static const char hex[] = "0123456789abcdef";
    char output[EVP_MAX_MD_SIZE * 2];
    for (unsigned int i = 0; i < length; i += 1) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 15];
    }
    return eg_string_from_bytes(output, (int64_t)length * 2);
}

eg_string_t* eg_openasp_sha256(eg_string_t* input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context
        || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1
        || EVP_DigestUpdate(context, eg_string_cstr(input), (size_t)eg_string_len(input)) != 1
        || EVP_DigestFinal_ex(context, digest, &digest_length) != 1) {
        EVP_MD_CTX_free(context);
        return eg_empty_string();
    }
    EVP_MD_CTX_free(context);
    return eg_hex_digest(digest, digest_length);
}

eg_string_t* eg_openasp_hmac_sha256(eg_string_t* key, eg_string_t* input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (!HMAC(EVP_sha256(), eg_string_cstr(key), (int)eg_string_len(key), (const unsigned char*)eg_string_cstr(input), (size_t)eg_string_len(input), digest, &digest_length)) {
        return eg_empty_string();
    }
    return eg_hex_digest(digest, digest_length);
}

eg_string_t* eg_openasp_base64_encode(eg_string_t* input, int64_t url_safe) {
    size_t input_length = (size_t)eg_string_len(input);
    size_t output_length = 4 * ((input_length + 2) / 3);
    unsigned char* output = (unsigned char*)malloc(output_length + 1);
    if (!output) {
        return eg_empty_string();
    }
    int written = EVP_EncodeBlock(output, (const unsigned char*)eg_string_cstr(input), (int)input_length);
    if (written < 0) {
        free(output);
        return eg_empty_string();
    }
    if (url_safe) {
        for (int i = 0; i < written; i += 1) {
            if (output[i] == '+') output[i] = '-';
            if (output[i] == '/') output[i] = '_';
        }
        while (written > 0 && output[written - 1] == '=') {
            written -= 1;
        }
    }
    eg_string_t* result = eg_string_from_bytes((const char*)output, written);
    free(output);
    return result;
}

eg_string_t* eg_openasp_base64_decode(eg_string_t* input, int64_t url_safe) {
    size_t input_length = (size_t)eg_string_len(input);
    size_t padded_length = (input_length + 3) & ~(size_t)3;
    unsigned char* normalized = (unsigned char*)malloc(padded_length + 1);
    unsigned char* output = (unsigned char*)malloc((padded_length / 4) * 3 + 1);
    if (!normalized || !output) {
        free(normalized);
        free(output);
        return eg_empty_string();
    }
    memcpy(normalized, eg_string_cstr(input), input_length);
    for (size_t i = 0; i < input_length; i += 1) {
        if (url_safe && normalized[i] == '-') normalized[i] = '+';
        if (url_safe && normalized[i] == '_') normalized[i] = '/';
    }
    for (size_t i = input_length; i < padded_length; i += 1) normalized[i] = '=';
    normalized[padded_length] = '\0';
    int written = EVP_DecodeBlock(output, normalized, (int)padded_length);
    if (written < 0) {
        free(normalized);
        free(output);
        return eg_empty_string();
    }
    while (padded_length > 0 && normalized[padded_length - 1] == '=') {
        written -= 1;
        padded_length -= 1;
    }
    eg_string_t* result = eg_string_from_bytes((const char*)output, written);
    free(normalized);
    free(output);
    return result;
}

eg_string_t* eg_openasp_random_bytes(int64_t count) {
    if (count < 1 || count > 1048576) {
        return eg_empty_string();
    }
    unsigned char* output = (unsigned char*)malloc((size_t)count);
    if (!output || RAND_bytes(output, (int)count) != 1) {
        free(output);
        return eg_empty_string();
    }
    eg_string_t* result = eg_string_from_bytes((const char*)output, count);
    free(output);
    return result;
}

int64_t eg_openasp_constant_time_equals(eg_string_t* left, eg_string_t* right) {
    size_t left_length = (size_t)eg_string_len(left);
    size_t right_length = (size_t)eg_string_len(right);
    size_t maximum = left_length > right_length ? left_length : right_length;
    const unsigned char* a = (const unsigned char*)eg_string_cstr(left);
    const unsigned char* b = (const unsigned char*)eg_string_cstr(right);
    size_t difference = left_length ^ right_length;
    for (size_t i = 0; i < maximum; i += 1) {
        unsigned char av = i < left_length ? a[i] : 0;
        unsigned char bv = i < right_length ? b[i] : 0;
        difference |= (size_t)(av ^ bv);
    }
    return difference == 0;
}

static void eg_json_space(eg_json_parser_t* parser) {
    while (parser->offset < parser->length && isspace(parser->data[parser->offset])) parser->offset += 1;
}

static int eg_json_value(eg_json_parser_t* parser, int depth);

static int eg_json_string(eg_json_parser_t* parser) {
    if (parser->offset >= parser->length || parser->data[parser->offset++] != '"') return 0;
    while (parser->offset < parser->length) {
        unsigned char value = parser->data[parser->offset++];
        if (value == '"') return 1;
        if (value < 0x20) return 0;
        if (value != '\\') continue;
        if (parser->offset >= parser->length) return 0;
        value = parser->data[parser->offset++];
        if (strchr("\"\\/bfnrt", value)) continue;
        if (value != 'u' || parser->offset + 4 > parser->length) return 0;
        for (int i = 0; i < 4; i += 1) {
            if (!isxdigit(parser->data[parser->offset++])) return 0;
        }
    }
    return 0;
}

static int eg_json_number(eg_json_parser_t* parser) {
    size_t start = parser->offset;
    if (parser->offset < parser->length && parser->data[parser->offset] == '-') parser->offset += 1;
    if (parser->offset >= parser->length) return 0;
    if (parser->data[parser->offset] == '0') {
        parser->offset += 1;
    } else {
        if (!isdigit(parser->data[parser->offset])) return 0;
        while (parser->offset < parser->length && isdigit(parser->data[parser->offset])) parser->offset += 1;
    }
    if (parser->offset < parser->length && parser->data[parser->offset] == '.') {
        parser->offset += 1;
        if (parser->offset >= parser->length || !isdigit(parser->data[parser->offset])) return 0;
        while (parser->offset < parser->length && isdigit(parser->data[parser->offset])) parser->offset += 1;
    }
    if (parser->offset < parser->length && (parser->data[parser->offset] == 'e' || parser->data[parser->offset] == 'E')) {
        parser->offset += 1;
        if (parser->offset < parser->length && (parser->data[parser->offset] == '+' || parser->data[parser->offset] == '-')) parser->offset += 1;
        if (parser->offset >= parser->length || !isdigit(parser->data[parser->offset])) return 0;
        while (parser->offset < parser->length && isdigit(parser->data[parser->offset])) parser->offset += 1;
    }
    return parser->offset > start;
}

static int eg_json_literal(eg_json_parser_t* parser, const char* literal) {
    size_t length = strlen(literal);
    if (parser->offset + length > parser->length || memcmp(parser->data + parser->offset, literal, length) != 0) return 0;
    parser->offset += length;
    return 1;
}

static int eg_json_value(eg_json_parser_t* parser, int depth) {
    if (depth > 128) return 0;
    eg_json_space(parser);
    if (parser->offset >= parser->length) return 0;
    unsigned char token = parser->data[parser->offset];
    if (token == '"') return eg_json_string(parser);
    if (token == '-' || isdigit(token)) return eg_json_number(parser);
    if (token == 't') return eg_json_literal(parser, "true");
    if (token == 'f') return eg_json_literal(parser, "false");
    if (token == 'n') return eg_json_literal(parser, "null");
    if (token == '[') {
        parser->offset += 1;
        eg_json_space(parser);
        if (parser->offset < parser->length && parser->data[parser->offset] == ']') {
            parser->offset += 1;
            return 1;
        }
        for (;;) {
            if (!eg_json_value(parser, depth + 1)) return 0;
            eg_json_space(parser);
            if (parser->offset >= parser->length) return 0;
            if (parser->data[parser->offset] == ']') {
                parser->offset += 1;
                return 1;
            }
            if (parser->data[parser->offset++] != ',') return 0;
        }
    }
    if (token == '{') {
        parser->offset += 1;
        eg_json_space(parser);
        if (parser->offset < parser->length && parser->data[parser->offset] == '}') {
            parser->offset += 1;
            return 1;
        }
        for (;;) {
            eg_json_space(parser);
            if (!eg_json_string(parser)) return 0;
            eg_json_space(parser);
            if (parser->offset >= parser->length || parser->data[parser->offset++] != ':') return 0;
            if (!eg_json_value(parser, depth + 1)) return 0;
            eg_json_space(parser);
            if (parser->offset >= parser->length) return 0;
            if (parser->data[parser->offset] == '}') {
                parser->offset += 1;
                return 1;
            }
            if (parser->data[parser->offset++] != ',') return 0;
        }
    }
    return 0;
}

int64_t eg_openasp_json_validate(eg_string_t* input) {
    eg_json_parser_t parser = { (const unsigned char*)eg_string_cstr(input), (size_t)eg_string_len(input), 0 };
    if (!eg_json_value(&parser, 0)) return 0;
    eg_json_space(&parser);
    return parser.offset == parser.length;
}

eg_string_t* eg_openasp_json_minify(eg_string_t* input) {
    if (!eg_openasp_json_validate(input)) return eg_empty_string();
    const unsigned char* source = (const unsigned char*)eg_string_cstr(input);
    size_t length = (size_t)eg_string_len(input);
    eg_buffer_t output = { 0 };
    int quoted = 0;
    int escaped = 0;
    for (size_t i = 0; i < length; i += 1) {
        unsigned char value = source[i];
        if (!quoted && isspace(value)) continue;
        if (!eg_buffer_append(&output, &value, 1)) {
            free(output.data);
            return eg_empty_string();
        }
        if (quoted) {
            if (escaped) escaped = 0;
            else if (value == '\\') escaped = 1;
            else if (value == '"') quoted = 0;
        } else if (value == '"') {
            quoted = 1;
        }
    }
    eg_string_t* result = eg_string_from_bytes(output.data ? output.data : "", (int64_t)output.length);
    free(output.data);
    return result;
}

eg_string_t* eg_openasp_json_escape(eg_string_t* input) {
    const unsigned char* source = (const unsigned char*)eg_string_cstr(input);
    size_t length = (size_t)eg_string_len(input);
    eg_buffer_t output = { 0 };
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i += 1) {
        unsigned char value = source[i];
        const char* escape = NULL;
        if (value == '"') escape = "\\\"";
        else if (value == '\\') escape = "\\\\";
        else if (value == '\b') escape = "\\b";
        else if (value == '\f') escape = "\\f";
        else if (value == '\n') escape = "\\n";
        else if (value == '\r') escape = "\\r";
        else if (value == '\t') escape = "\\t";
        if (escape) {
            if (!eg_buffer_append(&output, escape, 2)) goto fail;
        } else if (value < 0x20) {
            char encoded[6] = { '\\', 'u', '0', '0', hex[value >> 4], hex[value & 15] };
            if (!eg_buffer_append(&output, encoded, sizeof(encoded))) goto fail;
        } else if (!eg_buffer_append(&output, &value, 1)) {
            goto fail;
        }
    }
    {
        eg_string_t* result = eg_string_from_bytes(output.data ? output.data : "", (int64_t)output.length);
        free(output.data);
        return result;
    }
fail:
    free(output.data);
    return eg_empty_string();
}

static int64_t eg_now_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static eg_string_t* eg_process_error(const char* message) {
    char header[96];
    size_t length = strlen(message);
    int header_length = snprintf(header, sizeof(header), "-1\n0\n0\n%zu\n", length);
    eg_buffer_t packet = { 0 };
    if (!eg_buffer_append(&packet, header, (size_t)header_length)
        || !eg_buffer_append(&packet, message, length)) {
        free(packet.data);
        return eg_empty_string();
    }
    eg_string_t* result = eg_string_from_bytes(packet.data, (int64_t)packet.length);
    free(packet.data);
    return result;
}

/*
 * Parse a command line into argv without invoking a shell. Quotes and
 * backslashes provide grouping only; metacharacters never gain shell meaning.
 * argv points into one owned storage allocation returned to the caller.
 */
static int eg_parse_arguments(const char* executable, const char* arguments, char*** out_argv, char** out_storage) {
    size_t length = strlen(arguments);
    char** argv = (char**)calloc(length / 2 + 3, sizeof(char*));
    char* storage = (char*)malloc(length + 1);
    size_t input = 0, output = 0, count = 0;
    if (!argv || !storage) {
        free(argv);
        free(storage);
        return ENOMEM;
    }
    argv[count++] = (char*)executable;
    while (input < length) {
        while (input < length && isspace((unsigned char)arguments[input])) input += 1;
        if (input >= length) break;
        argv[count++] = storage + output;
        int quoted = 0;
        while (input < length) {
            char value = arguments[input++];
            if (value == '"') {
                quoted = !quoted;
            } else if (value == '\\' && input < length) {
                storage[output++] = arguments[input++];
            } else if (!quoted && isspace((unsigned char)value)) {
                break;
            } else {
                storage[output++] = value;
            }
        }
        if (quoted) {
            free(argv);
            free(storage);
            return EINVAL;
        }
        storage[output++] = '\0';
    }
    argv[count] = NULL;
    *out_argv = argv;
    *out_storage = storage;
    return 0;
}

static void eg_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void eg_close_inherited_descriptors(int preserved_fd) {
    long maximum = sysconf(_SC_OPEN_MAX);
    if (maximum < 0 || maximum > 1048576) {
        maximum = 65536;
    }
    for (int fd = STDERR_FILENO + 1; fd < maximum; fd += 1) {
        if (fd != preserved_fd) close(fd);
    }
}

/* Drain a nonblocking pipe up to its cap and close it on EOF or hard failure. */
static void eg_read_pipe(int fd, eg_buffer_t* output, size_t maximum, int* open_flag) {
    char chunk[8192];
    for (;;) {
        ssize_t count = read(fd, chunk, sizeof(chunk));
        if (count > 0) {
            size_t accepted = (size_t)count;
            if (output->length + accepted > maximum) accepted = maximum - output->length;
            if (accepted > 0) (void)eg_buffer_append(output, chunk, accepted);
            continue;
        }
        if (count == 0) {
            close(fd);
            *open_flag = 0;
        }
        break;
    }
}

/*
 * Run a child in its own process group while concurrently pumping stdin,
 * stdout, and stderr. Closing stdin after EPIPE/complete input is essential:
 * leaving a failed descriptor in poll would spin at full CPU. Timeout kills the
 * whole process group, and output is capped independently per stream.
 */
eg_string_t* eg_openasp_process_run(eg_string_t* executable_value, eg_string_t* arguments_value, eg_string_t* directory_value, eg_string_t* stdin_value, int64_t timeout_ms, int64_t max_output) {
    const char* executable = eg_string_cstr(executable_value);
    const char* arguments = eg_string_cstr(arguments_value);
    const char* directory = eg_string_cstr(directory_value);
    size_t argument_length = (size_t)eg_string_len(arguments_value);
    size_t stdin_length = (size_t)eg_string_len(stdin_value);
    if (!executable
        || eg_string_len(executable_value) < 1
        || (size_t)eg_string_len(executable_value) != strlen(executable)
        || !arguments
        || argument_length != strlen(arguments)
        || argument_length > EG_COMPONENT_MAX_ARGUMENT_BYTES
        || !directory
        || (size_t)eg_string_len(directory_value) != strlen(directory)
        || stdin_length > EG_COMPONENT_MAX_ARGUMENT_BYTES) {
        return eg_process_error("invalid process arguments");
    }
    if (max_output < 1) max_output = EG_COMPONENT_DEFAULT_OUTPUT;
    if (max_output > 64 * 1024 * 1024) max_output = 64 * 1024 * 1024;
    char** argv = NULL;
    char* storage = NULL;
    int parse_error = eg_parse_arguments(executable, arguments, &argv, &storage);
    if (parse_error) return eg_process_error("cannot parse process arguments");
    int in_pipe[2] = { -1, -1 }, out_pipe[2] = { -1, -1 }, err_pipe[2] = { -1, -1 };
    if (pipe(in_pipe) || pipe(out_pipe) || pipe(err_pipe)) {
        if (in_pipe[0] >= 0) close(in_pipe[0]);
        if (in_pipe[1] >= 0) close(in_pipe[1]);
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (out_pipe[1] >= 0) close(out_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        free(argv); free(storage);
        return eg_process_error("cannot create process pipes");
    }
    pid_t child = fork();
    if (child == 0) {
        (void)setpgid(0, 0);
        (void)dup2(in_pipe[0], STDIN_FILENO);
        (void)dup2(out_pipe[1], STDOUT_FILENO);
        (void)dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        if (directory && *directory && chdir(directory) != 0) _exit(126);
        eg_close_inherited_descriptors(-1);
        execvp(executable, argv);
        _exit(127);
    }
    free(argv); free(storage);
    if (child < 0) {
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        return eg_process_error("cannot fork process");
    }
    (void)setpgid(child, child);
    close(in_pipe[0]); close(out_pipe[1]); close(err_pipe[1]);
    size_t input_offset = 0;
    int input_open = 1, output_open = 1, error_open = 1, timed_out = 0, status = 0, exited = 0;
    eg_set_nonblocking(in_pipe[1]); eg_set_nonblocking(out_pipe[0]); eg_set_nonblocking(err_pipe[0]);
    int64_t started = eg_now_ms();
    eg_buffer_t output = { 0 }, error = { 0 };
    while (!exited || output_open || error_open) {
        if (timeout_ms > 0 && eg_now_ms() - started >= timeout_ms && !exited) {
            (void)kill(-child, SIGKILL);
            timed_out = 1;
        }
        struct pollfd fds[3];
        int count = 0;
        if (input_open) fds[count++] = (struct pollfd){ in_pipe[1], POLLOUT, 0 };
        if (output_open) fds[count++] = (struct pollfd){ out_pipe[0], POLLIN | POLLHUP, 0 };
        if (error_open) fds[count++] = (struct pollfd){ err_pipe[0], POLLIN | POLLHUP, 0 };
        (void)poll(fds, (nfds_t)count, 20);
        if (input_open) {
            if (input_offset < stdin_length) {
                ssize_t sent = write(in_pipe[1], eg_string_cstr(stdin_value) + input_offset, stdin_length - input_offset);
                if (sent > 0) input_offset += (size_t)sent;
            }
            if (input_offset >= stdin_length) {
                close(in_pipe[1]);
                input_open = 0;
            }
        }
        if (output_open) eg_read_pipe(out_pipe[0], &output, (size_t)max_output, &output_open);
        if (error_open) eg_read_pipe(err_pipe[0], &error, (size_t)max_output, &error_open);
        if (!exited) {
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) exited = 1;
        }
    }
    if (input_open) close(in_pipe[1]);
    int exit_code = timed_out ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    char header[128];
    int header_length = snprintf(header, sizeof(header), "%d\n%d\n%zu\n%zu\n", exit_code, timed_out, output.length, error.length);
    eg_buffer_t packet = { 0 };
    (void)eg_buffer_append(&packet, header, (size_t)header_length);
    (void)eg_buffer_append(&packet, output.data ? output.data : "", output.length);
    (void)eg_buffer_append(&packet, error.data ? error.data : "", error.length);
    eg_string_t* result = eg_string_from_bytes(packet.data, (int64_t)packet.length);
    free(output.data); free(error.data); free(packet.data);
    return result;
}

/*
 * Start asynchronously and use a close-on-exec error pipe to distinguish a
 * successful exec from a child that merely forked and then failed setup.
 */
int64_t eg_openasp_process_start(eg_string_t* executable_value, eg_string_t* arguments_value, eg_string_t* directory_value) {
    const char* executable = eg_string_cstr(executable_value);
    const char* arguments = eg_string_cstr(arguments_value);
    const char* directory = eg_string_cstr(directory_value);
    size_t executable_length = (size_t)eg_string_len(executable_value);
    size_t argument_length = (size_t)eg_string_len(arguments_value);
    size_t directory_length = (size_t)eg_string_len(directory_value);
    if (!executable || executable_length < 1 || executable_length != strlen(executable)
        || !arguments || argument_length != strlen(arguments) || argument_length > EG_COMPONENT_MAX_ARGUMENT_BYTES
        || !directory || directory_length != strlen(directory)) {
        return -EINVAL;
    }
    char** argv = NULL;
    char* storage = NULL;
    int parse_error = eg_parse_arguments(executable, arguments, &argv, &storage);
    if (parse_error) return -parse_error;
    int error_pipe[2] = { -1, -1 };
    if (pipe(error_pipe) != 0) {
        free(argv);
        free(storage);
        return -errno;
    }
    if (fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        int error_number = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        free(argv);
        free(storage);
        return -error_number;
    }
    pid_t child = fork();
    if (child == 0) {
        close(error_pipe[0]);
        (void)setpgid(0, 0);
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO && null_fd != error_pipe[1]) close(null_fd);
        }
        if (directory_length > 0 && chdir(directory) != 0) {
            int error_number = errno;
            eg_process_write_error_number(error_pipe[1], error_number);
            _exit(126);
        }
        eg_close_inherited_descriptors(error_pipe[1]);
        execvp(executable, argv);
        int error_number = errno;
        eg_process_write_error_number(error_pipe[1], error_number);
        _exit(127);
    }
    int fork_error = errno;
    close(error_pipe[1]);
    free(argv);
    free(storage);
    if (child < 0) {
        close(error_pipe[0]);
        return -fork_error;
    }
    (void)setpgid(child, child);
    int launch_error = 0;
    ssize_t received;
    do {
        received = read(error_pipe[0], &launch_error, sizeof(launch_error));
    } while (received < 0 && errno == EINTR);
    close(error_pipe[0]);
    if (received == 0) return child;
    (void)waitpid(child, NULL, 0);
    return received == (ssize_t)sizeof(launch_error) ? -launch_error : -EIO;
}

int64_t eg_openasp_process_wait(int64_t pid, int64_t timeout_ms) {
    if (pid <= 0) return -2;
    int status = 0;
    int64_t started = eg_now_ms();
    for (;;) {
        pid_t result = waitpid((pid_t)pid, &status, WNOHANG);
        if (result == (pid_t)pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return -2;
        }
        if (result < 0) return -2;
        if (timeout_ms == 0 || (timeout_ms > 0 && eg_now_ms() - started >= timeout_ms)) return -1;
        (void)poll(NULL, 0, 10);
    }
}

int64_t eg_openasp_process_terminate(int64_t pid, int64_t signal_number) {
    if (pid <= 0 || signal_number < 1 || signal_number > 64) return 0;
    return kill((pid_t)(-pid), (int)signal_number) == 0;
}

int64_t eg_openasp_process_fork(void) {
    pid_t child = fork();
    return child < 0 ? -errno : (int64_t)child;
}

eg_string_t* eg_openasp_process_waitpid(int64_t pid, int64_t timeout_ms) {
    if (pid == 0 || pid < -1) {
        char packet[96];
        int length = snprintf(packet, sizeof(packet), "-1\n-1\n0\n%d\n", EINVAL);
        return eg_string_from_bytes(packet, length);
    }
    int status = 0;
    int64_t started = eg_now_ms();
    for (;;) {
        pid_t result = waitpid((pid_t)pid, &status, timeout_ms < 0 ? 0 : WNOHANG);
        if (result > 0) {
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            int term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            char packet[96];
            int length = snprintf(packet, sizeof(packet), "%lld\n%d\n%d\n0\n", (long long)result, exit_code, term_signal);
            return eg_string_from_bytes(packet, length);
        }
        if (result < 0) {
            if (errno == EINTR) continue;
            char packet[96];
            int length = snprintf(packet, sizeof(packet), "-1\n-1\n0\n%d\n", errno);
            return eg_string_from_bytes(packet, length);
        }
        if (timeout_ms == 0 || (timeout_ms > 0 && eg_now_ms() - started >= timeout_ms)) {
            return eg_string_from_bytes("0\n-1\n0\n0\n", 9);
        }
        (void)poll(NULL, 0, 10);
    }
}

int64_t eg_openasp_process_signal(int64_t pid, int64_t signal_number) {
    if (pid <= 0 || signal_number < 1 || signal_number > 64) return 0;
    return kill((pid_t)pid, (int)signal_number) == 0;
}

int64_t eg_openasp_process_exit(int64_t exit_code) {
    _exit((int)(exit_code & 255));
}

static const char* eg_fpm_socket_path(eg_string_t* value) {
    if (!value || eg_string_len(value) <= 0) return NULL;
    const char* path = eg_string_cstr(value);
    if (!path || (size_t)eg_string_len(value) != strlen(path)) return NULL;
    return path;
}

int64_t eg_openasp_fpm_socket_prepare(eg_string_t* path_value) {
    const char* path = eg_fpm_socket_path(path_value);
    struct stat info;
    if (!path) return -1;
    if (lstat(path, &info) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(info.st_mode)) return -2;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address;
    size_t path_length = strlen(path);
    if (path_length >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, path_length + 1);
    int connect_rc = connect(fd, (struct sockaddr*)&address, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_length + 1));
    int connect_error = errno;
    close(fd);
    if (connect_rc == 0 || (connect_error != ECONNREFUSED && connect_error != ENOENT)) return -3;
    if (connect_error == ENOENT) return 0;
    return unlink(path) == 0 ? 1 : -1;
}

int64_t eg_openasp_fpm_socket_chmod(eg_string_t* path_value, int64_t mode) {
    const char* path = eg_fpm_socket_path(path_value);
    if (!path || mode < 0 || mode > 0777) return -1;
    return chmod(path, (mode_t)mode) == 0 ? 0 : -1;
}

int64_t eg_openasp_fpm_socket_cleanup(eg_string_t* path_value) {
    const char* path = eg_fpm_socket_path(path_value);
    struct stat info;
    if (!path) return -1;
    if (lstat(path, &info) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(info.st_mode)) return -2;
    return unlink(path) == 0 ? 0 : -1;
}
