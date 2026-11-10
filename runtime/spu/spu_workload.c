/* spu_workload.c — SPU workload / task dispatch registry (see spu_workload.h).
 *
 * Maps a registered SPU image (by FNV-1a-64 content fingerprint) to its
 * pre-lifted native entry, loads the image into a 256 KB local store, and runs
 * it with the SPURS task ABI. cellSpurs's AddWorkload/CreateTask call
 * spu_workload_dispatch(); the registry is populated by the title's lifted set.
 */
#include "spu_workload.h"
#include "spu_lifted_job.h"   /* spu_run_lifted_job */
#include "spu_reservation.h"
#include "../memory/guest_world_rendezvous.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Big-endian guest accessor from ppu_loader.cpp. */
extern uint64_t vm_read64(uint32_t ea);
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
extern void tj_spu_pc_time_job_begin(int image_id);
extern void tj_spu_pc_time_job_complete(int image_id);
extern void tj_spu_pc_hist_job_complete(int image_id);
#else
#include <pthread.h>
#include <stdatomic.h>
#endif

/* Set by the MFC DMA engine (spu_dma.h) when the cri task (image 22) issues a
 * real video-payload GET (>256B from a non-context EA) = it actually decoded,
 * as opposed to the 64-byte context handshake DMAs. The dispatcher's
 * YDKJ_CRI_RESUME poll-loop watches this to know real work arrived. */
int g_cri_video_dma = 0;

/* Tokyo Jungle late-Survival diagnostic.  cellSpurs arms this only after the
 * failing completion waiter is registered.  Keep the workload trace separate
 * from PS3_SPU_JOB_TRACE so startup traffic stays out of the capture. */
extern volatile uint32_t g_spu_event_flag_watch_ea;

static int tj_late_job_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TJ_LATE_JOB_TRACE") ? 1 : 0;
    return enabled && g_spu_event_flag_watch_ea != 0;
}

#ifdef _WIN32
static volatile LONG s_tj_late_job_trace_sequence = 0;

static unsigned long tj_late_job_trace_next_sequence(void)
{
    return (unsigned long)InterlockedIncrement(&s_tj_late_job_trace_sequence);
}

static unsigned long tj_late_job_trace_thread_id(void)
{
    return (unsigned long)GetCurrentThreadId();
}
#else
static unsigned long s_tj_late_job_trace_sequence;

static unsigned long tj_late_job_trace_next_sequence(void)
{
    return ++s_tj_late_job_trace_sequence;
}

static unsigned long tj_late_job_trace_thread_id(void)
{
    return 0;
}
#endif

static int spu_job_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("PS3_SPU_JOB_TRACE") ? 1 : 0;
    return enabled;
}

/* Optional one-shot proof for a single SPURS job image. Keep it independent
 * from PS3_SPU_JOB_TRACE so normal per-job tracing remains off. */
#ifdef _WIN32
static volatile LONG s_target_fp_init = 0;
static volatile LONG s_target_fp_enabled = 0;
static uint64_t s_target_fp;
static volatile LONG s_target_hit_logged = 0;

static int spu_job_trace_target_matches(uint64_t fp)
{
    if (s_target_fp_init == 2)        /* initialized: plain reads per job */
        return s_target_fp_enabled && s_target_fp == fp;
    LONG state = InterlockedCompareExchange(&s_target_fp_init, 1, 0);
    if (state == 0) {
        const char* text = getenv("PS3_SPU_JOB_TRACE_FP");
        char* end = NULL;
        if (text && *text) {
            uint64_t parsed = (uint64_t)strtoull(text, &end, 0);
            if (end && *end == '\0') {
                s_target_fp = parsed;
                InterlockedExchange(&s_target_fp_enabled, 1);
            }
        }
        InterlockedExchange(&s_target_fp_init, 2);
    } else {
        while (InterlockedCompareExchange(&s_target_fp_init, 2, 2) != 2)
            Sleep(0);
    }
    return InterlockedCompareExchange(&s_target_fp_enabled, 0, 0) && s_target_fp == fp;
}

static int spu_job_trace_target_take_hit(void)
{
    return InterlockedCompareExchange(&s_target_hit_logged, 1, 0) == 0;
}

#else
static atomic_int s_target_fp_init = ATOMIC_VAR_INIT(0);
static atomic_int s_target_fp_enabled = ATOMIC_VAR_INIT(0);
static uint64_t s_target_fp;
static atomic_int s_target_hit_logged = ATOMIC_VAR_INIT(0);

static int spu_job_trace_target_matches(uint64_t fp)
{
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&s_target_fp_init, &expected, 1,
                                                memory_order_acq_rel, memory_order_acquire)) {
        const char* text = getenv("PS3_SPU_JOB_TRACE_FP");
        char* end = NULL;
        if (text && *text) {
            uint64_t parsed = (uint64_t)strtoull(text, &end, 0);
            if (end && *end == '\0') {
                s_target_fp = parsed;
                atomic_store_explicit(&s_target_fp_enabled, 1, memory_order_release);
            }
        }
        atomic_store_explicit(&s_target_fp_init, 2, memory_order_release);
    } else {
        while (atomic_load_explicit(&s_target_fp_init, memory_order_acquire) != 2) {
        }
    }
    return atomic_load_explicit(&s_target_fp_enabled, memory_order_acquire) && s_target_fp == fp;
}

static int spu_job_trace_target_take_hit(void)
{
    return atomic_exchange_explicit(&s_target_hit_logged, 1, memory_order_acq_rel) == 0;
}

#endif

/* ---- fingerprint ------------------------------------------------------- */

uint64_t spu_workload_fingerprint(const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ULL;          /* FNV offset basis */
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;                     /* FNV prime */
    }
    return h;
}

/* Capture a runtime-only SPURS job image for lifting. Set SPU_DUMP_MISS to an
 * existing directory; the content fingerprint and exact byte count make the
 * filename deterministic. Existing captures are preserved. */
static void spu_dump_miss(const uint8_t* image, uint32_t image_size, uint64_t fp)
{
    const char* dir = getenv("SPU_DUMP_MISS");
    if (!dir || !*dir || !image || !image_size) return;

    char path[1024];
    int n = snprintf(path, sizeof path, "%s%cspujob_%016llX_%u.bin", dir,
#ifdef _WIN32
                     '\\',
#else
                     '/',
#endif
                     (unsigned long long)fp, image_size);
    if (n <= 0 || (size_t)n >= sizeof path) {
        fprintf(stderr, "[spu_workload] SPU_DUMP_MISS path too long\n");
        return;
    }

    FILE* existing = fopen(path, "rb");
    if (existing) {
        fclose(existing);
        fprintf(stderr, "[spu_workload] miss capture already exists: %s\n", path);
        return;
    }

    FILE* out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "[spu_workload] failed to create miss capture: %s\n", path);
        return;
    }
    size_t written = fwrite(image, 1, image_size, out);
    fclose(out);
    if (written == image_size)
        fprintf(stderr, "[spu_workload] captured missing image: %s\n", path);
    else
        fprintf(stderr, "[spu_workload] short miss capture: %s (%zu/%u)\n",
                path, written, image_size);
}

/* ---- registry ---------------------------------------------------------- */

#ifndef SPU_WORKLOAD_MAX
#define SPU_WORKLOAD_MAX 256
#endif

typedef struct {
    uint64_t            fp;
    spu_lifted_entry_fn fn;
    int                 image_id;
    const char*         name;
} spu_workload_entry;

static spu_workload_entry s_registry[SPU_WORKLOAD_MAX];
static unsigned           s_registry_count = 0;

/* SPURS job chains dispatch the same immutable raw job image many times.  A
 * scalar FNV pass over that image on every job is measurable, but pointer-only
 * memoization is incorrect: guest memory may reuse the same address for new
 * bytes.  Keep a small, fixed-memory cache and require an exact byte comparison
 * against a private snapshot before reusing the fingerprint and registry
 * result.  Oversized images take the canonical fingerprint/registry path.
 *
 * The lock also makes registration + cache invalidation atomic with lookup.
 * Windows uses shared readers so independent dispatch threads do not serialize
 * their memcmp fast path; the portable pthread fallback uses one mutex. */
#define SPU_JOB_LOOKUP_CACHE_SLOTS 16   /* 9 job images are live per frame */
#define SPU_JOB_LOOKUP_CACHE_MAX_IMAGE SPU_LS_SIZE

typedef struct spu_job_lookup_cache_entry {
    const uint8_t*      image;
    uint32_t            image_size;
    uint64_t            fp;
    spu_lifted_entry_fn fn;
    int                 image_id;
    int                 valid;
    uint8_t             snapshot[SPU_JOB_LOOKUP_CACHE_MAX_IMAGE];
} spu_job_lookup_cache_entry;

static spu_job_lookup_cache_entry
    s_spu_job_lookup_cache[SPU_JOB_LOOKUP_CACHE_SLOTS];
static unsigned s_spu_job_lookup_cache_next;

#ifdef _WIN32
static SRWLOCK s_spu_job_lookup_lock = SRWLOCK_INIT;
static void spu_job_lookup_lock_shared(void)
{ AcquireSRWLockShared(&s_spu_job_lookup_lock); }
static void spu_job_lookup_unlock_shared(void)
{ ReleaseSRWLockShared(&s_spu_job_lookup_lock); }
static void spu_job_lookup_lock_exclusive(void)
{ AcquireSRWLockExclusive(&s_spu_job_lookup_lock); }
static void spu_job_lookup_unlock_exclusive(void)
{ ReleaseSRWLockExclusive(&s_spu_job_lookup_lock); }
#else
static pthread_mutex_t s_spu_job_lookup_lock = PTHREAD_MUTEX_INITIALIZER;
static void spu_job_lookup_lock_shared(void)
{ pthread_mutex_lock(&s_spu_job_lookup_lock); }
static void spu_job_lookup_unlock_shared(void)
{ pthread_mutex_unlock(&s_spu_job_lookup_lock); }
static void spu_job_lookup_lock_exclusive(void)
{ pthread_mutex_lock(&s_spu_job_lookup_lock); }
static void spu_job_lookup_unlock_exclusive(void)
{ pthread_mutex_unlock(&s_spu_job_lookup_lock); }
#endif

static void spu_job_lookup_cache_invalidate_locked(void)
{
    for (unsigned i = 0; i < SPU_JOB_LOOKUP_CACHE_SLOTS; ++i)
        s_spu_job_lookup_cache[i].valid = 0;
    s_spu_job_lookup_cache_next = 0;
}

static void spu_registry_lookup_locked(uint64_t fp,
                                       spu_lifted_entry_fn* fn_out,
                                       int* image_id_out)
{
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; ++i) {
        if (s_registry[i].fp == fp) {
            fn = s_registry[i].fn;
            image_id = s_registry[i].image_id;
            break;
        }
    }
    *fn_out = fn;
    *image_id_out = image_id;
}

static void spu_job_lookup(const uint8_t* image, uint32_t image_size,
                           uint64_t* fp_out, spu_lifted_entry_fn* fn_out,
                           int* image_id_out)
{
    if (image_size <= SPU_JOB_LOOKUP_CACHE_MAX_IMAGE) {
        spu_job_lookup_lock_shared();
        for (unsigned i = 0; i < SPU_JOB_LOOKUP_CACHE_SLOTS; ++i) {
            const spu_job_lookup_cache_entry* entry =
                &s_spu_job_lookup_cache[i];
            if (entry->valid && entry->image == image &&
                entry->image_size == image_size &&
                memcmp(entry->snapshot, image, image_size) == 0) {
                *fp_out = entry->fp;
                *fn_out = entry->fn;
                *image_id_out = entry->image_id;
                spu_job_lookup_unlock_shared();
                return;
            }
        }
        spu_job_lookup_unlock_shared();

        /* Recheck after upgrading the lock: another dispatcher may have filled
         * this exact entry while the shared lock was released. */
        spu_job_lookup_lock_exclusive();
        for (unsigned i = 0; i < SPU_JOB_LOOKUP_CACHE_SLOTS; ++i) {
            const spu_job_lookup_cache_entry* entry =
                &s_spu_job_lookup_cache[i];
            if (entry->valid && entry->image == image &&
                entry->image_size == image_size &&
                memcmp(entry->snapshot, image, image_size) == 0) {
                *fp_out = entry->fp;
                *fn_out = entry->fn;
                *image_id_out = entry->image_id;
                spu_job_lookup_unlock_exclusive();
                return;
            }
        }

        {   /* Each miss re-hashes the image under the exclusive lock. */
            static unsigned s_misses;
            const unsigned n = ++s_misses;
            if (n >= 32 && (n & (n - 1)) == 0)
                fprintf(stderr, "[tj-spu-lookup] cache misses=%u\n", n);
        }
        unsigned replace = s_spu_job_lookup_cache_next++ %
                           SPU_JOB_LOOKUP_CACHE_SLOTS;
        for (unsigned i = 0; i < SPU_JOB_LOOKUP_CACHE_SLOTS; ++i) {
            if (s_spu_job_lookup_cache[i].valid &&
                s_spu_job_lookup_cache[i].image == image &&
                s_spu_job_lookup_cache[i].image_size == image_size) {
                replace = i;
                break;
            }
        }

        spu_job_lookup_cache_entry* entry =
            &s_spu_job_lookup_cache[replace];
        memcpy(entry->snapshot, image, image_size);
        entry->fp = spu_workload_fingerprint(entry->snapshot, image_size);
        spu_registry_lookup_locked(entry->fp, &entry->fn, &entry->image_id);
        entry->image = image;
        entry->image_size = image_size;
        entry->valid = 1;

        *fp_out = entry->fp;
        *fn_out = entry->fn;
        *image_id_out = entry->image_id;
        spu_job_lookup_unlock_exclusive();
        return;
    }

    /* The cache is deliberately memory-bounded.  Preserve canonical behavior
     * for any image too large to snapshot. */
    *fp_out = spu_workload_fingerprint(image, image_size);
    spu_job_lookup_lock_shared();
    spu_registry_lookup_locked(*fp_out, fn_out, image_id_out);
    spu_job_lookup_unlock_shared();
}

/* Default-off dispatcher split: prove whether repeated full-image fingerprint
 * lookup or the jm2 staging/lifted job owns a slow SPURS chain.  Aggregate by
 * lifted image so high-frequency chains do not flood stderr per job. */
#ifdef _WIN32
#define TJ_SPURS_DISPATCH_PERF_ROWS 64
typedef struct tj_spurs_dispatch_perf_row {
    volatile LONG64 calls;
    volatile LONG64 lookup_ticks;
    volatile LONG64 lookup_max_ticks;
    volatile LONG64 run_ticks;
    volatile LONG64 run_max_ticks;
    volatile LONG64 total_ticks;
    volatile LONG64 total_max_ticks;
} tj_spurs_dispatch_perf_row;

static tj_spurs_dispatch_perf_row
    s_tj_spurs_dispatch_perf[TJ_SPURS_DISPATCH_PERF_ROWS];
static LARGE_INTEGER s_tj_spurs_dispatch_perf_frequency;
static volatile LONG s_tj_spurs_dispatch_perf_state;
static volatile LONG64 s_tj_spurs_dispatch_perf_last_report_ms;
static SRWLOCK s_tj_spurs_dispatch_perf_lock = SRWLOCK_INIT;
static __declspec(thread) int s_tj_spurs_dispatch_perf_tls = -1;

