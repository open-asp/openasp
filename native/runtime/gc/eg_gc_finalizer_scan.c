// Copyright (c) 2026 OpenASP.dev
// SPDX-License-Identifier: MIT

/* Included inside the SDK runtime unity translation unit by the local overlay.
 * Snapshot only registered graph finalizers. Scanning the complete object heap
 * in 4096-object batches caused a full remark for every batch, even when the
 * heap contained only a few dozen graph finalizers.
 */
static int eg_gc_preserve_finalizable_white_some_locked(void) {
    static eg_gc_obj_t** candidates = NULL;
    static size_t candidate_capacity = 0;
    static size_t candidate_count = 0;
    static size_t candidate_position = 0;
    uint64_t max_us = eg_gc_incremental_finalizable_preserve_budget_us();
    uint64_t preserve_start_us = max_us ? eg_gc_now_us() : 0;
    uint32_t scan_budget = eg_gc_incremental_finalizable_preserve_scan_budget();
    int budget_exceeded = 0;

    /* The collector owns eg_gc_collecting and eg_gc_mu. No sweep or full
     * collection may reclaim these headers while this incremental mark is
     * active. Metadata changes may unlink entries, so a raw list cursor must
     * not be retained across mutator progress. Reset/abort clears active and
     * causes the next invocation to rebuild the snapshot before using it.
     */
    if (!eg_gc_incremental_finalizable_preserve_active) {
        candidate_count = 0;
        candidate_position = 0;
        eg_gc_finalizer_meta_lock();
        for (eg_gc_obj_t* cur = eg_gc_graph_finalizer_head; cur; cur = cur->graph_finalizer_next) {
            if (candidate_count == candidate_capacity) {
                size_t capacity = candidate_capacity ? candidate_capacity * 2u : 64u;
                if (capacity < candidate_capacity || capacity > SIZE_MAX / sizeof(*candidates)) {
                    eg_gc_finalizer_meta_unlock();
                    return 0;
                }
                eg_gc_obj_t** items = (eg_gc_obj_t**)realloc(candidates, capacity * sizeof(*items));
                if (!items) {
                    eg_gc_finalizer_meta_unlock();
                    return 0;
                }
                candidates = items;
                candidate_capacity = capacity;
            }
            candidates[candidate_count++] = cur;
        }
        eg_gc_finalizer_meta_unlock();
        eg_gc_incremental_finalizable_preserve_active = 1;
    }

    while (candidate_position < candidate_count && scan_budget > 0) {
        if (max_us && preserve_start_us && eg_gc_elapsed_us_since(preserve_start_us) >= max_us) {
            budget_exceeded = 1;
            break;
        }
        eg_gc_obj_t* cur = candidates[candidate_position++];
        scan_budget--;
        if (!eg_gc_obj_is_white(cur)) continue;
        if (!eg_gc_obj_needs_graph_preserve_finalizer(cur)) continue;
        cur->dead = 0;
        if (!eg_gc_incremental_finalizable_preserve_push(cur)) {
            /* Retrying must not lose a white finalizable candidate. */
            candidate_position--;
            budget_exceeded = 1;
            break;
        }
        eg_gc_mark_payload((void*)((unsigned char*)cur + sizeof(eg_gc_obj_t)));
        int drain_complete = eg_gc_drain_gray_until_budget(preserve_start_us, max_us);
        if (!drain_complete) {
            budget_exceeded = 1;
            break;
        }
    }

    eg_gc_incremental_finalizable_preserve_cursor = candidate_position < candidate_count ? candidates[candidate_position] : NULL;
    if (eg_gc_incremental_finalizable_preserve_cursor && scan_budget == 0) budget_exceeded = 1;
    if (eg_gc_incremental_finalizable_preserve_cursor) return 0;
    /* The last candidate may have exhausted the graph-drain budget too.
     * Keep marking enabled until its descendants have all been traced.
     */
    if (!eg_gc_gray_empty()) return 0;
    eg_gc_incremental_finalizable_preserve_enqueue_locked();
    eg_gc_incremental_finalizable_preserve_reset();
    return 1;
}
