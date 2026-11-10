/* Renderer-owned observation of Windows guest-memory write tracking.
 * The allocation must use MEM_WRITE_WATCH. Query+reset is one OS operation;
 * separate ResetWriteWatch calls could lose concurrent guest writes.
 * Page epochs preserve invalidation when overlapping texture ranges consume
 * one another's dirty notifications. Zero always requires byte comparison. */
#ifndef PS3RECOMP_RSX_TEXTURE_WRITE_WATCH_H
#define PS3RECOMP_RSX_TEXTURE_WRITE_WATCH_H

#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct RsxTextureWriteWatch {
    const unsigned char* base;
    uint64_t span, epoch;
    uint64_t* page_epochs;
    void** dirty_pages;
    size_t dirty_capacity;
    DWORD page_size;
    int state; /* 0 uninitialized, 1 active, -1 permanently disabled */
    UINT error;
} RsxTextureWriteWatch;

/* Owned by one texture source. Clear/destroy every hint together with its
 * watch context; epochs are only meaningful within that watch's lifetime. */
typedef struct RsxTextureWriteStampHint {
    uint64_t first, last, epoch, value;
} RsxTextureWriteStampHint;

static void rsx_texture_write_watch_destroy(RsxTextureWriteWatch* watch)
{
    free(watch->page_epochs);
    free(watch->dirty_pages);
    memset(watch, 0, sizeof(*watch));
}

static uint64_t rsx_texture_write_watch_fail(RsxTextureWriteWatch* watch, UINT error)
{
    watch->state = -1;
    watch->error = error;
    return 0;
}

/* Only the renderer calls this helper; guest writers need no added hooks.
 * A successful nonzero stamp describes all writes observed before this call.
 * Writes during/after it remain dirty and invalidate the next observation.
 * Destroy this context with its texture owners before freeing/replacing VM. */
static uint64_t rsx_texture_write_watch_stamp_cached(RsxTextureWriteWatch* watch,
    const unsigned char* base, uint64_t span, uint32_t off, uint32_t bytes,
    RsxTextureWriteStampHint* hint)
{
    if (watch->state < 0 || !base || !span || !bytes ||
        (uint64_t)off + bytes > span || span > SIZE_MAX)
        return 0;
    if (!watch->state) {
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        if (!info.dwPageSize || (info.dwPageSize & (info.dwPageSize - 1u)) ||
            span % info.dwPageSize || span / info.dwPageSize > SIZE_MAX / sizeof(uint64_t))
            return rsx_texture_write_watch_fail(watch, ERROR_INVALID_PARAMETER);
        watch->page_epochs = (uint64_t*)calloc((size_t)(span / info.dwPageSize), sizeof(uint64_t));
        if (!watch->page_epochs)
            return rsx_texture_write_watch_fail(watch, ERROR_NOT_ENOUGH_MEMORY);
        watch->base = base;
        watch->span = span;
        watch->page_size = info.dwPageSize;
        watch->state = 1;
    }
    if (watch->base != base || watch->span != span)
        return rsx_texture_write_watch_fail(watch, ERROR_INVALID_ADDRESS);
    const uint64_t first = (uint64_t)off / watch->page_size;
    const uint64_t last = ((uint64_t)off + bytes - 1u) / watch->page_size;
    const size_t page_count = (size_t)(last - first + 1u);
    if (page_count > watch->dirty_capacity) {
        void** expanded = (void**)realloc(watch->dirty_pages, page_count * sizeof(void*));
        if (!expanded)
            return rsx_texture_write_watch_fail(watch, ERROR_NOT_ENOUGH_MEMORY);
        watch->dirty_pages = expanded;
        watch->dirty_capacity = page_count;
    }
    ULONG_PTR count = page_count;
    DWORD granularity = 0;
    UINT error = GetWriteWatch(WRITE_WATCH_FLAG_RESET,
        (void*)(base + first * watch->page_size), page_count * watch->page_size,
        watch->dirty_pages, &count, &granularity);
    if (error || granularity != watch->page_size || count > page_count ||
        watch->epoch >= UINT64_MAX - 1u)
        return rsx_texture_write_watch_fail(watch, error ? error : ERROR_INVALID_DATA);
    if (count) {
        ++watch->epoch;
        for (ULONG_PTR i = 0; i < count; ++i) {
            const uintptr_t address = (uintptr_t)watch->dirty_pages[i];
            const uintptr_t allocation = (uintptr_t)base;
            if (address < allocation || address - allocation >= span ||
                (address - allocation) % watch->page_size)
                return rsx_texture_write_watch_fail(watch, ERROR_INVALID_DATA);
            const uint64_t page = (address - allocation) / watch->page_size;
            if (page < first || page > last)
                return rsx_texture_write_watch_fail(watch, ERROR_INVALID_DATA);
            watch->page_epochs[page] = watch->epoch;
        }
    } else if (hint && hint->value && hint->first == first &&
               hint->last == last && hint->epoch == watch->epoch) {
        /* The OS query still ran. No page epoch changed since this exact
         * range's maximum was captured, including overlapping observers. */
        return hint->value;
    }
    /* Any validated dirty page now has the global maximum epoch. Older page
     * epochs cannot exceed it, so dirty ranges need no second page walk. */
    uint64_t latest = count ? watch->epoch : 0;
    if (!count)
        for (uint64_t page = first; page <= last; ++page)
            if (watch->page_epochs[page] > latest) latest = watch->page_epochs[page];
    const uint64_t value = latest + 1u;
    if (hint) {
        hint->first = first;
        hint->last = last;
        hint->epoch = watch->epoch;
        hint->value = value;
    }
    return value; /* zero is reserved for unavailable/invalid tracking */
}