static int tj_spurs_dispatch_perf_enabled(void)
{
    if (s_tj_spurs_dispatch_perf_tls >= 0)
        return s_tj_spurs_dispatch_perf_tls;

    /* 0=uninitialized, 1=initializing, 2=disabled, 3=enabled. */
    LONG state = InterlockedCompareExchange(
        &s_tj_spurs_dispatch_perf_state, 1, 0);
    if (state == 0) {
        int enabled = getenv("TJ_SPURS_DISPATCH_PERF") ? 1 : 0;
        if (enabled && !QueryPerformanceFrequency(
                           &s_tj_spurs_dispatch_perf_frequency))
            enabled = 0;
        InterlockedExchange(&s_tj_spurs_dispatch_perf_state,
                            enabled ? 3 : 2);
        s_tj_spurs_dispatch_perf_tls = enabled;
        return s_tj_spurs_dispatch_perf_tls;
    }
    while ((state = InterlockedCompareExchange(
                &s_tj_spurs_dispatch_perf_state, 0, 0)) == 1)
        Sleep(0);
    s_tj_spurs_dispatch_perf_tls = state == 3;
    return s_tj_spurs_dispatch_perf_tls;
}

static uint64_t tj_spurs_dispatch_perf_ticks_to_us(uint64_t ticks)
{
    uint64_t frequency =
        (uint64_t)s_tj_spurs_dispatch_perf_frequency.QuadPart;
    if (!frequency) return 0;
    return (ticks / frequency) * 1000000ull +
           ((ticks % frequency) * 1000000ull) / frequency;
}

static void tj_spurs_dispatch_perf_max(volatile LONG64* target,
                                       LONG64 value)
{
    LONG64 observed = InterlockedCompareExchange64(target, 0, 0);
    while (value > observed) {
        LONG64 previous = InterlockedCompareExchange64(target, value, observed);
        if (previous == observed) return;
        observed = previous;
    }
}

/* One printed row, copied under the lock and formatted after releasing it. */
typedef struct tj_spurs_dispatch_perf_snapshot {
    unsigned image;
    uint64_t calls;
    uint64_t lookup_ticks, lookup_max_ticks;
    uint64_t run_ticks, run_max_ticks;
    uint64_t total_ticks, total_max_ticks;
} tj_spurs_dispatch_perf_snapshot;

/* One stderr write per report line: stderr is unbuffered, so fprintf writes
 * one character per system call (~2.6 us each to the TJ_STDERR_FILE log).
 * fwrite holds the stream lock, so the line stays whole. */
#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void tj_spurs_dispatch_log_line(const char* format, ...)
{
    char line[2048];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length >= 0 && (size_t)length < sizeof(line)) {
        fwrite(line, 1, (size_t)length, stderr);
        return;
    }
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

/* Winning reporter only, entered holding s_tj_spurs_dispatch_perf_lock. Out
 * of line so the snapshot buffer never enlarges the hot dispatch frame. */
static __declspec(noinline) void tj_spurs_dispatch_perf_report_unlock(void)
{
    /* Copy one coherent snapshot under the lock, then print after releasing
     * it. Every SPU dispatch takes this lock, so an fprintf that blocked here
     * (full launcher stderr pipe) held all SPU dispatch: 11.1 s run max in the
     * 2026-10-17 traced run. Same rows, values and line format as before. */
    tj_spurs_dispatch_perf_snapshot snapshot[TJ_SPURS_DISPATCH_PERF_ROWS];
    unsigned snapshot_count = 0;
    for (unsigned i = 0; i < TJ_SPURS_DISPATCH_PERF_ROWS; ++i) {
        tj_spurs_dispatch_perf_row* source = &s_tj_spurs_dispatch_perf[i];
        uint64_t calls = (uint64_t)InterlockedCompareExchange64(
            &source->calls, 0, 0);
        if (!calls) continue;
        tj_spurs_dispatch_perf_snapshot* copy = &snapshot[snapshot_count++];
        copy->image = i;
        copy->calls = calls;
        copy->lookup_ticks = (uint64_t)InterlockedCompareExchange64(
            &source->lookup_ticks, 0, 0);
        copy->run_ticks = (uint64_t)InterlockedCompareExchange64(
            &source->run_ticks, 0, 0);
        copy->total_ticks = (uint64_t)InterlockedCompareExchange64(
            &source->total_ticks, 0, 0);
        copy->lookup_max_ticks = (uint64_t)InterlockedCompareExchange64(
            &source->lookup_max_ticks, 0, 0);
        copy->run_max_ticks = (uint64_t)InterlockedCompareExchange64(
            &source->run_max_ticks, 0, 0);
        copy->total_max_ticks = (uint64_t)InterlockedCompareExchange64(
            &source->total_max_ticks, 0, 0);
    }
    ReleaseSRWLockExclusive(&s_tj_spurs_dispatch_perf_lock);

    for (unsigned n = 0; n < snapshot_count; ++n) {
        const tj_spurs_dispatch_perf_snapshot* current = &snapshot[n];
        const uint64_t calls = current->calls;
        const uint64_t lookup = current->lookup_ticks;
        const uint64_t run = current->run_ticks;
        const uint64_t total = current->total_ticks;
        tj_spurs_dispatch_log_line(
                "[TJ-SPURS-DISPATCH-PERF] image=%u calls=%llu "
                "lookup={total_us=%llu avg_us=%llu max_us=%llu} "
                "run={total_us=%llu avg_us=%llu max_us=%llu} "
                "total={total_us=%llu avg_us=%llu max_us=%llu}\n",
                current->image, (unsigned long long)calls,
                (unsigned long long)tj_spurs_dispatch_perf_ticks_to_us(lookup),
                (unsigned long long)tj_spurs_dispatch_perf_ticks_to_us(lookup) / calls,
                (unsigned long long)tj_spurs_dispatch_perf_ticks_to_us(
                    current->lookup_max_ticks),
                (unsigned long long)tj_spurs_dispatch_perf_ticks_to_us(run),
                (unsigned long long)tj_spurs_dispatch_perf_ticks_to_us(run) / calls,
                (unsigned long long)tj_spurs_dispatch_perf_ticks_to_us(
                    current->run_max_ticks),
                (unsigned long long)tj_spurs_dispatch_perf_ticks_to_us(total),
                (unsigned long long)tj_spurs_dispatch_perf_ticks_to_us(total) / calls,
                (unsigned long long)tj_spurs_dispatch_perf_ticks_to_us(
                    current->total_max_ticks));
    }
}

static void tj_spurs_dispatch_perf_record(int image_id,
                                           uint64_t lookup_ticks,
                                           uint64_t run_ticks,
                                           uint64_t total_ticks)
{
    AcquireSRWLockExclusive(&s_tj_spurs_dispatch_perf_lock);
    unsigned row_index = image_id >= 0 &&
                         image_id < TJ_SPURS_DISPATCH_PERF_ROWS - 1
        ? (unsigned)image_id : TJ_SPURS_DISPATCH_PERF_ROWS - 1;
    tj_spurs_dispatch_perf_row* row =
        &s_tj_spurs_dispatch_perf[row_index];
    InterlockedIncrement64(&row->calls);
    InterlockedAdd64(&row->lookup_ticks, (LONG64)lookup_ticks);
    InterlockedAdd64(&row->run_ticks, (LONG64)run_ticks);
    InterlockedAdd64(&row->total_ticks, (LONG64)total_ticks);
    tj_spurs_dispatch_perf_max(&row->lookup_max_ticks, (LONG64)lookup_ticks);
    tj_spurs_dispatch_perf_max(&row->run_max_ticks, (LONG64)run_ticks);
    tj_spurs_dispatch_perf_max(&row->total_max_ticks, (LONG64)total_ticks);

    LONG64 now_ms = (LONG64)GetTickCount64();
    LONG64 previous = InterlockedCompareExchange64(
        &s_tj_spurs_dispatch_perf_last_report_ms, 0, 0);
    if (!previous) {
        InterlockedCompareExchange64(
            &s_tj_spurs_dispatch_perf_last_report_ms, now_ms, 0);
        ReleaseSRWLockExclusive(&s_tj_spurs_dispatch_perf_lock);
        return;
    }
    if (now_ms - previous < 1000 ||
        InterlockedCompareExchange64(
            &s_tj_spurs_dispatch_perf_last_report_ms, now_ms, previous) != previous) {
        ReleaseSRWLockExclusive(&s_tj_spurs_dispatch_perf_lock);
        return;
    }

    tj_spurs_dispatch_perf_report_unlock();
}
#else
static int tj_spurs_dispatch_perf_enabled(void) { return 0; }
#endif

void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                               int image_id, const char* name)
{
    if (!fn) return;
    spu_job_lookup_lock_exclusive();
    for (unsigned i = 0; i < s_registry_count; i++) {
        if (s_registry[i].fp == fingerprint) {     /* idempotent on fingerprint */
            s_registry[i].fn       = fn;
            s_registry[i].image_id = image_id;
            s_registry[i].name     = name;
            spu_job_lookup_cache_invalidate_locked();
            spu_job_lookup_unlock_exclusive();
            return;
        }
    }
    if (s_registry_count >= SPU_WORKLOAD_MAX) {
        fprintf(stderr, "[spu_workload] registry full (%u); dropping '%s'\n",
                SPU_WORKLOAD_MAX, name ? name : "?");
        spu_job_lookup_unlock_exclusive();
        return;
    }
    s_registry[s_registry_count].fp       = fingerprint;
    s_registry[s_registry_count].fn       = fn;
    s_registry[s_registry_count].image_id = image_id;
    s_registry[s_registry_count].name     = name;
    s_registry_count++;
    spu_job_lookup_cache_invalidate_locked();
    spu_job_lookup_unlock_exclusive();
}

void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name)
{
    spu_workload_register_img(fingerprint, fn, 0, name);
}

spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint)
{
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fingerprint)
            return s_registry[i].fn;
    return NULL;
}

spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint, int* image_id_out)
{
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fingerprint) {
            if (image_id_out) *image_id_out = s_registry[i].image_id;
            return s_registry[i].fn;
        }
    if (image_id_out) *image_id_out = 0;
    return NULL;
}

unsigned spu_workload_count(void) { return s_registry_count; }

/* ---- SPU ELF loader (32-bit big-endian) -------------------------------- */

static uint16_t rd_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls, uint32_t* entry_out)
{
    if (!image || !ls || image_size < 0x34) return 0;

    /* ELF ident: 0x7F 'E' 'L' 'F', ELFCLASS32 (1), ELFDATA2MSB (2). */
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 /*ELFCLASS32*/ || image[5] != 2 /*ELFDATA2MSB*/)
        return 0;

    uint32_t e_entry     = rd_be32(image + 0x18);
    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    if (e_phentsize < 0x20) e_phentsize = 0x20;

    for (uint16_t i = 0; i < e_phnum; i++) {
        size_t po = (size_t)e_phoff + (size_t)i * e_phentsize;
        if (po + 0x20 > image_size) break;
        const uint8_t* ph = image + po;

        uint32_t p_type   = rd_be32(ph + 0x00);
        if (p_type != 1 /*PT_LOAD*/) continue;
        uint32_t p_offset = rd_be32(ph + 0x04);
        uint32_t p_vaddr  = rd_be32(ph + 0x08);
        uint32_t p_filesz = rd_be32(ph + 0x10);
        uint32_t p_memsz  = rd_be32(ph + 0x14);

        /* bounds: segment must fit in local store and in the image */
        if ((uint64_t)p_vaddr + p_memsz > SPU_LS_SIZE)            return 0;
        if ((uint64_t)p_offset + p_filesz > image_size)          return 0;

        if (p_filesz) memcpy(ls + p_vaddr, image + p_offset, p_filesz);
        if (p_memsz > p_filesz)
            memset(ls + p_vaddr + p_filesz, 0, p_memsz - p_filesz);
    }
    if (entry_out) *entry_out = e_entry;
    return 1;
}

size_t spu_elf_image_size(const uint8_t* image, size_t max_avail)
{
    if (!image || max_avail < 0x34) return 0;
    if (!(image[0] == 0x7F && image[1] == 'E' && image[2] == 'L' && image[3] == 'F'))
        return 0;
    if (image[4] != 1 || image[5] != 2) return 0;     /* ELFCLASS32, ELFDATA2MSB */

    uint32_t e_phoff     = rd_be32(image + 0x1C);
    uint32_t e_shoff     = rd_be32(image + 0x20);
    uint16_t e_phentsize = rd_be16(image + 0x2A);
    uint16_t e_phnum     = rd_be16(image + 0x2C);
    uint16_t e_shentsize = rd_be16(image + 0x2E);
    uint16_t e_shnum     = rd_be16(image + 0x30);

    uint64_t end = (uint64_t)e_shoff + (uint64_t)e_shnum * e_shentsize;

    for (uint16_t k = 0; k < e_phnum; k++) {           /* program headers */
        size_t po = (size_t)e_phoff + (size_t)k * e_phentsize;
        if (po + 0x14 > max_avail) break;
        uint32_t p_offset = rd_be32(image + po + 0x04);
        uint32_t p_filesz = rd_be32(image + po + 0x10);
        uint64_t e = (uint64_t)p_offset + p_filesz;
        if (e > end) end = e;
    }
    for (uint16_t k = 0; k < e_shnum; k++) {           /* section headers */
        size_t so = (size_t)e_shoff + (size_t)k * e_shentsize;
        if (so + 0x18 > max_avail) break;
        uint32_t sh_type   = rd_be32(image + so + 0x04);
        uint32_t sh_offset = rd_be32(image + so + 0x10);
        uint32_t sh_size   = rd_be32(image + so + 0x14);
        if (sh_type != 8 /*SHT_NOBITS*/) {
            uint64_t e = (uint64_t)sh_offset + sh_size;
            if (e > end) end = e;
        }
    }
    if (end > max_avail) end = max_avail;
    return (size_t)end;
}

/* ---- dispatch ---------------------------------------------------------- */

int spu_workload_dispatch(const uint8_t* image, uint32_t image_size,
                          uint32_t args_ea)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr,
            "[spu_workload] dispatch MISS fp=0x%016llX size=%u "
            "(no lifted SPU binary registered for this image)\n",
            (unsigned long long)fp, image_size);
        return 0;
    }

    /* Load the SPU ELF into a fresh local store, then run the lifted entry with
     * the task arg in r3. 256 KB is heap-allocated (too large for the stack,
     * and spu_run_lifted_job already builds a full spu_context on its stack). */
    uint8_t* ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    if (!ls) return 0;

    uint32_t entry = 0;
    if (!spu_elf_load_to_ls(image, image_size, ls, &entry)) {
        fprintf(stderr, "[spu_workload] dispatch fp=0x%016llX: not a valid SPU ELF\n", (unsigned long long)fp);
        free(ls);
        return 0;
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT fp=0x%016llX entry=0x%05X args=0x%08X image=%d -> running\n",
        (unsigned long long)fp, entry, args_ea, image_id);

    spu_run_lifted_job_img(fn, ls, args_ea, image_id);

    free(ls);
    return 1;
}

/* Async dispatch: run the SPU job on its OWN host thread so the PPU caller is
 * not blocked. SPURS service/worker tasks are persistent — they loop waiting on
 * PPU-side signals (DMA, event flags, event queues), so running them inline (as
 * spu_workload_dispatch does) deadlocks: the PPU can never deliver the signal
 * the SPU is waiting for because it is stuck inside the dispatch. Real SPUs run
 * concurrently with the PPU; a detached host thread models that. The image bytes
 * and args live in the shared guest arena (vm_base), so they stay valid. */
typedef struct {
    const uint8_t*      image;
    uint32_t            image_size;
    uint32_t            args_ea;
    spu_lifted_entry_fn fn;
    int                 image_id;
    uint32_t            r3[4];        /* captured race-free at dispatch time */
    int                 have_r3;
    uint32_t            taskset_ea;   /* captured race-free at dispatch (globals get clobbered) */
    uint32_t            taskid;
} spu_async_job;

