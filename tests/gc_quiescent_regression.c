// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

/* Exercise the real runtime against a protected conservative candidate and a
 * disposal safepoint inside request quiescence. Build with EG_RUNTIME_SOURCE
 * naming the runtime unity source; no replacement GC implementation is used. */
#include <assert.h>
#include <sys/mman.h>
#include <string.h>
#ifndef EG_RUNTIME_SOURCE
#define EG_RUNTIME_SOURCE "../build/safe-runtime/runtime/core/eg_rt.c"
#endif
#include EG_RUNTIME_SOURCE

static eg_string_t* request_owned_string;

/* Runtime statistics are unrelated to these regressions. Fixed stubs satisfy
 * the unity runtime ABI without introducing mutable counters or allocations. */
int64_t __std_string_alloc_count(void) { return 0; }
int64_t __std_string_alloc_bytes(void) { return 0; }
int64_t __std_string_concat_count(void) { return 0; }
int64_t __std_string_substr_count(void) { return 0; }
int64_t __std_string_cstr_copy_count(void) { return 0; }
int64_t __std_string_alloc_bucket_count(int64_t bucket) {
    (void)bucket;
    return 0;
}

int64_t eg_asp_request_string_is_owned(void* string_object) {
    return string_object == request_owned_string;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    eg_gc_set_enabled(1);
    /* Each mode isolates one historical failure so a crashing guard-page case
     * cannot hide the state-transition assertions in later scenarios. */
    if (strcmp(argv[1], "guard-page") == 0) {
        size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
        void* mapping = mmap(NULL, page_size * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(mapping != MAP_FAILED);
        /* Model the crash: the aligned register value lies within the heap
         * envelope, but the preceding object header is on a protected page. */
        eg_gc_heap_min = (uintptr_t)mapping;
        eg_gc_heap_max = (uintptr_t)mapping + page_size * 3;
        eg_gc_enabled_fast_latch = 0;
        assert(eg_gc_obj_from_conservative_payload((char*)mapping + page_size) == NULL);
        eg_gc_enabled_fast_latch = 1;
        assert(eg_gc_obj_from_conservative_payload((char*)mapping + page_size) == NULL);
        assert(munmap(mapping, page_size * 3) == 0);
    } else if (strcmp(argv[1], "dispose") == 0) {
        eg_gc_threshold = UINT64_MAX;
        eg_gc_request_quiescent_enter();
        eg_gc_dispose_events = 20;
        eg_gc_dispose_freed_bytes = 123;
        eg_gc_dispose_safepoint();
        /* Deferred work must remain queued until allocation publication and
         * barrier restoration at leave, rather than being silently lost. */
        assert(eg_gc_dispose_events == 20);
        assert(eg_gc_dispose_freed_bytes == 123);
        assert(eg_gc_enabled_fast_latch == 0);
        eg_gc_request_quiescent_leave();
        assert(eg_gc_enabled_fast_latch == 1);
        eg_gc_dispose_safepoint();
        assert(eg_gc_dispose_events == 0);
        assert(eg_gc_dispose_freed_bytes == 0);
    } else if (strcmp(argv[1], "pending-object") == 0) {
        eg_gc_threshold = UINT64_MAX;
        eg_gc_request_quiescent_enter();
        void* payload = eg_gc_alloc(32, 0, NULL);
        assert(payload != NULL);
        assert(eg_gc_obj_from_payload(payload) != NULL);
        assert(eg_gc_obj_from_conservative_payload(payload) != NULL);
        eg_gc_request_quiescent_leave();
        assert(eg_gc_obj_from_payload(payload) != NULL);
        assert(eg_gc_obj_from_conservative_payload(payload) != NULL);
    } else if (strcmp(argv[1], "immortal") == 0) {
        eg_gc_threshold = UINT64_MAX;
        uint32_t child_offset = 0;
        void* child = eg_gc_alloc(32, 0, NULL);
        void** parent = eg_gc_alloc(sizeof(void*), 1, &child_offset);
        assert(child != NULL && parent != NULL);
        *parent = child;
        int64_t promoted = eg_gc_promote_immortal(parent);
        assert(promoted > 0);
        assert(eg_gc_obj_from_payload(parent)->immortal == 1);
        assert(eg_gc_obj_from_payload(child)->immortal == 1);
        uint64_t effective_before = eg_gc_effective_heap_bytes();
        eg_gc_collect_full();
        assert(eg_gc_obj_from_payload(parent) != NULL);
        assert(eg_gc_obj_from_payload(child) != NULL);
        assert(eg_gc_effective_heap_bytes() <= effective_before);
    } else if (strcmp(argv[1], "request-string-envelope") == 0) {
        size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
        void* mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(mapping != MAP_FAILED);
        request_owned_string = (eg_string_t*)mapping;
        request_owned_string->len = 5;
        request_owned_string->data = (char*)mapping + sizeof(*request_owned_string);
        memcpy((char*)request_owned_string->data, "arena", 6);
        eg_gc_heap_min = (uintptr_t)mapping - page_size;
        eg_gc_heap_max = (uintptr_t)mapping + page_size * 2;
        assert(eg_gc_obj_from_payload(request_owned_string) == NULL);
        assert(eg_string_len(request_owned_string) == 5);
        assert(strcmp(eg_string_cstr(request_owned_string), "arena") == 0);
        request_owned_string = NULL;
        assert(munmap(mapping, page_size) == 0);
    } else {
        return 2;
    }
    return 0;
}