static uint64_t rsx_texture_write_watch_stamp(RsxTextureWriteWatch* watch,
    const unsigned char* base, uint64_t span, uint32_t off, uint32_t bytes)
{
    return rsx_texture_write_watch_stamp_cached(watch, base, span, off, bytes, NULL);
}

/* ---- Batched observation (renderer opt-in TJ_WATCH_BATCH=1) ----------------
 * The functions above are unchanged; nothing below runs unless the renderer
 * chooses it. The caller owns the pass policy: when to query, which pages a
 * query covered and when a later write makes that coverage stale. */

/* One query+reset over watch-relative pages [first, last] of an ACTIVE watch.
 * Every reported page gets a new epoch exactly as the per-call query records
 * it, so stamps from either path share one epoch space. Returns 0 on success
 * (*dirty = pages reported). An OS error, or an allocation failure before the
 * call, returns that error with the watch still active: the OS failed before
 * reporting/resetting anything, so per-call queries may still be used. A
 * result that cannot be applied after a successful reset lost notifications;
 * the watch is then permanently disabled, as the per-call path does. */
static UINT rsx_texture_write_watch_query_pages(RsxTextureWriteWatch* watch,
    uint64_t first, uint64_t last, uint64_t* dirty)
{
    if (dirty) *dirty = 0;
    if (watch->state != 1 || !watch->page_size || first > last ||
        last >= watch->span / watch->page_size ||
        last - first >= SIZE_MAX / sizeof(void*))
        return ERROR_INVALID_PARAMETER;
    const size_t page_count = (size_t)(last - first + 1u);
    if (page_count > watch->dirty_capacity) {
        void** expanded = (void**)realloc(watch->dirty_pages, page_count * sizeof(void*));
        if (!expanded) return ERROR_NOT_ENOUGH_MEMORY;
        watch->dirty_pages = expanded;
        watch->dirty_capacity = page_count;
    }
    ULONG_PTR count = page_count;
    DWORD granularity = 0;
    UINT error = GetWriteWatch(WRITE_WATCH_FLAG_RESET,
        (void*)(watch->base + first * watch->page_size), page_count * watch->page_size,
        watch->dirty_pages, &count, &granularity);
    if (error) return error;
    if (granularity != watch->page_size || count > page_count ||
        watch->epoch >= UINT64_MAX - 1u) {
        rsx_texture_write_watch_fail(watch, ERROR_INVALID_DATA);
        return ERROR_INVALID_DATA;
    }
    if (count) {
        ++watch->epoch;
        const uintptr_t allocation = (uintptr_t)watch->base;
        for (ULONG_PTR i = 0; i < count; ++i) {
            const uintptr_t address = (uintptr_t)watch->dirty_pages[i];
            if (address < allocation || address - allocation >= watch->span ||
                (address - allocation) % watch->page_size) {
                rsx_texture_write_watch_fail(watch, ERROR_INVALID_DATA);
                return ERROR_INVALID_DATA;
            }
            const uint64_t page = (address - allocation) / watch->page_size;
            if (page < first || page > last) {
                rsx_texture_write_watch_fail(watch, ERROR_INVALID_DATA);
                return ERROR_INVALID_DATA;
            }
            watch->page_epochs[page] = watch->epoch;
        }
    }
    if (dirty) *dirty = count;
    return 0;
}

/* Stamp of [off, off + bytes) from page epochs alone: no OS call. It equals
 * what the per-call query returns when that query finds no dirty page in the
 * range (same value and hint contract), so it describes only writes observed
 * by earlier queries. The caller must know that every page of the range was
 * queried after its last ordering point; a write after that query is not in
 * this stamp. Zero = unavailable. */
static uint64_t rsx_texture_write_watch_stamp_epochs(RsxTextureWriteWatch* watch,
    uint32_t off, uint32_t bytes, RsxTextureWriteStampHint* hint)
{
    if (watch->state != 1 || !watch->page_size || !bytes ||
        (uint64_t)off + bytes > watch->span)
        return 0;
    const uint64_t first = (uint64_t)off / watch->page_size;
    const uint64_t last = ((uint64_t)off + bytes - 1u) / watch->page_size;
    if (hint && hint->value && hint->first == first &&
        hint->last == last && hint->epoch == watch->epoch)
        return hint->value;
    uint64_t latest = 0;
    for (uint64_t page = first; page <= last; ++page)
        if (watch->page_epochs[page] > latest) latest = watch->page_epochs[page];
    const uint64_t value = latest + 1u;
    if (hint) {
        hint->first = first;
        hint->last = last;
        hint->epoch = watch->epoch;
        hint->value = value;
    }
    return value;
}

#endif