/* ---- Persistent per-taskset local store (LBP_PERSIST_LS) ------------------
 * Real SPURS time-multiplexes a taskset's tasks onto SPUs, saving/restoring a
 * shared taskset context in LS. Our default model runs each task ONCE on a
 * fresh calloc'd LS, so state one task establishes (e.g. FMOD's SPU DSP handler
 * table at LS 0x39Dxx, built by the init task) is invisible to the next task of
 * the same taskset, which reads the uninitialised image sentinel and
 * branch-to-0's. Give each taskset a PERSISTENT LS reused across its task
 * dispatches (image loaded once, state retained) and SERIALIZE its tasks (one
 * at a time) so they observe each other's setup without racing the shared
 * store -- a cooperative approximation of the kernel's context save/restore.
 * With LBP_WS_DRAIN (a task parked in WAIT_SIGNAL resumes + releases the
 * taskset, letting the next task run), the FMOD init + DSP tasks share one LS
 * and the handler table is populated when the DSP dispatch reads it. Opt-in;
 * default keeps per-dispatch fresh LS. */
typedef struct {
    uint32_t taskset_ea;   /* 0 = free slot */
    uint32_t taskid;       /* slot key is (taskset, task): on real SPURS each
                            * task has a PRIVATE LS context (the kernel saves/
                            * restores per task); one shared slot per TASKSET
                            * serialized siblings behind one lock, and FMOD's
                            * persistent mixer (task 0, never returns) starved
                            * its producer (task 1) forever -- the mixer then
                            * DMA-polled a NULL control EA for the whole run. */
    uint8_t* ls;           /* persistent LS (NULL until first task loads it) */
    int      loaded;       /* image loaded into ls */
#ifdef _WIN32
    SRWLOCK  lock;         /* serializes re-dispatches of this ONE task */
#else
    pthread_mutex_t lock;
#endif
} spu_ts_ls_slot;

#define SPU_TS_LS_MAX 24
static spu_ts_ls_slot s_ts_ls[SPU_TS_LS_MAX];
#ifdef _WIN32
static SRWLOCK s_ts_ls_dir = SRWLOCK_INIT;
#else
static pthread_mutex_t s_ts_ls_dir = PTHREAD_MUTEX_INITIALIZER;
#endif

static int spu_persist_ls_enabled(void)
{ static int _p = -1; if (_p < 0) _p = getenv("LBP_PERSIST_LS") ? 1 : 0; return _p; }

/* ---- Global SPU execution serialization (LBP_SPU_SERIAL) ------------------
 * Default runs each SPU task on its own detached host thread, so N tasks race:
 * timing jitter sends each boot down a different path to a different dead end
 * (one run hangs on the FMOD DSP null, the next on an unrelated PPU init spin),
 * which makes any single failure impossible to reproduce and debug. Serialize:
 * only ONE SPU task executes at a time (global lock), so the SPU stage becomes
 * a fixed, repeatable sequence. A task that parks in WAIT_SIGNAL RELEASES the
 * lock so another task can run (and possibly signal it) -- cooperative, mirrors
 * a single SPU's task scheduling; with LBP_WS_DRAIN a genuinely unsignalled
 * task drains and releases. Opt-in; default keeps concurrent per-thread SPU. */
static int spu_serial_enabled(void)
{ static int _s = -1; if (_s < 0) _s = getenv("LBP_SPU_SERIAL") ? 1 : 0; return _s; }
#ifdef _WIN32
static SRWLOCK s_spu_serial = SRWLOCK_INIT;
void spu_serial_acquire(void){ if (spu_serial_enabled()) AcquireSRWLockExclusive(&s_spu_serial); }
void spu_serial_release(void){ if (spu_serial_enabled()) ReleaseSRWLockExclusive(&s_spu_serial); }
#else
static pthread_mutex_t s_spu_serial = PTHREAD_MUTEX_INITIALIZER;
void spu_serial_acquire(void){ if (spu_serial_enabled()) pthread_mutex_lock(&s_spu_serial); }
void spu_serial_release(void){ if (spu_serial_enabled()) pthread_mutex_unlock(&s_spu_serial); }
#endif

/* Find-or-create the slot for a (taskset, task) pair (directory lock held
 * briefly). Per-task keying is load-bearing: see spu_ts_ls_slot.taskid. */
static spu_ts_ls_slot* ts_ls_get(uint32_t taskset_ea, uint32_t taskid)
{
    spu_ts_ls_slot* slot = NULL;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_ts_ls_dir);
#else
    pthread_mutex_lock(&s_ts_ls_dir);
#endif
    for (int i = 0; i < SPU_TS_LS_MAX && !slot; i++)
        if (s_ts_ls[i].taskset_ea == taskset_ea && s_ts_ls[i].taskid == taskid)
            slot = &s_ts_ls[i];
    for (int i = 0; i < SPU_TS_LS_MAX && !slot; i++)
        if (s_ts_ls[i].taskset_ea == 0) {
            s_ts_ls[i].taskset_ea = taskset_ea;
            s_ts_ls[i].taskid = taskid;
#ifdef _WIN32
            InitializeSRWLock(&s_ts_ls[i].lock);
#else
            pthread_mutex_init(&s_ts_ls[i].lock, NULL);
#endif
            slot = &s_ts_ls[i];
        }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_ts_ls_dir);
#else
    pthread_mutex_unlock(&s_ts_ls_dir);
#endif
    return slot;
}

static void spu_async_run(spu_async_job* j)
{
    /* Persistent per-taskset LS: acquire (+serialize) the taskset's LS, or fall
     * back to a fresh per-dispatch LS when disabled / no taskset. */
    spu_ts_ls_slot* ts_slot = NULL;
    int did_load = 0;           /* whether THIS dispatch (re)loaded the image */
    if (spu_persist_ls_enabled() && j->taskset_ea && j->image_id != 22)
        ts_slot = ts_ls_get(j->taskset_ea, j->taskid);
    uint8_t* ls;
    if (ts_slot) {
#ifdef _WIN32
        AcquireSRWLockExclusive(&ts_slot->lock);   /* serialize this taskset */
#else
        pthread_mutex_lock(&ts_slot->lock);
#endif
        if (!ts_slot->ls) ts_slot->ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
        ls = ts_slot->ls;
        { static int _n = 0; if (_n++ < 24)
            fprintf(stderr, "[persist-ls] taskset=0x%08X task=%u image=%d %s\n",
                    j->taskset_ea, j->taskid, j->image_id,
                    ts_slot->loaded ? "REUSE (state retained)" : "first load"); fflush(stderr); }
    } else {
        ls = (uint8_t*)calloc(1, SPU_LS_SIZE);
    }
    if (ls) {
        uint32_t entry = 0;
        /* Load the image only on a taskset's FIRST task (or every dispatch when
         * not persisting) -- reusing preserves the prior task's LS state. */
        int loaded = (ts_slot && ts_slot->loaded)
                   ? 1
                   : (did_load = 1, spu_elf_load_to_ls(j->image, j->image_size, ls, &entry));
        if (ts_slot && did_load && loaded) ts_slot->loaded = 1;
        if (!loaded)
            fprintf(stderr, "[spu_workload] async image=%d ELF LOAD FAILED\n",
                    j->image_id), fflush(stderr);
        if (loaded) {
            /* YDKJ_CRI_POLICY (cri build experiment): the cri SPU task
             * (image 22) calls the SPURS task-API via a jump table at LS 0x2700
             * and reads its task descriptor at LS 0x2FB0 — both POLICY-provided,
             * not in the task ELF (so it branch-to-0's at func_00026DE0). Preload
             * the policy module (libsre @0x30021480 -> LS 0xA00) and write the
             * task descriptor {0xFFFFFFFF, 0x400, 0x2700, 0x3000} at 0x2FB0, then
             * run the cri task, to see how much further it gets. Diagnostic only. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_TASKSET")) {
                /* cri build: load the TASKSET POLICY module (libsre 0x23680 ->
                 * LS 0xA00) alongside the cri task (already at 0x3000), then RUN
                 * THE POLICY ENTRY (tsp_spu_func_00000A00) under image 23. The
                 * policy builds the 0x2700 task-API table + dispatches the cri
                 * task. Needs the SpursKernelContext at LS[0x1C0] (instance ptr)
                 * + r80=0x100 context base, mirroring the kernel->policy handoff. */
                extern uint8_t* vm_base;
                extern void tsp_spu_func_00000A00(spu_context*);
                if (vm_base) memcpy(ls + 0xA00, vm_base + 0x30023680, 0x2200);
                /* Build the REAL SpursTasksetContext at LS 0x2700 (taskset ptr @0x27B8,
                 * TaskInfo @0x2780, syscallAddr @0x27C4) from the actual game taskset,
                 * so the policy's DMAs read valid data instead of garbage. The instance
                 * ptr (LS[0x1C0]) was a hardcoded 0x40009D00 that mismatched the game's
                 * real 0x40009F00 -> policy DMA'd garbage -> null branches. */
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                if (j->taskset_ea)
                    spurs_pm_build_context(ls, j->taskset_ea, j->taskid,
                                           j->taskid & 7u, 0);
                /* Use this async job's captured taskset.  Reading the global
                 * latest-taskset here races the next CreateTask and can build
                 * image 22 with another task's descriptors. */
                uint32_t inst = j->taskset_ea
                              ? (uint32_t)vm_read64(j->taskset_ea + 0x60)
                              : 0x40009D00u;
                uint8_t* p = ls + 0x1C0;
                p[0]=0; p[1]=0; p[2]=0; p[3]=0;                 /* hi32 of u64 */
                p[4]=(uint8_t)(inst>>24); p[5]=(uint8_t)(inst>>16);
                p[6]=(uint8_t)(inst>>8);  p[7]=(uint8_t)inst;   /* lo32 */
                fprintf(stderr, "[cri] YDKJ_CRI_TASKSET: loaded taskset policy@0xA00, LS[0x1C0]=inst, running policy entry (img23)\n");
                fflush(stderr);
                /* Run the taskset policy entry instead of the cri task entry. */
                int32_t prc = spu_run_lifted_job_abi(tsp_spu_func_00000A00, ls,
                                                     j->args_ea, 23, 1, j->have_r3 ? j->r3 : 0, 0);
                fprintf(stderr, "[cri] taskset policy RETURNED rc=%d\n", prc);
                fflush(stderr);
                free(ls); free(j); return;
            }
            /* YDKJ HLE cri-task path (image 22, cri_mpvps3spurs.elf): the real
             * SPURS kernel/policy would build the task's SpursTasksetContext (LS
             * 0x2700) before dispatch. In the HLE-cellSpurs path we dispatch the
             * task ELF directly, so LS 0x2700 is empty and the task branch-to-0's:
             * cri func_00026DE0 reads syscallAddr@0x27C4 (=0) and branches there.
             * Plant a minimal context so the task-API call lands on the HLE syscall
             * trampoline (LS 0xA70, intercepted in spu_channels.c) + the task
             * descriptor @0x2FB0. Adopted from JonathanDC64/ps3recomp (aaea4158).
             * Gate: default on for image 22 unless YDKJ_NO_CRI_CTX. */
            if (j->image_id == 22 && !getenv("YDKJ_NO_CRI_CTX")) {
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                #define LSBE32(o,v) do{uint32_t _v=(v);ls[(o)+0]=(uint8_t)(_v>>24);ls[(o)+1]=(uint8_t)(_v>>16);ls[(o)+2]=(uint8_t)(_v>>8);ls[(o)+3]=(uint8_t)_v;}while(0)
                if (j->taskset_ea && !getenv("YDKJ_MINIMAL_CTX")) {
                    /* REAL taskset context: build the SpursTasksetContext at LS 0x2700
                     * from the actual BE CellSpursTaskset (spurs ptr, args, TaskInfo)
                     * so the cri leaf reads valid data instead of my planted guesses. */
                    uint64_t elf = spurs_pm_build_context(ls, j->taskset_ea,
                                                          j->taskid,
                                                          j->taskid & 7u, 0);
                    /* still plant the cri-specific task descriptor @0x2FB0 that build_context
                     * doesn't cover (cri func_00026DE0 reads it). */
                    LSBE32(0x2FB0, 0xFFFFFFFFu); LSBE32(0x2FB4, 0x400);
                    LSBE32(0x2FB8, 0x2700);      LSBE32(0x2FBC, 0x3000);
                    fprintf(stderr, "[cri] REAL SpursTasksetContext built from taskset 0x%08X task %u (elf=0x%llX)\n",
                            j->taskset_ea, j->taskid, (unsigned long long)elf);
                } else {
                    LSBE32(0x27C0, 0x100);     /* kernelMgmtAddr -> SPURS kernel ctx @LS 0x100 */
                    LSBE32(0x27C4, 0xA70);     /* syscallAddr -> HLE PM syscall trampoline (0xA70) */
                    LSBE32(0x2FB0, 0xFFFFFFFFu); LSBE32(0x2FB4, 0x400);
                    LSBE32(0x2FB8, 0x2700);      LSBE32(0x2FBC, 0x3000);
                    fprintf(stderr, "[cri] HLE cri-task ctx (minimal plant): syscallAddr@0x27C4=0xA70\n");
                }
                #undef LSBE32
                fflush(stderr);
            }
            /* Dump the 64-byte decode context the cri task will DMA from eaContext
             * (r3.word1 = args_ea). This is the job the game's cri_mpv PPU layer is
             * supposed to populate (video-data EA, output buffer). If it's zero/garbage
             * the PPU layer never wrote a real job -> the task decodes nothing. */
            if (j->image_id == 22 && j->args_ea) {
                extern uint8_t* vm_base;
                const uint8_t* c = vm_base + (j->args_ea & 0x0FFFFFFFu);
                fprintf(stderr, "[cri] eaContext=0x%08X 64B decode-context:", j->args_ea);
                for (int i = 0; i < 64; i++) { if ((i&15)==0) fprintf(stderr,"\n     +%02X:", i); fprintf(stderr, " %02X", c[i]); }
                fprintf(stderr, "\n"); fflush(stderr);
            }
            /* Async dispatch is the SPURS-task path: the entry expects the SPURS
             * task kernel ABI in r3 ({0x40 marker, eaContext, queue EA, ...}),
             * captured at dispatch time (j->r3) so it doesn't race the PPU
             * overwriting the stack-allocated context. */
            /* Generic SPURS taskset task (LBP's audio SPEEX/MultiStream tasks,
             * image 6/7 — anything but the cri image 22 handled above): the real
             * kernel runs the task UNDER the taskset policy, which plants a
             * SpursTasksetContext at LS 0x2700 (taskset header, TaskInfo, and
             * syscallAddr=0xA70) and enters it with r3 = task args, r4 = {spurs,
             * taskset args}. We dispatch the task ELF directly, so without this it
             * runs with r3=r4=0 and no context and SPINS FOREVER (observed: image
             * 6/7 ENTER + RUN, never RETURN, 0 DMA, PPU blocks on EventFlag 0x0100).
             * Build the context here from the real taskset CreateTask recorded;
             * spu_run_lifted_job_abi then sets r3/r4 off the 0xA70 sentinel.
             * Env-gated while bringing this up — default leaves every path as-is. */
            /* Timing probe: a real SPURS task is dispatched when work is signaled.
             * We dispatch immediately at CreateTask. If LBP fills the task's work
             * buffer slightly LATER, delaying the run lets it see real data (=>
             * timing bug, fix = defer/re-dispatch on signal); if it still loops on
             * a null base, the buffer is never filled (=> an audio HLE gap). */
            if (j->image_id != 22) {
                const char* d = getenv("LBP_TASK_DELAY");
                if (d && *d) {
#ifdef _WIN32
                    Sleep((unsigned)(atoi(d) * 1000));
#endif
                }
            }
            /* A generic SPURS taskset task (image != cri 22) MUST get its
             * SpursTasksetContext planted at LS 0x2700 -- the real kernel builds
             * it from the taskset before entry, and the task reads its args and
             * every DMA base pointer out of it. Without it the task runs with a
             * null base and DMAs EVERYTHING from EA 0 (observed on LBP's FMOD
             * mixer: 105k GETs from ea=0x0, degenerate handler targets ->
             * branch-to-0, and the PPU pump blocks forever on EventFlag 0x0100).
             * Planting it makes the mixer read the real FMOD control block
             * (0x0094F5xx) and the EventFlag handshake completes. Default ON;
             * LBP_NO_TASKSET restores the old direct-dispatch for comparison. */
            if (j->image_id != 22 && !getenv("LBP_NO_TASKSET")) {
                extern uint64_t spurs_pm_build_context(uint8_t*, uint32_t, uint32_t, uint32_t, uint32_t);
                /* Use the taskset+taskid captured for THIS job at dispatch (not the
                 * globals, which the next CreateTask clobbers -- the race that made
                 * both audio tasks run task 1's descriptor). */
                if (j->taskset_ea) {
                    /* Each concurrently hosted task needs a distinct logical SPU
                     * number in its taskset context.  Stamping every task as SPU 0
                     * makes SPU-side event-flag barriers collide on one waiter slot:
                     * Tokyo Jungle's two Survival workers both register slot 0 and
                     * then sleep forever in WAIT_SIGNAL.  This HLE maps the stable
                     * per-task host worker to the matching virtual SPU lane. */
                    spurs_pm_build_context(ls, j->taskset_ea, j->taskid,
                                           j->taskid & 7u, 0);
                    fprintf(stderr, "[taskset] built SpursTasksetContext image=%d "
                            "taskset=0x%08X task=%u\n", j->image_id,
                            j->taskset_ea, j->taskid); fflush(stderr);
                }
            }
            /* ---- Bink-layer probe + real-policy A/B (image 3 = binkspu) ----
             * (1) dump binkspu's Bink work-descriptor at its arg EA so we can see
             *     the output-plane EAs and the plane+0x50 "needs-decode" gate that
             *     gates the frame-output PUT (all prior PUTs went to the 0x927E80
             *     control block, never a plane). This tells us whether the missing
             *     output EA is a Bink/PPU-layer field (this descriptor) or a SPURS
             *     taskset-context field (LS 0x2700, below).
             * (2) LBP_REAL_POLICY: run the REAL lifted taskset policy to build its
             *     SpursTasksetContext and diff it against the C reimpl build_context
             *     planted in `ls` -- shows exactly which fields differ. */
            if (j->image_id == 3) {
                extern uint8_t* vm_base;
                uint32_t arg = j->args_ea;
                /* SPU DMA maps EAs as vm_base + (uint32_t)ea with NO masking
                 * (spu_dma.h:186); vm_base spans the full 4GB so VRAM 0xC0000000+
                 * is directly addressable. (The earlier &0x0FFFFFFF mask read main
                 * code at 0x10B88 instead of the real VRAM descriptor.) */
                #define RDBE32(base, off) (((uint32_t)(base)[(off)]<<24)|((uint32_t)(base)[(off)+1]<<16)| \
                                           ((uint32_t)(base)[(off)+2]<<8)|(base)[(off)+3])
                if (vm_base && arg >= 0x10000) {
                    const uint8_t* d = vm_base + arg;         /* FULL EA, no mask */
                    fprintf(stderr, "[bink-desc] arg=0x%08X 256B work-descriptor:", arg);
                    uint32_t words[64];
                    for (int i = 0; i < 256; i += 16) {
                        fprintf(stderr, "\n   +%03X:", i);
                        for (int k = 0; k < 16; k += 4) {
                            uint32_t w = RDBE32(d, i+k); words[(i+k)/4] = w;
                            fprintf(stderr, " %08X", w);
                        }
                    }
                    fprintf(stderr, "\n");
                    /* Follow each plausible pointer one level: dump +0x00..+0x60 so
                     * the output-plane EA + the plane+0x50 "needs-decode" gate show. */
                    for (int wi = 0; wi < 64; wi++) {
                        uint32_t p = words[wi];
                        if (p < 0x10000 || (p >= 0x50000000u && p < 0xC0000000u)) continue;
                        const uint8_t* q = vm_base + p;
                        fprintf(stderr, "  [desc+%02X]=0x%08X -> +00:%08X +40:%08X +50:%08X(gate) +54:%08X\n",
                                wi*4, p, RDBE32(q,0), RDBE32(q,0x40), RDBE32(q,0x50), RDBE32(q,0x54));
                    }
                    fflush(stderr);
                }
                if (getenv("LBP_REAL_POLICY") && j->taskset_ea) {
                    extern int spurs_run_taskset_policy_probe(uint32_t,uint32_t,uint32_t,
                                                              uint64_t,uint32_t,uint8_t*,uint32_t);
                    /* spurs EA from the taskset header lo32 @ +0x64 (be64 @0x60). */
                    uint32_t ts = j->taskset_ea;
                    uint32_t spurs_ea = 0;
                    if (vm_base) { const uint8_t* t = vm_base + ts;
                        spurs_ea = RDBE32(t, 0x64);
                        /* Why did the policy exit without dispatching? Dump the task
                         * bitsets it reads (task N = bit 127-N; task 2 = bit 125). */
                        fprintf(stderr, "[taskset-bits] RUNNING=%08X%08X READY=%08X%08X ENABLED=%08X%08X SIGNALLED=%08X%08X WAITING=%08X%08X\n",
                                RDBE32(t,0x00),RDBE32(t,0x04), RDBE32(t,0x10),RDBE32(t,0x14),
                                RDBE32(t,0x30),RDBE32(t,0x34), RDBE32(t,0x40),RDBE32(t,0x44),
                                RDBE32(t,0x50),RDBE32(t,0x54)); }
                    uint8_t real2700[0x180];
                    int st = spurs_run_taskset_policy_probe(ts, j->taskid, spurs_ea,
                                                            (uint64_t)ts, 3, real2700, sizeof real2700);
                    fprintf(stderr, "[real-pm] status=0x%X -- LS 0x2700 diff (build_context vs real policy):\n", st);
                    for (uint32_t o = 0; o < sizeof real2700; o += 4) {
                        uint32_t bc = ((uint32_t)ls[0x2700+o]<<24)|((uint32_t)ls[0x2700+o+1]<<16)|
                                      ((uint32_t)ls[0x2700+o+2]<<8)|ls[0x2700+o+3];
                        uint32_t rp = ((uint32_t)real2700[o]<<24)|((uint32_t)real2700[o+1]<<16)|
                                      ((uint32_t)real2700[o+2]<<8)|real2700[o+3];
                        if (bc != rp)
                            fprintf(stderr, "   0x%04X: build=%08X real=%08X\n", 0x2700+o, bc, rp);
                    }
                    /* EMPIRICAL TEST (LBP_POLICY_CTX): overlay the REAL policy's
                     * SpursTasksetContext onto binkspu's LS 0x2700, replacing the C
                     * reimpl, then let binkspu run with it. If the movie plane fills
                     * (not green) the policy's fuller ctx is the fix (H1); if still
                     * green, the frame-output gate is Bink-layer (H2). Keeps the
                     * build_context 0x100 kernel ctx (moduleId "TK") intact. */
                    if (getenv("LBP_POLICY_CTX")) {
                        memcpy(ls + 0x2700, real2700, sizeof real2700);
                        fprintf(stderr, "[real-pm] OVERLAID policy ctx onto binkspu LS 0x2700 (LBP_POLICY_CTX)\n");
                    }
                    fflush(stderr);
                }
                #undef RDBE32
             }
             spu_serial_acquire();       /* one SPU task runs at a time (LBP_SPU_SERIAL) */
#ifdef _WIN32
             tj_spu_pc_time_job_begin(j->image_id);
#endif
             int32_t rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                                 1, j->have_r3 ? j->r3 : 0, 0);
