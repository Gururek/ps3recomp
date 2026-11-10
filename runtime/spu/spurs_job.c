/* spurs_job.c — stage and enter a SPURS jobchain job the way Sony's jm2 does.
 *
 * A job binary is not an SPU ELF and not a policy module: it is a raw image
 * built by the SDK's job_elf-to-bin, linked against Sony's job_start_w_crt.o.
 * That CRT is what every job entry actually runs first — it saves r3/r4, runs
 * the ctors, and calls
 *
 *     void cellSpursJobMain2(CellSpursJobContext2 *jobContext,   // r3
 *                            CellSpursJob256      *job);         // r4
 *
 * (target/spu/include/cell/spurs/job_chain.h). Both are LOCAL STORE pointers,
 * so the manager must have laid the whole working set out in LS before entry.
 * Entering a job without that context is what left LBP's job parked with zero
 * DMA traffic: it reads its ioBuffer pointer out of a zeroed context and stalls.
 *
 * The layout is pinned by _cellSpursCheckJob() in
 * target/common/include/cell/spurs/job_descriptor.h, which sums the exact LS a
 * job consumes. For a jobchain it runs with align = 1024, isScratchStackUnified
 * = 1, maxUsableMemorySize = CELL_SPURS_MAX_SIZE_JOB_MEMORY (234 KB):
 *
 *     align1024(sizeBinary<<4)                                   binary + bss
 *   + align1024(sizeInOrInOut)                                   ioBuffer
 *   + align1024(sizeOut)                                         oBuffer
 *   + align1024((sizeScratch<<4) + (sizeStack ? sizeStack<<4 : 8192))
 *                                                                scratch + stack
 *   + per cache entry: align1024(size)                           cacheBuffer[0..3]
 *
 * We reproduce that arithmetic rather than invent a layout, so a job that
 * range-checks its own buffers sees what it would on hardware.
 *
 * Load address: the job binary is position-independent (job_start_w_crt derives
 * its load delta with a brsl-to-next / subtract-link-time-address idiom), so jm2
 * may place it anywhere. We place it at LS 0 — the lifted C is keyed to
 * link-time addresses, which makes the delta zero and the lifted branch targets
 * exact. The shared LS data-load helper supplies the absolute kernel identity
 * quadword separately, so reading spuNum at 0x1C8 cannot read a job opcode.
 */

#include "spu_workload.h"
#include "spu_context.h"
#include "spu_interp.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
static int tj_env_lbp_job_dump(void){ static int c = -1; if (c < 0) c = getenv("LBP_JOB_DUMP") != NULL; return c; }
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*)(spu_context*), spu_context*);
#ifdef _WIN32
extern void tj_spu_pc_hist_job_complete(int image_id);
extern void tj_spu_pc_time_job_begin(int image_id);
extern void tj_spu_pc_time_job_complete(int image_id);
#endif

