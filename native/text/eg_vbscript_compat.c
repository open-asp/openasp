// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

#include "eg_vbscript_compat.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EG_VBS_INVALID_DATE INT64_MIN

void eg_vbs_prepare_timezone(void) {
    /*
     * macOS treats an unset TZ as a request to rediscover /etc/localtime in
     * mktime/localtime hot paths.  Pin the equivalent system-local source once
     * while preserving an explicitly configured application timezone.
     */
    if (!getenv("TZ")) {
        (void)setenv("TZ", ":/etc/localtime", 0);
    }
    tzset();
}

static int eg_vbs_ascii_equal(const eg_string_t* value, const char* literal) {
    size_t length;
    size_t i;
    if (!value || !value->data || !literal || value->len < 0) return 0;
    length = strlen(literal);
    if ((size_t)value->len != length) return 0;
    for (i = 0; i < length; i++) {
        unsigned char left = (unsigned char)value->data[i];
        unsigned char right = (unsigned char)literal[i];
        if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + ('a' - 'A'));
        if (left != right) return 0;
    }
    return 1;
}

int64_t eg_vbs_string_to_bool(eg_string_t* text) {
    eg_string_t view;
    const char* start;
    const char* end;
    const char* cursor;
    char local[128];
    char* buffer;
    char* parsed_end;
    double number;
    size_t length;
    int digits = 0;
    int64_t result;
    if (!text || !text->data || text->len <= 0) return -13;
    start = text->data;
    end = start + text->len;
    while (start < end && (*start == ' ' || (*start >= '\t' && *start <= '\r'))) start++;
    while (end > start && (end[-1] == ' ' || (end[-1] >= '\t' && end[-1] <= '\r'))) end--;
    if (start == end) return -13;
    view = *text;
    view.data = (char*)start;
    view.len = end - start;
    if (eg_vbs_ascii_equal(&view, "false")) return 0;
    if (eg_vbs_ascii_equal(&view, "true")) return 1;

    /* Validate the entire VB numeric token before calling libc. In particular,
     * strtod's NaN, infinity, C hex literals and partial parses are not valid
     * VBScript Boolean strings. Return negative VB error numbers on failure. */
    cursor = start;
    if (*cursor == '+' || *cursor == '-') cursor++;
    if (end - cursor >= 2 && cursor[0] == '&' &&
            (cursor[1] == 'H' || cursor[1] == 'h' || cursor[1] == 'O' || cursor[1] == 'o')) {
        unsigned base = cursor[1] == 'H' || cursor[1] == 'h' ? 16 : 8;
        uint64_t value = 0;
        int overflow = 0;
        cursor += 2;
        if (cursor == end) return -13;
        while (cursor < end) {
            unsigned char ch = (unsigned char)*cursor++;
            unsigned digit = ch >= '0' && ch <= '9' ? ch - '0'
                : (ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : (ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 16));
            if (digit >= base) return -13;
            if (value > (UINT32_MAX - digit) / base) overflow = 1;
            if (!overflow) value = value * base + digit;
        }
        return overflow ? -6 : value != 0;
    }
    while (cursor < end && *cursor >= '0' && *cursor <= '9') { cursor++; digits = 1; }
    if (cursor < end && *cursor == '.') {
        cursor++;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') { cursor++; digits = 1; }
    }
    if (!digits) return -13;
    if (cursor < end && (*cursor == 'e' || *cursor == 'E')) {
        cursor++;
        if (cursor < end && (*cursor == '+' || *cursor == '-')) cursor++;
        digits = 0;
        while (cursor < end && *cursor >= '0' && *cursor <= '9') { cursor++; digits = 1; }
        if (!digits) return -13;
    }
    if (cursor != end) return -13;
    length = (size_t)(end - start);
    buffer = length < sizeof(local) ? local : malloc(length + 1);
    if (!buffer) return -7;
    memcpy(buffer, start, length);
    buffer[length] = '\0';
    number = strtod(buffer, &parsed_end);
    result = parsed_end != buffer + length ? -13 : (!isfinite(number) ? -6 : number != 0.0);
    if (buffer != local) free(buffer);
    return result;
}