#ifdef _WIN32
             tj_spu_pc_time_job_complete(j->image_id);
             tj_spu_pc_hist_job_complete(j->image_id);
#endif
            /* YDKJ_CRI_RESUME: a real SPURS task is PERSISTENT -- on yield (num=0)
             * the kernel re-enters it when work is signaled. Our HLE runs it once,
             * so it polls the (concurrently PPU-updated) eaContext, finds no work,
             * yields and exits before the PPU marks work-ready. Approximate the
             * resume by re-running the task (fresh context re-read from eaContext)
             * in a bounded loop until it actually DECODES (a >256B video GET sets
             * g_cri_video_dma) or we time out. This does NOT fake data: the task
             * only decodes if the PPU genuinely populated real work meanwhile. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_RESUME") && !g_cri_video_dma) {
                for (int attempt = 0; attempt < 400 && !g_cri_video_dma; attempt++) {
#ifdef _WIN32
                    Sleep(3);
#endif
                    memset(ls, 0, SPU_LS_SIZE);
                    uint32_t e2 = 0;
                    if (!spu_elf_load_to_ls(j->image, j->image_size, ls, &e2)) break;
                    rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,
                                                1, j->have_r3 ? j->r3 : 0, 0);
                    if (g_cri_video_dma) {
                        fprintf(stderr, "[cri] RESUME: cri task DECODED real video (attempt %d)\n", attempt);
                        break;
                    }
                }
                if (!g_cri_video_dma)
                    fprintf(stderr, "[cri] RESUME: no work-ready after 400 polls (PPU never marked work)\n");
                fflush(stderr);
            }
            /* YDKJ_CRI_WAKE probe: on cri task (image 22) completion, wake any PPU
             * completion-waiter (the SPU->PPU cellSpursEventFlag completion isn't
             * propagated yet). Tests whether the game then advances to draw content. */
            if (j->image_id == 22 && getenv("YDKJ_CRI_WAKE")) {
                extern void ydkj_wake_all_event_flags(void);
                ydkj_wake_all_event_flags();
                fprintf(stderr, "[cri] YDKJ_CRI_WAKE: woke all event-flag waiters on cri completion\n");
            }
            fprintf(stderr, "[spu_workload] async image=%d RETURNED rc=%d "
                    "(job ran to completion, did not loop)\n", j->image_id, rc);
            spu_serial_release();
        }
        /* Persistent LS is retained in its taskset slot for the next task; a
         * per-dispatch LS is freed. */
        if (!ts_slot) free(ls);
    }
    if (ts_slot) {
#ifdef _WIN32
        ReleaseSRWLockExclusive(&ts_slot->lock);
#else
        pthread_mutex_unlock(&ts_slot->lock);
#endif
    }
    free(j);
}

#ifdef _WIN32
static DWORD WINAPI spu_async_thread(LPVOID p) {
    /* Reserve handler headroom so a stack overflow in lifted SPU code (e.g. a
     * runaway brsl recursion) can actually reach the STACKOVERFLOW reporter
     * instead of killing the process silently with 0x80000001. */
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
    if (!guest_world_register_current("spu-async")) return 0;
    spu_async_run((spu_async_job*)p);
    guest_world_unregister_current();
    return 0;
}
#else
static void* spu_async_thread(void* p) {
    if (!guest_world_register_current("spu-async")) return NULL;
    spu_async_run((spu_async_job*)p);
    guest_world_unregister_current();
    return NULL;
}
#endif

int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size,
                              uint32_t job_ea, uint32_t job_desc_size,
                              uint32_t spurs_ea)
{
    if (!image || image_size == 0) return 0;

#ifdef _WIN32
    const int dispatch_perf = tj_spurs_dispatch_perf_enabled();
    LARGE_INTEGER dispatch_perf_begin = {0};
    LARGE_INTEGER dispatch_perf_lookup_end = {0};
    if (dispatch_perf) QueryPerformanceCounter(&dispatch_perf_begin);
#endif

    uint64_t fp;
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    spu_job_lookup(image, image_size, &fp, &fn, &image_id);
#ifdef _WIN32
    if (dispatch_perf) QueryPerformanceCounter(&dispatch_perf_lookup_end);
#endif
    const int tj_trace = tj_late_job_trace_enabled();
    const int target_trace = spu_job_trace_target_matches(fp);
    if (image_id == 13)
        tj_late_eventflag_trace_note_image13_dispatch(job_ea, 0, 1);
    if (tj_trace) {
        fprintf(stderr,
                "[TJ-LATE-JOB] seq=%lu tid=%lu phase=DISPATCH_BEGIN image=%d job=0x%08X fp=0x%016llX rc=pending\n",
                tj_late_job_trace_next_sequence(), tj_late_job_trace_thread_id(),
                image_id, job_ea, (unsigned long long)fp);
        fflush(stderr);
    }
    if (!fn) {
        extern uint8_t* vm_base;
        uint32_t image_ea = vm_base ? (uint32_t)(image - vm_base) : 0;
        fprintf(stderr, "[spurs-job] dispatch MISS fp=0x%016llX ea=0x%08X size=%u job=0x%08X\n",
                (unsigned long long)fp, image_ea, image_size, job_ea);
        spu_dump_miss(image, image_size, fp);
        if (tj_trace) {
            fprintf(stderr,
                    "[TJ-LATE-JOB] seq=%lu tid=%lu phase=DISPATCH_END image=%d job=0x%08X fp=0x%016llX rc=0\n",
                    tj_late_job_trace_next_sequence(), tj_late_job_trace_thread_id(),
                    image_id, job_ea, (unsigned long long)fp);
            fflush(stderr);
        }
        if (image_id == 13)
            tj_late_eventflag_trace_note_image13_dispatch(job_ea, 0, 0);
        return 0;
    }
    if (spu_job_trace_enabled()) {
        fprintf(stderr, "[spurs-job] dispatch HIT fp=0x%016llX image=%d job=0x%08X\n",
                (unsigned long long)fp, image_id, job_ea);
        fflush(stderr);
    }
    const int target_trace_claimed = target_trace &&
                                     spu_job_trace_target_take_hit();
    if (target_trace_claimed) {
        fprintf(stderr, "[spurs-job] TARGET HIT fp=0x%016llX image_id=%d job_ea=0x%08X rc=pending\n",
                (unsigned long long)fp, image_id, job_ea);
        fflush(stderr);
    }
#ifdef _WIN32
    static int tj_geometry_stage_trace = -1;
    if (tj_geometry_stage_trace < 0)
        tj_geometry_stage_trace = getenv("TJ_GEOMETRY_STAGE_TRACE") ? 1 : 0;
    int tj_geometry_dispatch_trace = 0;
    if (image_id == 17 && tj_geometry_stage_trace) {
        extern volatile LONG g_tj_geometry_trace_armed;
        static volatile LONG counts[3];
        LONG phase = InterlockedCompareExchange(&g_tj_geometry_trace_armed, 0, 0);
        LONG hit = InterlockedIncrement(&counts[phase]);
        tj_geometry_dispatch_trace = hit <= 4;
        if (tj_geometry_dispatch_trace)
            fprintf(stderr, "[TJ-GEOMETRY-DISPATCH] phase=enter stage=%ld hit=%ld job=%08X size=%u\n",
                phase, hit, job_ea, job_desc_size);
    }
#endif
    int rc = spu_run_spurs_job(fn, image_id, job_ea, job_desc_size, spurs_ea);
#ifdef _WIN32
    if (tj_geometry_dispatch_trace)
        fprintf(stderr, "[TJ-GEOMETRY-DISPATCH] phase=return job=%08X rc=%d\n", job_ea, rc);
    if (image_id == 18 && rc == 0 && tj_geometry_stage_trace) {
        extern volatile LONG g_tj_geometry_trace_armed;
        if (InterlockedExchange(&g_tj_geometry_trace_armed, 2) != 2)
            fprintf(stderr, "[TJ-GEOMETRY-PHASE] phase=2 source=image18_return\n");
    }
    if (dispatch_perf) {
        LARGE_INTEGER dispatch_perf_end;
        QueryPerformanceCounter(&dispatch_perf_end);
        tj_spurs_dispatch_perf_record(
            image_id,
            (uint64_t)(dispatch_perf_lookup_end.QuadPart -
                       dispatch_perf_begin.QuadPart),
            (uint64_t)(dispatch_perf_end.QuadPart -
                       dispatch_perf_lookup_end.QuadPart),
            (uint64_t)(dispatch_perf_end.QuadPart -
                       dispatch_perf_begin.QuadPart));
    }
#endif
    if (spu_job_trace_enabled()) {
        fprintf(stderr, "[spurs-job] job 0x%08X RETURNED rc=%d\n", job_ea, rc);
        fflush(stderr);
    }
    if (target_trace_claimed) {
        fprintf(stderr, "[spurs-job] TARGET WRAPPER_RETURN fp=0x%016llX image_id=%d job_ea=0x%08X rc=%d\n",
                (unsigned long long)fp, image_id, job_ea, rc);
        fflush(stderr);
    }
    if (tj_trace) {
        fprintf(stderr,
                "[TJ-LATE-JOB] seq=%lu tid=%lu phase=DISPATCH_END image=%d job=0x%08X fp=0x%016llX rc=%d\n",
                tj_late_job_trace_next_sequence(), tj_late_job_trace_thread_id(),
                image_id, job_ea, (unsigned long long)fp, rc);
        fflush(stderr);
    }
    if (image_id == 13)
        tj_late_eventflag_trace_note_image13_dispatch(job_ea, 0, 0);
    return 1;
}