/* ---- guest reads (big-endian) ------------------------------------------ */
static uint32_t g32(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static uint16_t g16(uint32_t ea)
{
    const uint8_t* p = vm_base + ea;
    return (uint16_t)(((uint32_t)p[0] << 8) | (uint32_t)p[1]);
}
static uint64_t g64(uint32_t ea)
{
    return ((uint64_t)g32(ea) << 32) | (uint64_t)g32(ea + 4);
}

/* ---- local-store writes (big-endian) ----------------------------------- */
static void ls32(uint8_t* ls, uint32_t o, uint32_t v)
{
    ls[o] = (uint8_t)(v >> 24); ls[o+1] = (uint8_t)(v >> 16);
    ls[o+2] = (uint8_t)(v >> 8); ls[o+3] = (uint8_t)v;
}
static void ls16(uint8_t* ls, uint32_t o, uint16_t v)
{
    ls[o] = (uint8_t)(v >> 8); ls[o+1] = (uint8_t)v;
}

/* CellSpursJobHeader, 48 B — job_descriptor.h */
enum {
    JH_EA_BINARY           = 0x00,   /* be u64; low bit = CACHE_INHIBIT flag */
    JH_SIZE_BINARY         = 0x08,   /* be u16; bytes >> 4                   */
    JH_SIZE_DMA_LIST       = 0x0A,   /* be u16; input list size in BYTES     */
    JH_EA_HIGH_INPUT       = 0x0C,
    JH_USE_INOUT_BUFFER    = 0x10,
    JH_SIZE_IN_OR_INOUT    = 0x14,
    JH_SIZE_OUT            = 0x18,
    JH_SIZE_STACK          = 0x1C,   /* be u16; quadwords */
    JH_SIZE_SCRATCH        = 0x1E,   /* be u16; quadwords */
    JH_EA_HIGH_CACHE       = 0x20,
    JH_SIZE_CACHE_DMA_LIST = 0x24,   /* be u32; bytes */
    JH_JOB_TYPE            = 0x2C,
    JH_SIZE                = 0x30,
};

/* CellSpursJobContext2, 48 B — job_context_types.h. SPU pointers are 32-bit LS
 * offsets and the struct lives in local store, so every field is big-endian. */
enum {
    JC_IO_BUFFER          = 0x00,
    JC_CACHE_BUFFER       = 0x04,   /* void *cacheBuffer[4] */
    JC_SIZE_JOB_DESC      = 0x14,   /* sizeJobDescriptor:4 + pad:28 */
    JC_NUM_IO_BUFFER      = 0x18,
    JC_NUM_CACHE_BUFFER   = 0x1A,
    JC_O_BUFFER           = 0x1C,
    JC_S_BUFFER           = 0x20,
    JC_DMA_TAG            = 0x24,
    JC_EA_JOB_DESCRIPTOR  = 0x28,   /* be u64 */
    JC_SIZE               = 0x30,
};

#define ALIGN1024(x)  (((x) + 1023u) & ~1023u)
#define JOB_DMA_TAG   20u          /* "one of: {20,21}" — job_context_types.h */
#define JOB_STACK_DEFAULT 8192u    /* _cellSpursCheckJob's sizeStack==0 default */

/* A host thread is a jobchain, not a physical SPU. Lease a distinct in-range
 * jm2 worker number for each running job in a CellSpurs instance. Geometry
 * allocators use this number to select persistent per-worker ring records.
 * Stack-owned list nodes need neither an arbitrary instance limit nor heap
 * allocation; every normal/halt return below releases its lease. */
typedef struct SpursJobLane {
    struct SpursJobLane* next;
    uint32_t spurs_ea;
    uint32_t number;
} SpursJobLane;

static SpursJobLane* s_job_lanes;
#ifdef _WIN32
static SRWLOCK s_job_lane_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_job_lane_ready = CONDITION_VARIABLE_INIT;
#define JOB_LANE_LOCK() AcquireSRWLockExclusive(&s_job_lane_lock)
#define JOB_LANE_UNLOCK() ReleaseSRWLockExclusive(&s_job_lane_lock)
#define JOB_LANE_WAIT() SleepConditionVariableSRW(&s_job_lane_ready, &s_job_lane_lock, INFINITE, 0)
#define JOB_LANE_WAKE() WakeAllConditionVariable(&s_job_lane_ready)
#else
static pthread_mutex_t s_job_lane_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_job_lane_ready = PTHREAD_COND_INITIALIZER;
#define JOB_LANE_LOCK() pthread_mutex_lock(&s_job_lane_lock)
#define JOB_LANE_UNLOCK() pthread_mutex_unlock(&s_job_lane_lock)
#define JOB_LANE_WAIT() (pthread_cond_wait(&s_job_lane_ready, &s_job_lane_lock) == 0)
#define JOB_LANE_WAKE() pthread_cond_broadcast(&s_job_lane_ready)
#endif

static int spurs_job_lane_acquire(SpursJobLane* lane, uint32_t spurs_ea)
{
    /* Zero owner is the existing standalone staging/test ABI, not a SPURS
     * kernel dispatch. It leaves ordinary local-store reads unchanged. */
    if (!spurs_ea) return 0;
    const uint32_t nspus = vm_base[spurs_ea + 0x76u];
    if (!nspus || nspus > 8) {
        fprintf(stderr, "[spurs-job] invalid CellSpurs worker count %u at 0x%08X\n",
                nspus, spurs_ea);
        return -1;
    }
    const uint32_t available = (1u << nspus) - 1u;
    JOB_LANE_LOCK();
    for (;;) {
        uint32_t busy = 0;
        for (const SpursJobLane* active = s_job_lanes; active; active = active->next)
            if (active->spurs_ea == spurs_ea) busy |= 1u << active->number;
        if (available & ~busy) {
            uint32_t number = 0;
            while (busy & (1u << number)) ++number;
            lane->spurs_ea = spurs_ea;
            lane->number = number;
            lane->next = s_job_lanes;
            s_job_lanes = lane;
            JOB_LANE_UNLOCK();
            return 0;
        }
        if (!JOB_LANE_WAIT()) {
            JOB_LANE_UNLOCK();
            return -1;
        }
    }
}

static void spurs_job_lane_release(SpursJobLane* lane)
{
    if (!lane->spurs_ea) return;
    JOB_LANE_LOCK();
    SpursJobLane** link = &s_job_lanes;
    while (*link && *link != lane) link = &(*link)->next;
    if (*link) *link = lane->next;
    lane->spurs_ea = 0;
    JOB_LANE_WAKE();
    JOB_LANE_UNLOCK();
}

/* Job contexts come from a small heap pool instead of a fresh 260 KB stack
 * frame per job (hundreds of millions of jobs per long run).
 *
 * Default: every job still gets a fully zeroed context, as before.
 * TJ_SPU_CTX_REUSE=1: a recycled context is returned to the same all-zero
 * state by clearing only what the previous job could own -- registers and
 * channel state, LS [0, its region end) and the descriptor/context block at
 * the top of LS. Full zeroing was 20-55% of SPU job time. Real jm2 does not
 * clear LS between jobs at all, so this stays at least as strict.
 * TJ_SPU_CTX_VERIFY=1 scans the rest of LS after every job and logs any
 * write outside the job's regions (that slot is then fully cleared). */
#define JOB_LS_TOP_BLOCK 0x440u   /* descriptor (0x400) + context (0x40) */

typedef struct SpursJobCtxSlot {
    struct SpursJobCtxSlot* next;
    uint32_t dirty_end;   /* LS [0, dirty_end) may be nonzero after a job */
    int      used;        /* 0 = never run: the allocation is already zero */
    spu_context ctx;
} SpursJobCtxSlot;

static SpursJobCtxSlot* s_job_ctx_free;
static long s_job_ctx_count;
#ifdef _WIN32
static SRWLOCK s_job_ctx_lock = SRWLOCK_INIT;
#define JOB_CTX_LOCK() AcquireSRWLockExclusive(&s_job_ctx_lock)
#define JOB_CTX_UNLOCK() ReleaseSRWLockExclusive(&s_job_ctx_lock)
#define JOB_CTX_ALLOC(n) _aligned_malloc((n), 64)
#else
static pthread_mutex_t s_job_ctx_lock = PTHREAD_MUTEX_INITIALIZER;
#define JOB_CTX_LOCK() pthread_mutex_lock(&s_job_ctx_lock)
#define JOB_CTX_UNLOCK() pthread_mutex_unlock(&s_job_ctx_lock)
static void* job_ctx_alloc(size_t n) { void* p = NULL; return posix_memalign(&p, 64, n) ? NULL : p; }
#define JOB_CTX_ALLOC(n) job_ctx_alloc(n)
#endif

static int tj_env_job_ctx_reuse(void)
{
    static int c = -1;
    if (c < 0) {
        const char* v = getenv("TJ_SPU_CTX_REUSE");
        c = v && *v && strcmp(v, "0") != 0;
        if (c) fprintf(stderr, "[tj-spu-ctx] job context reuse on\n");
    }
    return c;
}

static int tj_env_job_ctx_verify(void)
{
    static int c = -1;
    if (c < 0) {
        const char* v = getenv("TJ_SPU_CTX_VERIFY");
        c = v && *v && strcmp(v, "0") != 0;
    }
    return c;
}

static SpursJobCtxSlot* spurs_job_ctx_acquire(void)
{
    JOB_CTX_LOCK();
    SpursJobCtxSlot* slot = s_job_ctx_free;
    if (slot) s_job_ctx_free = slot->next;
    JOB_CTX_UNLOCK();
    if (slot) return slot;

    slot = (SpursJobCtxSlot*)JOB_CTX_ALLOC(sizeof(*slot));
    if (!slot) {
        fprintf(stderr, "[spurs-job] job context allocation failed\n");
        return NULL;
    }
    memset(slot, 0, sizeof(*slot));
    spu_context_init(&slot->ctx, 0);
    long n;
    JOB_CTX_LOCK();
    n = ++s_job_ctx_count;
    JOB_CTX_UNLOCK();
    if ((n & (n - 1)) == 0)
        fprintf(stderr, "[tj-spu-ctx] job context pool size %ld\n", n);
    return slot;
}

static void spurs_job_ctx_release(SpursJobCtxSlot* slot)
{
    JOB_CTX_LOCK();
    slot->next = s_job_ctx_free;
    s_job_ctx_free = slot;
    JOB_CTX_UNLOCK();
}

/* Bring a slot to the state spu_context_init gives. `bin_end` is where the
 * caller copies the job binary next ([0, bin_end) is overwritten anyway). */
static void spurs_job_ctx_reset(SpursJobCtxSlot* slot, uint32_t bin_end)
{
    spu_context* ctx = &slot->ctx;
    if (!slot->used) {
        slot->used = 1;
        return;                        /* fresh allocation is all zero */
    }
    if (!tj_env_job_ctx_reuse() || slot->dirty_end >= SPU_LS_SIZE - JOB_LS_TOP_BLOCK) {
        spu_context_init(ctx, 0);
        return;
    }
    const size_t ls_begin = offsetof(spu_context, ls);
    const size_t ls_end = ls_begin + SPU_LS_SIZE;
    memset(ctx, 0, ls_begin);
    memset((uint8_t*)ctx + ls_end, 0, sizeof(*ctx) - ls_end);
    if (slot->dirty_end > bin_end)
        memset(ctx->ls + bin_end, 0, slot->dirty_end - bin_end);
    memset(ctx->ls + SPU_LS_SIZE - JOB_LS_TOP_BLOCK, 0, JOB_LS_TOP_BLOCK);
    ctx->spu_id = 0;
    ctx->status = SPU_STATUS_STOPPED;
}

/* Record how much LS the finished job may have dirtied. */
static void spurs_job_ctx_finish(SpursJobCtxSlot* slot, uint32_t region_end,
                                 int image_id, uint32_t job_ea)
{
    uint32_t end = region_end;
    if (end > SPU_LS_SIZE) end = SPU_LS_SIZE;
    if (tj_env_job_ctx_reuse() && tj_env_job_ctx_verify()) {
        const uint8_t* ls = slot->ctx.ls;
        const uint32_t top = SPU_LS_SIZE - JOB_LS_TOP_BLOCK;
        uint32_t first = 0, count = 0;
        for (uint32_t o = end; o < top; ++o) {
            if (ls[o]) {
                if (!count) first = o;
                ++count;
            }
        }
        if (count) {
            static volatile long s_stray_logs;
#ifdef _WIN32
            const long nlog = InterlockedIncrement(&s_stray_logs);
#else
            const long nlog = __sync_add_and_fetch(&s_stray_logs, 1);
#endif
            if (nlog <= 32)
                fprintf(stderr,
                        "[tj-spu-ctx] stray LS write image=%d job=0x%08X "
                        "region_end=0x%05X first=0x%05X bytes=%u\n",
                        image_id, job_ea, end, first, count);
            end = SPU_LS_SIZE;         /* clear it all before the next job */
        }
    }
    slot->dirty_end = end;
}

/* Default-off phase timing for synchronous jm2 jobs. Keep it here, at the
 * staging owner, so dispatcher lookup time remains a separate measurement.
 * All counters are cumulative and atomic; the winning reporter only reads
 * snapshots after a completed job and never holds a semantic SPU lock. */
#ifdef _WIN32
#define TJ_SPURS_JOB_PHASE_ROWS 64

enum tj_spurs_job_phase {
    TJ_SPURS_JOB_PHASE_DESCRIPTOR = 0,
    TJ_SPURS_JOB_PHASE_CTX_ZERO,
    TJ_SPURS_JOB_PHASE_STAGING,
    TJ_SPURS_JOB_PHASE_CTX_SETUP,
    TJ_SPURS_JOB_PHASE_LS_COPY,
    TJ_SPURS_JOB_PHASE_LIFTED,
    TJ_SPURS_JOB_PHASE_FREE,
    TJ_SPURS_JOB_PHASE_TOTAL,
    TJ_SPURS_JOB_PHASE_COUNT
};

typedef struct tj_spurs_job_phase_row {
    volatile LONG64 calls;
    volatile LONG64 ticks[TJ_SPURS_JOB_PHASE_COUNT];
    volatile LONG64 max_ticks[TJ_SPURS_JOB_PHASE_COUNT];
} tj_spurs_job_phase_row;

static tj_spurs_job_phase_row
    s_tj_spurs_job_phase_rows[TJ_SPURS_JOB_PHASE_ROWS];
static LARGE_INTEGER s_tj_spurs_job_phase_frequency;
static volatile LONG s_tj_spurs_job_phase_state;
static volatile LONG64 s_tj_spurs_job_phase_last_report_qpc;
static SRWLOCK s_tj_spurs_job_phase_lock = SRWLOCK_INIT;
static __declspec(thread) int s_tj_spurs_job_phase_tls = -1;

static int tj_spurs_job_phase_perf_enabled(void)
{
    if (s_tj_spurs_job_phase_tls >= 0)
        return s_tj_spurs_job_phase_tls;

    /* 0=uninitialized, 1=initializing, 2=disabled, 3=enabled. */
    LONG state = InterlockedCompareExchange(&s_tj_spurs_job_phase_state, 1, 0);
    if (state == 0) {
        const char* value = getenv("TJ_SPURS_JOB_PHASE_PERF");
        int enabled = value && *value && strcmp(value, "0") != 0;
        if (enabled && !QueryPerformanceFrequency(
                           &s_tj_spurs_job_phase_frequency))
            enabled = 0;
        InterlockedExchange(&s_tj_spurs_job_phase_state, enabled ? 3 : 2);
        s_tj_spurs_job_phase_tls = enabled;
        return s_tj_spurs_job_phase_tls;
    }
    while ((state = InterlockedCompareExchange(
                &s_tj_spurs_job_phase_state, 0, 0)) == 1)
        YieldProcessor();
    s_tj_spurs_job_phase_tls = state == 3;
    return s_tj_spurs_job_phase_tls;
}

static uint64_t tj_spurs_job_phase_qpc(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static uint64_t tj_spurs_job_phase_ticks_to_us(uint64_t ticks)
{
    uint64_t frequency = (uint64_t)s_tj_spurs_job_phase_frequency.QuadPart;
    if (!frequency) return 0;
    return (ticks / frequency) * 1000000ull +
           ((ticks % frequency) * 1000000ull) / frequency;
}

static void tj_spurs_job_phase_max(volatile LONG64* target, LONG64 value)
{
    LONG64 observed = InterlockedCompareExchange64(target, 0, 0);
    while (value > observed) {
        LONG64 previous = InterlockedCompareExchange64(target, value, observed);
        if (previous == observed) return;
        observed = previous;
    }
}

/* One printed row, copied under the lock and formatted after releasing it. */
typedef struct tj_spurs_job_phase_snapshot {
    unsigned image;
    uint64_t calls;
    uint64_t ticks[TJ_SPURS_JOB_PHASE_COUNT];
    uint64_t max_ticks[TJ_SPURS_JOB_PHASE_COUNT];
} tj_spurs_job_phase_snapshot;

/* One stderr write per report line: stderr is unbuffered, so fprintf writes
 * one character per system call (~2.6 us each to the TJ_STDERR_FILE log).
 * fwrite holds the stream lock, so the line stays whole. */
#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
static void tj_spurs_job_log_line(const char* format, ...)
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

/* Winning reporter only, entered holding s_tj_spurs_job_phase_lock. Out of
 * line so the snapshot buffer never enlarges the hot job frame. */
static __declspec(noinline) void tj_spurs_job_phase_report_unlock(void)
{
    /* Copy one coherent snapshot under the lock, then print after releasing
     * it. Every SPU job takes this lock, so an fprintf that blocked here (full
     * launcher stderr pipe) held all SPU work: 11.1 s in the 2026-10-17 traced
     * run. Same rows, values and line format as before. */
    tj_spurs_job_phase_snapshot snapshot[TJ_SPURS_JOB_PHASE_ROWS];
    unsigned snapshot_count = 0;
    for (unsigned i = 0; i < TJ_SPURS_JOB_PHASE_ROWS; ++i) {
        tj_spurs_job_phase_row* source = &s_tj_spurs_job_phase_rows[i];
        uint64_t calls = (uint64_t)InterlockedCompareExchange64(
            &source->calls, 0, 0);
        if (!calls) continue;
        tj_spurs_job_phase_snapshot* copy = &snapshot[snapshot_count++];
        copy->image = i;
        copy->calls = calls;
        for (unsigned phase = 0; phase < TJ_SPURS_JOB_PHASE_COUNT; ++phase) {
            copy->ticks[phase] = (uint64_t)InterlockedCompareExchange64(
                &source->ticks[phase], 0, 0);
            copy->max_ticks[phase] = (uint64_t)InterlockedCompareExchange64(
                &source->max_ticks[phase], 0, 0);
        }
    }
    ReleaseSRWLockExclusive(&s_tj_spurs_job_phase_lock);

    for (unsigned n = 0; n < snapshot_count; ++n) {
        const tj_spurs_job_phase_snapshot* current = &snapshot[n];
        const uint64_t calls = current->calls;
        const uint64_t* cumulative_ticks = current->ticks;
        uint64_t total_us[TJ_SPURS_JOB_PHASE_COUNT];
        uint64_t max_us[TJ_SPURS_JOB_PHASE_COUNT];
        for (unsigned phase = 0; phase < TJ_SPURS_JOB_PHASE_COUNT; ++phase) {
            total_us[phase] = tj_spurs_job_phase_ticks_to_us(
                cumulative_ticks[phase]);
            max_us[phase] = tj_spurs_job_phase_ticks_to_us(
                current->max_ticks[phase]);
        }
        uint64_t named_ticks = 0;
        for (unsigned phase = TJ_SPURS_JOB_PHASE_DESCRIPTOR;
             phase < TJ_SPURS_JOB_PHASE_TOTAL; ++phase)
            named_ticks += cumulative_ticks[phase];
        uint64_t other_us = tj_spurs_job_phase_ticks_to_us(
            cumulative_ticks[TJ_SPURS_JOB_PHASE_TOTAL] > named_ticks
                ? cumulative_ticks[TJ_SPURS_JOB_PHASE_TOTAL] - named_ticks : 0);
        tj_spurs_job_log_line(
                "[TJ-SPURS-JOB-PHASE-PERF] image=%u calls=%llu "
                "descriptor={total_us=%llu avg_us=%llu max_us=%llu} "
                "ctx_zero={total_us=%llu avg_us=%llu max_us=%llu} "
                "staging={total_us=%llu avg_us=%llu max_us=%llu} "
                "ctx_setup={total_us=%llu avg_us=%llu max_us=%llu} "
                "ls_copy={total_us=%llu avg_us=%llu max_us=%llu} "
                "lifted={total_us=%llu avg_us=%llu max_us=%llu} "
                "free={total_us=%llu avg_us=%llu max_us=%llu} "
                "other_us=%llu total={total_us=%llu avg_us=%llu max_us=%llu}\n",
                current->image, (unsigned long long)calls,
                (unsigned long long)total_us[TJ_SPURS_JOB_PHASE_DESCRIPTOR],
                (unsigned long long)(total_us[TJ_SPURS_JOB_PHASE_DESCRIPTOR] / calls),
                (unsigned long long)max_us[TJ_SPURS_JOB_PHASE_DESCRIPTOR],
                (unsigned long long)total_us[TJ_SPURS_JOB_PHASE_CTX_ZERO],
                (unsigned long long)(total_us[TJ_SPURS_JOB_PHASE_CTX_ZERO] / calls),
                (unsigned long long)max_us[TJ_SPURS_JOB_PHASE_CTX_ZERO],
                (unsigned long long)total_us[TJ_SPURS_JOB_PHASE_STAGING],
                (unsigned long long)(total_us[TJ_SPURS_JOB_PHASE_STAGING] / calls),
                (unsigned long long)max_us[TJ_SPURS_JOB_PHASE_STAGING],
                (unsigned long long)total_us[TJ_SPURS_JOB_PHASE_CTX_SETUP],
                (unsigned long long)(total_us[TJ_SPURS_JOB_PHASE_CTX_SETUP] / calls),
                (unsigned long long)max_us[TJ_SPURS_JOB_PHASE_CTX_SETUP],
                (unsigned long long)total_us[TJ_SPURS_JOB_PHASE_LS_COPY],
                (unsigned long long)(total_us[TJ_SPURS_JOB_PHASE_LS_COPY] / calls),
                (unsigned long long)max_us[TJ_SPURS_JOB_PHASE_LS_COPY],
                (unsigned long long)total_us[TJ_SPURS_JOB_PHASE_LIFTED],
                (unsigned long long)(total_us[TJ_SPURS_JOB_PHASE_LIFTED] / calls),
                (unsigned long long)max_us[TJ_SPURS_JOB_PHASE_LIFTED],
                (unsigned long long)total_us[TJ_SPURS_JOB_PHASE_FREE],
                (unsigned long long)(total_us[TJ_SPURS_JOB_PHASE_FREE] / calls),
                (unsigned long long)max_us[TJ_SPURS_JOB_PHASE_FREE],
                (unsigned long long)other_us,
                (unsigned long long)total_us[TJ_SPURS_JOB_PHASE_TOTAL],
                (unsigned long long)(total_us[TJ_SPURS_JOB_PHASE_TOTAL] / calls),
                (unsigned long long)max_us[TJ_SPURS_JOB_PHASE_TOTAL]);
    }
}

static void tj_spurs_job_phase_record(int image_id,
                                      const uint64_t phase_ticks[
                                          TJ_SPURS_JOB_PHASE_COUNT],
                                      uint64_t report_qpc)
{
    AcquireSRWLockExclusive(&s_tj_spurs_job_phase_lock);
    unsigned row_index = image_id >= 0 &&
                         image_id < TJ_SPURS_JOB_PHASE_ROWS - 1
        ? (unsigned)image_id : TJ_SPURS_JOB_PHASE_ROWS - 1;
    tj_spurs_job_phase_row* row = &s_tj_spurs_job_phase_rows[row_index];
    InterlockedIncrement64(&row->calls);
    for (unsigned phase = 0; phase < TJ_SPURS_JOB_PHASE_COUNT; ++phase) {
        InterlockedAdd64(&row->ticks[phase], (LONG64)phase_ticks[phase]);
        tj_spurs_job_phase_max(&row->max_ticks[phase],
                               (LONG64)phase_ticks[phase]);
    }

    LONG64 previous = InterlockedCompareExchange64(
        &s_tj_spurs_job_phase_last_report_qpc, 0, 0);
    if (!previous) {
        InterlockedCompareExchange64(&s_tj_spurs_job_phase_last_report_qpc,
                                     (LONG64)report_qpc, 0);
        ReleaseSRWLockExclusive(&s_tj_spurs_job_phase_lock);
        return;
    }
    uint64_t frequency = (uint64_t)s_tj_spurs_job_phase_frequency.QuadPart;
    if (report_qpc - (uint64_t)previous < frequency ||
        InterlockedCompareExchange64(&s_tj_spurs_job_phase_last_report_qpc,
                                     (LONG64)report_qpc, previous) != previous) {
        ReleaseSRWLockExclusive(&s_tj_spurs_job_phase_lock);
        return;
    }

    tj_spurs_job_phase_report_unlock();
}
#endif

int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size,
                      uint32_t spurs_ea)
{
    if (!entry || !job_ea) return -1;

#ifdef _WIN32
    const int tj_phase_perf = tj_spurs_job_phase_perf_enabled();
    const uint64_t tj_phase_total_begin = tj_phase_perf
        ? tj_spurs_job_phase_qpc() : 0;
#endif

    /* ---- descriptor ---------------------------------------------------- */
    uint32_t ea_bin   = (uint32_t)(g64(job_ea + JH_EA_BINARY) & ~1ull);
    uint32_t size_bin = (uint32_t)g16(job_ea + JH_SIZE_BINARY) << 4;
    uint32_t n_dma    = (uint32_t)g16(job_ea + JH_SIZE_DMA_LIST) / 8u;
    uint32_t use_io   = g32(job_ea + JH_USE_INOUT_BUFFER);
    uint32_t size_io  = g32(job_ea + JH_SIZE_IN_OR_INOUT);
    uint32_t size_out = g32(job_ea + JH_SIZE_OUT);
    uint32_t size_stk = (uint32_t)g16(job_ea + JH_SIZE_STACK) << 4;
    uint32_t size_scr = (uint32_t)g16(job_ea + JH_SIZE_SCRATCH) << 4;
    uint32_t n_cache  = g32(job_ea + JH_SIZE_CACHE_DMA_LIST) / 8u;
    /* Job type bit 1 enables the original JOBCRT buffer guards. The CRT
     * subtracts one quadword from its local size fields and poisons that
     * quadword. jm2 must supply padded LOCAL sizes, never shrink live input.
     * Observed in the original animation image at 0x92E8..0x950C. */
    const uint32_t guard = (vm_base[job_ea + JH_JOB_TYPE] & 2u) ? 16u : 0u;

    static int64_t s_watch_job = -2;
    if (s_watch_job == -2) {
        const char* e = getenv("SPURS_JOB_WATCH");
        s_watch_job = e ? (int64_t)strtoul(e, NULL, 0) : -1;
    }
    int watch_job = s_watch_job >= 0 && job_ea == (uint32_t)s_watch_job;
    if (watch_job) {
        fprintf(stderr,
                "[spurs-job-watch] job=0x%08X bin=0x%08X size=%u dma=%u "
                "useIo=%u io=%u out=%u stack=%u scratch=%u cache=%u\n",
                job_ea, ea_bin, size_bin, n_dma, use_io, size_io, size_out,
                size_stk, size_scr, n_cache);
        for (uint32_t i = 0; i < n_dma; i++) {
            uint64_t e = g64(job_ea + JH_SIZE + i * 8u);
            fprintf(stderr, "[spurs-job-watch] dma[%u] size=%u ea=0x%08X raw=%016llX\n",
                    i, (uint32_t)((e >> 32) & 0x7FFFu), (uint32_t)e,
                    (unsigned long long)e);
        }
        fflush(stderr);
    }

    if (n_cache > 4) n_cache = 4;                    /* jm2 caps at 4 */
    if (!size_stk) size_stk = JOB_STACK_DEFAULT;
    if (!size_bin || size_bin > SPU_LS_SIZE) {
        fprintf(stderr, "[spurs-job] job 0x%08X: bad sizeBinary=%u -- not run\n",
                job_ea, size_bin);
        return -1;
    }

#ifdef _WIN32
    const uint64_t tj_phase_descriptor_end = tj_phase_perf
        ? tj_spurs_job_phase_qpc() : 0;
#endif
    SpursJobCtxSlot* const ctx_slot = spurs_job_ctx_acquire();
    if (!ctx_slot) return -1;
    spurs_job_ctx_reset(ctx_slot, size_bin);
    spu_context* const ctx = &ctx_slot->ctx;
    uint8_t *const ls = ctx->ls;
#ifdef _WIN32
    const uint64_t tj_phase_ctx_zero_end = tj_phase_perf
        ? tj_spurs_job_phase_qpc() : 0;
#endif

    /* ---- binary at LS 0 (PIC, so delta 0 keeps the lifted addresses) ---- */
    memcpy(ls, vm_base + ea_bin, size_bin);

    /* ---- regions, following _cellSpursCheckJob's own arithmetic --------- */
    uint32_t p         = ALIGN1024(size_bin);
    const uint32_t io_capacity = size_io ? size_io + guard : 0;
    const uint32_t out_capacity = size_out ? size_out + guard : 0;
    uint32_t io_ls     = size_io  ? p : 0;  p += ALIGN1024(io_capacity);
    uint32_t out_ls    = size_out ? p : 0;  p += ALIGN1024(out_capacity);
    if (use_io && !out_ls) out_ls = io_ls;    /* in-out: output shares ioBuffer */
    /* Scratch and stack share their boundary guard; reserve it even when
     * scratch is empty. Alignment slack remains outside logical payload. */
    uint32_t ss_size   = ALIGN1024(size_scr + size_stk + guard);
    uint32_t s_ls      = size_scr ? p : 0;
    uint32_t stack_top = p + ss_size;
    p += ss_size;

    /* ---- cache-hinted read-only inputs ---------------------------------- */
    uint32_t dma_base   = job_ea + JH_SIZE;          /* input list ... */
    uint32_t cache_base = dma_base + n_dma * 8u;     /* ... then cache list */
    uint32_t cache_ls[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n_cache; i++) {
        uint64_t e   = g64(cache_base + i * 8u);
        uint32_t sz  = (uint32_t)(e >> 32);
        uint32_t eal = (uint32_t)e;
        if ((!sz && !guard) || (!eal && sz)) continue;
        if (p + sz + guard > SPU_LS_SIZE) break;
        cache_ls[i] = p;
        if (sz) memcpy(ls + p, vm_base + eal, sz);
        p += ALIGN1024(sz + guard);
    }

    /* ---- input DMA list -> ioBuffer, each entry 16-byte aligned ---------- */
    uint32_t io_cur = io_ls, io_used = 0;
    for (uint32_t i = 0; i < n_dma; i++) {
        uint64_t e   = g64(dma_base + i * 8u);
        uint32_t sz  = (uint32_t)((e >> 32) & 0x7FFFu);
        uint32_t eal = (uint32_t)e;
        if (!sz || !eal) continue;
        if (!io_ls || io_cur + sz > SPU_LS_SIZE) break;
        memcpy(ls + io_cur, vm_base + eal, sz);
        io_cur  += (sz + 15u) & ~15u;
        io_used += (sz + 15u) & ~15u;
    }
    /* Highest LS byte staging writes below the top block: the input list is
     * bounded only by LS size, so it can run past the planned layout end. */
    const uint32_t ls_dirty_end = io_cur > p ? io_cur : p;
    if (watch_job && io_ls && io_used) {
        fprintf(stderr, "[spurs-job-watch] io_ls=0x%05X used=%u head/tail:", io_ls, io_used);
        for (uint32_t i = 0; i < 64 && i < io_used; i += 4) {
            uint32_t o = i < 32 ? i : io_used - 64 + i;
            if ((i & 15u) == 0) fprintf(stderr, "\n    +%05X:", o);
            fprintf(stderr, " %02X%02X%02X%02X", ls[io_ls + o], ls[io_ls + o + 1],
                    ls[io_ls + o + 2], ls[io_ls + o + 3]);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }

    /* ---- context + descriptor, parked above the job's memory region ------
     * jm2 keeps these in its own bss (outside job memory); we load the job at
     * LS 0, so the top of LS is the equivalent free space. */
    uint32_t desc_ls = SPU_LS_SIZE - 0x400;     /* descriptors are <= 1024 B */
    uint32_t ctx_ls  = desc_ls - 0x40;          /* CellSpursJobContext2, 48 B */
    uint32_t dsz     = job_desc_size ? job_desc_size : 256u;
    if (dsz > 0x400) dsz = 0x400;
    if (watch_job) {
        fprintf(stderr, "[spurs-job-watch] descriptor (%u bytes):", dsz);
        for (uint32_t o = 0; o < dsz; o += 4) {
            if ((o & 15u) == 0) fprintf(stderr, "\n    +%03X:", o);
            fprintf(stderr, " %08X", g32(job_ea + o));
        }
        fprintf(stderr, "\n");
        uint32_t src = g32(job_ea + 0x54u);
        uint32_t end = g32(job_ea + 0x5Cu);
        if (src && end > src && end - src <= 0x100000u) {
            uint32_t nonzero = 0, first = UINT32_MAX, last = 0;
            for (uint32_t o = 0; o < end - src; o++) {
                if (vm_base[src + o]) {
                    if (first == UINT32_MAX) first = o;
                    last = o;
                    nonzero++;
                }
            }
            fprintf(stderr,
                    "[spurs-job-watch] source 0x%08X..0x%08X bytes=%u nonzero=%u first=0x%X last=0x%X tail:",
                    src, end, end - src, nonzero,
                    first == UINT32_MAX ? 0u : first, last);
            uint32_t base = (end - src > 64u) ? end - 64u : src;
            for (uint32_t ea = base; ea < end; ea += 4u) {
                if (((ea - base) & 15u) == 0) fprintf(stderr, "\n    %08X:", ea);
                fprintf(stderr, " %08X", g32(ea));
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    if (p > ctx_ls) {
        fprintf(stderr, "[spurs-job] job 0x%08X: WARNING regions reach 0x%X, "
                        "past the context at 0x%X\n", job_ea, p, ctx_ls);
        if (guard) {
            spurs_job_ctx_finish(ctx_slot, ls_dirty_end, image_id, job_ea);
            spurs_job_ctx_release(ctx_slot);
            return -1;
        }
    }
    memcpy(ls + desc_ls, vm_base + job_ea, dsz);
    if (guard) {
        ls32(ls, desc_ls + JH_SIZE_IN_OR_INOUT, io_capacity);
        ls32(ls, desc_ls + JH_SIZE_OUT, out_capacity);
        if (size_scr)
            ls16(ls, desc_ls + JH_SIZE_SCRATCH, (uint16_t)((size_scr + guard) >> 4));
        for (uint32_t i = 0; i < n_cache; ++i) {
            uint32_t offset = JH_SIZE + (n_dma + i) * 8u;
            ls32(ls, desc_ls + offset, g32(job_ea + offset) + guard);
        }
    }

    ls32(ls, ctx_ls + JC_IO_BUFFER, io_ls);
    for (int i = 0; i < 4; i++)
        ls32(ls, ctx_ls + JC_CACHE_BUFFER + (uint32_t)i * 4u, cache_ls[i]);
    /* sizeJobDescriptor:4 is the first bitfield of a big-endian u32, so it
     * occupies the TOP 4 bits. 0 means 64 bytes, otherwise n*128. */
    ls32(ls, ctx_ls + JC_SIZE_JOB_DESC,
         (dsz == 64 ? 0u : (dsz / 128u) & 0xFu) << 28);
    ls16(ls, ctx_ls + JC_NUM_IO_BUFFER,    (uint16_t)n_dma);
    ls16(ls, ctx_ls + JC_NUM_CACHE_BUFFER, (uint16_t)n_cache);
    ls32(ls, ctx_ls + JC_O_BUFFER, out_ls);
    ls32(ls, ctx_ls + JC_S_BUFFER, s_ls);
    ls32(ls, ctx_ls + JC_DMA_TAG,  JOB_DMA_TAG);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR,     0);
    ls32(ls, ctx_ls + JC_EA_JOB_DESCRIPTOR + 4, job_ea);

    { static int _n = 0;
      if (_n < 4 && _n++ < 4)
          fprintf(stderr,
              "[spurs-job] job 0x%08X: bin 0x%08X+%u -> LS 0 | io=0x%05X(%u, %u dma, %u used)"
              " out=0x%05X(%u) scratch=0x%05X(%u) stack_top=0x%05X(%u) cache=%u"
              " ctx=0x%05X desc=0x%05X(%u)\n",
              job_ea, ea_bin, size_bin, io_ls, size_io, n_dma, io_used,
              out_ls, size_out, s_ls, size_scr, stack_top, size_stk, n_cache,
              ctx_ls, desc_ls, dsz); }

    /* LBP job protocol probe (LBP_JOB_DUMP): the game's one shared job binary
     * ("JOBCRT Ver13" crt + main @0x1570) reads a be u64 EA out of the
     * descriptor's USER DATA at +0x30, GETs a 128-byte command block from it,
     * then jump-tables on the command TYPE (word1 of the block, cases 0..5).
     * The header's input-DMA-list/io fields are legitimately zero for it. Dump
     * the user data + that command block so the bail path is attributable. */
    uint32_t dump_cmd = 0;
    if (tj_env_lbp_job_dump()) {
        uint64_t ud  = g64(job_ea + JH_SIZE);        /* desc+0x30 */
        uint32_t cmd = (uint32_t)ud;                 /* low word = EA */
        fprintf(stderr, "[spurs-job] job 0x%08X userdata: %016llX %016llX %016llX %016llX\n",
                job_ea, (unsigned long long)ud,
                (unsigned long long)g64(job_ea + JH_SIZE + 8),
                (unsigned long long)g64(job_ea + JH_SIZE + 16),
                (unsigned long long)g64(job_ea + JH_SIZE + 24));
        if (cmd && cmd < 0x50000000u) {
            dump_cmd = cmd;
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X:", cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(cmd + o));
            }
            fprintf(stderr, "\n");
            /* cmdblock+0x1C points at a 2048-byte WORK-ITEM table the type-0/1
             * handler GETs (pc=0x1A78) before entering its per-item worker with
             * count = cmdblock+0x24. Dump its head: all-zero means the game
             * never filled the items (upstream/ordering); real entries mean the
             * SPU worker mishandles them. */
            uint32_t tbl = g32(cmd + 0x1C);
            if (tbl && tbl < 0x50000000u) {
                fprintf(stderr, "[spurs-job]   worktable @0x%08X (n=%u):", tbl, g32(cmd + 0x24));
                for (int o = 0; o < 96; o += 4) {
                    if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                    fprintf(stderr, " %08X", g32(tbl + o));
                }
                fprintf(stderr, "\n");
            }
        }
        fflush(stderr);
    }

    /* ---- enter the job -------------------------------------------------- */
#ifdef _WIN32
    const uint64_t tj_phase_staging_end = tj_phase_perf
        ? tj_spurs_job_phase_qpc() : 0;
#endif
    SpursJobLane lane = {0};
    if (spurs_job_lane_acquire(&lane, spurs_ea) != 0) {
        spurs_job_ctx_finish(ctx_slot, ls_dirty_end, image_id, job_ea);
        spurs_job_ctx_release(ctx_slot);
        return -1;
    }
    ctx->spu_id = lane.number;
    ctx->spurs_job_kernel = spurs_ea != 0;
    ctx->image_id = image_id;
    ctx->spu_group_id = spurs_ea; /* guest CellSpurs EA for user-event routing */
    ctx->gpr[1]._u32[0] = (stack_top - 16u) & ~15u;   /* SPU stack grows down */
    if (guard) {
        /* AI updates every lane as frames grow. Lane 1 is remaining stack
         * space, excluding the initial linkage quadword; word0-word1 stays
         * constant. JOBCRT uses that difference to locate the bottom guard. */
        ctx->gpr[1]._u32[1] = size_stk - 16u;
    }
    ctx->gpr[3]._u32[0] = ctx_ls;                     /* CellSpursJobContext2* */
    ctx->gpr[4]._u32[0] = desc_ls;                    /* CellSpursJob256*      */
#ifdef _WIN32
    const uint64_t tj_phase_ctx_setup_end = tj_phase_perf
        ? tj_spurs_job_phase_qpc() : 0;
#endif

    { static int _n = 0;
      if (_n < 4 && _n++ < 4)
          fprintf(stderr, "[spurs-job] ENTER r3=%08X r4=%08X r1=%08X entry-fn image=%d\n",
                  ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[1]._u32[0],
                  image_id); }
    /* NOTE: after its work the job's runtime jumps to LS 0 ("return to the
     * jm2 kernel" -- real layout has the kernel at 0, jobs above; we load the
     * job at 0). That second lap re-enters the job's crt with dead registers
     * and trips its parameter guard, which HALTS -- ending the run. So the
     * HALT-ASSERT heqi 0x1650 seen once per jm2 job is NORMAL COMPLETION
     * noise, not a failure: the job's real work finished before the jump. */
    static int64_t s_interp_job = -2;
    if (s_interp_job == -2) {
        const char* e = getenv("SPURS_JOB_INTERP");
        s_interp_job = e ? (int64_t)strtoul(e, NULL, 0) : -1;
    }
#ifdef _WIN32
    tj_spu_pc_time_job_begin(image_id);
    const uint64_t tj_phase_lifted_begin = tj_phase_perf
        ? tj_spurs_job_phase_qpc() : 0;
#endif
    if (s_interp_job >= 0 && job_ea == (uint32_t)s_interp_job) {
        fprintf(stderr, "[spurs-job] full interpreter for job 0x%08X\n", job_ea);
        ctx->image_id = -1; /* pure interpreter: do not rejoin this image's lift */
        spu_interp_run(ctx, 0);
    } else {
        spu_run_with_halt(entry, ctx);
    }
    spurs_job_lane_release(&lane);
#ifdef _WIN32
    const uint64_t tj_phase_lifted_end = tj_phase_perf
        ? tj_spurs_job_phase_qpc() : 0;
    tj_spu_pc_time_job_complete(image_id);
    tj_spu_pc_hist_job_complete(image_id);
#endif

    if (tj_env_lbp_job_dump()) {
        fprintf(stderr, "[spurs-job] job 0x%08X exit: status=0x%X stop=0x%X pc=0x%05X\n",
                job_ea, ctx->status, ctx->stop_code, ctx->pc);
        /* Post-run view of the same command block: the game polls result/status
         * fields the job writes back -- a before/after diff shows whether the
         * handler delivered its completion or the PPU waits on stale state. */
        if (dump_cmd) {
            fprintf(stderr, "[spurs-job]   cmdblock @0x%08X AFTER:", dump_cmd);
            for (int o = 0; o < 64; o += 4) {
                if ((o & 15) == 0) fprintf(stderr, "\n      +%02X:", o);
                fprintf(stderr, " %08X", g32(dump_cmd + o));
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

#ifdef _WIN32
    if (tj_phase_perf) {
        const uint64_t tj_phase_total_end = tj_spurs_job_phase_qpc();
        const uint64_t tj_phase_ticks[TJ_SPURS_JOB_PHASE_COUNT] = {
            tj_phase_descriptor_end - tj_phase_total_begin,
            tj_phase_ctx_zero_end - tj_phase_descriptor_end,
            tj_phase_staging_end - tj_phase_ctx_zero_end,
            tj_phase_ctx_setup_end - tj_phase_staging_end,
            0,
            tj_phase_lifted_end - tj_phase_lifted_begin,
            0,
            tj_phase_total_end - tj_phase_total_begin
        };
        tj_spurs_job_phase_record(image_id, tj_phase_ticks,
                                  tj_phase_total_end);
    }
#endif
    spurs_job_ctx_finish(ctx_slot, ls_dirty_end, image_id, job_ea);
    spurs_job_ctx_release(ctx_slot);
    return 0;
}