static int eg_vbs_name_in(const eg_string_t* name,
                          const char* const* names, size_t count) {
    size_t i;
    for (i = 0; i < count; i++) {
        if (eg_vbs_ascii_equal(name, names[i])) return 1;
    }
    return 0;
}

int64_t eg_vbs_is_compat_builtin(eg_string_t* name) {
    static const char* const names[] = {
        "Asc", "AscB", "AscW", "ChrW", "Date", "Now", "Time", "CDate",
        "DateAdd", "DateDiff", "DatePart", "DateSerial", "Day", "Hour",
        "Minute", "Month", "Second", "Weekday", "Year", "IsArray",
        "IsDate", "LenB", "LeftB", "MidB", "InStrB", "FormatNumber", "Space",
        "StrComp", "TypeName", "VarType", "Server.URLEncode",
        "Server.HTMLEncode", "CBool", "RGB"
    };
    return eg_vbs_name_in(name, names, sizeof(names) / sizeof(names[0]));
}

int64_t eg_vbs_is_values_only_builtin(eg_string_t* name) {
    static const char* const names[] = {
        "IIf", "Split", "Join", "CByte", "ChrB", "Hex", "String", "Abs",
        "Round", "Sqr", "Sgn", "Atn", "Sin", "Cos", "Tan", "Exp", "Log",
        "vbsescape", "vbsunescape"
    };
    return eg_vbs_name_in(name, names, sizeof(names) / sizeof(names[0]));
}

int64_t eg_vbs_is_runtime_builtin(eg_string_t* name) {
    static const char* const names[] = {
        "Request", "Request.QueryString", "Request.Form", "Request.Cookies",
        "Request.ServerVariables", "Request.BinaryRead", "Session",
        "Session.Contents", "Application", "Application.Contents", "Array",
        "IIf", "Split", "Join", "Server.MapPath", "CreateObject",
        "Server.CreateObject", "CStr", "CInt", "CLng", "CDbl", "CByte",
        "Chr", "ChrB", "ChrW", "Asc", "AscB", "AscW", "Len", "LenB",
        "Left", "LeftB", "Right", "Mid", "MidB", "InStr", "InStrB", "InStrRev",
        "Replace", "LCase", "UCase", "Trim", "LTrim", "RTrim", "Space",
        "String", "StrComp", "Hex", "IsNull", "IsEmpty", "IsNumeric",
        "IsArray", "IsDate", "TypeName", "VarType", "Date", "Now", "Time",
        "CDate", "DateAdd", "DateDiff", "DatePart", "DateSerial", "Day",
        "Hour", "Minute", "Month", "Second", "Weekday", "Year",
        "FormatDateTime", "FormatNumber", "Timer", "Rnd", "Randomize",
        "Abs", "Int", "Fix", "Round", "Sqr", "Sgn", "Atn", "Sin", "Cos",
        "Tan", "Exp", "Log", "Eval", "Execute", "GetRef", "CBool", "RGB",
        "Server.URLEncode", "Server.HTMLEncode", "vbsescape", "vbsunescape"
    };
    return eg_vbs_name_in(name, names, sizeof(names) / sizeof(names[0]));
}

int64_t eg_asp_parse_decimal_range(eg_string_t* text, int64_t start, int64_t length) {
    int negative = 0;
    int64_t value = 0;
    int64_t end;
    int64_t pos;
    if (!text || !text->data || start < 0 || length <= 0 || start > text->len || length > text->len - start) {
        return 0;
    }
    end = start + length;
    pos = start;
    if (text->data[pos] == '-') {
        negative = 1;
        pos++;
        if (pos == end) {
            return 0;
        }
    }
    while (pos < end) {
        int digit = (unsigned char)text->data[pos] - '0';
        if (digit < 0 || digit > 9 || value > (INT64_MAX - digit) / 10) {
            return 0;
        }
        value = value * 10 + digit;
        pos++;
    }
    return negative ? -value : value;
}

void* eg_gc_register_global_root_range_handle(void* lo, void* hi);
void eg_gc_unregister_global_root_range_handle(void* handle);

/* A pin owns the registration handle, not the managed object. Keeping the
 * object pointer in stable native storage gives the moving collector a root
 * slot it may rewrite if the object moves. Callers must pair every nonzero
 * handle with eg_asp_gc_unpin_object. */