/* ---- SPURS task signal channel (real WAIT_SIGNAL semantics) --------------
 *
 * Mirrors the kernel/RPCS3 contract (spursTasksetProcessSyscall):
 *   - _cellSpursSendSignal / cellSpursEventFlagSet mark the task's bit in the
 *     taskset's `signalled` bitset (BE, in the GUEST CellSpursTaskset) and
 *     wake it if sleeping.
 *   - The WAIT_SIGNAL taskset syscall consumes a pending signal, or sleeps
 *     the task until one arrives (POLL_SIGNAL-then-wait, no lost wakeups:
 *     the guest bit is set under the same lock the waiter checks it).
 * The bitset is guest state (SPU-visible); the host lock/condvar exist only
 * to block and wake the task's host thread. */
#include "../../libs/spurs/spurs_taskset.h"

#ifdef _WIN32
static SRWLOCK            s_sig_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_sig_cv;   /* zero-init == CONDITION_VARIABLE_INIT */
#else
static pthread_mutex_t s_sig_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_sig_cv   = PTHREAD_COND_INITIALIZER;
#endif

#define SPU_SIGNAL_TASKSET_MAX 64
typedef struct SpuSignalTaskset {
    uint32_t ea;
    uint8_t shutdown_requested;
    uint16_t pending[128];
    uint8_t task_exited[128];
    int32_t task_exit_code[128];
    uint64_t delivery_epoch[128];
    uint64_t consume_epoch[128];
    uint64_t ca9c_substitution_debt[128];
    uint8_t waiter_active[128];
    int32_t waiter_image_id[128];
    uint32_t waiter_return_pc[128];
    uint64_t waiter_started_ms[128];
    uint64_t generation;
    uint64_t retry_delivered_generation;
    uint64_t followup_retry_delivered_generation;
    uint32_t generation_owner_ea;
    uint32_t generation_count;
    int32_t generation_expected_image_id;
    uint32_t generation_completion_ea;
    uint8_t generation_create_ca9c_substitution_debt;
    uint32_t generation_task_ids[128];
    uint64_t generation_delivery_epoch[128];
    uint64_t generation_consume_epoch[128];
#ifdef _WIN32
    LONGLONG signal_tick[128];
#endif
} SpuSignalTaskset;

static SpuSignalTaskset s_signal_tasksets[SPU_SIGNAL_TASKSET_MAX];

enum { SPU_SIGNAL_SNAPSHOT_BITSETS = 5 };

typedef struct SpuSignalSnapshotTaskset {
    uint32_t ea;
    uint16_t pending[128];
    uint8_t waiter_active[128];
    int32_t waiter_image_id[128];
    uint32_t waiter_return_pc[128];
    uint64_t waiter_started_ms[128];
    uint32_t bits[SPU_SIGNAL_SNAPSHOT_BITSETS][4];
} SpuSignalSnapshotTaskset;

static int signal_snapshot_bit(const uint32_t bits[4], uint32_t task_id)
{
    return (bits[task_id / 32u] & ((1u << 31) >> (task_id % 32u))) != 0;
}

#ifdef _WIN32
typedef struct SpuTaskPerf {
    uint64_t samples;
    uint64_t run_ticks;
    uint64_t block_ticks;
    uint64_t wake_ticks;
    uint64_t wake_samples;
    uint64_t max_run_ticks;
    uint64_t max_block_ticks;
    uint64_t max_wake_ticks;
} SpuTaskPerf;

enum { SPU_TASK_PERF_SLOTS = 32, SPU_TASK_PERF_REPORT_WAITS = 256 };

typedef struct SpuTaskPerfRow {
    int image_id;
    uint32_t taskset_ea;
    uint32_t task_id;
    uint32_t wait_return_pc;
    SpuTaskPerf perf;
} SpuTaskPerfRow;

typedef struct SpuTaskPerfTable {
    uint64_t completed_waits;
    uint64_t overflow_waits;
    SpuTaskPerfRow rows[SPU_TASK_PERF_SLOTS];
    SpuTaskPerf overflow;
} SpuTaskPerfTable;

static LARGE_INTEGER s_task_perf_frequency;
static SpuTaskPerfTable s_task_perf;
static _Thread_local LONGLONG s_task_perf_last_exit;
static INIT_ONCE s_task_perf_init_once = INIT_ONCE_STATIC_INIT;
static volatile LONG s_task_perf_enabled = -1;
static _Thread_local int s_task_perf_enabled_tls = -1;

static BOOL CALLBACK task_perf_init_once(PINIT_ONCE once, PVOID parameter,
                                         PVOID* context)
{
    (void)once;
    (void)parameter;
    (void)context;
    const char* value = getenv("TJ_TASK_PERF");
    int enabled = value && value[0] && strcmp(value, "0") != 0;
    if (enabled) QueryPerformanceFrequency(&s_task_perf_frequency);
    /* Publish only after frequency is initialized. */
    InterlockedExchange(&s_task_perf_enabled, enabled ? 1 : 0);
    return TRUE;
}

static int task_perf_enabled(void)
{
    int cached = s_task_perf_enabled_tls;
    if (cached >= 0) return cached;
    LONG enabled = InterlockedCompareExchange(&s_task_perf_enabled, 0, 0);
    if (enabled < 0) {
        InitOnceExecuteOnce(&s_task_perf_init_once, task_perf_init_once, NULL, NULL);
        enabled = InterlockedCompareExchange(&s_task_perf_enabled, 0, 0);
    }
    cached = enabled > 0;
    s_task_perf_enabled_tls = cached;
    return cached;
}

static double task_perf_ms(uint64_t ticks)
{
    return s_task_perf_frequency.QuadPart
        ? (double)ticks * 1000.0 / (double)s_task_perf_frequency.QuadPart
        : 0.0;
}

static void task_perf_add(SpuTaskPerf* p, uint64_t run_ticks,
                          uint64_t block_ticks, uint64_t wake_ticks,
                          int have_wake)
{
    p->samples++;
    p->run_ticks += run_ticks;
    p->block_ticks += block_ticks;
    if (have_wake) {
        p->wake_ticks += wake_ticks;
        p->wake_samples++;
    }
    if (run_ticks > p->max_run_ticks) p->max_run_ticks = run_ticks;
    if (block_ticks > p->max_block_ticks) p->max_block_ticks = block_ticks;
    if (have_wake && wake_ticks > p->max_wake_ticks) p->max_wake_ticks = wake_ticks;
}

static void task_perf_print_row(const char* prefix, const SpuTaskPerf* p)
{
    fprintf(stderr,
            "%s samples=%llu run_total_ms=%.4f run_avg_ms=%.4f run_max_ms=%.4f "
            "block_total_ms=%.4f block_avg_ms=%.4f block_max_ms=%.4f "
            "wake_total_ms=%.4f wake_avg_ms=%.4f wake_max_ms=%.4f wake_samples=%llu\n",
            prefix, (unsigned long long)p->samples,
            task_perf_ms(p->run_ticks),
            p->samples ? task_perf_ms(p->run_ticks) / (double)p->samples : 0.0,
            task_perf_ms(p->max_run_ticks),
            task_perf_ms(p->block_ticks),
            p->samples ? task_perf_ms(p->block_ticks) / (double)p->samples : 0.0,
            task_perf_ms(p->max_block_ticks),
            task_perf_ms(p->wake_ticks),
            p->wake_samples ? task_perf_ms(p->wake_ticks) / (double)p->wake_samples : 0.0,
            task_perf_ms(p->max_wake_ticks), (unsigned long long)p->wake_samples);
}

/* Called with s_sig_lock held. Snapshot/reset is lock-protected; printing is
 * deliberately deferred until after the lock is released. */
static void task_perf_record_locked(uint32_t taskset_ea, uint32_t task_id,
                                    int image_id, uint32_t wait_return_pc,
                                    uint64_t run_ticks, uint64_t block_ticks,
                                    uint64_t wake_ticks, int have_wake,
                                    SpuTaskPerfTable* report_out,
                                    int* have_report)
{
    SpuTaskPerfTable* table = &s_task_perf;
    SpuTaskPerfRow* row = NULL;
    for (unsigned i = 0; i < SPU_TASK_PERF_SLOTS; ++i) {
        SpuTaskPerfRow* candidate = &table->rows[i];
        if (candidate->perf.samples && candidate->image_id == image_id &&
            candidate->taskset_ea == taskset_ea && candidate->task_id == task_id &&
            candidate->wait_return_pc == wait_return_pc) {
            row = candidate;
            break;
        }
        if (!row && !candidate->perf.samples) row = candidate;
    }
    if (row) {
        if (!row->perf.samples) {
            row->image_id = image_id;
            row->taskset_ea = taskset_ea;
            row->task_id = task_id;
            row->wait_return_pc = wait_return_pc;
        }
        task_perf_add(&row->perf, run_ticks, block_ticks, wake_ticks, have_wake);
    } else {
        table->overflow_waits++;
        task_perf_add(&table->overflow, run_ticks, block_ticks, wake_ticks, have_wake);
    }
    table->completed_waits++;
    if (table->completed_waits >= SPU_TASK_PERF_REPORT_WAITS) {
        *report_out = *table;
        *have_report = 1;
        memset(table, 0, sizeof(*table));
    }
}

static void task_perf_print_report(const SpuTaskPerfTable* table)
{
    unsigned occupied = 0;
    for (unsigned i = 0; i < SPU_TASK_PERF_SLOTS; ++i)
        if (table->rows[i].perf.samples) occupied++;
    fprintf(stderr, "[TJ-TASK-PERF] completed=%llu rows=%u overflow=%llu\n",
            (unsigned long long)table->completed_waits, occupied,
            (unsigned long long)table->overflow_waits);
    for (unsigned i = 0; i < SPU_TASK_PERF_SLOTS; ++i) {
        const SpuTaskPerfRow* candidate = &table->rows[i];
        if (!candidate->perf.samples) continue;
        char prefix[160];
        snprintf(prefix, sizeof(prefix),
                 "[TJ-TASK-PERF] image=%d taskset=0x%08X task=%u return-pc=0x%05X",
                 candidate->image_id, candidate->taskset_ea, candidate->task_id,
                 candidate->wait_return_pc);
        task_perf_print_row(prefix, &candidate->perf);
    }
    task_perf_print_row("[TJ-TASK-PERF] overflow", &table->overflow);
    fflush(stderr);
}
#endif

/* Per persistent-task host thread.  Instruction tracing can skip expensive
 * startup/first-work passes and arm on a later WAIT_SIGNAL return. */
_Thread_local uint32_t g_spu_trace_wait_count;

static int signal_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("SPU_SIGNAL_TRACE") ? 1 : 0;
    return enabled;
}

/* Bounded provenance for registered Survival tasksets. This is kept
 * separate from SPU_SIGNAL_TRACE: it arms only at the title's scene boundary
 * and never changes a pending bit, reservation, wake, or generation. */
enum { TJ_SIGNAL_TRACE_CAP = 64 };
static volatile long s_tj_signal_trace_enabled = -1;
static volatile long s_tj_signal_trace_armed;
static volatile long s_tj_signal_trace_count;

static long tj_signal_trace_load(volatile long* value)
{
#ifdef _WIN32
    return _InterlockedCompareExchange(value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static void tj_signal_trace_init_enabled(void)
{
    if (tj_signal_trace_load(&s_tj_signal_trace_enabled) >= 0) return;
    long requested = getenv("TJ_SPU_SIGNAL_TRACE") ? 1 : 0;
#ifdef _WIN32
    _InterlockedCompareExchange(&s_tj_signal_trace_enabled, requested, -1);
#else
    __sync_val_compare_and_swap(&s_tj_signal_trace_enabled, -1, requested);
#endif
}

static long tj_signal_trace_next_sequence(void)
{
#ifdef _WIN32
    return _InterlockedIncrement(&s_tj_signal_trace_count);
#else
    return __sync_add_and_fetch(&s_tj_signal_trace_count, 1);
#endif
}

static int tj_signal_trace_enabled(uint32_t taskset_ea)
{
    tj_signal_trace_init_enabled();
    long enabled = tj_signal_trace_load(&s_tj_signal_trace_enabled);
    return enabled > 0 &&
           tj_signal_trace_load(&s_tj_signal_trace_armed) != 0 &&
           taskset_ea != 0;
}

static void tj_signal_trace(uint32_t taskset_ea, uint32_t task_id,
                            const char* phase, const char* source,
                            uint16_t pending_before, uint16_t pending_after,
                            int guest_bit, uint64_t generation,
                            uint64_t debt_before, uint64_t debt_after)
{
    if (!tj_signal_trace_enabled(taskset_ea)) return;
    long sequence = tj_signal_trace_next_sequence();
    if (sequence > TJ_SIGNAL_TRACE_CAP) return;
    fprintf(stderr,
            "[TJ-SIGNAL-TRACE] seq=%ld phase=%s source=%s taskset=0x%08X "
            "task=%u pending=%u->%u guest=%d generation=%llu debt=%llu->%llu\n",
            sequence, phase, source, taskset_ea, task_id,
            pending_before, pending_after, guest_bit,
            (unsigned long long)generation,
            (unsigned long long)debt_before,
            (unsigned long long)debt_after);
}

void spu_taskset_signal_trace_arm_after_scene(void)
{
    tj_signal_trace_init_enabled();
    if (tj_signal_trace_load(&s_tj_signal_trace_enabled) > 0) {
#ifdef _WIN32
        _InterlockedExchange(&s_tj_signal_trace_armed, 1);
#else
        __atomic_store_n(&s_tj_signal_trace_armed, 1, __ATOMIC_RELEASE);
#endif
    }
}

/* s_sig_lock must be held. Fixed storage keeps waiter pointers stable. */
static SpuSignalTaskset* signal_taskset_find_locked(uint32_t taskset_ea)
{
    for (unsigned i = 0; i < SPU_SIGNAL_TASKSET_MAX; i++)
        if (s_signal_tasksets[i].ea == taskset_ea)
            return &s_signal_tasksets[i];
    return NULL;
}

static int signal_copy_unique_task_ids(uint32_t task_ids_ea, uint32_t count,
                                       uint32_t task_ids[128])
{
    if (!task_ids_ea || !count || count > 128) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t task_id = vm_read32(task_ids_ea + i * 4u);
        if (task_id >= 128) return 0;
        for (uint32_t j = 0; j < i; ++j)
            if (task_ids[j] == task_id) return 0;
        task_ids[i] = task_id;
    }
    return 1;
}

/* Serialize host taskset-bit access with SPU GETLLAR/PUTLLC and invalidate any
 * stale reservation whenever host code changes the shared 128-byte line. */
static int signal_guest_bit_test_reservation(uint32_t taskset_ea,
                                              uint32_t task_id)
{
    spu_reservation_lock();
    int set = spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, task_id);
    spu_reservation_unlock();
    return set;
}