typedef struct {
    void* object;
    void* root_handle;
} eg_asp_gc_pin_t;

static int eg_asp_gc_disabled(void) {
    const char* value = getenv("EG_GC_DISABLE");
    return value && value[0] && strcmp(value, "0") != 0
        && strcmp(value, "false") != 0
        && strcmp(value, "False") != 0
        && strcmp(value, "FALSE") != 0;
}

int64_t eg_asp_gc_pin_object(void* object) {
    eg_asp_gc_pin_t* pin;
    if (!object) {
        return 0;
    }
    pin = (eg_asp_gc_pin_t*)calloc(1, sizeof(*pin));
    if (!pin) {
        return 0;
    }
    pin->object = object;
    pin->root_handle = eg_gc_register_global_root_range_handle(&pin->object, (unsigned char*)&pin->object + sizeof(pin->object));
    if (!pin->root_handle) {
        if (eg_asp_gc_disabled()) {
            return (int64_t)(intptr_t)pin;
        }
        free(pin);
        return 0;
    }
    return (int64_t)(intptr_t)pin;
}

int64_t eg_asp_gc_pin_field(void* object, int64_t offset) {
    eg_asp_gc_pin_t* pin;
    unsigned char* slot;
    if (!object || offset < 0) {
        return 0;
    }
    pin = (eg_asp_gc_pin_t*)calloc(1, sizeof(*pin));
    if (!pin) {
        return 0;
    }
    pin->object = object;
    /* Field pins are for managed container backing pointers. The caller owns
     * the layout-specific offset; this layer only registers the pointer-sized
     * slot and deliberately does not retain the containing object separately. */
    slot = (unsigned char*)object + offset;
    pin->root_handle = eg_gc_register_global_root_range_handle(slot, slot + sizeof(void*));
    if (!pin->root_handle) {
        if (eg_asp_gc_disabled()) {
            return (int64_t)(intptr_t)pin;
        }
        free(pin);
        return 0;
    }
    return (int64_t)(intptr_t)pin;
}

int64_t eg_asp_gc_pin_compiled_page(void* object) {
    return eg_asp_gc_pin_object(object);
}

int64_t eg_asp_gc_pin_page_nodes(void* object) {
    return eg_asp_gc_pin_object(object);
}

int64_t eg_asp_gc_pin_program_blobs(void* object) {
    return eg_asp_gc_pin_object(object);
}

int64_t eg_asp_gc_pin_programs(void* object) {
    return eg_asp_gc_pin_object(object);
}

int64_t eg_asp_gc_pin_program(void* object) {
    return eg_asp_gc_pin_object(object);
}

int64_t eg_asp_gc_pin_page_nodes_data(void* object) {
    return eg_asp_gc_pin_field(object, (int64_t)sizeof(void*));
}

int64_t eg_asp_gc_pin_programs_data(void* object) {
    return eg_asp_gc_pin_field(object, (int64_t)sizeof(void*));
}

void* eg_asp_gc_pinned_object(int64_t handle) {
    eg_asp_gc_pin_t* pin = (eg_asp_gc_pin_t*)(intptr_t)handle;
    return pin ? pin->object : NULL;
}

void eg_asp_gc_unpin_object(int64_t handle) {
    eg_asp_gc_pin_t* pin = (eg_asp_gc_pin_t*)(intptr_t)handle;
    if (!pin) {
        return;
    }
    if (pin->root_handle) {
        eg_gc_unregister_global_root_range_handle(pin->root_handle);
    }
    free(pin);
}

static int64_t eg_vbs_tm_to_millis(struct tm* value) {
    /* Let libc resolve daylight-saving transitions for the configured local
     * timezone. mktime also normalizes overflowing fields, which DateAdd uses
     * intentionally; strict constructors validate normalized fields later. */
    value->tm_isdst = -1;
    time_t seconds = mktime(value);
    if (seconds == (time_t)-1) {
        return EG_VBS_INVALID_DATE;
    }
    return (int64_t)seconds * 1000;
}

static int eg_vbs_local_tm(int64_t millis, struct tm* out) {
    enum { EG_VBS_TM_CACHE_SIZE = 32, EG_VBS_TZ_KEY_SIZE = 95 };
    typedef struct {
        int64_t millis;
        struct tm value;
        int valid;
    } eg_vbs_tm_cache_entry_t;
    static _Thread_local eg_vbs_tm_cache_entry_t cache[EG_VBS_TM_CACHE_SIZE];
    static _Thread_local unsigned cache_next;
    static _Thread_local char cache_tz[EG_VBS_TZ_KEY_SIZE + 1];
    const char* tz = getenv("TZ");
    const char* tz_key = tz ? tz : "";
    /* Date-heavy pages repeatedly decompose the same values. A thread-local
     * ring avoids global synchronization, while the timezone key prevents
     * stale civil times after an application-level TZ change. */
    if (strncmp(cache_tz, tz_key, EG_VBS_TZ_KEY_SIZE) != 0
        || (strlen(tz_key) <= EG_VBS_TZ_KEY_SIZE
            && cache_tz[strlen(tz_key)] != '\0')) {
        memset(cache, 0, sizeof(cache));
        strncpy(cache_tz, tz_key, EG_VBS_TZ_KEY_SIZE);
        cache_tz[EG_VBS_TZ_KEY_SIZE] = '\0';
        cache_next = 0;
    }
    for (unsigned i = 0; i < EG_VBS_TM_CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].millis == millis) {
            *out = cache[i].value;
            return 1;
        }
    }
    time_t seconds = (time_t)(millis / 1000);
    if (localtime_r(&seconds, out) == NULL) {
        return 0;
    }
    eg_vbs_tm_cache_entry_t* entry = &cache[cache_next++ % EG_VBS_TM_CACHE_SIZE];
    entry->millis = millis;
    entry->value = *out;
    entry->valid = 1;
    return 1;
}

static int eg_vbs_days_in_month(int year, int month) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month == 2 && ((year % 400 == 0) || (year % 4 == 0 && year % 100 != 0))) {
        return 29;
    }
    return days[month - 1];
}