static int signal_guest_bit_set_reservation(uint32_t taskset_ea,
                                             uint32_t task_id)
{
    spu_reservation_lock();
    int was_set = spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, task_id);
    if (!was_set) {
        spurs_bitset_set(taskset_ea + CSTS_SIGNALLED, task_id);
        spu_reservation_bump_locked(taskset_ea);
    }
    spu_reservation_unlock();
    return was_set;
}

static void signal_guest_bit_clear_reservation(uint32_t taskset_ea,
                                                uint32_t task_id)
{
    spu_reservation_lock();
    if (spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, task_id)) {
        spurs_bitset_clear(taskset_ea + CSTS_SIGNALLED, task_id);
        spu_reservation_bump_locked(taskset_ea);
    }
    spu_reservation_unlock();
}

static void signal_pending_add_locked(SpuSignalTaskset* state, uint32_t taskId)
{
    if (state)
        state->pending[taskId] =
            spu_task_signal_merge_pending(state->pending[taskId]);
}

void spu_taskset_register_signal_state(uint32_t taskset_ea)
{
    if (!taskset_ea) return;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    if (!state) {
        for (unsigned i = 0; i < SPU_SIGNAL_TASKSET_MAX; i++) {
            if (!s_signal_tasksets[i].ea) {
                state = &s_signal_tasksets[i];
                break;
            }
        }
    }
    if (state) {
        memset(state, 0, sizeof(*state));
        state->ea = taskset_ea;
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_unlock(&s_sig_lock);
#endif
}

void spu_taskset_unregister_signal_state(uint32_t taskset_ea)
{
    if (!taskset_ea) return;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    if (state) memset(state, 0, sizeof(*state));
#ifdef _WIN32
    WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
}

void spu_taskset_mark_task_started(uint32_t taskset_ea, uint32_t taskId)
{
    if (!taskset_ea || taskId >= 128) return;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    if (state) {
        state->task_exited[taskId] = 0;
        state->task_exit_code[taskId] = 0;
        state->pending[taskId] = 0;
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_unlock(&s_sig_lock);
#endif
}

void spu_taskset_mark_task_exit(uint32_t taskset_ea, uint32_t taskId,
                                int32_t exit_code)
{
    if (!taskset_ea || taskId >= 128) return;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    if (state) {
        state->task_exited[taskId] = 1;
        state->task_exit_code[taskId] = exit_code;
        state->pending[taskId] = 0;
        state->waiter_active[taskId] = 0;
    }

    /* EXIT removes the task from every scheduler set. Keep the complete
     * 128-byte taskset header update atomic with SPU reservations so a PM
     * GETLLAR/PUTLLC cannot publish stale RUNNING/ENABLED state afterward. */
    spu_reservation_lock();
    static const uint32_t bitsets[] = {
        CSTS_RUNNING, CSTS_READY, CSTS_PENDING_READY, CSTS_ENABLED,
        CSTS_SIGNALLED, CSTS_WAITING,
    };
    for (unsigned i = 0; i < sizeof(bitsets) / sizeof(bitsets[0]); ++i)
        spurs_bitset_clear(taskset_ea + bitsets[i], taskId);
    spu_reservation_bump_locked(taskset_ea);
    spu_reservation_unlock();

#ifdef _WIN32
    WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
}

int spu_taskset_wait_task_exit(uint32_t taskset_ea, uint32_t taskId,
                               int32_t* exit_code)
{
    if (!taskset_ea || taskId >= 128) return -1;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    while (state && state->ea && !state->task_exited[taskId])
        SleepConditionVariableSRW(&s_sig_cv, &s_sig_lock, INFINITE, 0);
    if (!state || !state->ea) {
        ReleaseSRWLockExclusive(&s_sig_lock);
        return -1;
    }
    if (exit_code) *exit_code = state->task_exit_code[taskId];
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    while (state && state->ea && !state->task_exited[taskId])
        pthread_cond_wait(&s_sig_cv, &s_sig_lock);
    if (!state || !state->ea) {
        pthread_mutex_unlock(&s_sig_lock);
        return -1;
    }
    if (exit_code) *exit_code = state->task_exit_code[taskId];
    pthread_mutex_unlock(&s_sig_lock);
#endif
    return 0;
}

int spu_taskset_request_shutdown(uint32_t taskset_ea)
{
    if (!taskset_ea) return -1;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    if (!state) {
#ifdef _WIN32
        ReleaseSRWLockExclusive(&s_sig_lock);
#else
        pthread_mutex_unlock(&s_sig_lock);
#endif
        return -1;
    }
    state->shutdown_requested = 1;
#ifdef _WIN32
    WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    return 0;
}

static int signal_taskset_has_enabled_tasks(uint32_t taskset_ea)
{
    int enabled = 0;
    spu_reservation_lock();
    for (unsigned word = 0; word < 4; ++word)
        enabled |= vm_read32(taskset_ea + CSTS_ENABLED + word * 4u) != 0;
    spu_reservation_unlock();
    return enabled;
}

int spu_taskset_wait_all_tasks_exit(uint32_t taskset_ea)
{
    if (!taskset_ea) return -1;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    while (state && state->ea && signal_taskset_has_enabled_tasks(taskset_ea))
        SleepConditionVariableSRW(&s_sig_cv, &s_sig_lock, INFINITE, 0);
    int ok = state && state->ea;
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    while (state && state->ea && signal_taskset_has_enabled_tasks(taskset_ea))
        pthread_cond_wait(&s_sig_cv, &s_sig_lock);
    int ok = state && state->ea;
    pthread_mutex_unlock(&s_sig_lock);
#endif
    return ok ? 0 : -1;
}

void spu_taskset_signal_snapshot(FILE* out)
{
    extern unsigned long long ps3_ms_now(void);
    static const uint32_t bitset_offsets[SPU_SIGNAL_SNAPSHOT_BITSETS] = {
        CSTS_RUNNING, CSTS_READY, CSTS_PENDING_READY, CSTS_SIGNALLED,
        CSTS_WAITING,
    };
    SpuSignalSnapshotTaskset* snapshot = (SpuSignalSnapshotTaskset*)calloc(
        SPU_SIGNAL_TASKSET_MAX, sizeof(*snapshot));
    unsigned count = 0;
    if (!out) out = stderr;
    if (!snapshot) {
        fprintf(out, "[spu-signal-snapshot] error=allocation-failed\n");
        fflush(out);
        return;
    }

#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    for (unsigned i = 0; i < SPU_SIGNAL_TASKSET_MAX; i++) {
        const SpuSignalTaskset* state = &s_signal_tasksets[i];
        if (!state->ea) continue;
        SpuSignalSnapshotTaskset* row = &snapshot[count++];
        row->ea = state->ea;
        memcpy(row->pending, state->pending, sizeof(row->pending));
        memcpy(row->waiter_active, state->waiter_active,
               sizeof(row->waiter_active));
        memcpy(row->waiter_image_id, state->waiter_image_id,
               sizeof(row->waiter_image_id));
        memcpy(row->waiter_return_pc, state->waiter_return_pc,
               sizeof(row->waiter_return_pc));
        memcpy(row->waiter_started_ms, state->waiter_started_ms,
               sizeof(row->waiter_started_ms));
        /* SPU PUTLLC commits this 128-byte taskset line under the reservation
         * lock. Pair with it so the five guest bitsets come from one state. */
        spu_reservation_lock();
        for (unsigned bitset = 0; bitset < SPU_SIGNAL_SNAPSHOT_BITSETS;
             bitset++) {
            for (unsigned word = 0; word < 4; word++) {
                row->bits[bitset][word] = vm_read32(
                    state->ea + bitset_offsets[bitset] + word * 4u);
            }
        }
        spu_reservation_unlock();
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_unlock(&s_sig_lock);
#endif

    unsigned long long now_ms = ps3_ms_now();
    fprintf(out, "[spu-signal-snapshot] tasksets=%u\n", count);
    for (unsigned i = 0; i < count; i++) {
        const SpuSignalSnapshotTaskset* row = &snapshot[i];
        unsigned active_tasks = 0;
        for (uint32_t task_id = 0; task_id < 128; task_id++) {
            active_tasks += row->pending[task_id] != 0 ||
                row->waiter_active[task_id] ||
                signal_snapshot_bit(row->bits[0], task_id) ||
                signal_snapshot_bit(row->bits[1], task_id) ||
                signal_snapshot_bit(row->bits[2], task_id) ||
                signal_snapshot_bit(row->bits[3], task_id) ||
                signal_snapshot_bit(row->bits[4], task_id);
        }
        fprintf(out, "[spu-signal-snapshot] taskset=0x%08X active_tasks=%u\n",
                row->ea, active_tasks);
        for (uint32_t task_id = 0; task_id < 128; task_id++) {
            if (row->pending[task_id] == 0 &&
                !row->waiter_active[task_id] &&
                !signal_snapshot_bit(row->bits[0], task_id) &&
                !signal_snapshot_bit(row->bits[1], task_id) &&
                !signal_snapshot_bit(row->bits[2], task_id) &&
                !signal_snapshot_bit(row->bits[3], task_id) &&
                !signal_snapshot_bit(row->bits[4], task_id)) {
                continue;
            }
            fprintf(out,
                    "[spu-signal-snapshot] taskset=0x%08X task=%u "
                    "pending=%u running=%u ready=%u pending_ready=%u "
                    "signalled=%u waiting=%u waiter_active=%u image=%d "
                    "return_pc=0x%05X wait_ms=%llu\n",
                    row->ea, task_id, row->pending[task_id],
                    signal_snapshot_bit(row->bits[0], task_id),
                    signal_snapshot_bit(row->bits[1], task_id),
                    signal_snapshot_bit(row->bits[2], task_id),
                    signal_snapshot_bit(row->bits[3], task_id),
                    signal_snapshot_bit(row->bits[4], task_id),
                    row->waiter_active[task_id],
                    row->waiter_image_id[task_id],
                    row->waiter_return_pc[task_id],
                    row->waiter_active[task_id] &&
                            now_ms >= row->waiter_started_ms[task_id]
                        ? now_ms - row->waiter_started_ms[task_id] : 0ull);
        }
    }
    fflush(out);
    free(snapshot);
}

void spu_taskset_putllc_begin(void)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    spu_reservation_lock();
}

/* Called while both locks from spu_taskset_putllc_begin are held. Only newly
 * set SIGNALLED bits are deliveries. The waiter cannot consume the committed
 * guest bit before this host-shadow publication. */
void spu_taskset_putllc_end(uint32_t taskset_ea,
                           const uint8_t before[128],
                           const uint8_t after[128], int success)
{
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    int delivered = 0;
    if (success && taskset_ea && before && after && state) {
        for (uint32_t byte = 0; byte < 16; byte++) {
            uint8_t added = (uint8_t)(after[CSTS_SIGNALLED + byte] &
                                      (uint8_t)~before[CSTS_SIGNALLED + byte]);
            for (uint32_t bit = 0; bit < 8; bit++) {
                if (added & (uint8_t)(0x80u >> bit)) {
                    uint32_t task_id = byte * 8 + bit;
                    uint16_t before_pending = state->pending[task_id];
                    int guest_bit_set =
                        spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, task_id);
                    /* `added` came from this successful PUTLLC before/after
                     * pair. Publish it to the host shadow before either lock
                     * is released; keep the shadow level-triggered. */
                    state->pending[task_id] =
                        spu_task_signal_merge_observed_edge(
                            state->pending[task_id], 1);
                    if (before_pending == 0)
                        ++state->delivery_epoch[task_id];
#ifdef _WIN32
                    if (task_perf_enabled() && before_pending == 0) {
                        LARGE_INTEGER now;
                        QueryPerformanceCounter(&now);
                        state->signal_tick[task_id] = now.QuadPart;
                    }
#endif
                    if (signal_trace_enabled())
                        fprintf(stderr,
                                "[spu-signal] source=spu-atomic taskset=0x%08X "
                                "task=%u added=1 guest=%d pending=%u->%u "
                                "epoch=%llu\n",
                                taskset_ea, task_id, guest_bit_set,
                                before_pending, state->pending[task_id],
                                (unsigned long long)
                                    state->delivery_epoch[task_id]);
                    tj_signal_trace(taskset_ea, task_id, "putllc",
                                    "spu-atomic", before_pending,
                                    state->pending[task_id], guest_bit_set,
                                    state->generation, 0, 0);
                    delivered = 1;
                }
            }
        }
    }
#ifdef _WIN32
    spu_reservation_unlock();
    if (delivered) WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    spu_reservation_unlock();
    if (delivered) pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
}

/* s_sig_lock must be held. Shared by generic HLE and exact CA9C delivery. */
static void spu_taskset_signal_task_locked(SpuSignalTaskset* state,
                                           uint32_t taskset_ea,
                                           uint32_t taskId,
                                           const char* source)
{
    uint16_t before_pending = state ? state->pending[taskId] : 0;
    int guest_before = signal_guest_bit_set_reservation(taskset_ea, taskId);
    signal_pending_add_locked(state, taskId);
    if (state && before_pending == 0 && !guest_before)
        ++state->delivery_epoch[taskId];
#ifdef _WIN32
    if (state && task_perf_enabled() && before_pending == 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        state->signal_tick[taskId] = now.QuadPart;
    }
#endif
    if (signal_trace_enabled())
        fprintf(stderr,
                "[spu-signal] source=%s taskset=0x%08X task=%u "
                "pending=%u->%u generation=%llu\n",
                source, taskset_ea, taskId, before_pending,
                state ? state->pending[taskId] : 0,
                (unsigned long long)(state ? state->generation : 0));
    tj_signal_trace(taskset_ea, taskId, "enqueue", source, before_pending,
                    state ? state->pending[taskId] : 0,
                    signal_guest_bit_test_reservation(taskset_ea, taskId),
                    state ? state->generation : 0, 0, 0);
}

static void spu_taskset_signal_task_impl(uint32_t taskset_ea, uint32_t taskId)
{
    if (!taskset_ea || taskId >= 128) return;
    int delivered = 0;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    spu_taskset_signal_task_locked(state, taskset_ea, taskId, "ppu");
    delivered = 1;
#ifdef _WIN32
    if (delivered) WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    if (delivered) pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    if (delivered) { static int _n = 0; if (_n++ < 24)
        fprintf(stderr, "[spu_workload] signal task %u (taskset 0x%08X)\n",
                taskId, taskset_ea); fflush(stderr); }
}

/* Generic HLE/event delivery. */
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId)
{
    spu_taskset_signal_task_impl(taskset_ea, taskId);
}

uint64_t spu_taskset_arm_signal_generation(uint32_t owner_ea,
                                            uint32_t taskset_ea,
                                            uint32_t task_ids_ea,
                                            uint32_t count,
                                            int expected_image_id,
                                            uint32_t completion_ea,
                                            int create_ca9c_substitution_debt)
{
    uint32_t task_ids[128];
    uint64_t generation = 0;
    if (!owner_ea || !taskset_ea || !completion_ea || expected_image_id < 0 ||
        !signal_copy_unique_task_ids(task_ids_ea, count, task_ids)) return 0;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    if (state) {
        generation = ++state->generation;
        if (!generation) generation = ++state->generation;
        state->retry_delivered_generation = 0;
        state->followup_retry_delivered_generation = 0;
        state->generation_owner_ea = owner_ea;
        state->generation_count = count;
        state->generation_expected_image_id = expected_image_id;
        state->generation_completion_ea = completion_ea;
        state->generation_create_ca9c_substitution_debt =
            create_ca9c_substitution_debt != 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t task_id = task_ids[i];
            state->generation_task_ids[i] = task_id;
            state->generation_delivery_epoch[i] =
                state->delivery_epoch[task_id];
            state->generation_consume_epoch[i] =
                state->consume_epoch[task_id];
        }
    }
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_unlock(&s_sig_lock);
#endif
    return generation;
}

/* Tokyo Jungle normally kicks this manager batch through an asynchronous
 * F5AAC0DE callback which reaches CA9C. That callback can be delayed or lost
 * by the recompiled host queue while the persistent image-15 tasks are
 * already parked in WAIT_SIGNAL. Publish the complete batch as one atomic
 * signal-state operation, and record one substitution per task so a late
 * real CA9C callback cannot run the same batch twice. */
uint64_t spu_taskset_dispatch_signal_generation(
    uint32_t owner_ea, uint32_t taskset_ea, uint32_t task_ids_ea,
    uint32_t count, int expected_image_id, uint32_t completion_ea,
    int create_ca9c_substitution_debt)
{
    uint32_t task_ids[128];
    uint64_t generation = 0;
    if (!owner_ea || !taskset_ea || !completion_ea || expected_image_id < 0 ||
        !signal_copy_unique_task_ids(task_ids_ea, count, task_ids)) return 0;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    if (state) {
        generation = ++state->generation;
        if (!generation) generation = ++state->generation;
        state->retry_delivered_generation = generation;
        state->followup_retry_delivered_generation = 0;
        state->generation_owner_ea = owner_ea;
        state->generation_count = count;
        state->generation_expected_image_id = expected_image_id;
        state->generation_completion_ea = completion_ea;
        state->generation_create_ca9c_substitution_debt =
            create_ca9c_substitution_debt != 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t task_id = task_ids[i];
            state->generation_task_ids[i] = task_id;
            state->generation_delivery_epoch[i] =
                state->delivery_epoch[task_id];
            state->generation_consume_epoch[i] =
                state->consume_epoch[task_id];
            spu_taskset_signal_task_locked(
                state, taskset_ea, task_id, "manager-dispatch");
            if (create_ca9c_substitution_debt) {
                state->ca9c_substitution_debt[task_id] =
                    spu_task_signal_add_ca9c_substitution_debt(
                        state->ca9c_substitution_debt[task_id]);
            }
        }
    } else {
        /* An unregistered taskset has no generation/debt state. Do not inject
         * raw signals here: the later CA9C callback remains its sole delivery
         * owner, preventing an unpaired synthetic wake or double delivery. */
    }
#ifdef _WIN32
    WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    return generation;
}

uint64_t spu_taskset_current_signal_generation(uint32_t owner_ea,
                                                uint32_t taskset_ea)
{
    uint64_t generation = 0;
    if (!owner_ea || !taskset_ea) return 0;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    if (state && state->generation_owner_ea == owner_ea)
        generation = state->generation;
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_unlock(&s_sig_lock);
#endif
    return generation;
}

/* Exact generated CA9C task send. Under one signal lock it either consumes a
 * recovery-created debt or performs the normal HLE delivery itself. */
int spu_taskset_consume_ca9c_substitution(
    uint32_t taskset_ea, uint32_t taskId)
{
    int handled = 0;
    int delivered = 0;
    if (!taskset_ea || taskId >= 128) return 0;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    uint64_t debt_before = state ? state->ca9c_substitution_debt[taskId] : 0;
    if (state) {
        handled = 1;
        if (spu_task_signal_ca9c_action(debt_before) ==
            SPU_CA9C_ACTION_CONSUME_DEBT) {
            state->ca9c_substitution_debt[taskId] =
                spu_task_signal_consume_ca9c_substitution_debt(debt_before);
        } else {
            spu_taskset_signal_task_locked(state, taskset_ea, taskId, "ca9c");
            delivered = 1;
        }
    }
    if (signal_trace_enabled() && handled && debt_before != 0)
        fprintf(stderr,
                "[spu-signal] source=ca9c-substitution taskset=0x%08X "
                "task=%u generation=%llu debt=%llu->%llu\n",
                taskset_ea, taskId,
                (unsigned long long)(state ? state->generation : 0),
                (unsigned long long)debt_before,
                (unsigned long long)(state
                    ? state->ca9c_substitution_debt[taskId] : 0));
    if (handled) {
        const uint64_t debt_after = state->ca9c_substitution_debt[taskId];
        tj_signal_trace(taskset_ea, taskId,
                        debt_before ? "ca9c-substitute" : "ca9c-normal",
                        "ca9c", state->pending[taskId], state->pending[taskId],
                        signal_guest_bit_test_reservation(taskset_ea, taskId),
                        state->generation, debt_before, debt_after);
    }
#ifdef _WIN32
    if (delivered) WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    if (delivered) pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    return handled;
}

/* s_sig_lock must be held. Atomically validate and deliver one waiter-owned
 * fallback batch for the exact C904 generation. */
static int spu_taskset_retry_signal_generation_locked(
    SpuSignalTaskset* state, uint32_t owner_ea, uint32_t taskset_ea,
    const uint32_t task_ids[128], uint32_t count, int expected_image_id,
    uint32_t completion_ea, uint64_t generation,
    int create_ca9c_substitution_debt, int followup_recovery)
{
    int result = 0;
    if (!state || !owner_ea || !taskset_ea || !completion_ea || !generation ||
        !task_ids || count == 0 || count > 128) return -1;
    int can_claim = state && (followup_recovery
        ? spu_task_signal_generation_followup_can_claim(
              state->generation, generation,
              state->followup_retry_delivered_generation)
        : spu_task_signal_generation_can_claim(
              state->generation, generation,
              state->retry_delivered_generation));
    int metadata_matches = state->generation_owner_ea == owner_ea &&
        state->generation_count == count &&
        state->generation_expected_image_id == expected_image_id &&
        state->generation_completion_ea == completion_ea &&
        spu_task_signal_generation_retry_policy_matches(
            state->generation_create_ca9c_substitution_debt,
            create_ca9c_substitution_debt, followup_recovery);
    if (!metadata_matches || !can_claim) {
        result = -1;
    } else {
        int all_ready = 1;
        /* One global reservation lock covers the completion line and taskset
         * line, so an SPU completion PUTLLC cannot win between the zero check
         * and fallback delivery, and taskset PUTLLC cannot overwrite the bits. */
        spu_reservation_lock();
        if (vm_read32(completion_ea) != 0) {
            result = -1;
            all_ready = 0;
        }
        for (uint32_t i = 0; i < count; ++i) {
            if (!all_ready) break;
            uint32_t task_id = task_ids[i];
            uint64_t delivery_epoch = state->delivery_epoch[task_id];
            uint64_t consume_epoch = state->consume_epoch[task_id];
            uint64_t armed_delivery_epoch =
                state->generation_delivery_epoch[i];
            uint64_t armed_consume_epoch =
                state->generation_consume_epoch[i];
            int followup_epoch_state = followup_recovery
                ? spu_task_signal_generation_followup_epoch_state(
                    delivery_epoch, armed_delivery_epoch,
                    consume_epoch, armed_consume_epoch)
                : 1;
            int epochs_match = followup_recovery
                ? followup_epoch_state == 1
                : delivery_epoch == armed_delivery_epoch &&
                  consume_epoch == armed_consume_epoch;
            if (state->generation_task_ids[i] != task_id) {
                result = -1;
                all_ready = 0;
                break;
            }
            if (followup_recovery && followup_epoch_state == 0) {
                result = 0;
                all_ready = 0;
                break;
            }
            if (!epochs_match) {
                result = -1;
                all_ready = 0;
                break;
            }
            int candidate = followup_recovery
                ? spu_task_signal_generation_followup_retry_candidate(
                    state->pending[task_id],
                    spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, task_id),
                    state->waiter_active[task_id],
                    state->waiter_image_id[task_id], expected_image_id,
                    delivery_epoch, armed_delivery_epoch,
                    consume_epoch, armed_consume_epoch)
                : spu_task_signal_generation_retry_candidate(
                    state->pending[task_id],
                    spurs_bitset_test(taskset_ea + CSTS_SIGNALLED, task_id),
                    state->waiter_active[task_id],
                    state->waiter_image_id[task_id], expected_image_id,
                    delivery_epoch, armed_delivery_epoch,
                    consume_epoch, armed_consume_epoch);
            if (!candidate) {
                result = 0;
                all_ready = 0;
                break;
            }
        }
        if (spu_task_signal_generation_retry_batch_ready(
                state->generation, generation,
                followup_recovery
                    ? state->followup_retry_delivered_generation
                    : state->retry_delivered_generation,
                metadata_matches, all_ready,
                vm_read32(completion_ea) == 0)) {
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t task_id = task_ids[i];
                signal_pending_add_locked(state, task_id);
                spurs_bitset_set(taskset_ea + CSTS_SIGNALLED, task_id);
                ++state->delivery_epoch[task_id];
                if (create_ca9c_substitution_debt && !followup_recovery) {
                    state->ca9c_substitution_debt[task_id] =
                        spu_task_signal_add_ca9c_substitution_debt(
                            state->ca9c_substitution_debt[task_id]);
                }
#ifdef _WIN32
                if (task_perf_enabled()) {
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    state->signal_tick[task_id] = now.QuadPart;
                }
#endif
            }
            spu_reservation_bump_locked(taskset_ea);
            if (followup_recovery)
                state->followup_retry_delivered_generation = generation;
            else
                state->retry_delivered_generation = generation;
            result = 1;
        } else if (all_ready) {
            result = -1;
        }
        spu_reservation_unlock();
    }
    return result;
}

/* s_sig_lock must be held. This is the same opt-in experiment as the PPU
 * completion-wait recovery, not a normal WAIT_SIGNAL delivery source. A new
 * C904 descriptor may already be armed while the graphics callback that owns
 * its wake is still behind an earlier label acquire. Waking parked workers
 * here would let that batch overwrite the label before the GPU consumes it. */
static int spu_taskset_retry_current_generation_locked(
    SpuSignalTaskset* state, uint32_t taskset_ea)
{
    static int recovery = -1;
    if (recovery < 0) recovery = getenv("TJ_SPU_COMPLETION_WAIT_RECOVERY") ? 1 : 0;
    if (!recovery) return -1;
    if (!state || state->ea != taskset_ea || !state->generation ||
        !state->generation_owner_ea || !state->generation_completion_ea ||
        state->generation_count == 0 || state->generation_count > 128 ||
        state->generation_expected_image_id < 0) return -1;
    return spu_taskset_retry_signal_generation_locked(
        state, state->generation_owner_ea, taskset_ea,
        state->generation_task_ids, state->generation_count,
        state->generation_expected_image_id, state->generation_completion_ea,
        state->generation,
        state->generation_create_ca9c_substitution_debt, 0);
}

/* Public retry keeps the established argument validation, diagnostics, and
 * wake semantics. The wait-entry recovery reuses the locked helper above. */
int spu_taskset_retry_signal_generation(uint32_t owner_ea,
                                         uint32_t taskset_ea,
                                         uint32_t task_ids_ea,
                                         uint32_t count,
                                         int expected_image_id,
                                         uint32_t completion_ea,
                                         uint64_t generation,
                                         int create_ca9c_substitution_debt,
                                         int followup_recovery)
{
    uint32_t task_ids[128];
    if (!owner_ea || !taskset_ea || !completion_ea || !generation ||
        !signal_copy_unique_task_ids(task_ids_ea, count, task_ids)) return -1;
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
#endif
    SpuSignalTaskset* state = signal_taskset_find_locked(taskset_ea);
    int result = spu_taskset_retry_signal_generation_locked(
        state, owner_ea, taskset_ea, task_ids, count, expected_image_id,
        completion_ea, generation, create_ca9c_substitution_debt,
        followup_recovery);
    if (signal_trace_enabled() && result != 0)
        fprintf(stderr,
                "[spu-signal] source=%s "
                "owner=0x%08X taskset=0x%08X generation=%llu result=%d "
                "ca9c_debt=%d\n",
                followup_recovery ? "completion-generation-followup"
                                  : "completion-generation-retry",
                owner_ea, taskset_ea, (unsigned long long)generation, result,
                result == 1 && create_ca9c_substitution_debt &&
                    !followup_recovery ? 1 : 0);
#ifdef _WIN32
    if (result == 1) WakeAllConditionVariable(&s_sig_cv);
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    if (result == 1) pthread_cond_broadcast(&s_sig_cv);
    pthread_mutex_unlock(&s_sig_lock);
#endif
    return result;
}

/* WAIT_SIGNAL from the task side (runs ON the task's host thread, called by
 * the 0xA70 taskset-syscall intercept). Consumes a pending signal or blocks
 * until one is delivered. Returns 0 (the syscall's success rc). */
int spu_taskset_wait_signal(uint32_t taskset_ea, uint32_t taskId,
                            int image_id, uint32_t wait_return_pc)
{
    if (!taskset_ea || taskId >= 128) return 0;
#ifdef _WIN32
    tj_spu_pc_time_job_complete(image_id);
    tj_spu_pc_hist_job_complete(image_id);
#endif
#ifdef _WIN32
    int perf_trace = task_perf_enabled();
    LONGLONG perf_enter = 0;
    uint64_t perf_run_ticks = 0;
    LONGLONG perf_signal = 0;
    int perf_completed_wait = 0;
    int perf_report_ready = 0;
    SpuTaskPerfTable* perf_report = NULL;
    if (perf_trace) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        perf_enter = now.QuadPart;
        if (s_task_perf_last_exit)
            perf_run_ticks = (uint64_t)(perf_enter - s_task_perf_last_exit);
    }