int64_t eg_vbs_now_millis(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int64_t eg_vbs_make_date(int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute, int64_t second) {
    struct tm value;
    memset(&value, 0, sizeof(value));
    value.tm_year = (int)year - 1900;
    value.tm_mon = (int)month - 1;
    value.tm_mday = (int)day;
    value.tm_hour = (int)hour;
    value.tm_min = (int)minute;
    value.tm_sec = (int)second;
    return eg_vbs_tm_to_millis(&value);
}

int64_t eg_vbs_date_add(int64_t millis, int64_t interval, int64_t amount) {
    struct tm value;
    if (!eg_vbs_local_tm(millis, &value)) {
        return EG_VBS_INVALID_DATE;
    }
    /* Interval codes are assigned by the Egret evaluator. Years, quarters and
     * months clamp to the destination month's last day; smaller units rely on
     * mktime normalization so local DST behavior matches other date builtins. */
    if (interval == 0) {
        int original_day = value.tm_mday;
        value.tm_mday = 1;
        value.tm_year += (int)amount;
        value.tm_mday = original_day > eg_vbs_days_in_month(value.tm_year + 1900, value.tm_mon + 1)
            ? eg_vbs_days_in_month(value.tm_year + 1900, value.tm_mon + 1)
            : original_day;
    } else if (interval == 1) {
        int original_day = value.tm_mday;
        value.tm_mday = 1;
        value.tm_mon += (int)(amount * 3);
        if (eg_vbs_tm_to_millis(&value) == EG_VBS_INVALID_DATE) {
            return EG_VBS_INVALID_DATE;
        }
        if (!eg_vbs_local_tm(eg_vbs_tm_to_millis(&value), &value)) {
            return EG_VBS_INVALID_DATE;
        }
        value.tm_mday = original_day > eg_vbs_days_in_month(value.tm_year + 1900, value.tm_mon + 1)
            ? eg_vbs_days_in_month(value.tm_year + 1900, value.tm_mon + 1)
            : original_day;
    } else if (interval == 2) {
        int original_day = value.tm_mday;
        value.tm_mday = 1;
        value.tm_mon += (int)amount;
        if (eg_vbs_tm_to_millis(&value) == EG_VBS_INVALID_DATE) {
            return EG_VBS_INVALID_DATE;
        }
        if (!eg_vbs_local_tm(eg_vbs_tm_to_millis(&value), &value)) {
            return EG_VBS_INVALID_DATE;
        }
        value.tm_mday = original_day > eg_vbs_days_in_month(value.tm_year + 1900, value.tm_mon + 1)
            ? eg_vbs_days_in_month(value.tm_year + 1900, value.tm_mon + 1)
            : original_day;
    } else if (interval == 3 || interval == 4 || interval == 5) {
        value.tm_mday += (int)amount;
    } else if (interval == 6) {
        value.tm_mday += (int)(amount * 7);
    } else if (interval == 7) {
        value.tm_hour += (int)amount;
    } else if (interval == 8) {
        value.tm_min += (int)amount;
    } else if (interval == 9) {
        value.tm_sec += (int)amount;
    }
    return eg_vbs_tm_to_millis(&value);
}

int64_t eg_vbs_date_part(int64_t millis, int64_t part) {
    struct tm value;
    if (!eg_vbs_local_tm(millis, &value)) {
        return 0;
    }
    if (part == 0) {
        return value.tm_year + 1900;
    }
    if (part == 1) {
        return value.tm_mon + 1;
    }
    if (part == 2) {
        return value.tm_mday;
    }
    if (part == 3) {
        return value.tm_hour;
    }
    if (part == 4) {
        return value.tm_min;
    }
    if (part == 5) {
        return value.tm_sec;
    }
    if (part == 6) {
        return value.tm_wday + 1;
    }
    if (part == 7) {
        return value.tm_yday + 1;
    }
    if (part == 8) {
        return value.tm_yday / 7 + 1;
    }
    if (part == 9) {
        return value.tm_mon / 3 + 1;
    }
    return 0;
}

int64_t eg_vbs_parse_date(eg_string_t* text) {
    enum { EG_VBS_DATE_CACHE_SIZE = 32, EG_VBS_DATE_CACHE_KEY = 47 };
    typedef struct {
        char key[EG_VBS_DATE_CACHE_KEY + 1];
        size_t len;
        int64_t millis;
        int valid;
    } eg_vbs_date_cache_entry_t;
    static _Thread_local eg_vbs_date_cache_entry_t cache[EG_VBS_DATE_CACHE_SIZE];
    static _Thread_local unsigned cache_next;
    const char* source = eg_string_cstr(text);
    size_t source_len = text && text->len >= 0 ? (size_t)text->len : 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int matched = 0;
    if (!source) {
        return EG_VBS_INVALID_DATE;
    }
    /* Cache only bounded keys so malformed or attacker-controlled long input
     * cannot enlarge thread-local state. Failed parses are cheap and are not
     * cached, keeping the cache useful for repeated application constants. */
    if (source_len <= EG_VBS_DATE_CACHE_KEY) {
        for (unsigned i = 0; i < EG_VBS_DATE_CACHE_SIZE; i++) {
            if (cache[i].valid && cache[i].len == source_len
                && memcmp(cache[i].key, source, source_len) == 0) {
                return cache[i].millis;
            }
        }
    }
    int short_year = 0;
    int short_month = 0;
    int consumed = 0;
    char separator = 0;
    // An unambiguous year/month date denotes the first day of that month.
    // Require the entire input so a truncated day or trailing junk cannot
    // accidentally become a valid month. Keep complete date/time parsing below.
    int short_fields = sscanf(source, " %4d %c %2d %n", &short_year, &separator, &short_month, &consumed);
    if (short_fields == 3 && (separator == '-' || separator == '/')
        && (size_t)consumed == source_len && short_year >= 100 && short_year <= 9999
        && short_month >= 1 && short_month <= 12) {
        year = short_year;
        month = short_month;
        day = 1;
        matched = 3;
    } else {
        matched = sscanf(source, " %d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);
        if (matched < 3) {
            matched = sscanf(source, " %d/%d/%d %d:%d:%d", &month, &day, &year, &hour, &minute, &second);
        }
    }
    if (matched < 3) {
        return EG_VBS_INVALID_DATE;
    }
    if (year >= 0 && year < 30) {
        year += 2000;
    } else if (year >= 30 && year < 100) {
        year += 1900;
    }
    int64_t millis = eg_vbs_make_date(year, month, day, hour, minute, second);
    struct tm checked;
    if (millis == EG_VBS_INVALID_DATE || !eg_vbs_local_tm(millis, &checked)
        || checked.tm_year + 1900 != year || checked.tm_mon + 1 != month || checked.tm_mday != day
        || checked.tm_hour != hour || checked.tm_min != minute || checked.tm_sec != second) {
        return EG_VBS_INVALID_DATE;
    }
    if (source_len <= EG_VBS_DATE_CACHE_KEY) {
        eg_vbs_date_cache_entry_t* entry = &cache[cache_next++ % EG_VBS_DATE_CACHE_SIZE];
        memcpy(entry->key, source, source_len);
        entry->key[source_len] = '\0';
        entry->len = source_len;
        entry->millis = millis;
        entry->valid = 1;
    }
    return millis;
}

eg_string_t* eg_vbs_format_date(int64_t millis, int64_t mode) {
    struct tm value;
    char output[64];
    int length = 0;
    if (mode == 3) {
        time_t seconds = (time_t)(millis / 1000);
        if (gmtime_r(&seconds, &value) == NULL) {
            return eg_string_from_bytes("", 0);
        }
    } else if (!eg_vbs_local_tm(millis, &value)) {
        return eg_string_from_bytes("", 0);
    }
    if (mode == 3) {
        static const char* weekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        length = snprintf(output, sizeof(output), "%s, %02d %s %04d %02d:%02d:%02d GMT", weekdays[value.tm_wday], value.tm_mday, months[value.tm_mon], value.tm_year + 1900, value.tm_hour, value.tm_min, value.tm_sec);
    } else if (mode == 1) {
        length = snprintf(output, sizeof(output), "%d/%d/%d", value.tm_mon + 1, value.tm_mday, value.tm_year + 1900);
    } else if (mode == 2) {
        length = snprintf(output, sizeof(output), "%02d:%02d:%02d", value.tm_hour, value.tm_min, value.tm_sec);
    } else if (value.tm_hour == 0 && value.tm_min == 0 && value.tm_sec == 0) {
        length = snprintf(output, sizeof(output), "%d/%d/%d", value.tm_mon + 1, value.tm_mday, value.tm_year + 1900);
    } else {
        length = snprintf(output, sizeof(output), "%d/%d/%d %02d:%02d:%02d", value.tm_mon + 1, value.tm_mday, value.tm_year + 1900, value.tm_hour, value.tm_min, value.tm_sec);
    }
    if (length < 0) {
        return eg_string_from_bytes("", 0);
    }
    return eg_string_from_bytes(output, length);
}

eg_string_t* eg_vbs_chr_w(int64_t codepoint) {
    unsigned char output[4];
    int64_t length = 0;
    /* VBScript ChrW accepts signed 16-bit values; adding 65536 preserves their
     * code-unit interpretation. Positive supplementary values are encoded as
     * UTF-8 as an engine extension, while out-of-range values yield empty. */
    if (codepoint < 0) {
        codepoint += 65536;
    }
    if (codepoint <= 0x7f) {
        output[0] = (unsigned char)codepoint;
        length = 1;
    } else if (codepoint <= 0x7ff) {
        output[0] = (unsigned char)(0xc0 | (codepoint >> 6));
        output[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
        length = 2;
    } else if (codepoint <= 0xffff) {
        output[0] = (unsigned char)(0xe0 | (codepoint >> 12));
        output[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        output[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
        length = 3;
    } else if (codepoint <= 0x10ffff) {
        output[0] = (unsigned char)(0xf0 | (codepoint >> 18));
        output[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
        output[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        output[3] = (unsigned char)(0x80 | (codepoint & 0x3f));
        length = 4;
    }
    return eg_string_from_bytes(output, length);
}

int64_t eg_vbs_sgn(double value) {
    return value < 0.0 ? -1 : (value > 0.0 ? 1 : 0);
}

double eg_vbs_atn(double value) {
    return atan(value);
}

double eg_vbs_sin(double value) {
    return sin(value);
}

double eg_vbs_cos(double value) {
    return cos(value);
}

double eg_vbs_tan(double value) {
    return tan(value);
}

double eg_vbs_exp(double value) {
    return exp(value);
}

double eg_vbs_log(double value) {
    return log(value);
}