#endif
    /* ran= is the wall time this task spent OUT of the wait (decode/mix work
     * per round) -- decisive for "is the Bink decode CPU-bound". TLS: each
     * task runs on its own host thread. */
    static _Thread_local unsigned long long s_wait_exit_ms;
    if (signal_trace_enabled()) {
      extern unsigned long long ps3_ms_now(void);
      unsigned long long _now = ps3_ms_now();
      static int _n = 0; if (_n++ < 200 || (_n % 500) == 0)
        fprintf(stderr, "[spu_workload] WAIT_SIGNAL#%d enter task=%u taskset=0x%08X ran=%llums\n",
                _n, taskId, taskset_ea,
                s_wait_exit_ms ? (_now - s_wait_exit_ms) : 0ull);
    }
    /* LBP_SYNC_ACK: taskset sync-lane bookkeeping. On real SPURS the taskset
     * POLICY MODULE advances the per-SPU progress lanes (sync+0x40+16*row:
     * ticket u16 @+0, consumer lanes @+2..) as tasks are processed; our HLE
     * runs the task on a dedicated thread and re-runs the PM ~never, so the
     * lanes froze and the skip path's min-of-lanes barrier (sub_4D3730) hung
     * forever. A task ENTERING WAIT_SIGNAL has by definition drained all work
     * dispatched to it, so advancing the active lanes to the published ticket
     * here is semantically the PM's bookkeeping, batched. Validation gate. */
    static int s_sync_ack = -1;
    if (s_sync_ack < 0) s_sync_ack = getenv("LBP_SYNC_ACK") ? 1 : 0;
    if (s_sync_ack) {
        extern uint32_t g_barrier_sync_watch;
        uint32_t b = g_barrier_sync_watch;
        if (b) {
            for (int row = 0; row < 4; row++) {
                uint32_t p = b + 0x40 + 16u * (uint32_t)row;
                extern uint8_t* vm_base;
                uint16_t ticket = (uint16_t)((vm_base[p] << 8) | vm_base[p+1]);
                for (int lane = 0; lane < 7; lane++) {
                    uint32_t la = p + 2 + 2u * (uint32_t)lane;
                    uint16_t cur = (uint16_t)((vm_base[la] << 8) | vm_base[la+1]);
                    if (cur != 0xFFFF && cur < ticket) {
                        vm_base[la]   = (uint8_t)(ticket >> 8);
                        vm_base[la+1] = (uint8_t)ticket;
                    }
                }
            }
            { static int _a = 0; if (_a++ < 8)
                fprintf(stderr, "[sync-ack] lanes advanced to tickets (sync=0x%08X)\n", b); }
        }
    }
    unsigned secs = 0;
    /* LBP_WS_DRAIN=<secs>: EXPERIMENT (general work-drained approximation).
     * A task ENTERING WAIT_SIGNAL has drained the work dispatched to it. In our
     * one-shot SPU model the producer task ran to completion already, so the
     * signal never arrives and the consumer blocks forever (movie/audio/loading
     * deadlock). If set, after <secs> of no real signal we RETURN rc=0 as if the
     * kernel resumed the task with "no pending work" -- lets it run its post-wait
     * path (complete its loop / set completion flags) instead of hanging. 0/unset
     * keeps the block-forever behaviour. Reversible, opt-in. */
    unsigned drain = 0; { static long long s_drain = -1;
      if (s_drain < 0) { const char* e = getenv("LBP_WS_DRAIN"); unsigned d = 0;
        if (e) { d = (unsigned)atoi(e); if (!d) d = 1; } s_drain = d; }
      drain = (unsigned)s_drain; }
    int drained = 0;
    int shutting_down = 0;
    /* Release the global SPU-serial lock while parked so another SPU task can
     * run (and possibly signal us); re-acquired before returning to resume. */
    spu_serial_release();
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_sig_lock);
    SpuSignalTaskset* sig_state = signal_taskset_find_locked(taskset_ea);
    if (sig_state) {
        extern unsigned long long ps3_ms_now(void);
        sig_state->waiter_active[taskId] = 1;
        sig_state->waiter_image_id[taskId] = image_id;
        sig_state->waiter_return_pc[taskId] = wait_return_pc;
        sig_state->waiter_started_ms[taskId] = ps3_ms_now();
    }
    if (spu_taskset_retry_current_generation_locked(sig_state, taskset_ea) == 1)
        WakeAllConditionVariable(&s_sig_cv);
    if (signal_trace_enabled())
        fprintf(stderr,
                "[spu-signal] source=wait-enter taskset=0x%08X task=%u "
                "pending=%u guest=%u\n", taskset_ea, taskId,
                sig_state ? sig_state->pending[taskId] : 0,
                signal_guest_bit_test_reservation(taskset_ea, taskId));
    tj_signal_trace(taskset_ea, taskId, "wait-enter", "wait-signal",
                    sig_state ? sig_state->pending[taskId] : 0,
                    sig_state ? sig_state->pending[taskId] : 0,
                    signal_guest_bit_test_reservation(taskset_ea, taskId),
                    sig_state ? sig_state->generation : 0, 0, 0);
    if (!(sig_state && sig_state->shutdown_requested) &&
        !spu_task_signal_is_pending(sig_state ? sig_state->pending[taskId] : 0,
                                    signal_guest_bit_test_reservation(taskset_ea, taskId)))
        tj_signal_trace(taskset_ea, taskId, "wait-park", "wait-signal",
                        sig_state ? sig_state->pending[taskId] : 0,
                        sig_state ? sig_state->pending[taskId] : 0, 0,
                        sig_state ? sig_state->generation : 0, 0, 0);
    while (!(sig_state && sig_state->shutdown_requested) &&
           !spu_task_signal_is_pending(
               sig_state ? sig_state->pending[taskId] : 0,
               signal_guest_bit_test_reservation(taskset_ea, taskId))) {
        if (!SleepConditionVariableSRW(&s_sig_cv, &s_sig_lock, 1000, 0)) {
            static int _n = 0;
            if (++secs && _n < 16) { _n++;
                fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) sleeping "
                        "%us in WAIT_SIGNAL\n", taskId, taskset_ea, secs); fflush(stderr); }
            if (drain && secs >= drain) { drained = 1; break; }
        }
    }
    shutting_down = sig_state && sig_state->shutdown_requested;
    if (!drained && !shutting_down) {
        uint16_t before_pending = sig_state ? sig_state->pending[taskId] : 0;
#ifdef _WIN32
        if (perf_trace) {
            /* Capture under s_sig_lock before consumption clears this token. */
            perf_signal = sig_state ? sig_state->signal_tick[taskId] : 0;
            perf_completed_wait = 1;
        }
#endif
        if (sig_state && sig_state->pending[taskId])
            sig_state->pending[taskId] =
                spu_task_signal_consume_pending(sig_state->pending[taskId]);
        if (sig_state) ++sig_state->consume_epoch[taskId];
        if (!sig_state || !sig_state->pending[taskId]) {
            signal_guest_bit_clear_reservation(taskset_ea, taskId);
#ifdef _WIN32
            if (sig_state) sig_state->signal_tick[taskId] = 0;
#endif
        }
        if (signal_trace_enabled())
            fprintf(stderr,
                    "[spu-signal] source=wait-consume taskset=0x%08X task=%u "
                    "pending=%u->%u guest=%u\n", taskset_ea, taskId,
                    before_pending, sig_state ? sig_state->pending[taskId] : 0,
                    signal_guest_bit_test_reservation(taskset_ea, taskId));
        tj_signal_trace(taskset_ea, taskId, "wait-consume", "wait-signal",
                        before_pending, sig_state ? sig_state->pending[taskId] : 0,
                        signal_guest_bit_test_reservation(taskset_ea, taskId),
                        sig_state ? sig_state->generation : 0, 0, 0);
    }
    if (sig_state) sig_state->waiter_active[taskId] = 0;
    ReleaseSRWLockExclusive(&s_sig_lock);
#else
    pthread_mutex_lock(&s_sig_lock);
    SpuSignalTaskset* sig_state = signal_taskset_find_locked(taskset_ea);
    if (sig_state) {
        extern unsigned long long ps3_ms_now(void);
        sig_state->waiter_active[taskId] = 1;
        sig_state->waiter_image_id[taskId] = image_id;
        sig_state->waiter_return_pc[taskId] = wait_return_pc;
        sig_state->waiter_started_ms[taskId] = ps3_ms_now();
    }
    if (spu_taskset_retry_current_generation_locked(sig_state, taskset_ea) == 1)
        pthread_cond_broadcast(&s_sig_cv);
    while (!(sig_state && sig_state->shutdown_requested) &&
           !spu_task_signal_is_pending(
               sig_state ? sig_state->pending[taskId] : 0,
               signal_guest_bit_test_reservation(taskset_ea, taskId))) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
        if (pthread_cond_timedwait(&s_sig_cv, &s_sig_lock, &ts) != 0) {
            static int _n = 0;
            if (++secs && _n < 16) { _n++;
                fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) sleeping "
                        "%us in WAIT_SIGNAL\n", taskId, taskset_ea, secs); fflush(stderr); }
            if (drain && secs >= drain) { drained = 1; break; }
        }
    }
    shutting_down = sig_state && sig_state->shutdown_requested;
    if (!drained && !shutting_down) {
        uint16_t before_pending = sig_state ? sig_state->pending[taskId] : 0;
        if (sig_state && sig_state->pending[taskId])
            sig_state->pending[taskId] =
                spu_task_signal_consume_pending(sig_state->pending[taskId]);
        if (sig_state) ++sig_state->consume_epoch[taskId];
        if (!sig_state || !sig_state->pending[taskId])
            signal_guest_bit_clear_reservation(taskset_ea, taskId);
        if (signal_trace_enabled())
            fprintf(stderr,
                    "[spu-signal] source=wait-consume taskset=0x%08X task=%u "
                    "pending=%u->%u guest=%u\n", taskset_ea, taskId,
                    before_pending, sig_state ? sig_state->pending[taskId] : 0,
                    signal_guest_bit_test_reservation(taskset_ea, taskId));
    }
    if (sig_state) sig_state->waiter_active[taskId] = 0;
    pthread_mutex_unlock(&s_sig_lock);
#endif
    if (drained) { static int _d = 0; if (_d++ < 24)
        fprintf(stderr, "[spu_workload] task %u (taskset 0x%08X) WS_DRAIN resume "
                "after %us (no signal)\n", taskId, taskset_ea, secs); fflush(stderr); }
    spu_serial_acquire();       /* resume: re-take the serial lock */
#ifdef _WIN32
    tj_spu_pc_time_job_begin(image_id);
#endif
#ifdef _WIN32
    if (perf_trace && perf_completed_wait) {
        LARGE_INTEGER resumed;
        QueryPerformanceCounter(&resumed);
        /* End block/wake intervals after serial-gate reacquire, then update
         * the shared table under its original signal lock coverage. */
        AcquireSRWLockExclusive(&s_sig_lock);
        if (s_task_perf.completed_waits + 1 >= SPU_TASK_PERF_REPORT_WAITS)
            perf_report = (SpuTaskPerfTable*)_alloca(sizeof(*perf_report));
        task_perf_record_locked(
            taskset_ea, taskId, image_id, wait_return_pc, perf_run_ticks,
            (uint64_t)(resumed.QuadPart - perf_enter),
            perf_signal ? (uint64_t)(resumed.QuadPart - perf_signal) : 0,
            perf_signal != 0, perf_report, &perf_report_ready);
        ReleaseSRWLockExclusive(&s_sig_lock);
    }
#endif
    if (signal_trace_enabled()) {
      extern unsigned long long ps3_ms_now(void);
      s_wait_exit_ms = ps3_ms_now();     /* stamp for the next traced entry */
    }
#ifdef _WIN32
    if (perf_report_ready)
        task_perf_print_report(perf_report);
    if (perf_trace) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        s_task_perf_last_exit = now.QuadPart;
    }
#endif
    g_spu_trace_wait_count++;
    return shutting_down ? 1 : 0;
}

int spu_workload_dispatch_async_task(const uint8_t* image, uint32_t image_size,
                                     uint32_t args_ea, uint32_t taskset_ea,
                                     uint32_t task_id)
{
    if (!image || image_size == 0) return 0;

    uint64_t fp = spu_workload_fingerprint(image, image_size);
    spu_lifted_entry_fn fn = NULL;
    int image_id = 0;
    for (unsigned i = 0; i < s_registry_count; i++)
        if (s_registry[i].fp == fp) { fn = s_registry[i].fn; image_id = s_registry[i].image_id; break; }
    if (!fn) {
        fprintf(stderr, "[spu_workload] async dispatch MISS fp=0x%016llX size=%u\n",
                (unsigned long long)fp, image_size);
        return 0;
    }

    spu_async_job* j = (spu_async_job*)malloc(sizeof(*j));
    if (!j) return 0;
    j->image = image; j->image_size = image_size; j->args_ea = args_ea;
    j->fn = fn; j->image_id = image_id;
    j->taskset_ea = taskset_ea;
    j->taskid = task_id;
    /* Capture the SPURS task r3 NOW (PPU thread, synchronous) from the game's
     * descriptor at eaContext+0x10 = {0x40-marker handle, workload EAs}; the
     * async SPU thread reading it later would race the PPU stack. word1 is
     * overridden to args_ea (eaContext) in spu_run_lifted_job_abi. */
    j->have_r3 = 0;
    if (args_ea) {
        extern uint8_t* vm_base;
        const uint8_t* c = vm_base + args_ea + 0x10;
        for (int k = 0; k < 4; k++)
            j->r3[k] = ((uint32_t)c[k*4]<<24)|((uint32_t)c[k*4+1]<<16)|
                       ((uint32_t)c[k*4+2]<<8)|c[k*4+3];
        if ((j->r3[0] >> 16) == 0x40) j->have_r3 = 1;   /* valid marker */
    }

    fprintf(stderr,
        "[spu_workload] dispatch HIT (async) fp=0x%016llX args=0x%08X image=%d -> spawning thread\n",
        (unsigned long long)fp, args_ea, image_id);
    if (args_ea) { extern uint8_t* vm_base; const uint8_t* c = vm_base + args_ea;
        static int _d=0; if (_d++ < 1) {
            /* Dump a larger window of the task context buffer + scan for any word
             * that looks like the LS[0xBEC0] target (i.e. a small LS-range value),
             * to see if the kernel-restored LS data lives here (real game data). */
            /* The policy module restores the task context from a save buffer into
             * LS 0xB200; LS[0xBEC0] = ctxbuf + 0xCC0. Check the heap-EA candidates
             * in the descriptor for a save buffer whose +0xCC0/+0xCC8 holds a
             * small LS pointer (a valid P for LS[0xBEC8]). */
            uint32_t cand[5];
            cand[0]=((uint32_t)c[0x14]<<24)|((uint32_t)c[0x15]<<16)|((uint32_t)c[0x16]<<8)|c[0x17];
            cand[1]=((uint32_t)c[0x1C]<<24)|((uint32_t)c[0x1D]<<16)|((uint32_t)c[0x1E]<<8)|c[0x1F];
            cand[2]=((uint32_t)c[0x98]<<24)|((uint32_t)c[0x99]<<16)|((uint32_t)c[0x9A]<<8)|c[0x9B];
            cand[3]=((uint32_t)c[0xB8]<<24)|((uint32_t)c[0xB9]<<16)|((uint32_t)c[0xBA]<<8)|c[0xBB];
            cand[4]=((uint32_t)c[0x16C]<<24)|((uint32_t)c[0x16D]<<16)|((uint32_t)c[0x16E]<<8)|c[0x16F];
            for (int ci=0; ci<5; ci++) {
                uint32_t ea=cand[ci]; if (!ea || ea>=0x10000000) continue;
                const uint8_t* b = vm_base + ea + 0xCC0;
                fprintf(stderr, "[ctxbuf cand 0x%08X +0xCC0]:", ea);
                for (int k=0;k<0x20;k+=4) {
                    uint32_t w=((uint32_t)b[k]<<24)|((uint32_t)b[k+1]<<16)|((uint32_t)b[k+2]<<8)|b[k+3];
                    fprintf(stderr, " %08X%s", w, (w>0&&w<0x40000)?"<LS":"");
                }
                fprintf(stderr, "\n");
            } } }

#ifdef _WIN32
    /* 256MB RESERVE (not commit): the Bink SPU decoder's per-macroblock brsl
     * chains are deep host call chains with fat lifted frames -- a 1MB stack
     * blew the guard page (STATUS_GUARD_PAGE_VIOLATION 0x80000001, silent
     * process death) the moment the real decode path first executed. */
    HANDLE th = CreateThread(NULL, 256u << 20, spu_async_thread, j,
                             STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!th) { free(j); return 0; }
    CloseHandle(th);   /* detached */
#else
    pthread_t th;
    if (pthread_create(&th, NULL, spu_async_thread, j) != 0) { free(j); return 0; }
    pthread_detach(th);
#endif
    return 1;
}

int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size,
                                uint32_t args_ea)
{
    /* Non-task callers (for example sys_event's service-image wakeup) own no
     * taskset.  The old global capture could attach whichever task was created
     * most recently and build a foreign SpursTasksetContext. */
    return spu_workload_dispatch_async_task(image, image_size, args_ea, 0, 0);
}
