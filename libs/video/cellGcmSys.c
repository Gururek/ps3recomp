/*
 * ps3recomp - cellGcmSys HLE module implementation
 *
 * Manages RSX initialization, display buffer registration, flip control,
 * command buffer control, tile/zcull configuration, IO memory mapping,
 * report/label areas, and address-to-offset translation.
 *
 * Actual rendering is handled elsewhere -- this module just tracks state.
 */
/* Where the HLE layer injects the argument block a cellGcm* call passes to the
 * guest (the lifter reads it back through this base). master keeps it here too. */
uint32_t ppu_hle_inject_base = 0x20000000u;

#include "cellGcmSys.h"
#include "rsx_ordered_display_alias.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32 (translate + byte-swap, OOB-safe) */
#include "../../runtime/spu/spu_dma.h"       /* optional image17 PUT diagnostic seam */
#include "../../runtime/spu/spu_main_write_gate.h"
#include "rsx_commands.h"                    /* rsx_state, rsx_process_command_buffer */
#include "rsx_scaled_blit.h"
#include "rsx_boundary_policy.h"
/* The GCM front end drives the RSX through the backend-neutral rsx_state plus
 * the D3D12 backend's completion/boundary API. On POSIX hosts that API is
 * implemented by rsx_vulkan_backend.c, with rsx_backend_shims.c covering the
 * pieces the Vulkan backend has no equivalent for -- so take the declarations
 * (and RsxD3D12CompletionReason) on every host. */
#include "rsx_d3d12_backend.h"

/* Guest EA of the GCM context (begin/end/current/callback) the title writes its
 * command stream into; recorded by cellGcmSetupContext, drained by the RSX. */
static u32 s_gcm_context_ea = 0;
/* io address of the title's command ring = the top of the local-memory IO
 * window (libgcm carves the FIFO ring off the top of local memory); the GCM
 * control block's put/get are offsets from it. Set by cellGcmInit. */
static u32 s_fifo_io_base = 0;
static void gcm_fifo_objects_reset(void);
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#ifdef _WIN32
#define GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES 16u
#define GCM2D_BLIT_SNAPSHOT_POOL_MAX_BYTES (32ULL << 20)
typedef struct Gcm2dBlitSnapshotPoolEntry {
    u8* data;
    u64 bytes;
} Gcm2dBlitSnapshotPoolEntry;
static Gcm2dBlitSnapshotPoolEntry
    s_gcm2d_blit_snapshot_pool[GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES];
static atomic_flag s_gcm2d_blit_snapshot_pool_lock = ATOMIC_FLAG_INIT;
static atomic_int s_gcm2d_blit_snapshot_pool_enabled = ATOMIC_VAR_INIT(0);
static u32 s_gcm2d_blit_snapshot_pool_count;
static u64 s_gcm2d_blit_snapshot_pool_bytes;

static void gcm2d_blit_snapshot_pool_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_gcm2d_blit_snapshot_pool_lock,
                                             memory_order_acquire)) {}
}

static void gcm2d_blit_snapshot_pool_unlock(void)
{
    atomic_flag_clear_explicit(&s_gcm2d_blit_snapshot_pool_lock,
                               memory_order_release);
}

/* Called with the metadata lock held. Returned pointers remain ordinary
 * malloc allocations; only released, exact-size buffers enter this cache. */
static u32 gcm2d_blit_snapshot_pool_detach_locked(
    u8* detached[GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES])
{
    u32 count = 0;
    for (u32 i = 0; i < GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES; ++i) {
        if (s_gcm2d_blit_snapshot_pool[i].data)
            detached[count++] = s_gcm2d_blit_snapshot_pool[i].data;
        s_gcm2d_blit_snapshot_pool[i].data = NULL;
        s_gcm2d_blit_snapshot_pool[i].bytes = 0;
    }
    s_gcm2d_blit_snapshot_pool_count = 0;
    s_gcm2d_blit_snapshot_pool_bytes = 0;
    return count;
}

static void gcm2d_blit_snapshot_pool_free_detached(
    u8* detached[GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES], u32 count)
{
    for (u32 i = 0; i < count; ++i) free(detached[i]);
}

static void gcm2d_blit_snapshot_pool_drain_cache(void)
{
    u8* detached[GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES] = {0};
    gcm2d_blit_snapshot_pool_lock();
    const u32 count = gcm2d_blit_snapshot_pool_detach_locked(detached);
    gcm2d_blit_snapshot_pool_unlock();
    gcm2d_blit_snapshot_pool_free_detached(detached, count);
}

void rsx_d3d12_blit_snapshot_pool_init(void)
{
    const char* value = getenv("TJ_BLIT_SNAPSHOT_POOL");
    const int enabled = value && strcmp(value, "1") == 0;
    u8* detached[GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES] = {0};
    gcm2d_blit_snapshot_pool_lock();
    atomic_store_explicit(&s_gcm2d_blit_snapshot_pool_enabled, 0,
                          memory_order_release);
    const u32 count = gcm2d_blit_snapshot_pool_detach_locked(detached);
    atomic_store_explicit(&s_gcm2d_blit_snapshot_pool_enabled, enabled,
                          memory_order_release);
    gcm2d_blit_snapshot_pool_unlock();
    gcm2d_blit_snapshot_pool_free_detached(detached, count);
}

void rsx_d3d12_blit_snapshot_pool_shutdown(void)
{
    u8* detached[GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES] = {0};
    gcm2d_blit_snapshot_pool_lock();
    atomic_store_explicit(&s_gcm2d_blit_snapshot_pool_enabled, 0,
                          memory_order_release);
    const u32 count = gcm2d_blit_snapshot_pool_detach_locked(detached);
    gcm2d_blit_snapshot_pool_unlock();
    gcm2d_blit_snapshot_pool_free_detached(detached, count);
}

u8* rsx_d3d12_blit_snapshot_acquire(u64 bytes)
{
    if (!bytes || bytes > (u64)SIZE_MAX) return NULL;
    if (bytes <= GCM2D_BLIT_SNAPSHOT_POOL_MAX_BYTES &&
        atomic_load_explicit(&s_gcm2d_blit_snapshot_pool_enabled,
                             memory_order_acquire)) {
        u8* reused = NULL;
        gcm2d_blit_snapshot_pool_lock();
        if (atomic_load_explicit(&s_gcm2d_blit_snapshot_pool_enabled,
                                 memory_order_relaxed)) {
            for (u32 i = 0; i < GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES; ++i) {
                Gcm2dBlitSnapshotPoolEntry* entry = &s_gcm2d_blit_snapshot_pool[i];
                if (!entry->data || entry->bytes != bytes) continue;
                reused = entry->data;
                entry->data = NULL;
                entry->bytes = 0;
                --s_gcm2d_blit_snapshot_pool_count;
                s_gcm2d_blit_snapshot_pool_bytes -= bytes;
                break;
            }
        }
        gcm2d_blit_snapshot_pool_unlock();
        if (reused) return reused;
    }

    u8* allocated = (u8*)malloc((size_t)bytes);
    if (allocated ||
        !atomic_load_explicit(&s_gcm2d_blit_snapshot_pool_enabled,
                              memory_order_acquire)) return allocated;
    gcm2d_blit_snapshot_pool_drain_cache();
    return (u8*)malloc((size_t)bytes);
}

void rsx_d3d12_blit_snapshot_release(u8* snapshot, u64 bytes)
{
    if (!snapshot) return;
    if (!bytes || bytes > GCM2D_BLIT_SNAPSHOT_POOL_MAX_BYTES ||
        !atomic_load_explicit(&s_gcm2d_blit_snapshot_pool_enabled,
                              memory_order_acquire)) {
        free(snapshot);
        return;
    }

    int retained = 0;
    gcm2d_blit_snapshot_pool_lock();
    if (atomic_load_explicit(&s_gcm2d_blit_snapshot_pool_enabled,
                             memory_order_relaxed) &&
        s_gcm2d_blit_snapshot_pool_count < GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES &&
        bytes <= GCM2D_BLIT_SNAPSHOT_POOL_MAX_BYTES -
                     s_gcm2d_blit_snapshot_pool_bytes) {
        for (u32 i = 0; i < GCM2D_BLIT_SNAPSHOT_POOL_MAX_ENTRIES; ++i) {
            Gcm2dBlitSnapshotPoolEntry* entry = &s_gcm2d_blit_snapshot_pool[i];
            if (entry->data) continue;
            entry->data = snapshot;
            entry->bytes = bytes;
            ++s_gcm2d_blit_snapshot_pool_count;
            s_gcm2d_blit_snapshot_pool_bytes += bytes;
            retained = 1;
            break;
        }
    }
    gcm2d_blit_snapshot_pool_unlock();
    if (!retained) free(snapshot);
}
#endif

/* ---------------------------------------------------------------------------
 * Timestamp helpers (same pattern as sys_timer)
 * -----------------------------------------------------------------------*/

#ifdef _WIN32
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

static u64 get_timestamp_ns(void)
{
    LARGE_INTEGER now;
    ensure_qpc_init();
    QueryPerformanceCounter(&now);
    /* Convert to nanoseconds: (count * 1e9) / freq */
    return (u64)((double)now.QuadPart * 1000000000.0 / (double)s_qpc_freq.QuadPart);
}
#else
/* The compat layer maps QueryPerformanceCounter onto a nanosecond monotonic
 * clock, so the "frequency" is 1e9 and the init flag only has to be set. */
static LARGE_INTEGER s_qpc_freq;
static int           s_qpc_init = 0;

static void ensure_qpc_init(void)
{
    if (!s_qpc_init) {
        QueryPerformanceFrequency(&s_qpc_freq);
        s_qpc_init = 1;
    }
}

static u64 get_timestamp_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_gcm_initialized = 0;
/* Serializes FIFO walking with teardown. cellGcmTerminate takes this before
 * resetting the interrupt queue, so an active walker cannot publish into a
 * newly reset GCM lifetime. */
static SRWLOCK s_gcm_fifo_lock = SRWLOCK_INIT;
/* PUT may publish only part of a method packet. Retain the decoded header
 * while GET points at its next unread argument; that word is not a header.
 * Both walkers and the recycler access this under s_gcm_fifo_lock. */
typedef struct GcmFifoPacket {
    u32 raw, header_ea, header_get;
    u32 next_index, remaining;
} GcmFifoPacket;
static GcmFifoPacket s_fifo_packet;
/* NV406E frontend semaphore registers, owned by the FIFO lock. Both label
 * DMA contexts address the same guest report window; device DMA does not. */
static u32 s_fifo_semaphore_offset;
static u32 s_fifo_semaphore_dma = 0x66606660u;
static u64 s_fifo_semaphore_wait_since_ns;
static atomic_uint s_flip_mode = CELL_GCM_DISPLAY_VSYNC;
/* Written by the host present ticker and read by guest PPU threads. The value
 * is a single aligned word (matching the hardware status register); volatile
 * prevents a polling thread from caching it across HLE calls. Guest callback
 * delivery remains separately serialized through s_gcm_pending below. */
static atomic_uint s_flip_status = CELL_GCM_FLIP_STATUS_DONE;
static volatile int s_tj_survival_flip_diag = 0;
static atomic_bool s_tj_survival_profile_armed = ATOMIC_VAR_INIT(0);
static atomic_uint s_tj_flip_wait_streak = 0;
static atomic_uint s_tj_flip_trace_budget = 0;
static atomic_uint s_tj_flip_reset_count = 0;

static int tj_survival_flip_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TJ_SURV_FLIP_TRACE") ? 1 : 0;
    return enabled;
}

static u32 flip_status_load(void)
{
    return atomic_load_explicit(&s_flip_status, memory_order_acquire);
}

static void flip_status_store(u32 value)
{
    atomic_store_explicit(&s_flip_status, value, memory_order_release);
}

/* ---------------------------------------------------------------------------
 * Guest-thread poll waits: cellGcmSetWaitFlip and the two FIFO-recycle loops.
 *
 * TJ_FLIP_WAIT_EVENT=1: each Sleep(1) pause of those loops instead blocks on
 * a channel (SRW lock + condition variable + generation) that the producer
 * bumps right after the awaited state changes: a non-WAITING flip status
 * (ticket completion, immediate flip, SetFlipStatus, Init/Terminate) or a FIFO
 * walk releasing s_gcm_fifo_lock (GET/CALL/packet state). A waiter samples the
 * generation before each predicate check and sleeps only while it is
 * unchanged, so no wakeup is lost; a 2 ms timeout still bounds every pause.
 * Loop budgets count elapsed milliseconds instead of Sleep(1) calls, so extra
 * wakeups never shorten a grace period or a timeout. Predicates, results and
 * guest-visible writes are unchanged. Unset: the old Sleep(1) loops exactly.
 *
 * TJ_HLE_PERF (presence, as in ppu_hle.cpp): per-site calls, pauses and wall
 * time, printed by cellGcm_wait_perf_report() as one [TJ-GCM-WAIT-PERF] line
 * at the end of each TJ-HLE-PERF window (same 2 s window, main PPU thread).
 * flip_poll times the guest's own GetFlipStatus loop: first WAITING poll to
 * the next non-WAITING poll on the same thread (Tokyo Jungle func_001F31D4:
 * GetFlipStatus + sys_timer_usleep(100) at lr 0x001F3200).
 * -----------------------------------------------------------------------*/
enum { GCM_WAIT_CH_FLIP, GCM_WAIT_CH_FIFO, GCM_WAIT_CHANNELS };
enum { GCM_WAIT_SITE_SETWAITFLIP, GCM_WAIT_SITE_RECYCLE, GCM_WAIT_SITE_WINDOW,
       GCM_WAIT_SITE_FLIP_POLL, GCM_WAIT_SITES };
enum { GCM_WAIT_OUT_READY, GCM_WAIT_OUT_ALT, GCM_WAIT_OUT_LIMIT, GCM_WAIT_OUTS };
#define GCM_WAIT_EVENT_TIMEOUT_MS 2u

typedef struct GcmWaitChannel {
    SRWLOCK lock;                  /* static zero == SRWLOCK_INIT */
    CONDITION_VARIABLE cv;         /* static zero == CONDITION_VARIABLE_INIT */
    atomic_uint generation;        /* bumped only under lock */
} GcmWaitChannel;
static GcmWaitChannel s_gcm_wait_channels[GCM_WAIT_CHANNELS];
static atomic_int s_gcm_wait_event_enabled = -1;
static atomic_int s_gcm_wait_perf_enabled = -1;

static int gcm_wait_event_enabled(void)
{
    int enabled = atomic_load_explicit(&s_gcm_wait_event_enabled, memory_order_relaxed);
    if (enabled >= 0) return enabled;
    const char* value = getenv("TJ_FLIP_WAIT_EVENT");
    const int requested = value && strcmp(value, "1") == 0;
    int expected = -1;
    if (!atomic_compare_exchange_strong_explicit(&s_gcm_wait_event_enabled,
                                                 &expected, requested,
                                                 memory_order_relaxed,
                                                 memory_order_relaxed))
        return expected;
    if (requested)
        fprintf(stderr, "[tj-flip-wait-event] enabled timeout_ms=%u\n",
                GCM_WAIT_EVENT_TIMEOUT_MS);
    return requested;
}

static int gcm_wait_perf_enabled(void)
{
    int enabled = atomic_load_explicit(&s_gcm_wait_perf_enabled, memory_order_relaxed);
    if (enabled < 0) {
        enabled = getenv("TJ_HLE_PERF") ? 1 : 0;
        atomic_store_explicit(&s_gcm_wait_perf_enabled, enabled, memory_order_relaxed);
    }
    return enabled;
}

static u64 gcm_wait_qpc(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (u64)now.QuadPart;
}

static u64 gcm_wait_ticks_us(u64 ticks)
{
    ensure_qpc_init();
    const u64 freq = (u64)s_qpc_freq.QuadPart;
    return freq ? (ticks / freq) * 1000000ull + ((ticks % freq) * 1000000ull) / freq : 0;
}

/* Producer side: call after the awaited state is published and after any lock
 * guarding it is released. No-op unless TJ_FLIP_WAIT_EVENT=1. */
static void gcm_wait_signal(int channel)
{
    if (!gcm_wait_event_enabled()) return;
    GcmWaitChannel* ch = &s_gcm_wait_channels[channel];
    AcquireSRWLockExclusive(&ch->lock);
    atomic_fetch_add_explicit(&ch->generation, 1u, memory_order_release);
    ReleaseSRWLockExclusive(&ch->lock);
    WakeAllConditionVariable(&ch->cv);
}

typedef struct GcmWaitPerfSite {
    u64 calls, waited, pauses, wakes, timeouts;
    u64 ticks, main_ticks, max_ticks;
    u64 out[GCM_WAIT_OUTS];
} GcmWaitPerfSite;
typedef struct GcmWaitPerf {
    u64 window_qpc;
    u64 seen_ticks, seen_count;    /* flip_poll: DONE publication -> poll saw it */
    GcmWaitPerfSite site[GCM_WAIT_SITES];
} GcmWaitPerf;
static GcmWaitPerf s_gcm_wait_perf;
static atomic_flag s_gcm_wait_perf_lock = ATOMIC_FLAG_INIT;
/* Host thread of the TJ-HLE-PERF reporter (main PPU); 0 until its first
 * report, so the first window's main_us can undercount. */
static atomic_uint s_gcm_wait_perf_main_tid = 0;
static atomic_ullong s_gcm_flip_done_qpc = 0;   /* TJ_HLE_PERF only */

static void gcm_wait_perf_add(int site, u64 ticks, u64 pauses, u64 wakes,
                              u64 timeouts, int outcome, const u64* seen_ticks)
{
    const int main_thread = (u32)GetCurrentThreadId() ==
        atomic_load_explicit(&s_gcm_wait_perf_main_tid, memory_order_relaxed);
    while (atomic_flag_test_and_set_explicit(&s_gcm_wait_perf_lock,
                                             memory_order_acquire)) {}
    GcmWaitPerfSite* s = &s_gcm_wait_perf.site[site];
    s->calls++;
    if (pauses) s->waited++;
    s->pauses += pauses;
    s->wakes += wakes;
    s->timeouts += timeouts;
    s->ticks += ticks;
    if (main_thread) s->main_ticks += ticks;
    if (ticks > s->max_ticks) s->max_ticks = ticks;
    s->out[outcome]++;
    if (seen_ticks) {
        s_gcm_wait_perf.seen_ticks += *seen_ticks;
        s_gcm_wait_perf.seen_count++;
    }
    atomic_flag_clear_explicit(&s_gcm_wait_perf_lock, memory_order_release);
}

typedef struct GcmPollWait {
    int site, channel, event, timed;
    u32 seen;                      /* channel generation before the last check */
    u64 begin_qpc, pauses, wakes, timeouts;
    void (*kick)(void);            /* event mode: run before the first pause */
} GcmPollWait;

/* Call before the loop's first predicate check. */
static void gcm_poll_wait_begin(GcmPollWait* w, int site, int channel)
{
    w->site = site;
    w->channel = channel;
    w->event = gcm_wait_event_enabled();
    w->timed = gcm_wait_perf_enabled();
    w->seen = w->event
        ? atomic_load_explicit(&s_gcm_wait_channels[channel].generation,
                               memory_order_acquire)
        : 0;
    w->begin_qpc = (w->event || w->timed) ? gcm_wait_qpc() : 0;
    w->pauses = w->wakes = w->timeouts = 0;
    w->kick = NULL;
}

/* Replaces one `Sleep(1); spins++` of the old loops; returns the new count.
 * Event mode blocks until the generation moves past the value sampled before
 * the caller's last predicate check (or the timeout); the count is then the
 * elapsed whole milliseconds since gcm_poll_wait_begin. */
static int gcm_poll_wait_pause(GcmPollWait* w, int spins)
{
    if (!w->event) {
        Sleep(1);
        w->pauses++;
        return spins + 1;
    }
    if (!w->pauses && w->kick) w->kick();
    w->pauses++;
    GcmWaitChannel* ch = &s_gcm_wait_channels[w->channel];
    int failed = 0;
    AcquireSRWLockExclusive(&ch->lock);
    if (atomic_load_explicit(&ch->generation, memory_order_relaxed) == w->seen) {
        if (SleepConditionVariableSRW(&ch->cv, &ch->lock, GCM_WAIT_EVENT_TIMEOUT_MS, 0)) {
            w->wakes++;
        } else {
            w->timeouts++;
            failed = GetLastError() != ERROR_TIMEOUT;
        }
    } else {
        w->wakes++;                /* signalled after the last sample */
    }
    w->seen = atomic_load_explicit(&ch->generation, memory_order_relaxed);
    ReleaseSRWLockExclusive(&ch->lock);
    if (failed) Sleep(1);          /* never spin on a failing wait */
    const u64 elapsed_ms = gcm_wait_ticks_us(gcm_wait_qpc() - w->begin_qpc) / 1000ull;
    return elapsed_ms > 0x3FFFFFFFull ? 0x3FFFFFFF : (int)elapsed_ms;
}

static void gcm_poll_wait_end(const GcmPollWait* w, int outcome)
{
    if (!w->timed) return;
    gcm_wait_perf_add(w->site, gcm_wait_qpc() - w->begin_qpc, w->pauses,
                      w->wakes, w->timeouts, outcome, NULL);
}

/* A non-WAITING flip status was just published (call after the publication
 * lock is released): wake SetWaitFlip waiters, stamp flip_poll latency. */
static void gcm_flip_status_done_notify(void)
{
    if (gcm_wait_perf_enabled())
        atomic_store_explicit(&s_gcm_flip_done_qpc, gcm_wait_qpc(),
                              memory_order_relaxed);
    gcm_wait_signal(GCM_WAIT_CH_FLIP);
}

/* TJ_HLE_PERF only: the guest's GetFlipStatus poll loop, per thread. The
 * latency sample is dropped when the presenter's DONE stamp races the poll. */
static _Thread_local u64 s_gcm_flip_poll_begin_qpc;
static _Thread_local u64 s_gcm_flip_poll_polls;

static void gcm_flip_poll_observe(u32 status, u64 waiting_qpc)
{
    if (!gcm_wait_perf_enabled()) return;
    if (status == CELL_GCM_FLIP_STATUS_WAITING) {
        if (!s_gcm_flip_poll_polls) s_gcm_flip_poll_begin_qpc = waiting_qpc;
        s_gcm_flip_poll_polls++;
        return;
    }
    if (!s_gcm_flip_poll_polls) return;
    const u64 now = gcm_wait_qpc();
    const u64 begin = s_gcm_flip_poll_begin_qpc;
    const u64 done = atomic_load_explicit(&s_gcm_flip_done_qpc, memory_order_relaxed);
    const u64 seen = now - done;
    gcm_wait_perf_add(GCM_WAIT_SITE_FLIP_POLL, now - begin, s_gcm_flip_poll_polls,
                      0, 0, GCM_WAIT_OUT_READY,
                      done >= begin && done <= now ? &seen : NULL);
    s_gcm_flip_poll_polls = 0;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 4, 5)))
#endif
static int gcm_wait_perf_append(char* line, int len, int cap, const char* fmt, ...)
{
    if (len < 0 || len >= cap) return len;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line + len, (size_t)(cap - len), fmt, args);
    va_end(args);
    return n < 0 ? len : (len + n < cap ? len + n : cap - 1);
}

/* TJ_HLE_PERF: called by ppu_hle.cpp at the end of each TJ-HLE-PERF window
 * on the main PPU thread; that thread's waits are reported as main_us. */
void cellGcm_wait_perf_report(void);
void cellGcm_wait_perf_report(void)
{
    if (!gcm_wait_perf_enabled()) return;
    atomic_store_explicit(&s_gcm_wait_perf_main_tid, (u32)GetCurrentThreadId(),
                          memory_order_relaxed);
    const u64 now = gcm_wait_qpc();
    GcmWaitPerf snap;
    while (atomic_flag_test_and_set_explicit(&s_gcm_wait_perf_lock,
                                             memory_order_acquire)) {}
    snap = s_gcm_wait_perf;
    memset(&s_gcm_wait_perf, 0, sizeof(s_gcm_wait_perf));
    s_gcm_wait_perf.window_qpc = now;
    atomic_flag_clear_explicit(&s_gcm_wait_perf_lock, memory_order_release);

    static const char* const names[GCM_WAIT_SITES] = {
        "setwaitflip", "recycle", "window", "flip_poll" };
    static const char* const outcomes[GCM_WAIT_SITES][GCM_WAIT_OUTS] = {
        { "done", NULL, "budget" }, { "ready", "window", "fail" },
        { "restored", "extended", "fail" }, { NULL, NULL, NULL } };
    char line[1536];
    const int cap = (int)sizeof(line) - 1;     /* room for the newline */
    int len = gcm_wait_perf_append(line, 0, cap,
        "[TJ-GCM-WAIT-PERF] t_ms=%llu window_ms=%llu mode=%s",
        (unsigned long long)GetTickCount64(),
        (unsigned long long)(snap.window_qpc
            ? gcm_wait_ticks_us(now - snap.window_qpc) / 1000ull : 0),
        gcm_wait_event_enabled() ? "event" : "sleep");
    for (int i = 0; i < GCM_WAIT_SITES; ++i) {
        const GcmWaitPerfSite* s = &snap.site[i];
        len = gcm_wait_perf_append(line, len, cap,
            " %s={calls=%llu waited=%llu pauses=%llu total_us=%llu main_us=%llu max_us=%llu",
            names[i], (unsigned long long)s->calls, (unsigned long long)s->waited,
            (unsigned long long)s->pauses,
            (unsigned long long)gcm_wait_ticks_us(s->ticks),
            (unsigned long long)gcm_wait_ticks_us(s->main_ticks),
            (unsigned long long)gcm_wait_ticks_us(s->max_ticks));
        for (int k = 0; k < GCM_WAIT_OUTS; ++k)
            if (outcomes[i][k])
                len = gcm_wait_perf_append(line, len, cap, " %s=%llu", outcomes[i][k],
                                           (unsigned long long)s->out[k]);
        if (i == GCM_WAIT_SITE_FLIP_POLL)
            len = gcm_wait_perf_append(line, len, cap, " seen_n=%llu seen_us=%llu}",
                (unsigned long long)snap.seen_count,
                (unsigned long long)gcm_wait_ticks_us(snap.seen_ticks));
        else
            len = gcm_wait_perf_append(line, len, cap, " wakes=%llu timeouts=%llu}",
                (unsigned long long)s->wakes, (unsigned long long)s->timeouts);
    }
    line[len++] = '\n';
    fwrite(line, 1, (size_t)len, stderr);
}

static void tj_flip_trace(const char* event, u32 detail)
{
    unsigned left = atomic_load_explicit(&s_tj_flip_trace_budget,
                                         memory_order_relaxed);
    while (left != 0 &&
           !atomic_compare_exchange_weak_explicit(&s_tj_flip_trace_budget,
                                                  &left, left - 1,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
    if (left == 0) return;

    fprintf(stderr,
            "[TJ-FLIP-STALL] t=%llu tid=%lu event=%s detail=%u status=%u left=%u\n",
            (unsigned long long)(get_timestamp_ns() / 1000000ULL),
            (unsigned long)GetCurrentThreadId(), event, detail,
            flip_status_load(), left - 1);
    fflush(stderr);
}

/* Count of guest flip requests (SetFlipCommand / SetPrepareFlip). The present
 * loop uses this as the frame boundary: presenting only when a flip arrived
 * keeps partially-drained frames off screen (the ticker otherwise presents on
 * a fixed 16ms clock, and a drain that catches the guest mid-frame -- e.g.
 * while it's blocked in the FIFO-wrap recycle callback -- would show a
 * clear+few-draws frame as a visible flicker). */
static _Atomic(u32) s_flip_request_count = 0;
enum { FRAME_PROF_DIRECT_HLE = CELL_GCM_FLIP_SOURCE_DIRECT_HLE,
       FRAME_PROF_FIFO_DISPATCH = CELL_GCM_FLIP_SOURCE_FIFO };
static atomic_int s_frame_prof_enabled = ATOMIC_VAR_INIT(-1);
static atomic_flag s_frame_prof_lock = ATOMIC_FLAG_INIT;
static u32 s_frame_prof_generation = 0;
static u32 s_frame_prof_flip_sequence = 0;
static u32 s_frame_prof_source_kind = 0;
static u32 s_frame_prof_source_tid = 0;

static int frame_prof_enabled(void)
{
    int enabled = atomic_load_explicit(&s_frame_prof_enabled, memory_order_acquire);
    if (enabled >= 0) return enabled;
    int expected = -1;
    if (atomic_compare_exchange_strong_explicit(&s_frame_prof_enabled, &expected, -2,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        enabled = getenv("TJ_FRAME_PROF") ? 1 : 0;
        atomic_store_explicit(&s_frame_prof_enabled, enabled, memory_order_release);
        return enabled;
    }
    do {
        enabled = atomic_load_explicit(&s_frame_prof_enabled, memory_order_acquire);
    } while (enabled == -2);
    return enabled;
}

static void frame_prof_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_frame_prof_lock, memory_order_acquire)) {}
}

static void frame_prof_unlock(void)
{
    atomic_flag_clear_explicit(&s_frame_prof_lock, memory_order_release);
}

static u32 frame_prof_host_tid(void)
{
#ifdef _WIN32
    return (u32)GetCurrentThreadId();
#else
    return 0;
#endif
}

static void tj_geom_fifo_trace_arm_img17(u32 cmd, u32 lsa, u32 ea, u32 size,
                                         u32 pc, const u8* copied_bytes);

static void frame_prof_publish(u32 source_kind)
{
    if (!frame_prof_enabled()) return;
    frame_prof_lock();
    s_frame_prof_source_kind = source_kind;
    s_frame_prof_source_tid = frame_prof_host_tid();
    s_frame_prof_flip_sequence++;
    frame_prof_unlock();
}

static void frame_prof_reset(void)
{
    if (!frame_prof_enabled()) return;
    frame_prof_lock();
    s_frame_prof_generation++;
    s_frame_prof_flip_sequence = 0;
    s_frame_prof_source_kind = 0;
    s_frame_prof_source_tid = 0;
    frame_prof_unlock();
}

/* Opt-in timing for the host flip handshake.  The data is deliberately kept
 * here (rather than the present loop) so a SetFlipCommand request and the
 * TickFlip completion share one serialized, monotonic timeline. */
typedef struct TjFlipPerf {
    u64 report_start_ns;
    u64 last_tick_ns;
    u64 wait_start_ns;
    u64 wait_total_ns;
    u64 wait_max_ns;
    u64 tick_total_ns;
    u64 tick_max_ns;
    u32 requests;
    u32 overwritten;
    u32 completed;
    u32 ticks;
    u32 tick_intervals;
    int waiting;
} TjFlipPerf;

static atomic_int s_tj_flip_perf_enabled = ATOMIC_VAR_INIT(-1);
static atomic_flag s_tj_flip_perf_lock = ATOMIC_FLAG_INIT;
static TjFlipPerf s_tj_flip_perf;
#ifdef _WIN32
static __declspec(thread) int s_tj_flip_perf_tls_enabled = -1;
#else
static __thread int s_tj_flip_perf_tls_enabled = -1;
#endif

static int tj_flip_perf_enabled(void)
{
    int enabled = s_tj_flip_perf_tls_enabled;
    if (enabled >= 0) return enabled;

    enabled = atomic_load_explicit(&s_tj_flip_perf_enabled, memory_order_acquire);
    if (enabled < 0) {
        int expected = -1;
        if (atomic_compare_exchange_strong_explicit(&s_tj_flip_perf_enabled,
                                                    &expected, -2,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            enabled = getenv("TJ_FLIP_PERF") ? 1 : 0;
            atomic_store_explicit(&s_tj_flip_perf_enabled, enabled,
                                  memory_order_release);
        } else {
            do {
                enabled = atomic_load_explicit(&s_tj_flip_perf_enabled,
                                               memory_order_acquire);
            } while (enabled == -2);
        }
    }
    s_tj_flip_perf_tls_enabled = enabled;
    return enabled;
}

static void tj_flip_perf_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_tj_flip_perf_lock,
                                             memory_order_acquire)) {}
}

static void tj_flip_perf_unlock(void)
{
    atomic_flag_clear_explicit(&s_tj_flip_perf_lock, memory_order_release);
}

static void tj_flip_perf_log(const TjFlipPerf* perf, u64 now_ns, u64 report_end_ms)
{
    u64 window_ns = now_ns - perf->report_start_ns;
    u64 wait_avg_us = perf->completed ?
        (perf->wait_total_ns / perf->completed) / 1000ULL : 0;
    u64 tick_avg_us = perf->tick_intervals ?
        (perf->tick_total_ns / perf->tick_intervals) / 1000ULL : 0;
    fprintf(stderr,
            "[TJ-FLIP-PERF] t_ms=%llu window_ms=%llu request={total=%u overwritten=%u pending=%u} "
            "done={total=%u avg_us=%llu max_us=%llu} "
            "ticker={ticks=%u intervals=%u interval_avg_us=%llu interval_max_us=%llu}\n",
            (unsigned long long)report_end_ms,
            (unsigned long long)(window_ns / 1000000ULL),
            perf->requests, perf->overwritten, perf->waiting ? 1u : 0u,
            perf->completed, (unsigned long long)wait_avg_us,
            (unsigned long long)(perf->wait_max_ns / 1000ULL), perf->ticks,
            perf->tick_intervals,
            (unsigned long long)tick_avg_us,
            (unsigned long long)(perf->tick_max_ns / 1000ULL));
    fflush(stderr);
}

static void tj_flip_perf_set_waiting(void)
{
    if (!tj_flip_perf_enabled()) return;

    u64 now_ns = get_timestamp_ns();
    tj_flip_perf_lock();
    if (!s_tj_flip_perf.report_start_ns)
        s_tj_flip_perf.report_start_ns = now_ns;
    s_tj_flip_perf.requests++;
    /* A ticker may have completed the status between its store and this
     * diagnostic hook. Count the request, but do not arm a false wait. */
    if (flip_status_load() == CELL_GCM_FLIP_STATUS_WAITING) {
        if (s_tj_flip_perf.waiting)
            s_tj_flip_perf.overwritten++;
        s_tj_flip_perf.wait_start_ns = now_ns;
        s_tj_flip_perf.waiting = 1;
    } else {
        s_tj_flip_perf.waiting = 0;
    }
    tj_flip_perf_unlock();
}

static void tj_flip_perf_tick_done(void)
{
    TjFlipPerf snapshot = {0};
    u64 now_ns;
    u64 report_end_ms = 0;
    int report = 0;

    if (!tj_flip_perf_enabled()) return;
    now_ns = get_timestamp_ns();
    tj_flip_perf_lock();
    if (!s_tj_flip_perf.report_start_ns)
        s_tj_flip_perf.report_start_ns = now_ns;
    s_tj_flip_perf.ticks++;
    if (s_tj_flip_perf.last_tick_ns) {
        u64 interval_ns = now_ns - s_tj_flip_perf.last_tick_ns;
        s_tj_flip_perf.tick_total_ns += interval_ns;
        s_tj_flip_perf.tick_intervals++;
        if (interval_ns > s_tj_flip_perf.tick_max_ns)
            s_tj_flip_perf.tick_max_ns = interval_ns;
    }
    s_tj_flip_perf.last_tick_ns = now_ns;
    if (s_tj_flip_perf.waiting) {
        u64 wait_ns = now_ns - s_tj_flip_perf.wait_start_ns;
        s_tj_flip_perf.wait_total_ns += wait_ns;
        if (wait_ns > s_tj_flip_perf.wait_max_ns)
            s_tj_flip_perf.wait_max_ns = wait_ns;
        s_tj_flip_perf.completed++;
        s_tj_flip_perf.waiting = 0;
    }
    if (now_ns - s_tj_flip_perf.report_start_ns >= 1000000000ULL) {
#ifdef _WIN32
        report_end_ms = GetTickCount64();
#endif
        snapshot = s_tj_flip_perf;
        s_tj_flip_perf.report_start_ns = now_ns;
        s_tj_flip_perf.wait_total_ns = 0;
        s_tj_flip_perf.wait_max_ns = 0;
        s_tj_flip_perf.tick_total_ns = 0;
        s_tj_flip_perf.tick_max_ns = 0;
        s_tj_flip_perf.requests = 0;
        s_tj_flip_perf.overwritten = 0;
        s_tj_flip_perf.completed = 0;
        s_tj_flip_perf.ticks = 0;
        s_tj_flip_perf.tick_intervals = 0;
        /* Keep last_tick/wait_start/waiting: both may straddle windows. */
        report = 1;
    }
    tj_flip_perf_unlock();
    if (report) tj_flip_perf_log(&snapshot, now_ns, report_end_ms);
}

static void tj_flip_perf_reset(void)
{
    if (!tj_flip_perf_enabled()) return;
    tj_flip_perf_lock();
    memset(&s_tj_flip_perf, 0, sizeof(s_tj_flip_perf));
    tj_flip_perf_unlock();
}

static s32 cellGcmSetFlipCommand_source(u32 bufferId, u32 frame_prof_source);

u32 cellGcm_flip_request_count(void)
{
    return atomic_load_explicit(&s_flip_request_count, memory_order_acquire);
}
static u32  s_debug_level = CELL_GCM_DEBUG_LEVEL0;

/* Display buffers */
static CellGcmDisplayInfo s_display_buffers[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static int s_display_buffer_set[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
static _Atomic(u32) s_current_display_buffer_id = 0;
/* Backend label-early memo validates against this pair. Guest threads write
 * display/tile tables unlocked: busy before first store, gen after last. */
static atomic_uint s_gcm_identity_busy = 0;
static atomic_ullong s_gcm_identity_gen = 0;
static void gcm_identity_write_begin(void)
{ atomic_fetch_add_explicit(&s_gcm_identity_busy, 1u, memory_order_seq_cst); }
static void gcm_identity_write_end(void)
{
    atomic_fetch_add_explicit(&s_gcm_identity_gen, 1ull, memory_order_seq_cst);
    atomic_fetch_sub_explicit(&s_gcm_identity_busy, 1u, memory_order_seq_cst);
}
/* Backend query (not a guest API): 0 while a table write is in flight. */
int cellGcm_identity_generation(u64* gen)
{
    if (atomic_load_explicit(&s_gcm_identity_busy, memory_order_seq_cst)) return 0;
    *gen = (u64)atomic_load_explicit(&s_gcm_identity_gen, memory_order_seq_cst);
    return 1;
}

/* A one-deep pending ticket matches the pre-existing flip gate semantics:
 * another SetFlip before the ticker overwrites the earlier request.  Unlike
 * the former independent `pending` bool and display-id reads, this lock makes
 * generation, buffer id, and consumption one coherent handoff. */
typedef struct GcmFlipPublication {
    atomic_flag lock;
    u64 next_generation;
    CellGcmFlipTicket pending;
    CellGcmFlipTicket consumed;
    CellGcmFlipPublicationTiming pending_timing;
    CellGcmFlipPublicationTiming consumed_timing;
    u64 last_publication_qpc;
    int pending_valid;
    u64 completed_generation;
} GcmFlipPublication;
static GcmFlipPublication s_flip_publication = { ATOMIC_FLAG_INIT, 0 };
static atomic_uint s_flip_callbacks_pending = 0;
static atomic_int s_flip_publication_qpc_enabled = ATOMIC_VAR_INIT(-1);

static void gcm_flip_publication_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_flip_publication.lock,
                                             memory_order_acquire)) {}
}

static void gcm_flip_publication_unlock(void)
{
    atomic_flag_clear_explicit(&s_flip_publication.lock, memory_order_release);
}

static void gcm_flip_kick_presenter(void);

static int gcm_flip_publication_qpc_enabled(void)
{
#ifdef _WIN32
    int enabled = atomic_load_explicit(&s_flip_publication_qpc_enabled,
                                       memory_order_acquire);
    if (enabled >= 0) return enabled;
    const char* value = getenv("TJ_FLIP_TIMELINE");
    int requested = value && strcmp(value, "1") == 0;
    int expected = -1;
    if (!atomic_compare_exchange_strong_explicit(&s_flip_publication_qpc_enabled,
                                                  &expected, requested,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire))
        requested = expected;
    return requested;
#else
    return 0;
#endif
}

/* Called with publication lock held, immediately before a pending ticket is
 * visible. It is observation only: failures do not alter flip semantics. */
static CellGcmFlipPublicationTiming gcm_flip_publication_timestamp_locked(int pending)
{
    CellGcmFlipPublicationTiming timing = { 0, CELL_GCM_FLIP_PUBLICATION_QPC_OFF };
#ifdef _WIN32
    if (!pending || !gcm_flip_publication_qpc_enabled()) return timing;
    LARGE_INTEGER now = { 0 };
    if (!QueryPerformanceCounter(&now) || now.QuadPart <= 0 ||
        (s_flip_publication.last_publication_qpc &&
         (u64)now.QuadPart < s_flip_publication.last_publication_qpc)) {
        timing.publication_qpc_state = CELL_GCM_FLIP_PUBLICATION_QPC_FAILED;
        return timing;
    }
    timing.publication_qpc = (u64)now.QuadPart;
    timing.publication_qpc_state = CELL_GCM_FLIP_PUBLICATION_QPC_VALID;
    s_flip_publication.last_publication_qpc = timing.publication_qpc;
#else
    (void)pending;
#endif
    return timing;
}

static void gcm_flip_publish(u32 buffer_id, int pending, u32 source)
{
    gcm_flip_publication_lock();
    CellGcmFlipTicket ticket = { ++s_flip_publication.next_generation, buffer_id, source };
    if (pending) {
        CellGcmFlipPublicationTiming timing =
            gcm_flip_publication_timestamp_locked(1);
        s_flip_publication.pending = ticket;
        s_flip_publication.pending_timing = timing;
        s_flip_publication.pending_valid = 1;
        /* Serialize WAITING with generation publication. An older completion
         * must not clear a request published concurrently with presentation. */
        flip_status_store(CELL_GCM_FLIP_STATUS_WAITING);
    } else {
        /* SetPrepareFlip was immediate before this handoff existed: it has no
         * ticker-pending edge, so it directly owns the next clear boundary. */
        s_flip_publication.consumed = ticket;
        s_flip_publication.consumed_timing =
            (CellGcmFlipPublicationTiming){ 0, CELL_GCM_FLIP_PUBLICATION_QPC_OFF };
        s_flip_publication.pending_valid = 0;
    }
    atomic_store_explicit(&s_current_display_buffer_id, buffer_id,
                          memory_order_release);
    atomic_fetch_add_explicit(&s_flip_request_count, 1, memory_order_release);
    /* Immediate completion belongs to this generation too. Publish it before
     * unlocking so a later pending request cannot be cleared by its caller. */
    if (!pending) flip_status_store(CELL_GCM_FLIP_STATUS_DONE);
    gcm_flip_publication_unlock();
    if (pending) gcm_flip_kick_presenter();
    else gcm_flip_status_done_notify();
}

static void gcm_flip_publication_reset(void)
{
    gcm_flip_publication_lock();
    /* Keep this sequence monotonic across cellGcmTerminate/re-init: the
     * renderer's lifetime `last present` gate deliberately survives a backend
     * reset, so reusing generation 1 could be rejected as SAME_FLIP. */
    s_flip_publication.pending.generation = 0;
    s_flip_publication.pending.buffer_id = 0;
    s_flip_publication.pending.source = 0;
    s_flip_publication.pending_timing =
        (CellGcmFlipPublicationTiming){ 0, CELL_GCM_FLIP_PUBLICATION_QPC_OFF };
    s_flip_publication.consumed.generation = 0;
    s_flip_publication.consumed.buffer_id = 0;
    s_flip_publication.consumed.source = 0;
    s_flip_publication.consumed_timing =
        (CellGcmFlipPublicationTiming){ 0, CELL_GCM_FLIP_PUBLICATION_QPC_OFF };
    s_flip_publication.last_publication_qpc = 0;
    s_flip_publication.pending_valid = 0;
    s_flip_publication.completed_generation = 0;
    atomic_store_explicit(&s_flip_callbacks_pending, 0, memory_order_release);
    atomic_store_explicit(&s_current_display_buffer_id, 0, memory_order_release);
    atomic_store_explicit(&s_flip_request_count, 0, memory_order_release);
    gcm_flip_publication_unlock();
}

static int gcm_flip_take_ticket(CellGcmFlipTicket* out_ticket,
                                CellGcmFlipPublicationTiming* out_timing)
{
    if (!out_ticket) return 0;
    if (out_timing)
        *out_timing = (CellGcmFlipPublicationTiming){ 0, CELL_GCM_FLIP_PUBLICATION_QPC_OFF };
    gcm_flip_publication_lock();
    if (!s_flip_publication.pending_valid) {
        gcm_flip_publication_unlock();
        return 0;
    }
    *out_ticket = s_flip_publication.pending;
    if (out_timing) *out_timing = s_flip_publication.pending_timing;
    s_flip_publication.consumed = *out_ticket;
    s_flip_publication.consumed_timing = s_flip_publication.pending_timing;
    s_flip_publication.pending_valid = 0;
    gcm_flip_publication_unlock();
    return 1;
}

int cellGcm_take_flip_ticket(CellGcmFlipTicket* out_ticket)
{
    return gcm_flip_take_ticket(out_ticket, NULL);
}

int cellGcm_take_flip_ticket_timed(CellGcmFlipTicket* out_ticket,
                                   CellGcmFlipPublicationTiming* out_timing)
{
    return gcm_flip_take_ticket(out_ticket, out_timing);
}

int cellGcm_flip_consumed_ticket_snapshot(CellGcmFlipTicket* out_ticket)
{
    if (!out_ticket) return 0;
    gcm_flip_publication_lock();
    *out_ticket = s_flip_publication.consumed;
    int valid = out_ticket->generation != 0;
    gcm_flip_publication_unlock();
    return valid;
}

int cellGcm_take_flip_pending(void)
{
    CellGcmFlipTicket ignored;
    return cellGcm_take_flip_ticket(&ignored);
}


/* Configuration */
static CellGcmConfig s_config;
/* Offset pages (1MB) known to be LOCAL-memory-derived (set by
 * cellGcmAddressToOffset when the guest converts a localAddress-range EA). */
static u8 s_local_offset_page[1024];

/* Command buffer control */
static CellGcmControl s_control;

/* Offset table storage */
static u16 s_io_address_table[65536];
static u16 s_ea_address_table[65536];

static CellGcmOffsetTable s_offset_table = {
    s_io_address_table,
    s_ea_address_table,
};

/* Local memory bump allocator */
static u32 s_local_mem_allocated = 0;  /* next free offset in local memory */

/* IO memory mapping table */
typedef struct IoMapping {
    u32 ea;         /* effective address in main memory */
    u32 io;         /* IO offset (RSX-visible) */
    u32 size;       /* size of mapping */
    int active;     /* 1 if this slot is in use */
} IoMapping;

static IoMapping s_io_mappings[CELL_GCM_MAX_IO_MAPPINGS];
static int s_io_mapping_count = 0;
/* A deferred MAIN writeback resolves IO once during preflight. IO table or
 * configuration mutation invalidates that result; the commit checks this value
 * without resolving. Legacy LOCAL hints do not affect explicit translations. */
static atomic_ullong s_main_mapping_generation = ATOMIC_VAR_INIT(2);
static atomic_flag s_main_mapping_writer = ATOMIC_FLAG_INIT;
/* The generation detects publication changes; this lock also makes the
 * ordinary u16 page tables race-free for all in-runtime readers. */
static SRWLOCK s_main_mapping_lock = SRWLOCK_INIT;

u64 cellGcmMainMappingGeneration(void)
{
    return atomic_load_explicit(&s_main_mapping_generation, memory_order_acquire);
}

static void cellGcmMainMappingWriteBeginLocked(void)
{
    while (atomic_flag_test_and_set_explicit(&s_main_mapping_writer,
                                             memory_order_acquire)) { }
    /* even=published, odd=writer owns tables/metadata */
    atomic_fetch_add_explicit(&s_main_mapping_generation, 1, memory_order_acq_rel);
}

static void cellGcmMainMappingWriteEndLocked(void)
{
    atomic_fetch_add_explicit(&s_main_mapping_generation, 1, memory_order_release);
    atomic_flag_clear_explicit(&s_main_mapping_writer, memory_order_release);
}

static void cellGcmMainMappingWriteBegin(void)
{
    AcquireSRWLockExclusive(&s_main_mapping_lock);
    cellGcmMainMappingWriteBeginLocked();
}

static void cellGcmMainMappingWriteEnd(void)
{
    cellGcmMainMappingWriteEndLocked();
    ReleaseSRWLockExclusive(&s_main_mapping_lock);
}

/* LOCAL hints affect only the legacy unlocated ResolveOffset(). Deferred blits
 * use ResolveLocatedChecked(), which ignores these hints for both locations.
 * Keep hint readers/writers serialized, but do not invalidate queued MAIN
 * writebacks when the guest discovers another LOCAL page during GPU work.
 * Actual IO map/unmap and configuration changes still publish a generation. */
static void cellGcmMainMappingMarkLocalPage(u32 page)
{
    if (page >= sizeof(s_local_offset_page)) return;
    AcquireSRWLockExclusive(&s_main_mapping_lock);
    s_local_offset_page[page] = 1;
    ReleaseSRWLockExclusive(&s_main_mapping_lock);
}

/* Callback handlers — these are GUEST OPD addresses passed by the
 * recompiled game, not host function pointers. Stored as uint32_t and
 * invoked through g_ps3_guest_caller (see ps3emu/guest_call.h). */
static atomic_uint s_flip_handler_opd   = 0;
static atomic_uint s_vblank_handler_opd = 0;
static u32 s_user_handler_opd     = 0;
static u32 s_second_v_handler_opd = 0;
/* YDKJ_HANDLERFIX: the handler OPD's code word (*(opd)) gets clobbered to 0 by a
 * mislifted store, so g_ps3_guest_caller reads code=0 -> "not registered" and the
 * game's per-flip/vblank callback never fires -> render/advance loop stalls. We
 * capture the code at Set*Handler time (OPD still valid) and restore the OPD word
 * before each invocation if it's been clobbered. */
extern u32 vm_read32(unsigned int);
static atomic_uint s_flip_handler_code   = 0;
static atomic_uint s_vblank_handler_code = 0;
static u32 s_user_handler_code    = 0;
static void ydkj_restore_handler_opd(u32 opd, u32 code) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("YDKJ_HANDLERFIX") ? 1 : 0;
    if (!enabled || !opd || !code) return;
    extern u8* vm_base; if (!vm_base) return;
    if (vm_read32(opd) == 0) {
        u8* p = vm_base + opd; p[0]=(u8)(code>>24); p[1]=(u8)(code>>16); p[2]=(u8)(code>>8); p[3]=(u8)code;
        static int _n=0; if(_n++<6) fprintf(stderr,"[HANDLERFIX] restored clobbered OPD 0x%08X code=0x%08X\n",opd,code);
    }
}
/* Legacy host-typed slots kept around for any caller still treating
 * these as host pointers. New code should use the _opd slots. */
static CellGcmFlipHandler    s_flip_handler    = NULL;
static CellGcmVBlankHandler  s_vblank_handler  = NULL;
static CellGcmUserHandler    s_user_handler    = NULL;
static CellGcmSecondVHandler s_second_v_handler = NULL;

/* Host dispatcher lifetime is independent of guest cellGcmInit/Terminate.
 * The title registers its worker before running guest code. Other hosts may
 * retain cooperative pumping by leaving the dispatcher unregistered. */
static atomic_uint s_gcm_interrupt_host_tid = 0;
static _Atomic(CellGcmInterruptWake) s_gcm_interrupt_wake = NULL;

static void gcm_wake_interrupt_dispatcher(void)
{
    CellGcmInterruptWake wake = atomic_load_explicit(&s_gcm_interrupt_wake,
                                                    memory_order_acquire);
    if (wake) wake();
}

void cellGcmSetHostInterruptDispatcher(u32 host_thread_id, CellGcmInterruptWake wake)
{
    if (host_thread_id) {
        atomic_store_explicit(&s_gcm_interrupt_host_tid, host_thread_id,
                              memory_order_release);
        atomic_store_explicit(&s_gcm_interrupt_wake, wake, memory_order_release);
    } else {
        atomic_store_explicit(&s_gcm_interrupt_wake, NULL, memory_order_release);
        atomic_store_explicit(&s_gcm_interrupt_host_tid, 0, memory_order_release);
    }
}

static int gcm_interrupt_dispatch_allowed(void)
{
    u32 owner = atomic_load_explicit(&s_gcm_interrupt_host_tid, memory_order_acquire);
    if (!owner) return 1;
#ifdef _WIN32
    return owner == GetCurrentThreadId();
#else
    /* No non-Windows host registers this Windows thread-id API. */
    return 0;
#endif
}

/* FIFO SCE_DRIVER_INTERRUPT delivery is producer/consumer: the RSX FIFO
 * walker publishes only after it has advanced guest GET, then ppu_gcm_pump
 * invokes the captured guest handler on the selected dispatcher. Causes are
 * completion edges, so never coalesce or drop them. Chunks grow on demand; an
 * allocation failure is a hard process failure rather than silent loss. */
#define GCM_USER_INTERRUPT_CHUNK_CAP 64u
typedef struct GcmUserInterrupt {
    u32 cause, opd, code, generation, sequence;
} GcmUserInterrupt;
typedef struct GcmUserInterruptChunk {
    struct GcmUserInterruptChunk* next;
    u32 first, count;
    GcmUserInterrupt events[GCM_USER_INTERRUPT_CHUNK_CAP];
} GcmUserInterruptChunk;
static GcmUserInterruptChunk* s_user_interrupt_head = NULL;
static GcmUserInterruptChunk* s_user_interrupt_tail = NULL;
static atomic_flag s_user_interrupt_lock = ATOMIC_FLAG_INIT;
static atomic_uint s_user_interrupt_generation = ATOMIC_VAR_INIT(1);
static atomic_uint s_user_interrupt_sequence = ATOMIC_VAR_INIT(0);

static void gcm_user_interrupt_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_user_interrupt_lock,
                                             memory_order_acquire)) { }
}

static void gcm_user_interrupt_unlock(void)
{
    atomic_flag_clear_explicit(&s_user_interrupt_lock, memory_order_release);
}

static void gcm_user_interrupt_queue_reset_locked(void)
{
    GcmUserInterruptChunk* chunk = s_user_interrupt_head;
    while (chunk) {
        GcmUserInterruptChunk* next = chunk->next;
        free(chunk);
        chunk = next;
    }
    s_user_interrupt_head = NULL;
    s_user_interrupt_tail = NULL;
    atomic_fetch_add_explicit(&s_user_interrupt_generation, 1, memory_order_release);
    atomic_store_explicit(&s_user_interrupt_sequence, 0, memory_order_relaxed);
}

static void gcm_user_interrupt_reset_lifetime(void)
{
    gcm_user_interrupt_lock();
    s_user_handler = NULL;
    s_user_handler_opd = 0;
    s_user_handler_code = 0;
    gcm_user_interrupt_queue_reset_locked();
    gcm_user_interrupt_unlock();
}

static int gcm_user_interrupt_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("GCM_USER_INTERRUPT_TRACE") ? 1 : 0;
    return enabled;
}

/* Internal FIFO path. Snapshot handler identity when the interrupt is
 * committed, so handler replacement cannot retarget an already-raised edge. */
static void cellGcmQueueUserInterrupt(u32 cause, u32 committed_get)
{
    gcm_user_interrupt_lock();
    if (!s_user_interrupt_tail ||
        s_user_interrupt_tail->count == GCM_USER_INTERRUPT_CHUNK_CAP) {
        GcmUserInterruptChunk* chunk = (GcmUserInterruptChunk*)calloc(1, sizeof(*chunk));
        if (!chunk) {
            gcm_user_interrupt_unlock();
            fprintf(stderr, "[cellGcmSys] fatal: cannot queue user interrupt\n");
            abort();
        }
        if (s_user_interrupt_tail) s_user_interrupt_tail->next = chunk;
        else s_user_interrupt_head = chunk;
        s_user_interrupt_tail = chunk;
    }
    GcmUserInterrupt* event =
        &s_user_interrupt_tail->events[s_user_interrupt_tail->count++];
    event->cause = cause;
    event->opd = s_user_handler_opd;
    event->code = s_user_handler_code;
    event->generation = atomic_load_explicit(&s_user_interrupt_generation,
                                             memory_order_relaxed);
    event->sequence = atomic_fetch_add_explicit(&s_user_interrupt_sequence, 1,
                                                 memory_order_relaxed);
    if (gcm_user_interrupt_trace_enabled() && event->sequence < 64) {
        fprintf(stderr,
                "[GCM-USER-INTERRUPT] queued seq=%u cause=0x%08X opd=0x%08X get=0x%08X\n",
                event->sequence, event->cause, event->opd,
                committed_get);
    }
    gcm_user_interrupt_unlock();
    gcm_wake_interrupt_dispatcher();
}

static int gcm_user_interrupt_take(GcmUserInterrupt* event)
{
    gcm_user_interrupt_lock();
    GcmUserInterruptChunk* chunk = s_user_interrupt_head;
    if (!chunk) {
        gcm_user_interrupt_unlock();
        return 0;
    }
    *event = chunk->events[chunk->first++];
    if (chunk->first == chunk->count) {
        s_user_interrupt_head = chunk->next;
        if (!s_user_interrupt_head) s_user_interrupt_tail = NULL;
        free(chunk);
    }
    gcm_user_interrupt_unlock();
    return 1;
}

/* Read-only, bounded evidence for the game's one-shot late-stall snapshot. */
void cellGcm_user_interrupt_snapshot(FILE* out)
{
    GcmUserInterrupt first = {0};
    u32 queued = 0, generation;
    gcm_user_interrupt_lock();
    for (GcmUserInterruptChunk* chunk = s_user_interrupt_head; chunk; chunk = chunk->next)
        queued += chunk->count - chunk->first;
    if (s_user_interrupt_head)
        first = s_user_interrupt_head->events[s_user_interrupt_head->first];
    generation = atomic_load_explicit(&s_user_interrupt_generation, memory_order_relaxed);
    gcm_user_interrupt_unlock();
    fprintf(out, "[GCM-USER-INTERRUPT-SNAPSHOT] queued=%u first_seq=%u cause=0x%08X "
                 "opd=0x%08X generation=%u/%u\n",
            queued, first.sequence, first.cause, first.opd, first.generation, generation);
}

/* Flip timing */
static atomic_ullong s_last_flip_time = 0;

/* VBlank counter (incremented on each flip as approximation) */
static atomic_uint s_vblank_count = 0;

/* IO map reservation tracking */
static u32 s_io_map_reserved = 0;  /* bytes reserved for IO mapping */

/* Default FIFO mode */
static s32 s_default_fifo_mode = 0;

/* Graphics / queue handlers */
static CellGcmGraphicsHandler s_graphics_handler = NULL;
static CellGcmQueueHandler    s_queue_handler    = NULL;

/* Second V / VBlank frequency */
static u32 s_second_v_frequency = 0;
static u32 s_vblank_frequency   = 0;

/* User command */
static u32 s_user_command = 0;

/* Tile configuration (up to 15 tiles, 8 commonly used) */
static CellGcmTileInfo s_tiles[CELL_GCM_MAX_TILE_COUNT];

/* Zcull configuration (8 zcull regions) */
static CellGcmZcullInfo s_zcull[CELL_GCM_MAX_ZCULL_COUNT];

/* Report data area (256 slots, each 16 bytes) */
static CellGcmReportData s_report_data[CELL_GCM_MAX_REPORT_COUNT];

/* Label area (256 labels, each u32). Labels live in GUEST memory so the
 * recompiled game can poll them via vm_read32; cellGcmGetLabelAddress returns a
 * guest address into this window (16-byte spaced as on hardware). A host array
 * pointer would be read as a guest offset and land out of bounds. */
#define GCM_LABEL_GUEST_BASE  0x03000000u
#define GCM_LABEL_STRIDE      0x10u
static u32 s_labels[CELL_GCM_MAX_LABEL_COUNT];

/* GCM control register (put/get/ref). This MUST live in guest memory: the title
 * takes the pointer from cellGcmGetControlRegister, writes `put` when it kicks
 * the FIFO, and spins reading `get`/`ref` to know the RSX has caught up. A host
 * pointer would be read through vm_base as a guest address and never update, so
 * every FIFO-space / cellGcmFinish wait would hang. Placed just past the 256
 * label slots (0x03000000..0x03001000). Fields are big-endian (vm_write32). */
#define GCM_CONTROL_GUEST_ADDR 0x03002000u

/* Synthetic guest code EA for the FIFO command-buffer-full callback. The title's
 * inline gcmReserve calls context->callback(context, count) when `current` nears
 * `end`; we point the context's callback OPD at this EA and register it in the
 * PPU function table (ppu_sysprx.cpp: hle_gcm_callback) so the indirect call
 * routes back into cellGcm_fifo_recycle(). Lives in the injected control page,
 * never real guest code. MUST match GCM_FIFO_CALLBACK_SENTINEL_EA there. */
#define GCM_FIFO_CALLBACK_SENTINEL_EA 0x03002F00u

/* EA the ticker's RSX drain has consumed up to. Exposed so the wrap callback can
 * wait for the RSX to catch up before recycling the ring (else the frame's
 * just-written commands are lost). Updated in cellGcm_rsx_process_fifo. */
volatile u32 g_gcm_fifo_drained_ea = 0;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

/* Populate the offset table entries for an IO mapping */
static void populate_offset_table(u32 ea, u32 io, u32 size)
{
    /*
     * The offset table uses 1MB pages (20-bit shift).
     * ioAddress[ea >> 20] = io >> 20   (EA -> IO offset)
     * eaAddress[io >> 20] = ea >> 20   (IO offset -> EA)
     */
    u32 pages = size >> 20;  /* number of 1MB pages */
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = (u16)(io_page + i);
        s_ea_address_table[io_page + i] = (u16)(ea_page + i);
    }
}

/* Clear offset table entries for an IO mapping */
static void clear_offset_table(u32 ea, u32 io, u32 size)
{
    u32 pages = size >> 20;
    u32 ea_page = ea >> 20;
    u32 io_page = io >> 20;

    for (u32 i = 0; i < pages && (ea_page + i) < 65536 && (io_page + i) < 65536; i++) {
        s_io_address_table[ea_page + i] = 0xFFFF;
        s_ea_address_table[io_page + i] = 0xFFFF;
    }
}

/* Find an IO mapping by EA */
static IoMapping* find_mapping_by_ea(u32 ea)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].ea == ea)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find an IO mapping by IO offset */
static IoMapping* find_mapping_by_io(u32 io)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active && s_io_mappings[i].io == io)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Find a free IO mapping slot */
static IoMapping* find_free_mapping(void)
{
    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (!s_io_mappings[i].active)
            return &s_io_mappings[i];
    }
    return NULL;
}

/* Valid tiled pitch sizes (power-of-two aligned, common RSX values) */
static const u32 s_valid_pitches[] = {
    0x0200, 0x0300, 0x0400, 0x0500, 0x0600, 0x0700, 0x0800,
    0x0A00, 0x0C00, 0x0D00, 0x0E00, 0x1000, 0x1400, 0x1800,
    0x1A00, 0x1C00, 0x2000, 0x2800, 0x3000, 0x3400, 0x3800,
    0x4000, 0x5000, 0x6000, 0x6800, 0x7000, 0x8000, 0xA000,
    0xC000, 0xD000, 0xE000, 0x10000
};
static const int s_valid_pitch_count = sizeof(s_valid_pitches) / sizeof(s_valid_pitches[0]);

/* ---------------------------------------------------------------------------
 * API implementations -- Initialization
 * -----------------------------------------------------------------------*/

/* NID: 0xB2E761D4 */
s32 cellGcmInit(u32 cmdSize, u32 ioSize, u32 ioAddress)
{
    printf("[cellGcmSys] Init(cmdSize=0x%X, ioSize=0x%X, ioAddr=0x%08X)\n",
           cmdSize, ioSize, ioAddress);

    if (s_gcm_initialized) {
        printf("[cellGcmSys] WARNING: already initialized\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    frame_prof_reset();
    tj_flip_perf_reset();
    s_tj_survival_flip_diag = 0;
    atomic_store_explicit(&s_tj_survival_profile_armed, false,
                          memory_order_release);
    spu_dma_set_image17_put_callback(tj_geom_fifo_trace_arm_img17);
    cellGcmMainMappingWriteBegin();

    gcm_identity_write_begin();
    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    gcm_identity_write_end();
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    memset(s_local_offset_page, 0, sizeof(s_local_offset_page));
    gcm_fifo_objects_reset();
    memset(&s_fifo_packet, 0, sizeof(s_fifo_packet));
    s_fifo_semaphore_offset = 0;
    s_fifo_semaphore_dma = 0x66606660u;
    s_fifo_semaphore_wait_since_ns = 0;

    /*
     * Populate a plausible configuration.
     * In a real implementation these would reflect actual allocated memory.
     */
    s_config.localAddress    = 0xC0000000;  /* typical RSX local mem base */
    s_config.localSize       = 256 * 1024 * 1024;  /* 256 MB */
    s_config.ioAddress       = ioAddress;
    s_config.ioSize          = ioSize;
    s_config.memoryFrequency = 650000000;   /* 650 MHz */
    s_config.coreFrequency   = 500000000;   /* 500 MHz */

    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    memset(s_local_offset_page, 0, sizeof(s_local_offset_page));
    gcm_flip_publication_reset();
    s_flip_handler = NULL;
    s_vblank_handler = NULL;
    s_second_v_handler = NULL;
    s_flip_handler_opd = 0;
    s_vblank_handler_opd = 0;
    s_second_v_handler_opd = 0;
    s_flip_handler_code = 0;
    s_vblank_handler_code = 0;
    gcm_user_interrupt_reset_lifetime();
    s_last_flip_time = get_timestamp_ns();
    flip_status_store(CELL_GCM_FLIP_STATUS_DONE);
    s_flip_mode = CELL_GCM_DISPLAY_VSYNC;
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    s_graphics_handler = NULL;
    s_queue_handler = NULL;
    s_second_v_frequency = 0;
    s_vblank_frequency = 0;
    s_user_command = 0;

    /* Set up the initial IO mapping for the command buffer region.
     *
     * Tokyo Jungle calls _cellGcmInitBody with ioSize == 0 and an ioAddress
     * in guest memory, meaning "use the classic local-memory window": the
     * tiles it goes on to bind live at io 0x0/0x3A0000/0x740000. With no
     * window registered here, io 0 resolves to nothing, the first FIFO drain
     * resyncs get 0 straight to put and swallows the fences in that first
     * command buffer, so ctrl->ref never advances and the game parks in its
     * reference-wait loop. 16 MB is the usual SDK local size. */
    if (ioAddress != 0 && ioSize == 0) {
        ioSize = 0x2000000;
        s_config.ioSize = ioSize;
        printf("[cellGcmSys] ioSize=0: defaulting the IO window to 0x%X at io 0 (ea 0x%08X)\n",
               ioSize, ioAddress);
    }
    if (ioAddress != 0 && ioSize > 0) {
        /* libgcm carves the command ring off the TOP of local memory, i.e. at
         * io == ioSize (Tokyo Jungle's first FIFO word is an idle JUMP to
         * io 0x01000000 with a 16 MB window, 0x02000000 with 32 MB). Register
         * the ring's own span so gcm_io2ea() resolves it, and record it as the
         * base that control put/get are offsets from. */
        s_fifo_io_base = 0;
        populate_offset_table(ioAddress, 0, ioSize + 0x20000);

        s_io_mappings[0].ea     = ioAddress;
        s_io_mappings[0].io     = 0;
        s_io_mappings[0].size   = ioSize;
        s_io_mappings[0].active = 1;
        s_io_mapping_count = 1;
    }
    cellGcmMainMappingWriteEnd();

    /* Zero the guest-visible control register (put/get/ref). */
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, 0);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, 0);

    s_gcm_initialized = 1;
    gcm_flip_status_done_notify();      /* status reset to DONE above */
    return CELL_OK;
}

/* Reusable _cellGcmInitBody core (NID 0x15BAE46B). See cellGcmSys.h for why the
 * guest vm is injected as callbacks. The CellGcmContextData layout here is the
 * one proven in the shipping Simpsons port (begin@0/end@4/current@8/callback@C);
 * getting it wrong makes the game read `current` as the callback OPD and stall
 * in cellGcmFlush. The command buffer is the start of the game's IO region. */
u32 cellGcmSetupContext(u32 ctx_out_addr, u32 cmdSize, u32 ioSize, u32 ioAddress,
                        CellGcmGuestAlloc galloc, CellGcmGuestWrite32 gwrite32)
{
    if (cmdSize < 0x10000)
        cmdSize = 0x10000;

    /* PSL1GHT's gcmInitBody passes ioAddress = 0, expecting gcm to allocate the
     * IO region itself (the official SDK has the app allocate and pass it).
     * Taking the 0 literally put the FIFO command buffer at guest address 0 --
     * the guest then wrote its command stream over low memory and every offset
     * resolution failed. Allocate a guest region of ioSize instead. */
    if (ioAddress == 0 && galloc)
        ioAddress = galloc(ioSize ? ioSize : 0x100000, 0x100000);

    cellGcmInit(cmdSize, ioSize, ioAddress);

    u32 cmdbuf = ioAddress;                 /* FIFO lives at the IO region start */
    u32 cdata  = galloc ? galloc(16, 16) : 0;
    if (cdata && gwrite32) {
        gwrite32(cdata + 0x0, cmdbuf);              /* begin   */
        /* The reserve callback may place one JUMP at current == end. */
        gwrite32(cdata + 0x4, cmdbuf + cmdSize - 4); /* end / reserved JUMP */
        gwrite32(cdata + 0x8, cmdbuf);              /* current */
        /* Command-buffer-full callback: a guest OPD {func, toc} routed to
         * hle_gcm_callback -> cellGcm_fifo_recycle. Without a real callback the
         * ring never recycles and the FIFO wedges once `current` reaches `end`. */
        u32 opd = galloc ? galloc(8, 8) : 0;
        if (opd) {
            gwrite32(opd + 0x0, GCM_FIFO_CALLBACK_SENTINEL_EA);  /* func EA */
            gwrite32(opd + 0x4, 0);                              /* toc (unused) */
            gwrite32(cdata + 0xC, opd);                          /* callback OPD */
        } else {
            gwrite32(cdata + 0xC, 0);
        }
        if (ctx_out_addr)
            gwrite32(ctx_out_addr, cdata);          /* *context = &ctxdata */
        s_gcm_context_ea = cdata;                   /* RSX drains commands from here */
    }
    return cdata;
}

s32 cellGcmGetConfiguration(CellGcmConfig* config)
{
    /* `config` is a GUEST address; write the 6 u32 fields big-endian via
     * vm_write32 (a raw *config = s_config faults / is host-endian). The game
     * reads localAddress/localSize from here to lay out VRAM framebuffers. */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_GCM_ERROR_INVALID_VALUE;

    vm_write32(cfg +  0, s_config.localAddress);
    vm_write32(cfg +  4, s_config.ioAddress);
    vm_write32(cfg +  8, s_config.localSize);
    vm_write32(cfg + 12, s_config.ioSize);
    vm_write32(cfg + 16, s_config.memoryFrequency);
    vm_write32(cfg + 20, s_config.coreFrequency);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Command buffer control
 * -----------------------------------------------------------------------*/

/* NID: 0x8572A8E0 */
CellGcmControl* cellGcmGetControlRegister(void)
{
    /* Return the GUEST address of the control register (not &s_control, a host
     * pointer). The recompiled title reads/writes it through vm_base, so it must
     * be a guest EA for put/get/ref to actually flow. */
    return (CellGcmControl*)(uintptr_t)GCM_CONTROL_GUEST_ADDR;
}

/* ---------------------------------------------------------------------------
 * Display / flip
 * -----------------------------------------------------------------------*/

/* NID: 0xDB23E867 */
u32 cellGcmGetCurrentField(void)
{
    /* Returns 0 or 1 for interlaced; always 0 for progressive. */
    return 0;
}

/* NID: 0xA53D12AE */
void cellGcmSetFlipMode(u32 mode)
{
    printf("[cellGcmSys] SetFlipMode(mode=%u)\n", mode);
    s_flip_mode = mode;
}

int cellGcm_flip_uses_vsync(void)
{
    return atomic_load_explicit(&s_flip_mode, memory_order_acquire) ==
           CELL_GCM_DISPLAY_VSYNC;
}

/* NID: 0xC44D8F34 */
void cellGcmSetWaitFlip(void)
{
    /* This adapter yields for a bounded interval while the flip is pending.
     * Only the presenter completing the exact ticket may publish DONE. The
     * polling budget is not completion, and a newer request can arrive after
     * our final status read, so this waiter must never overwrite the status.
     * TJ_FLIP_WAIT_EVENT=1 wakes at the DONE publication instead of the next
     * Sleep(1); the 64 budget then counts elapsed milliseconds. */
    GcmPollWait wait;
    gcm_poll_wait_begin(&wait, GCM_WAIT_SITE_SETWAITFLIP, GCM_WAIT_CH_FLIP);
    int i = 0;
    while (i < 64 && flip_status_load() == CELL_GCM_FLIP_STATUS_WAITING)
        i = gcm_poll_wait_pause(&wait, i);
    gcm_poll_wait_end(&wait, i < 64 ? GCM_WAIT_OUT_READY : GCM_WAIT_OUT_LIMIT);
}

/* NID: 0x51C9D62B */
void cellGcmResetFlipStatus(void)
{
    if (s_tj_survival_flip_diag) {
        static int n = 0;
        if (n++ < 16) {
            fprintf(stderr, "[TJ-SURV-FLIP] reset WAITING\n");
            fflush(stderr);
        }
    }
    flip_status_store(CELL_GCM_FLIP_STATUS_WAITING);
    if (s_tj_survival_flip_diag) {
        unsigned resets = atomic_fetch_add_explicit(&s_tj_flip_reset_count, 1,
                                                     memory_order_relaxed) + 1;
        if ((resets % 6000u) == 0u) {
            fprintf(stderr, "[TJ-SURV-FLIP] reset-count=%u\n", resets);
            fflush(stderr);
        }
        tj_flip_trace("reset", resets);
    }
}

void cellGcm_frame_prof_snapshot(CellGcmFrameProfSnapshot* out)
{
    if (!out) return;
    frame_prof_lock();
    out->generation = s_frame_prof_generation;
    out->flip_sequence = s_frame_prof_flip_sequence;
    out->source_kind = s_frame_prof_source_kind;
    out->source_host_tid = s_frame_prof_source_tid;
    frame_prof_unlock();
    /* Best effort: FIFO fields can advance after publication, but vm_read32
     * remains address/bounds safe for this diagnostic-only observation. */
    out->put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    out->get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
    out->current = s_gcm_context_ea ? vm_read32(s_gcm_context_ea + 0x8) : 0;
}

/* Last time the title polled a still-pending flip (QPC). Read at vblank by the
 * Tokyo Jungle 30->60 FPS signal below. */
static atomic_ullong s_tj_flip_wait_poll_qpc = 0;

/* NID: 0xE315A0B2 */
u32 cellGcmGetFlipStatus(void)
{
    /* Avoid host-calling the guest OPDs directly (they're not valid host
     * function pointers). Use cellGcmTickVBlank() / TickFlip() instead,
     * which dispatch via g_ps3_guest_caller — see below. */
    /* Report the CURRENT status; presentation completes the matching ticket,
     * independently of deferred callbacks. The old self-completing version made
     * every wait-for-flip loop exit on its first poll, so titles ran
     * completely unpaced (wave: 95 fps with a fixed-dt simulation). */
    u32 status = flip_status_load();
#ifdef _WIN32
    if (status == CELL_GCM_FLIP_STATUS_WAITING) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        atomic_store_explicit(&s_tj_flip_wait_poll_qpc, (unsigned long long)now.QuadPart,
                              memory_order_relaxed);
        gcm_flip_poll_observe(status, (u64)now.QuadPart);
    } else {
        gcm_flip_poll_observe(status, 0);
    }
#endif
    if (s_tj_survival_flip_diag) {
        if (status == CELL_GCM_FLIP_STATUS_WAITING) {
            unsigned streak = atomic_fetch_add_explicit(&s_tj_flip_wait_streak,
                                                         1,
                                                         memory_order_relaxed) + 1;
            if (streak == 1000u) {
                atomic_store_explicit(&s_tj_flip_trace_budget, 240,
                                      memory_order_relaxed);
                fprintf(stderr,
                        "[TJ-FLIP-STALL] armed after %u consecutive WAITING polls\n",
                        streak);
                fflush(stderr);
            }
        } else {
            atomic_store_explicit(&s_tj_flip_wait_streak, 0,
                                  memory_order_relaxed);
        }
        tj_flip_trace("get", status);
    }
    if (s_tj_survival_flip_diag) {
        static int n = 0;
        if (n++ < 32) {
            fprintf(stderr, "[TJ-SURV-FLIP] get=%u\n", status);
            fflush(stderr);
        }
    }
    return status;
}

/* Host-side ticks. The game's host driver (e.g. flow main.cpp) calls
 * these periodically so registered guest handlers fire — that's what
 * drives the title-screen state machine for many games. */
#include "ps3emu/guest_call.h"

/* ---------------------------------------------------------------------------
 * Serialized vblank/flip handler delivery.
 *
 * The ticker publishes pending edges without executing guest code. A host may
 * give a dedicated interrupt thread exclusive delivery ownership; otherwise
 * legacy hosts deliver at main-PPU HLE boundaries. Callback bodies remain
 * serialized with each other. Dedicated dispatch must use a distinct guest
 * thread identity so guest mutex ownership is not borrowed from the main PPU.
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <windows.h>
static volatile LONG s_gcm_pending = 0;    /* bit0 = vblank, bit1 = flip */
#define GCM_PENDING_SET(bits)  do { InterlockedOr(&s_gcm_pending, (bits)); gcm_wake_interrupt_dispatcher(); } while (0)
#define GCM_PENDING_TAKE()     InterlockedExchange(&s_gcm_pending, 0)
#else
#include <stdatomic.h>
static atomic_int s_gcm_pending = 0;
#define GCM_PENDING_SET(bits)  do { atomic_fetch_or(&s_gcm_pending, (bits)); gcm_wake_interrupt_dispatcher(); } while (0)
#define GCM_PENDING_TAKE()     atomic_exchange(&s_gcm_pending, 0)
#endif

/* Legacy host entry point. Vblank advances independently; a flip notification
 * requires a submitted ticket, never merely the passage of host time. */
void cellGcm_request_tick(void)
{
    cellGcmTickVBlank();
    cellGcmTickFlip();
}

/* TJ_FLIP_GOV_TRACE=1: what Tokyo Jungle's 30/60 FPS governor sees. Its
 * vblank handler releases a flip every N vblanks (N at 0xC0410C) and counts
 * vblanks where put != get; it returns to 60 only when that count is > 1 for
 * several frames. Logs, per second, the handler calls, the vblank ticks folded
 * into one call (missed handler vblanks) and the put != get share. */
static void tj_flip_gov_trace_vblank(void)
{
#ifdef _WIN32
    static int enabled = -1;
    static u32 last_tick, last_mode, calls, folded, busy, ticks;
    static unsigned long long window_end;
    if (enabled < 0) enabled = getenv("TJ_FLIP_GOV_TRACE") != NULL;
    if (!enabled) return;
    const u32 tick = (u32)s_vblank_count;
    const u32 mode = vm_read32(0x00C0410Cu);
    const u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR);
    const u32 get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
    if (last_tick && tick - last_tick > 1) folded += tick - last_tick - 1;
    if (last_tick) ticks += tick - last_tick;
    last_tick = tick;
    calls++;
    if (put != get) busy++;
    if (mode != last_mode) {
        fprintf(stderr, "[tj-flipgov] mode N=%u -> %u at vblank %u\n",
                last_mode, mode, tick);
        last_mode = mode;
    }
    const unsigned long long now = GetTickCount64();
    if (!window_end) window_end = now + 1000;
    if (now >= window_end) {
        fprintf(stderr, "[tj-flipgov] N=%u ticks=%u handler_calls=%u folded=%u "
                        "put!=get=%u\n", mode, ticks, calls, folded, busy);
        calls = folded = busy = ticks = 0;
        window_end = now + 1000;
    }
#endif
}

/* TJ_FORCE_FLIP60=1 (measurement only): clear the game's own
 * "CheckFlipMode30" debug toggle (byte 0x3B439C) so its governor never drops
 * to 30 FPS, showing what frame rate the port reaches when it always tries 60. */
static void tj_force_flip60_vblank(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("TJ_FORCE_FLIP60") != NULL;
        if (enabled) fprintf(stderr, "[tj-flipgov] forcing 60 FPS mode (test)\n");
    }
    if (enabled && vm_read8(0x003B439Cu) != 0) vm_write8(0x003B439Cu, 0);
}

/* TJ_FLIP_PROMOTE=1: let Tokyo Jungle's own governor return to 60 FPS.
 * In 30 FPS mode it promotes only after frames where at least 2 vblanks saw
 * put != get, i.e. the next frame was already queued behind the flip wait.
 * Real RSX drains slowly enough for that; this port's FIFO drains at once, so
 * the count stayed at 1 and one early dip locked the game at 30 for good.
 * Count a vblank as "ahead" when the title is already polling a pending flip
 * (its frame is done early) and the FIFO looks idle. Demotion is unchanged. */
static void tj_flip_promote_vblank(void)
{
#ifdef _WIN32
    static int enabled = -1;
    static long long freq;
    if (enabled < 0) {
        LARGE_INTEGER f;
        enabled = getenv("TJ_FLIP_PROMOTE") != NULL && QueryPerformanceFrequency(&f);
        freq = enabled ? f.QuadPart : 0;
        if (enabled) fprintf(stderr, "[tj-flipgov] 60 FPS return signal on\n");
    }
    if (!enabled) return;
    /* The game's own tuning value "Flip30ModeLimitNum" (u32 at 0x3B43A0,
     * default 2): drop to 30 FPS once that many of the last 9 frames missed
     * a vblank. 2 trips on any brief hitch; TJ_FLIP30_LIMIT overrides it. */
    {
        static int limit = -1;
        if (limit < 0) {
            const char* v = getenv("TJ_FLIP30_LIMIT");
            limit = v && *v ? atoi(v) : 0;
            if (limit < 0 || limit > 9) limit = 0;
            if (limit) fprintf(stderr, "[tj-flipgov] Flip30ModeLimitNum=%d\n", limit);
        }
        if (limit && vm_read32(0x003B43A0u) != (u32)limit)
            vm_write32(0x003B43A0u, (u32)limit);
    }
    if (vm_read32(0x00C0410Cu) != 2) return;
    if (vm_read32(GCM_CONTROL_GUEST_ADDR) != vm_read32(GCM_CONTROL_GUEST_ADDR + 4))
        return;                        /* the title's handler counts this one */
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const unsigned long long poll = atomic_load_explicit(&s_tj_flip_wait_poll_qpc,
                                                         memory_order_relaxed);
    if (poll && (unsigned long long)now.QuadPart - poll < (unsigned long long)(freq / 500))
        vm_write32(0x00C0411Cu, vm_read32(0x00C0411Cu) + 1);   /* within 2 ms */
#endif
}

/* Only the selected interrupt owner may consume pending work. Check ownership
 * before exchanging counters: a main-thread safe point must not steal work
 * from the interrupt worker, especially while the main thread holds a lock. */
void ppu_gcm_pump(void)
{
#ifdef _WIN32
    static __declspec(thread) int in = 0;
#else
    static __thread int in = 0;
#endif
    if (in || !gcm_interrupt_dispatch_allowed()) return;
    long p = (long)GCM_PENDING_TAKE();
    unsigned flip_events = atomic_exchange_explicit(&s_flip_callbacks_pending, 0,
                                                    memory_order_acquire);
    GcmUserInterrupt user_event = {0};
    int have_user_cause = gcm_user_interrupt_take(&user_event);
    if (!p && !flip_events && !have_user_cause) return;
    if (s_tj_survival_flip_diag) tj_flip_trace("pump-begin", (u32)p);
    in = 1;
    u32 vblank_opd = s_vblank_handler_opd;
    if ((p & 1) && vblank_opd && g_ps3_guest_caller) {
        tj_flip_gov_trace_vblank();
        tj_force_flip60_vblank();
        tj_flip_promote_vblank();
        ydkj_restore_handler_opd(vblank_opd, s_vblank_handler_code);
        g_ps3_guest_caller(vblank_opd, (uint64_t)s_vblank_count,
                           0, 0, 0, 0, 0, 0, 0);
    }
    while (flip_events--) {
        /* Status was published at presentation. Re-publishing it here could
         * complete a newer frame that was requested before this safe point. */
        if (s_tj_survival_flip_diag) tj_flip_trace("pump-flip-callback", (u32)p);
        u32 flip_opd = s_flip_handler_opd;
        if (flip_opd && g_ps3_guest_caller) {
            ydkj_restore_handler_opd(flip_opd, s_flip_handler_code);
            g_ps3_guest_caller(flip_opd, 1, 0, 0, 0, 0, 0, 0, 0);
            if (s_tj_survival_flip_diag) tj_flip_trace("pump-flip-callback-return", (u32)p);
        }
    }
    /* SCE_DRIVER_INTERRUPT is emitted by the FIFO/present thread. Deliver it
     * only here, on the selected dispatcher, through the registered guest OPD.
     * A null handler still consumes the queued edge safely. */
    while (have_user_cause) {
        u32 current_generation = atomic_load_explicit(&s_user_interrupt_generation,
                                                       memory_order_acquire);
        int dispatch = user_event.generation == current_generation &&
                       user_event.opd && g_ps3_guest_caller;
        if (gcm_user_interrupt_trace_enabled() && user_event.sequence < 64) {
            fprintf(stderr,
                    "[GCM-USER-INTERRUPT] %s seq=%u cause=0x%08X opd=0x%08X generation=%u/%u\n",
                    dispatch ? "dispatch" : "discard",
                    user_event.sequence, user_event.cause, user_event.opd,
                    user_event.generation, current_generation);
        }
        if (dispatch) {
            ydkj_restore_handler_opd(user_event.opd, user_event.code);
            g_ps3_guest_caller(user_event.opd, user_event.cause,
                               0, 0, 0, 0, 0, 0, 0);
        }
        have_user_cause = gcm_user_interrupt_take(&user_event);
    }
    in = 0;
}

/* Back-compat: the old direct entry points publish a tick and wake the
 * dispatcher, without running guest code on the producer thread. */
void cellGcmTickVBlank(void) { s_vblank_count++; GCM_PENDING_SET(1); }
int cellGcm_complete_flip_ticket(const CellGcmFlipTicket* ticket)
{
    if (!ticket || !ticket->generation) return 0;
    gcm_flip_publication_lock();
    const CellGcmFlipTicket* consumed = &s_flip_publication.consumed;
    if (ticket->generation != consumed->generation ||
        ticket->buffer_id != consumed->buffer_id ||
        ticket->source != consumed->source ||
        ticket->generation <= s_flip_publication.completed_generation) {
        gcm_flip_publication_unlock();
        return 0;
    }
    s_flip_publication.completed_generation = ticket->generation;
    /* Publish scalar hardware status before/independently of the guest
     * callback, but only for the latest request. A newer pending ticket still
     * owns WAITING even when this older frame finishes presenting. */
    const int status_done = ticket->generation == s_flip_publication.next_generation;
    if (status_done)
        flip_status_store(CELL_GCM_FLIP_STATUS_DONE);
    s_last_flip_time = get_timestamp_ns();
    atomic_fetch_add_explicit(&s_flip_callbacks_pending, 1, memory_order_release);
    GCM_PENDING_SET(2);
    gcm_flip_publication_unlock();
    if (status_done) gcm_flip_status_done_notify();
    if (s_tj_survival_flip_diag) {
        static int n = 0;
        if (n++ < 16) {
            fprintf(stderr, "[TJ-SURV-FLIP] presented generation=%llu status=%u\n",
                    (unsigned long long)ticket->generation, flip_status_load());
            fflush(stderr);
        }
    }
    tj_flip_perf_tick_done();
    if (s_tj_survival_flip_diag) tj_flip_trace("tick-done", 0);
    return 1;
}

void cellGcmTickFlip(void)
{
    /* Compatibility for older host drivers which call this after presenting
     * without taking an explicit ticket. New drivers complete the exact ticket
     * they presented. Neither path can re-complete an old or nonexistent flip. */
    CellGcmFlipTicket ticket;
    if (cellGcm_take_flip_ticket(&ticket) ||
        cellGcm_flip_consumed_ticket_snapshot(&ticket))
        cellGcm_complete_flip_ticket(&ticket);
}

/* Drain the game's GCM FIFO into the RSX backend. Called from the present thread
 * (boot_main vblank_ticker). Not yet active: s_control.put stays 0 because
 * cellGcmGetControlRegister returns a HOST pointer, so the title's put writes
 * don't reach this struct (the control register must live in guest memory for
 * the recompiled code to update it via vm_write). Once that's fixed (+ the title
 * runs past its early self-exit to actually draw), parse get..put here with
 * rsx_process_command_buffer so the game's clears/draws render. */
/* Translate an RSX IO offset to a guest EA via the offset table (1MB pages,
 * populated by cellGcmInit / cellGcmMapMainMemory / MapEaIoAddress). */
static u16 cellGcmMainEaPage(u32 page)
{
    /* FIFO words and interleaved vertex attributes reuse a few MAIN pages.
     * Keep a small per-thread direct map so alternating buffers do not evict
     * every lookup. Reuse only the same complete mapping publication. A writer that
     * starts after this acquire may invalidate the result, just as one could
     * start after the old shared-lock release; no caller gains a lifetime pin.
     * LOCAL hints do not affect this explicit MAIN translation. */
    typedef struct { u64 generation; u32 page; u16 ea_page; } MainPageCache;
    static _Thread_local MainPageCache cache[32];
    if (page >= 65536) return 0xFFFF;
    MainPageCache* entry = &cache[page & 31u];
    u64 generation = cellGcmMainMappingGeneration();
    if (!(generation & 1) && generation != 0 &&
        entry->generation == generation && entry->page == page)
        return entry->ea_page;
    AcquireSRWLockShared(&s_main_mapping_lock);
    u16 ea_page = s_ea_address_table[page];
    entry->page = page;
    entry->ea_page = ea_page;
    entry->generation = cellGcmMainMappingGeneration();
    ReleaseSRWLockShared(&s_main_mapping_lock);
    return ea_page;
}

static u32 gcm_io2ea(u32 io)
{
    u16 ea_page = cellGcmMainEaPage(io >> 20);
    return ea_page == 0xFFFF ? 0 : ((u32)ea_page << 20) | (io & 0xFFFFFu);
}

/* Fast-walker-local io->ea memo. The generation is read BEFORE the lookup, so
 * a publication racing the lookup can only make later calls miss. Reuse only
 * an even, nonzero, unchanged generation and the same 1MB page (the same rule
 * cellGcmMainEaPage applies to its own per-thread cache). */
typedef struct { u64 generation; u32 page; u16 ea_page; } GcmIoMemo;
static u32 gcm_io2ea_memo(GcmIoMemo* memo, u32 io)
{
    u32 page = io >> 20;
    u64 generation = cellGcmMainMappingGeneration();
    u16 ea_page;
    if (generation != 0 && !(generation & 1) &&
        memo->generation == generation && memo->page == page) {
        ea_page = memo->ea_page;
    } else {
        ea_page = cellGcmMainEaPage(page);
        memo->generation = generation;
        memo->page = page;
        memo->ea_page = ea_page;
    }
    return ea_page == 0xFFFF ? 0 : ((u32)ea_page << 20) | (io & 0xFFFFFu);
}

/* RSX get pointer (IO offset) + one-deep CALL return slot. Only the FIFO
 * walkers advance these; recycling must not discard a pending return. */
unsigned long long ps3_ms_now(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    return 0;
#endif
}

static u32 s_fifo_getoff  = 0;
static u32 s_fifo_calloff = 0;

/* TJ_GEOM_FIFO_TRACE=1: bounded dump of CALL targets in the geometry side
 * arena. Both FIFO walkers run under s_gcm_fifo_lock, so this lazy config,
 * cap, and one pending outcome need no separate synchronization. */
static int s_tj_geom_fifo_trace_enabled = -1;
static unsigned s_tj_geom_fifo_trace_calls = 0;
static unsigned s_tj_geom_fifo_trace_pending_id = 0;
static u32 s_tj_geom_fifo_trace_pending_target = 0;
static unsigned s_tj_geom_fifo_trace_active_id = 0;
static unsigned s_tj_geom_fifo_trace_relevant_headers = 0;
static unsigned s_tj_geom_fifo_trace_nop_headers = 0;

static int tj_geom_fifo_trace_survival_only(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TJ_GEOM_FIFO_TRACE_SURVIVAL") != NULL;
    return enabled;
}

typedef struct TjGeomFifoTraceArm {
    u32 cmd, lsa, ea, size, pc;
    u8 bytes[16];
} TjGeomFifoTraceArm;

static TjGeomFifoTraceArm s_tj_geom_fifo_trace_arm;
#ifdef _WIN32
static volatile LONG s_tj_geom_fifo_trace_after_enabled = -1;
static volatile LONG s_tj_geom_fifo_trace_arm_state = 0; /* 0 unset, 1 writer, 2 published */
#else
static atomic_int s_tj_geom_fifo_trace_after_enabled = ATOMIC_VAR_INIT(-1);
static atomic_int s_tj_geom_fifo_trace_arm_state = ATOMIC_VAR_INIT(0);
#endif

static int tj_geom_fifo_trace_after_img17_enabled(void)
{
#ifdef _WIN32
    LONG enabled = InterlockedCompareExchange(&s_tj_geom_fifo_trace_after_enabled, 0, 0);
    if (enabled >= 0) return enabled;
    if (InterlockedCompareExchange(&s_tj_geom_fifo_trace_after_enabled, -2, -1) == -1) {
        const char* value = getenv("TJ_GEOM_FIFO_TRACE_AFTER_IMG17");
        enabled = value && value[0] && strcmp(value, "0") != 0;
        InterlockedExchange(&s_tj_geom_fifo_trace_after_enabled, enabled);
        return enabled;
    }
    do {
        enabled = InterlockedCompareExchange(&s_tj_geom_fifo_trace_after_enabled, 0, 0);
    } while (enabled == -2);
    return enabled;
#else
    int enabled = atomic_load_explicit(&s_tj_geom_fifo_trace_after_enabled,
                                       memory_order_acquire);
    if (enabled >= 0) return enabled;
    int expected = -1;
    if (atomic_compare_exchange_strong_explicit(&s_tj_geom_fifo_trace_after_enabled,
                                                &expected, -2,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        const char* value = getenv("TJ_GEOM_FIFO_TRACE_AFTER_IMG17");
        enabled = value && value[0] && strcmp(value, "0") != 0;
        atomic_store_explicit(&s_tj_geom_fifo_trace_after_enabled, enabled,
                              memory_order_release);
        return enabled;
    }
    do {
        enabled = atomic_load_explicit(&s_tj_geom_fifo_trace_after_enabled,
                                       memory_order_acquire);
    } while (enabled == -2);
    return enabled;
#endif
}

static int tj_geom_fifo_trace_arm_is_published(void)
{
#ifdef _WIN32
    return InterlockedCompareExchange(&s_tj_geom_fifo_trace_arm_state, 0, 0) == 2;
#else
    return atomic_load_explicit(&s_tj_geom_fifo_trace_arm_state,
                                memory_order_acquire) == 2;
#endif
}

static void tj_geom_fifo_trace_arm_img17(u32 cmd, u32 lsa, u32 ea, u32 size,
                                         u32 pc, const u8* copied_bytes)
{
    if (ea < 0x03F00000u || ea > 0x03FFFFFFu ||
        !tj_geom_fifo_trace_after_img17_enabled())
        return;

#ifdef _WIN32
    if (InterlockedCompareExchange(&s_tj_geom_fifo_trace_arm_state, 1, 0) != 0)
        return;
#else
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_tj_geom_fifo_trace_arm_state,
                                                 &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire))
        return;
#endif

    s_tj_geom_fifo_trace_arm.cmd = cmd;
    s_tj_geom_fifo_trace_arm.lsa = lsa;
    s_tj_geom_fifo_trace_arm.ea = ea;
    s_tj_geom_fifo_trace_arm.size = size;
    s_tj_geom_fifo_trace_arm.pc = pc;
    memset(s_tj_geom_fifo_trace_arm.bytes, 0, sizeof(s_tj_geom_fifo_trace_arm.bytes));
    if (copied_bytes) {
        u32 copy = size < sizeof(s_tj_geom_fifo_trace_arm.bytes) ? size :
                   (u32)sizeof(s_tj_geom_fifo_trace_arm.bytes);
        memcpy(s_tj_geom_fifo_trace_arm.bytes, copied_bytes, copy);
    }

#ifdef _WIN32
    InterlockedExchange(&s_tj_geom_fifo_trace_arm_state, 2);
#else
    atomic_store_explicit(&s_tj_geom_fifo_trace_arm_state, 2, memory_order_release);
#endif
    fprintf(stderr,
            "[TJ-GEOM-FIFO-ARM] cmd=0x%02X lsa=0x%05X ea=0x%08X size=%u pc=0x%05X bytes="
            "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
            cmd, lsa, ea, size, pc,
            s_tj_geom_fifo_trace_arm.bytes[0], s_tj_geom_fifo_trace_arm.bytes[1],
            s_tj_geom_fifo_trace_arm.bytes[2], s_tj_geom_fifo_trace_arm.bytes[3],
            s_tj_geom_fifo_trace_arm.bytes[4], s_tj_geom_fifo_trace_arm.bytes[5],
            s_tj_geom_fifo_trace_arm.bytes[6], s_tj_geom_fifo_trace_arm.bytes[7],
            s_tj_geom_fifo_trace_arm.bytes[8], s_tj_geom_fifo_trace_arm.bytes[9],
            s_tj_geom_fifo_trace_arm.bytes[10], s_tj_geom_fifo_trace_arm.bytes[11],
            s_tj_geom_fifo_trace_arm.bytes[12], s_tj_geom_fifo_trace_arm.bytes[13],
            s_tj_geom_fifo_trace_arm.bytes[14], s_tj_geom_fifo_trace_arm.bytes[15]);
    fflush(stderr);
}

static int tj_geom_fifo_trace_enabled(void)
{
    if (s_tj_geom_fifo_trace_enabled < 0) {
        const char* value = getenv("TJ_GEOM_FIFO_TRACE");
        s_tj_geom_fifo_trace_enabled = value && value[0] && strcmp(value, "0") != 0;
    }
    return s_tj_geom_fifo_trace_enabled;
}

static void tj_geom_fifo_trace_call(u32 target_io, u32 saved_return_io, u32 put_io)
{
    if (tj_geom_fifo_trace_survival_only() &&
        !atomic_load_explicit(&s_tj_survival_profile_armed, memory_order_acquire))
        return;
    int immediate = tj_geom_fifo_trace_enabled();
    int after_img17 = tj_geom_fifo_trace_after_img17_enabled();
    int armed = after_img17 && tj_geom_fifo_trace_arm_is_published();
    if ((!immediate && !armed) || s_tj_geom_fifo_trace_calls >= 32)
        return;

    u32 target_ea = gcm_io2ea(target_io);
    if (target_ea < 0x03F00000u || target_ea > 0x03FFFFFFu)
        return;

    u32 words[16];
    for (unsigned i = 0; i < 16; ++i) {
        u32 word_ea = gcm_io2ea(target_io + i * 4);
        words[i] = word_ea ? vm_read32(word_ea) : 0;
    }
    unsigned id = ++s_tj_geom_fifo_trace_calls;
    if (armed) {
        const TjGeomFifoTraceArm* arm = &s_tj_geom_fifo_trace_arm;
        fprintf(stderr,
                "[TJ-GEOM-FIFO-CALL] id=%u target_io=0x%08X target_ea=0x%08X "
                "return_io=0x%08X put_io=0x%08X words_be="
                "%08X %08X %08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %08X %08X %08X %08X "
                "arm={cmd=0x%02X lsa=0x%05X ea=0x%08X size=%u pc=0x%05X}\n",
                id, target_io, target_ea, saved_return_io, put_io,
                words[0], words[1], words[2], words[3], words[4], words[5],
                words[6], words[7], words[8], words[9], words[10], words[11],
                words[12], words[13], words[14], words[15],
                arm->cmd, arm->lsa, arm->ea, arm->size, arm->pc);
    } else {
        /* Keep TJ_GEOM_FIFO_TRACE=1's pre-arm output unchanged. */
        fprintf(stderr,
                "[TJ-GEOM-FIFO-CALL] id=%u target_io=0x%08X target_ea=0x%08X "
                "return_io=0x%08X put_io=0x%08X words_be="
                "%08X %08X %08X %08X %08X %08X %08X %08X "
                "%08X %08X %08X %08X %08X %08X %08X %08X\n",
                id, target_io, target_ea, saved_return_io, put_io,
                words[0], words[1], words[2], words[3], words[4], words[5],
                words[6], words[7], words[8], words[9], words[10], words[11],
                words[12], words[13], words[14], words[15]);
    }
    fflush(stderr);
    s_tj_geom_fifo_trace_pending_id = id;
    s_tj_geom_fifo_trace_pending_target = target_io;
    if (armed)
        s_tj_geom_fifo_trace_active_id = id;
}

static void tj_geom_fifo_trace_outcome(u32 header_io, u32 header_ea, u32 raw)
{
    if (!s_tj_geom_fifo_trace_pending_id ||
        header_io != s_tj_geom_fifo_trace_pending_target)
        return;

    unsigned id = s_tj_geom_fifo_trace_pending_id;
    s_tj_geom_fifo_trace_pending_id = 0;
    /* Keep classification in the same order as both FIFO walkers. */
    if ((raw & 0xFFFF0000u) == 0xFEAD0000u) {
        fprintf(stderr,
                "[TJ-GEOM-FIFO-OUTCOME] id=%u header_io=0x%08X header_ea=0x%08X "
                "raw=0x%08X outcome=FLIP buffer=%u\n",
                id, header_io, header_ea, raw, raw & 0xFFu);
    } else if ((raw >> 29) == 1) {
        fprintf(stderr,
                "[TJ-GEOM-FIFO-OUTCOME] id=%u header_io=0x%08X header_ea=0x%08X "
                "raw=0x%08X outcome=JUMP target_io=0x%08X\n",
                id, header_io, header_ea, raw, raw & 0x1FFFFFFCu);
    } else if ((raw & 3) == 2) {
        fprintf(stderr,
                "[TJ-GEOM-FIFO-OUTCOME] id=%u header_io=0x%08X header_ea=0x%08X "
                "raw=0x%08X outcome=CALL target_io=0x%08X\n",
                id, header_io, header_ea, raw, raw & 0x1FFFFFFCu);
    } else if (raw == 0x00020000u) {
        fprintf(stderr,
                "[TJ-GEOM-FIFO-OUTCOME] id=%u header_io=0x%08X header_ea=0x%08X "
                "raw=0x%08X outcome=RET\n", id, header_io, header_ea, raw);
    } else if ((raw >> 29) == 0 || (raw >> 29) == 2) {
        fprintf(stderr,
                "[TJ-GEOM-FIFO-OUTCOME] id=%u header_io=0x%08X header_ea=0x%08X "
                "raw=0x%08X outcome=METHOD type=%u subch=%u method=0x%04X count=%u\n",
                id, header_io, header_ea, raw, raw >> 29, (raw >> 13) & 7,
                raw & 0x1FFCu, (raw >> 18) & 0x7FF);
    } else {
        fprintf(stderr,
                "[TJ-GEOM-FIFO-OUTCOME] id=%u header_io=0x%08X header_ea=0x%08X "
                "raw=0x%08X outcome=OTHER type=%u\n",
                id, header_io, header_ea, raw, raw >> 29);
    }
    fflush(stderr);
}

static void tj_geom_fifo_trace_relevant_header(u32 header_io, u32 header_ea, u32 raw)
{
    if (!s_tj_geom_fifo_trace_active_id)
        return;

    u32 type = raw >> 29;
    u32 method = raw & 0x1FFCu;
    int is_ret = raw == 0x00020000u;
    int is_self_jump = type == 1 && (raw & 0x1FFFFFFCu) == header_io;
    int is_method = !is_ret && (raw & 3) != 2 && (type == 0 || type == 2);
    int relevant = is_ret || is_method &&
        (method == 0x0100u || method == 0x1808u || method == 0x1814u ||
         method == 0x1824u);
    /* Inline index payloads use NOP packets legitimately. Reserve the bounded
     * scene trace for draw/control commands after four representative NOPs. */
    if (tj_geom_fifo_trace_survival_only() && is_method && method == 0x0100u &&
        s_tj_geom_fifo_trace_nop_headers++ >= 4u)
        relevant = 0;

    if (relevant && s_tj_geom_fifo_trace_relevant_headers < 64) {
        ++s_tj_geom_fifo_trace_relevant_headers;
        fprintf(stderr,
                "[TJ-GEOM-FIFO-HEADER] call_id=%u header_io=0x%08X header_ea=0x%08X "
                "raw=0x%08X kind=%s count=%u\n",
                s_tj_geom_fifo_trace_active_id, header_io, header_ea, raw,
                is_ret ? "RET" : method == 0x0100u ? "NOP_0100" :
                method == 0x1808u ? "BEGIN_END" :
                method == 0x1814u ? "DRAW_ARRAYS" : "DRAW_INDEX_ARRAY",
                (raw >> 18) & 0x7FF);
        fflush(stderr);
    }
    if (is_ret || is_self_jump)
        s_tj_geom_fifo_trace_active_id = 0;
}

/* ---------------------------------------------------------------------------
 * 2D transfer engines (FIFO subchannels != 0).
 *
 * Tokyo Jungle binds NV3089 scaled-image to sub 6. Other SDK paths seen here
 * use sub 2/3 for NV3062 destination surfaces and sub 4/5 for NV308A inline
 * image data. SET_OBJECT bindings are not otherwise tracked yet.
 *
 * cellGcmSetFragmentProgramParameter patches a fragment program's inline
 * constants by emitting NV3062 (destination surface) + NV308A (COLOR data
 * words) through the FIFO -- it does NOT CPU-write the ucode copy. Without
 * this engine the patched constants stay zero in local memory (demosaic: all
 * its texture-size constants -> UVs scaled by 0 -> uniform output).
 * -----------------------------------------------------------------------*/
extern u64 cellGcmMainMappingGeneration(void);

static struct {
    u32 src_dma;      /* NV3062 0x0184 SET_CONTEXT_DMA_IMAGE_SOURCE  */
    u32 dst_dma;      /* NV3062 0x0188 SET_CONTEXT_DMA_IMAGE_DESTIN */
    u32 color_fmt;    /* NV3062 0x0300 SET_COLOR_FORMAT             */
    u32 pitch;        /* NV3062 0x0304 SET_PITCH (dst in bits 16-31) */
    u32 dst_offset;   /* NV3062 0x030C SET_OFFSET_DESTIN            */
    u32 point;        /* NV308A 0x0304 SET_POINT ((y<<16)|x)        */
    u32 size_out;     /* NV308A 0x0308 SET_SIZE_OUT                 */
    struct {
        u32 src_dma;         /* NV3089 0x0184, independent of NV3062 */
        u32 context_surface; /* NV3089 0x0198 NV3062/NV309E handle */
        u32 conversion;      /* NV3089 0x02FC color conversion */
        u32 src_format;      /* NV3089 0x0300 source format */
        u32 operation;       /* NV3089 0x0304 transfer operation */
        u32 clip_point;      /* NV3089 0x0308 y:x */
        u32 clip_size;       /* NV3089 0x030C h:w */
        u32 out_point;       /* NV3089 0x0310 y:x */
        u32 out_size;        /* NV3089 0x0314 h:w */
        u32 ds_dx;           /* NV3089 0x0318 signed 11.20 */
        u32 dt_dy;           /* NV3089 0x031C signed 11.20 */
        u32 in_size;         /* NV3089 0x0400 h:w */
        u32 in_format;       /* NV3089 0x0404 interp:origin:pitch */
        u32 in_offset;       /* NV3089 0x0408 source RSX offset */
    } scaled;
    struct {
        u32 dma_in;          /* NV0039 0x0184 SET_CONTEXT_DMA_BUFFER_IN */
        u32 dma_out;         /* NV0039 0x0188 SET_CONTEXT_DMA_BUFFER_OUT */
        u32 offset_in;       /* NV0039 0x030C */
        u32 offset_out;      /* NV0039 0x0310 */
        s32 pitch_in;        /* NV0039 0x0314 */
        s32 pitch_out;       /* NV0039 0x0318 */
        u32 line_length;     /* NV0039 0x031C: columns per row */
        u32 line_count;      /* NV0039 0x0320 */
        u32 format;          /* input byte stride [3:0], output [11:8] */
    } m2mf;
    struct {
        u32 dma;             /* NV309E 0x0184 SET_CONTEXT_DMA_IMAGE_DESTIN */
        u32 format;          /* NV309E 0x0300 */
        u32 offset;          /* NV309E 0x0304 */
    } swizzled;
} s_gcm2d;

typedef enum GcmFifoObjectClass {
    GCM_FIFO_OBJECT_UNKNOWN,
    GCM_FIFO_OBJECT_NV4097,
    GCM_FIFO_OBJECT_NV0039,
    GCM_FIFO_OBJECT_NV3062,
    GCM_FIFO_OBJECT_NV309E,
    GCM_FIFO_OBJECT_NV308A,
    GCM_FIFO_OBJECT_NV3089,
    GCM_FIFO_OBJECT_DRIVER
} GcmFifoObjectClass;

/* libgcm's object handles are stable public ABI values. Track SET_OBJECT so a
 * title may legally bind an engine to a different subchannel. The defaults
 * retain the two SDK layouts already observed here until their first bind. */
static GcmFifoObjectClass s_gcm_fifo_object_class[8];
static u32 s_gcm_fifo_object_handle[8];
static u8 s_gcm_fifo_object_trusted[8];

static GcmFifoObjectClass gcm_fifo_object_class_from_handle(u32 handle)
{
    switch (handle) {
    case 0x31337000u: return GCM_FIFO_OBJECT_NV4097;
    case 0x31337303u: return GCM_FIFO_OBJECT_NV0039;
    case 0x313371C3u: return GCM_FIFO_OBJECT_NV3062;
    case 0x31337A73u: return GCM_FIFO_OBJECT_NV309E;
    case 0x31337808u: return GCM_FIFO_OBJECT_NV308A;
    case 0x3137AF00u: return GCM_FIFO_OBJECT_NV3089;
    default: return GCM_FIFO_OBJECT_UNKNOWN;
    }
}

static void gcm_fifo_objects_reset(void)
{
    static const GcmFifoObjectClass defaults[8] = {
        GCM_FIFO_OBJECT_NV4097, GCM_FIFO_OBJECT_NV0039,
        GCM_FIFO_OBJECT_NV3062, GCM_FIFO_OBJECT_NV3062,
        GCM_FIFO_OBJECT_NV308A, GCM_FIFO_OBJECT_NV308A,
        GCM_FIFO_OBJECT_NV3089, GCM_FIFO_OBJECT_DRIVER
    };
    memcpy(s_gcm_fifo_object_class, defaults, sizeof(defaults));
    memset(s_gcm_fifo_object_handle, 0, sizeof(s_gcm_fifo_object_handle));
    memset(s_gcm_fifo_object_trusted, 1, sizeof(s_gcm_fifo_object_trusted));
    memset(&s_gcm2d, 0, sizeof(s_gcm2d));
    s_gcm2d.m2mf.dma_in = 0xFEED0001u;
    s_gcm2d.m2mf.dma_out = 0xFEED0000u;
    s_gcm2d.m2mf.format = 0x00000101u;
}

static void gcm_fifo_bind_object(u32 subch, u32 handle)
{
    if (subch >= 8) return;
    GcmFifoObjectClass object_class = gcm_fifo_object_class_from_handle(handle);
    s_gcm_fifo_object_handle[subch] = handle;
    s_gcm_fifo_object_trusted[subch] = object_class != GCM_FIFO_OBJECT_UNKNOWN;
    if (object_class != GCM_FIFO_OBJECT_UNKNOWN) {
        s_gcm_fifo_object_class[subch] = object_class;
        return;
    }
    /* Preserve the compatibility default for an unknown SDK object instead of
     * silently reclassifying every following method. Keep exact evidence. */
    static unsigned warned = 0;
    if (warned++ < 8)
        fprintf(stderr, "[cellGcmSys] FIFO subch %u unknown SET_OBJECT 0x%08X\n",
                subch, handle);
}

/* HLE flips include the driver's context-register reset. Reset only the
 * registers in that contract, not FIFO position, guest labels or transfer
 * dimensions. In particular NV3089's source DMA survives the NV3062 reset.
 * Reference: RPCS3 9b5cc7cda7821b3819a1f6d1b8fedb974bebf5fa,
 * rsx_methods.cpp flip_command -> thread::reset -> rsx_state::reset. */
static void gcm_fifo_context_reset(rsx_state* state)
{
    rsx_state_reset(state);
    gcm_fifo_bind_object(0, 0x31337000u);
    gcm_fifo_bind_object(1, 0x31337303u);
    gcm_fifo_bind_object(3, 0x313371C3u);
    gcm_fifo_bind_object(4, 0x31337A73u);
    gcm_fifo_bind_object(5, 0x31337808u);
    gcm_fifo_bind_object(6, 0x3137AF00u);
    s_gcm_fifo_object_class[7] = GCM_FIFO_OBJECT_DRIVER;
    s_gcm_fifo_object_handle[7] = 0xCAFEBABEu;
    s_gcm_fifo_object_trusted[7] = 1;
    s_gcm2d.m2mf.dma_in = RSX_CONTEXT_DMA_MAIN;
    s_gcm2d.m2mf.dma_out = RSX_CONTEXT_DMA_LOCAL;
    s_gcm2d.src_dma = s_gcm2d.dst_dma = RSX_CONTEXT_DMA_LOCAL;
    s_gcm2d.swizzled.dma = RSX_CONTEXT_DMA_LOCAL;
    s_fifo_semaphore_dma = 0x66616661u;
    s_fifo_semaphore_offset = 0x10;
    s_fifo_semaphore_wait_since_ns = 0;
}

static int gcm_m2mf_span_valid(u32 base, s32 pitch, u32 rows, u64 row_bytes)
{
    extern u32 ppu_vm_size;
    if (!rows || !row_bytes) return 0;
    int64_t last_delta = (int64_t)pitch * (int64_t)(rows - 1u);
    int64_t first = (int64_t)base + (last_delta < 0 ? last_delta : 0);
    int64_t last = (int64_t)base + (last_delta > 0 ? last_delta : 0);
    u64 end = (u64)last + row_bytes;
    u64 cap = ppu_vm_size ? (u64)ppu_vm_size : (UINT64_C(1) << 32);
    return first >= 0 && (u64)first < cap && end > (u64)last && end <= cap;
}

static int gcm_m2mf_buffer_notify(u32 notify)
{
    (void)notify;
    u32 columns = s_gcm2d.m2mf.line_length;
    u32 rows = s_gcm2d.m2mf.line_count;
    u32 in_stride = s_gcm2d.m2mf.format & 0xFu;
    u32 out_stride = (s_gcm2d.m2mf.format >> 8) & 0xFu;
    if (!columns || !rows || in_stride < 1 || in_stride > 4 ||
        out_stride < 1 || out_stride > 4)
        return 0;

    u64 in_row_bytes = (u64)(columns - 1u) * in_stride + 1u;
    u64 out_row_bytes = (u64)(columns - 1u) * out_stride + 1u;
    u32 src = 0, dst = 0;
    if (!cellGcmResolveLocatedChecked(s_gcm2d.m2mf.dma_in != 0xFEED0001u,
                                      s_gcm2d.m2mf.offset_in, &src) ||
        !cellGcmResolveLocatedChecked(s_gcm2d.m2mf.dma_out != 0xFEED0001u,
                                      s_gcm2d.m2mf.offset_out, &dst) ||
        !gcm_m2mf_span_valid(src, s_gcm2d.m2mf.pitch_in, rows, in_row_bytes) ||
        !gcm_m2mf_span_valid(dst, s_gcm2d.m2mf.pitch_out, rows, out_row_bytes)) {
        static unsigned warned = 0;
        if (warned++ < 8)
            fprintf(stderr,
                    "[GCM2D] NV0039 rejected span src=0x%08X dst=0x%08X len=%u rows=%u pitch=%d/%d stride=%u/%u\n",
                    src, dst, columns, rows, s_gcm2d.m2mf.pitch_in,
                    s_gcm2d.m2mf.pitch_out, in_stride, out_stride);
        return 0;
    }

    u64 sample_bytes = (u64)columns * rows;
    if (!sample_bytes || sample_bytes > (256ULL << 20) || sample_bytes > SIZE_MAX)
        return 0;

    /* Always stage the logical bytes. This is correct for overlapping 2D
     * rectangles, negative pitches, and NV0039 channel gather/scatter strides. */
    u8* staging = (u8*)malloc((size_t)sample_bytes);
    if (!staging) return 0;
    for (u32 row = 0; row < rows; ++row) {
        const u8* source = vm_ptr8((u32)((int64_t)src +
                                   (int64_t)s_gcm2d.m2mf.pitch_in * row));
        u8* packed = staging + (u64)row * columns;
        for (u32 column = 0; column < columns; ++column)
            packed[column] = source[(u64)column * in_stride];
    }
    for (u32 row = 0; row < rows; ++row) {
        u8* target = vm_ptr8((u32)((int64_t)dst +
                             (int64_t)s_gcm2d.m2mf.pitch_out * row));
        const u8* packed = staging + (u64)row * columns;
        for (u32 column = 0; column < columns; ++column)
            target[(u64)column * out_stride] = packed[column];
    }
    free(staging);
    return 0;
}

/* NV309E is the scaled-image destination surface. Its low byte uses the
 * transfer-destination enum, not NV3089's transfer-source enum. */
static int gcm_swizzled_destination_layout(u32 format, u32* out_width,
                                           u32* out_height, u32* out_bpp)
{
    u32 log2_width = (format >> 16) & 0xFFu;
    u32 log2_height = format >> 24;
    const u32 color_format = (u8)format;
    const u32 bpp = color_format == 4u ? 2u :
                    (color_format == 10u || color_format == 11u) ? 4u : 0u;
    if (!bpp || log2_width >= 16 || log2_height >= 16)
        return 0;
    u32 width = 1u << log2_width;
    u32 height = 1u << log2_height;
    if (!width || !height || width > UINT16_MAX || height > UINT16_MAX ||
        (u64)width * bpp > UINT16_MAX)
        return 0;
    *out_width = width;
    *out_height = height;
    *out_bpp = bpp;
    return 1;
}

/* Deswizzle a bounded NV309E destination into rows for the established CPU
 * scaler, then scatter only the rectangle it wrote back to Morton storage. */
static int gcm_swizzled_destination_stage(u32 ea, u32 width, u32 height, u32 bpp,
                                          u32 log2_width, u32 log2_height,
                                          u8** out_staging)
{
    extern u32 ppu_vm_size;
    u64 pixels = (u64)width * height;
    u64 bytes = pixels * bpp;
    u64 cap = ppu_vm_size ? (u64)ppu_vm_size : (UINT64_C(1) << 32);
    if (!pixels || !bytes || bytes > (256ULL << 20) || bytes > SIZE_MAX ||
        (u64)ea > cap || bytes > cap - ea)
        return 0;
    u8* staging = (u8*)malloc((size_t)bytes);
    if (!staging) return 0;
    const u8* source = vm_ptr8(ea);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u64 linear = ((u64)y * width + x) * bpp;
            u64 swizzled = (u64)rsx_scaled_swizzled_offset(
                x, y, log2_width, log2_height) * bpp;
            memcpy(staging + linear, source + swizzled, bpp);
        }
    }
    *out_staging = staging;
    return 1;
}

static void gcm_swizzled_destination_scatter(u32 ea, const u8* staging,
                                             u32 width, u32 x0, u32 y0,
                                             u32 copy_width, u32 copy_height,
                                             u32 bpp, u32 log2_width,
                                             u32 log2_height)
{
    u8* destination = vm_ptr8(ea);
    for (u32 y = 0; y < copy_height; ++y) {
        for (u32 x = 0; x < copy_width; ++x) {
            u64 linear = ((u64)(y0 + y) * width + x0 + x) * bpp;
            u64 swizzled = (u64)rsx_scaled_swizzled_offset(
                x0 + x, y0 + y, log2_width, log2_height) * bpp;
            memcpy(destination + swizzled, staging + linear, bpp);
        }
    }
}

/* Default-off aggregate timing for the CPU NV3089 path.  It deliberately
 * measures the HLE-side work without changing the FIFO ordering or taking a
 * lock while a transfer is in progress. */
enum {
    TJ_GCM2D_PERF_RESOLVE_SETUP,
    TJ_GCM2D_PERF_PRE_SNAPSHOT_ALLOC,
    TJ_GCM2D_PERF_CPU_BLIT,
    TJ_GCM2D_PERF_POST_SNAPSHOT,
    TJ_GCM2D_PERF_BACKEND_RECORD,
    TJ_GCM2D_PERF_BACKEND_COMPLETE,
    TJ_GCM2D_PERF_GATE_END,
    TJ_GCM2D_PERF_TOTAL,
    TJ_GCM2D_PERF_PHASE_COUNT
};

typedef struct TjGcm2dPerfCall {
    u64 phase_ns[TJ_GCM2D_PERF_PHASE_COUNT];
    u64 pre_snapshot_bytes;
    u64 pre_src_snapshot_bytes;
    u64 pre_dst_snapshot_bytes;
    u64 post_snapshot_bytes;
    unsigned result;
    int return_failed;
    int capture_attempted;
    int capture_ready;
    int record_called;
    int record_ok;
    int complete_called;
    int complete_local_main;
    int complete_failed;
    int gate_called;
    int gate_failed;
} TjGcm2dPerfCall;

typedef struct TjGcm2dPerf {
    u64 report_start_ns;
    /* Host tick bounds are sampled on the record path; the stderr write occurs later. */
    u64 report_start_ms;
    u64 report_end_ms;
    u64 calls;
    u64 results[RSX_SCALED_BLIT_UNSUPPORTED_OPERATION + 1];
    u64 return_failed;
    u64 capture_attempted;
    u64 capture_ready;
    u64 record_called;
    u64 record_ok;
    u64 complete_called;
    u64 complete_local_main;
    u64 complete_failed;
    u64 gate_called;
    u64 gate_failed;
    u64 pre_snapshot_bytes;
    u64 pre_src_snapshot_bytes;
    u64 pre_dst_snapshot_bytes;
    u64 post_snapshot_bytes;
    u64 phase_total_ns[TJ_GCM2D_PERF_PHASE_COUNT];
    u64 phase_max_ns[TJ_GCM2D_PERF_PHASE_COUNT];
} TjGcm2dPerf;

static atomic_int s_tj_gcm2d_perf_enabled = ATOMIC_VAR_INIT(-1);
static atomic_flag s_tj_gcm2d_perf_lock = ATOMIC_FLAG_INIT;
static TjGcm2dPerf s_tj_gcm2d_perf;
#ifdef _WIN32
static __declspec(thread) int s_tj_gcm2d_perf_tls_enabled = -1;
#else
static __thread int s_tj_gcm2d_perf_tls_enabled = -1;
#endif

static int tj_gcm2d_perf_enabled(void)
{
    int enabled = s_tj_gcm2d_perf_tls_enabled;
    if (enabled >= 0) return enabled;

    enabled = atomic_load_explicit(&s_tj_gcm2d_perf_enabled, memory_order_acquire);
    if (enabled < 0) {
        int expected = -1;
        if (atomic_compare_exchange_strong_explicit(&s_tj_gcm2d_perf_enabled,
                                                    &expected, -2,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            const char* value = getenv("TJ_GCM2D_PERF");
            enabled = value && value[0] && strcmp(value, "0") != 0;
            /* TJ_PREFIX_COST enables this HLE-side timer only.  Keep the
             * TJ_GCM2D_PERF backend switches independent so prefix timing
             * does not disable prefix overlap or alter ownership selection. */
            if (!enabled) {
                value = getenv("TJ_PREFIX_COST");
                enabled = value && strcmp(value, "1") == 0;
            }
            atomic_store_explicit(&s_tj_gcm2d_perf_enabled, enabled,
                                  memory_order_release);
        } else {
            do {
                enabled = atomic_load_explicit(&s_tj_gcm2d_perf_enabled,
                                               memory_order_acquire);
            } while (enabled == -2);
        }
    }
    return s_tj_gcm2d_perf_tls_enabled = enabled;
}

static u64 tj_gcm2d_perf_host_ms(void)
{
#ifdef _WIN32
    return (u64)GetTickCount64();
#else
    return get_timestamp_ns() / 1000000ULL;
#endif
}

static const char* tj_gcm2d_perf_host_clock(void)
{
#ifdef _WIN32
    return "GetTickCount64";
#else
    return "CLOCK_MONOTONIC";
#endif
}

static void tj_gcm2d_perf_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_tj_gcm2d_perf_lock,
                                             memory_order_acquire)) {}
}

static void tj_gcm2d_perf_unlock(void)
{
    atomic_flag_clear_explicit(&s_tj_gcm2d_perf_lock, memory_order_release);
}

static void tj_gcm2d_perf_log(const TjGcm2dPerf* perf, u64 now_ns, u64 log_ms)
{
    static const char* const phase_name[TJ_GCM2D_PERF_PHASE_COUNT] = {
        "resolve", "pre", "cpu", "post", "record", "complete", "gate", "total"
    };
    u64 window_ms = (now_ns - perf->report_start_ns) / 1000000ULL;
    fprintf(stderr,
            "[TJ-GCM2D-PERF] start_ms=%llu end_ms=%llu log_ms=%llu tick_source=%s tick_precision=coarse window_ms=%llu calls=%llu "
            "result={ok=%llu empty=%llu format=%llu surface=%llu operation=%llu} "
            "return_failed=%llu capture={attempted=%llu ready=%llu} "
            "record={calls=%llu ok=%llu fail=%llu} "
            "complete={calls=%llu local_main=%llu fail=%llu} "
            "gate={calls=%llu fail=%llu} "
            "bytes={pre=%llu post=%llu} pre_split={src=%llu dst=%llu}",
            (unsigned long long)perf->report_start_ms,
            (unsigned long long)perf->report_end_ms,
            (unsigned long long)log_ms,
            tj_gcm2d_perf_host_clock(),
            (unsigned long long)window_ms,
            (unsigned long long)perf->calls,
            (unsigned long long)perf->results[RSX_SCALED_BLIT_OK],
            (unsigned long long)perf->results[RSX_SCALED_BLIT_EMPTY],
            (unsigned long long)perf->results[RSX_SCALED_BLIT_UNSUPPORTED_FORMAT],
            (unsigned long long)perf->results[RSX_SCALED_BLIT_UNSUPPORTED_SURFACE],
            (unsigned long long)perf->results[RSX_SCALED_BLIT_UNSUPPORTED_OPERATION],
            (unsigned long long)perf->return_failed,
            (unsigned long long)perf->capture_attempted,
            (unsigned long long)perf->capture_ready,
            (unsigned long long)perf->record_called,
            (unsigned long long)perf->record_ok,
            (unsigned long long)(perf->record_called - perf->record_ok),
            (unsigned long long)perf->complete_called,
            (unsigned long long)perf->complete_local_main,
            (unsigned long long)perf->complete_failed,
            (unsigned long long)perf->gate_called,
            (unsigned long long)perf->gate_failed,
            (unsigned long long)perf->pre_snapshot_bytes,
            (unsigned long long)perf->post_snapshot_bytes,
            (unsigned long long)perf->pre_src_snapshot_bytes,
            (unsigned long long)perf->pre_dst_snapshot_bytes);
    for (unsigned i = 0; i < TJ_GCM2D_PERF_PHASE_COUNT; ++i) {
        fprintf(stderr, " %s={total_us=%llu max_us=%llu}", phase_name[i],
                (unsigned long long)(perf->phase_total_ns[i] / 1000ULL),
                (unsigned long long)(perf->phase_max_ns[i] / 1000ULL));
    }
    fputc('\n', stderr);
    fflush(stderr);
}

static void tj_gcm2d_perf_record(const TjGcm2dPerfCall* call, u64 now_ns)
{
    TjGcm2dPerf snapshot = {0};
    int report = 0;
    /* On Windows this is a coarse host tick. Capture the window end before
     * taking the stats lock; log_ms below is sampled after unlocking. */
    const u64 now_ms = tj_gcm2d_perf_host_ms();
    tj_gcm2d_perf_lock();
    TjGcm2dPerf* perf = &s_tj_gcm2d_perf;
    if (!perf->report_start_ns) {
        perf->report_start_ns = now_ns;
        perf->report_start_ms = now_ms;
    }
    perf->calls++;
    if (call->result <= RSX_SCALED_BLIT_UNSUPPORTED_OPERATION)
        perf->results[call->result]++;
    perf->return_failed += call->return_failed != 0;
    perf->capture_attempted += call->capture_attempted != 0;
    perf->capture_ready += call->capture_ready != 0;
    perf->record_called += call->record_called != 0;
    perf->record_ok += call->record_ok != 0;
    perf->complete_called += call->complete_called != 0;
    perf->complete_local_main += call->complete_local_main != 0;
    perf->complete_failed += call->complete_failed != 0;
    perf->gate_called += call->gate_called != 0;
    perf->gate_failed += call->gate_failed != 0;
    perf->pre_snapshot_bytes += call->pre_snapshot_bytes;
    perf->pre_src_snapshot_bytes += call->pre_src_snapshot_bytes;
    perf->pre_dst_snapshot_bytes += call->pre_dst_snapshot_bytes;
    perf->post_snapshot_bytes += call->post_snapshot_bytes;
    for (unsigned i = 0; i < TJ_GCM2D_PERF_PHASE_COUNT; ++i) {
        perf->phase_total_ns[i] += call->phase_ns[i];
        if (call->phase_ns[i] > perf->phase_max_ns[i])
            perf->phase_max_ns[i] = call->phase_ns[i];
    }
    if (now_ns - perf->report_start_ns >= 1000000000ULL) {
        snapshot = *perf;
        snapshot.report_end_ms = now_ms;
        memset(perf, 0, sizeof(*perf));
        perf->report_start_ns = now_ns;
        perf->report_start_ms = now_ms;
        report = 1;
    }
    tj_gcm2d_perf_unlock();
    if (report) tj_gcm2d_perf_log(&snapshot, now_ns, tj_gcm2d_perf_host_ms());
}

static int gcm_scaled_image_in(u32 in_point)
{
    struct rsx_scaled_blit_desc desc;
    u8* swizzled_staging = NULL;
    const int swizzled_destination =
        s_gcm2d.scaled.context_surface == 0x31337A73u;
    int tj_gcm2d_perf = tj_gcm2d_perf_enabled();
    TjGcm2dPerfCall tj_gcm2d_perf_call = {0};
    u64 tj_gcm2d_perf_start = tj_gcm2d_perf ? get_timestamp_ns() : 0;
    int return_status = 0;
    /* One generation brackets both endpoint translations and every byte
     * captured for deferred replay.  Raw IO offsets are not stable identities
     * across a Map/Unmap, so a changed/odd generation rejects GPU mirroring. */
    const u64 mapping_before = cellGcmMainMappingGeneration();
    u32 src_ea, dst_ea;
    if (!cellGcmResolveLocatedChecked(s_gcm2d.scaled.src_dma != 0xFEED0001u,
                                      s_gcm2d.scaled.in_offset, &src_ea) ||
        !cellGcmResolveLocatedChecked(
            swizzled_destination ? s_gcm2d.swizzled.dma != 0xFEED0001u :
                                    s_gcm2d.dst_dma != 0xFEED0001u,
            swizzled_destination ? s_gcm2d.swizzled.offset : s_gcm2d.dst_offset,
            &dst_ea))
        return 0;
    memset(&desc, 0, sizeof(desc));
    desc.src_pitch = (u16)(s_gcm2d.scaled.in_format & 0xFFFF);
    desc.dst_pitch = (u16)(s_gcm2d.pitch >> 16);
    desc.in_width = (u16)(s_gcm2d.scaled.in_size & 0xFFFF);
    desc.in_height = (u16)(s_gcm2d.scaled.in_size >> 16);
    desc.out_width = (u16)(s_gcm2d.scaled.out_size & 0xFFFF);
    desc.out_height = (u16)(s_gcm2d.scaled.out_size >> 16);
    desc.clip_x = (u16)(s_gcm2d.scaled.clip_point & 0xFFFF);
    desc.clip_y = (u16)(s_gcm2d.scaled.clip_point >> 16);
    desc.clip_width = (u16)(s_gcm2d.scaled.clip_size & 0xFFFF);
    desc.clip_height = (u16)(s_gcm2d.scaled.clip_size >> 16);
    desc.out_x = (u16)(s_gcm2d.scaled.out_point & 0xFFFF);
    desc.out_y = (u16)(s_gcm2d.scaled.out_point >> 16);
    /* Opt-in TJ_BLIT_CLIP_ABSOLUTE=1: Tokyo Jungle's 2D helper (func_001FC4EC)
     * writes one value to both CLIP_POINT and OUT_POINT, i.e. an absolute
     * destination clip. The blit code treats clip as relative to OUT_POINT, so
     * strips at y>=height were dropped as EMPTY and others landed at 2*offset.
     * Only the exact clip==out (and clip size==out size) form is normalised. */
    {
        static int clip_abs = -1;
        if (clip_abs < 0) {
            const char* v = getenv("TJ_BLIT_CLIP_ABSOLUTE");
            clip_abs = v && v[0] == '1' && v[1] == '\0';
            if (clip_abs) fprintf(stderr, "[tj-blit-clip] absolute clip normalisation enabled\n");
        }
        if (clip_abs && s_gcm2d.scaled.clip_point == s_gcm2d.scaled.out_point &&
            s_gcm2d.scaled.clip_size == s_gcm2d.scaled.out_size &&
            s_gcm2d.scaled.clip_point != 0) {
            desc.clip_x = 0;
            desc.clip_y = 0;
        }
    }
    desc.in_x_4 = (u16)(in_point & 0xFFFF);
    desc.in_y_4 = (u16)(in_point >> 16);
    desc.ds_dx = (int32_t)s_gcm2d.scaled.ds_dx;
    desc.dt_dy = (int32_t)s_gcm2d.scaled.dt_dy;
    desc.src_format = (u8)s_gcm2d.scaled.src_format;
    desc.dst_format = (u8)s_gcm2d.color_fmt;
    desc.origin = (u8)(s_gcm2d.scaled.in_format >> 16);
    desc.interpolator = (u8)(s_gcm2d.scaled.in_format >> 24);
    u32 swizzled_width = 0, swizzled_height = 0, swizzled_bpp = 0;
    u32 swizzled_x = 0, swizzled_y = 0, swizzled_copy_width = 0;
    u32 swizzled_copy_height = 0, swizzled_log2_width = 0;
    u32 swizzled_log2_height = 0;
    if (swizzled_destination) {
        u32 src_bpp = rsx_scaled_src_bpp(desc.src_format);
        u32 log2_width = (s_gcm2d.swizzled.format >> 16) & 0xFFu;
        u32 log2_height = s_gcm2d.swizzled.format >> 24;
        u32 copy_width = desc.clip_width;
        u32 copy_height = desc.clip_height;
        extern u32 ppu_vm_size;
        u64 cap = ppu_vm_size ? (u64)ppu_vm_size : (UINT64_C(1) << 32);
        u64 src_pitch = desc.src_pitch ? desc.src_pitch :
            (u64)desc.in_width * src_bpp;
        u64 src_bytes = desc.in_height
            ? (u64)(desc.in_height - 1u) * src_pitch +
              (u64)desc.in_width * src_bpp : 0;
        if (!gcm_swizzled_destination_layout(s_gcm2d.swizzled.format,
                                              &swizzled_width,
                                              &swizzled_height,
                                              &swizzled_bpp) ||
            !src_bpp || !src_pitch || src_pitch < (u64)desc.in_width * src_bpp ||
            !src_bytes || (u64)src_ea > cap ||
            src_bytes > cap - src_ea ||
            desc.clip_x >= desc.out_width || desc.clip_y >= desc.out_height)
            return 0;
        if (copy_width > desc.out_width - desc.clip_x)
            copy_width = desc.out_width - desc.clip_x;
        if (copy_height > desc.out_height - desc.clip_y)
            copy_height = desc.out_height - desc.clip_y;
        u64 dst_x = (u64)desc.out_x + desc.clip_x;
        u64 dst_y = (u64)desc.out_y + desc.clip_y;
        if (!copy_width || !copy_height || dst_x >= swizzled_width ||
            dst_y >= swizzled_height || copy_width > swizzled_width - dst_x ||
            copy_height > swizzled_height - dst_y ||
            !gcm_swizzled_destination_stage(dst_ea, swizzled_width,
                                             swizzled_height, swizzled_bpp,
                                             log2_width, log2_height,
                                             &swizzled_staging))
            return 0;
        desc.dst = swizzled_staging;
        desc.dst_pitch = (u16)(swizzled_width * swizzled_bpp);
        desc.dst_format = (u8)s_gcm2d.swizzled.format;
        swizzled_x = (u32)dst_x;
        swizzled_y = (u32)dst_y;
        swizzled_copy_width = copy_width;
        swizzled_copy_height = copy_height;
        swizzled_log2_width = log2_width;
        swizzled_log2_height = log2_height;
        desc.src = vm_ptr8(src_ea);
    } else {
        /* Validate both CPU endpoints before vm_ptr8.  NV3089 pitches and
         * IMAGE_OUT_POINT are guest-controlled, so keep every pitch/span and
         * cap calculation in 64-bit arithmetic until validation succeeds. */
        extern u32 ppu_vm_size;
        u32 src_bpp = rsx_scaled_src_bpp(desc.src_format);
        u32 dst_bpp = rsx_scaled_dst_bpp(desc.dst_format);
        u64 src_pitch = desc.src_pitch ? desc.src_pitch :
            (u64)desc.in_width * src_bpp;
        u64 dst_pitch = desc.dst_pitch ? desc.dst_pitch :
            (u64)desc.out_width * dst_bpp;
        u64 dst_width = (u64)desc.out_x + desc.out_width;
        u64 dst_height = (u64)desc.out_y + desc.out_height;
        if (!src_bpp || !dst_bpp || src_pitch > UINT32_MAX ||
            dst_pitch > UINT32_MAX || !dst_width || !dst_height ||
            dst_width > UINT32_MAX || dst_height > UINT32_MAX ||
            !rsx_scaled_blit_guest_surface_valid(src_ea, src_pitch,
                                                  desc.in_width, desc.in_height,
                                                  src_bpp, ppu_vm_size) ||
            !rsx_scaled_blit_guest_surface_valid(dst_ea, dst_pitch,
                                                  (u32)dst_width, (u32)dst_height,
                                                  dst_bpp, ppu_vm_size))
            return 0;
        desc.src = vm_ptr8(src_ea);
        desc.dst = vm_ptr8(dst_ea);
    }
    if (tj_gcm2d_perf)
        tj_gcm2d_perf_call.phase_ns[TJ_GCM2D_PERF_RESOLVE_SETUP] =
            get_timestamp_ns() - tj_gcm2d_perf_start;

#ifdef _WIN32
    /* Snapshot before the CPU transfer.  Deferred replay happens after the
     * FIFO batch has advanced, so reading guest RAM there would see a later
     * A->B/C->A state rather than this operation's input. */
    RsxD3D12ExactBlit gpu = {0};
    int gpu_snapshot_ready = 0;
    int gpu_snapshot_attempted = 0;
    /* Active only for a canonical LOCAL->MAIN exact region.  This is not a
     * guest-world lock: it delays only overlapping regular SPU PUT memcpys
     * until the prefix commit has passed its baseline guard. */
    u64 gpu_main_write_gate_token = 0;
    struct rsx_scaled_blit_exact_region region;
    u64 tj_gcm2d_pre_snapshot_start = tj_gcm2d_perf ? get_timestamp_ns() : 0;
    if (!swizzled_destination && rsx_d3d12_backend_exact_blit_capture_enabled() &&
        rsx_scaled_blit_exact_rgba_region(&desc, &region)) {
        gpu_snapshot_attempted = 1;
        extern u32 ppu_vm_size;
        u32 src_pitch = desc.src_pitch ? desc.src_pitch : desc.in_width * 4u;
        u32 dst_pitch = desc.dst_pitch ? desc.dst_pitch : desc.out_width * 4u;
        u32 src_w = src_pitch / 4u, dst_w = dst_pitch / 4u;
        u32 dst_h = desc.out_height;
        u32 dst_end = (u32)region.dst_y + region.height;
        if (dst_end > dst_h) dst_h = dst_end;
        gpu.src_raw = s_gcm2d.scaled.in_offset; gpu.dst_raw = s_gcm2d.dst_offset;
        gpu.src_ea = src_ea; gpu.dst_ea = dst_ea;
        gpu.mapping_generation = mapping_before;
        gpu.src_pitch = src_pitch; gpu.dst_pitch = dst_pitch;
        gpu.src_w = desc.in_width; gpu.src_h = desc.in_height;
        gpu.dst_w = desc.out_width; gpu.dst_h = dst_h;
        gpu.src_x = region.src_x; gpu.src_y = region.src_y;
        gpu.dst_x = region.dst_x; gpu.dst_y = region.dst_y;
        gpu.copy_w = region.width; gpu.copy_h = region.height;
        gpu.src_local = s_gcm2d.scaled.src_dma != 0xFEED0001u;
        gpu.dst_local = s_gcm2d.dst_dma != 0xFEED0001u;
        gpu.src_cpu_owned = gpu.src_local && rsx_d3d12_backend_local_source_is_cpu(
            gpu.src_raw, src_pitch, gpu.src_w, gpu.src_h);
        /* Preserve immutable bytes for MAIN and proven CPU-owned LOCAL
         * sources. GPU-owned LOCAL inputs still use their ordered resource;
         * no stale guest snapshot can replace those producers. */
        if (!(mapping_before & 1u) &&
            src_w <= 16384u && dst_w <= 16384u && gpu.src_h <= 16384u &&
            gpu.dst_h <= 16384u &&
            rsx_scaled_blit_guest_span_valid(src_ea, src_pitch, src_w, gpu.src_h,
                                             ppu_vm_size, &gpu.src_snapshot_bytes) &&
            rsx_scaled_blit_guest_span_valid(dst_ea, dst_pitch, dst_w, gpu.dst_h,
                                             ppu_vm_size, &gpu.dst_snapshot_bytes) &&
            gpu.src_snapshot_bytes <= (256ULL << 20) &&
            gpu.dst_snapshot_bytes <= (256ULL << 20) &&
            ((gpu.src_local && !gpu.src_cpu_owned) ||
             (gpu.src_snapshot = rsx_d3d12_blit_snapshot_acquire(
                 gpu.src_snapshot_bytes)) != NULL) &&
            (gpu.dst_local ||
             (gpu.dst_snapshot = rsx_d3d12_blit_snapshot_acquire(
                 gpu.dst_snapshot_bytes)) != NULL)) {
            int gate_ok = 1;
            if (gpu.src_local && !gpu.dst_local) {
                /* The rectangle may have pitch holes; guarding its enclosing
                 * half-open EA span is conservative and prevents an SPU PUT
                 * from racing either snapshot or the exact CPU copy. */
                const u32 pixels_per_row = gpu.dst_pitch / 4u;
                const u64 row_bytes = (u64)gpu.copy_w * 4u;
                const u64 first = (u64)dst_ea + (u64)gpu.dst_y * gpu.dst_pitch +
                                  (u64)gpu.dst_x * 4u;
                const u64 span = gpu.copy_h
                    ? (u64)(gpu.copy_h - 1u) * gpu.dst_pitch + row_bytes : 0;
                const u64 end = first + span;
                if (!gpu.dst_pitch || (gpu.dst_pitch & 3u) || !gpu.copy_w ||
                    !gpu.copy_h || gpu.copy_w > pixels_per_row ||
                    gpu.dst_x > pixels_per_row - gpu.copy_w || !span ||
                    end <= first || end > (UINT64_C(1) << 32) ||
                    span > UINT32_MAX ||
                    !(gpu_main_write_gate_token =
                        spu_main_write_gate_begin((u32)first, (u32)span))) {
                    gate_ok = 0;
                }
            }
            if (gate_ok) {
                if (!gpu.src_local || gpu.src_cpu_owned) {
                    memcpy(gpu.src_snapshot, vm_ptr8(src_ea), (size_t)gpu.src_snapshot_bytes);
                } else {
                    /* No source snapshot exists for LOCAL GPU-owned endpoints. */
                    gpu.src_snapshot_bytes = 0;
                }
                if (!gpu.dst_local) {
                    memcpy(gpu.dst_snapshot, vm_ptr8(dst_ea),
                           (size_t)gpu.dst_snapshot_bytes);
                } else {
                    /* No destination snapshot exists for LOCAL GPU-owned endpoints. */
                    gpu.dst_snapshot_bytes = 0;
                }
                gpu_snapshot_ready = 1;
                if (tj_gcm2d_perf) {
                    tj_gcm2d_perf_call.pre_snapshot_bytes +=
                        gpu.src_snapshot_bytes + gpu.dst_snapshot_bytes;
                    tj_gcm2d_perf_call.pre_src_snapshot_bytes += gpu.src_snapshot_bytes;
                    tj_gcm2d_perf_call.pre_dst_snapshot_bytes += gpu.dst_snapshot_bytes;
                }
            } else {
                rsx_d3d12_blit_snapshot_release(gpu.src_snapshot,
                                                  gpu.src_snapshot_bytes);
                rsx_d3d12_blit_snapshot_release(gpu.dst_snapshot,
                                                  gpu.dst_snapshot_bytes);
                memset(&gpu, 0, sizeof(gpu));
            }
        } else {
            rsx_d3d12_blit_snapshot_release(gpu.src_snapshot,
                                              gpu.src_snapshot_bytes);
            rsx_d3d12_blit_snapshot_release(gpu.dst_snapshot,
                                              gpu.dst_snapshot_bytes);
            memset(&gpu, 0, sizeof(gpu));
        }
    }
    if (tj_gcm2d_perf) {
        tj_gcm2d_perf_call.phase_ns[TJ_GCM2D_PERF_PRE_SNAPSHOT_ALLOC] =
            get_timestamp_ns() - tj_gcm2d_pre_snapshot_start;
        tj_gcm2d_perf_call.capture_attempted = gpu_snapshot_attempted;
    }
#endif

    enum rsx_scaled_blit_result result = RSX_SCALED_BLIT_UNSUPPORTED_SURFACE;
    if (s_gcm2d.scaled.context_surface == 0x313371C3u || swizzled_destination) {
        if (s_gcm2d.scaled.operation == 3u) {
            u64 tj_gcm2d_cpu_start = tj_gcm2d_perf ? get_timestamp_ns() : 0;
            result = rsx_scaled_blit(&desc);
            if (result == RSX_SCALED_BLIT_OK && swizzled_destination)
                gcm_swizzled_destination_scatter(
                    dst_ea, swizzled_staging, swizzled_width, swizzled_x,
                    swizzled_y, swizzled_copy_width, swizzled_copy_height,
                    swizzled_bpp, swizzled_log2_width, swizzled_log2_height);
            if (tj_gcm2d_perf)
                tj_gcm2d_perf_call.phase_ns[TJ_GCM2D_PERF_CPU_BLIT] +=
                    get_timestamp_ns() - tj_gcm2d_cpu_start;
        } else {
            result = RSX_SCALED_BLIT_UNSUPPORTED_OPERATION;
        }
    }

#ifdef _WIN32
    /* Preserve the guest CPU copy above, then mirror only a byte-exact RGBA
     * transfer into the deferred D3D record stream.  Calling D3D here would
     * run before previously recorded 3D producers, so the backend consumes
     * this descriptor later at the same FIFO position instead. */
    u64 tj_gcm2d_post_snapshot_start = tj_gcm2d_perf ? get_timestamp_ns() : 0;
    if (result == RSX_SCALED_BLIT_OK && gpu_snapshot_ready && !gpu.dst_local) {
        /* Preserve a MAIN destination's CPU-authoritative dirty rectangle after
         * the copy.  Deferred GPU writeback must compare this row-packed
         * baseline before it can touch guest RAM.  LOCAL destinations never
         * enter that writeback path. */
        u64 packed = (u64)gpu.copy_w * gpu.copy_h * 4u;
        if (!packed || packed > SIZE_MAX ||
            !(gpu.dst_post_snapshot = rsx_d3d12_blit_snapshot_acquire(packed))) {
            gpu_snapshot_ready = 0;
        } else {
            for (u32 y = 0; y < gpu.copy_h; ++y)
                memcpy(gpu.dst_post_snapshot + (u64)y * gpu.copy_w * 4u,
                       vm_ptr8(dst_ea) + (u64)(gpu.dst_y + y) * gpu.dst_pitch +
                           (u64)gpu.dst_x * 4u,
                       (size_t)gpu.copy_w * 4u);
            gpu.dst_post_snapshot_bytes = packed;
            if (tj_gcm2d_perf)
                tj_gcm2d_perf_call.post_snapshot_bytes += packed;
        }
    }
    if (gpu_snapshot_attempted &&
        !rsx_exact_blit_mapping_generation_stable(
            mapping_before, cellGcmMainMappingGeneration()))
        gpu_snapshot_ready = 0;
    if (tj_gcm2d_perf) {
        tj_gcm2d_perf_call.capture_ready = gpu_snapshot_ready;
        tj_gcm2d_perf_call.phase_ns[TJ_GCM2D_PERF_POST_SNAPSHOT] =
            get_timestamp_ns() - tj_gcm2d_post_snapshot_start;
    }
    if (result == RSX_SCALED_BLIT_OK && gpu_snapshot_attempted) {
        int gpu_exact_fault = 0;
        int gpu_record_ok = 0;
        if (gpu_snapshot_ready) {
            u64 tj_gcm2d_record_start = tj_gcm2d_perf ? get_timestamp_ns() : 0;
            gpu_record_ok = rsx_d3d12_backend_record_exact_blit(&gpu);
            if (tj_gcm2d_perf) {
                tj_gcm2d_perf_call.phase_ns[TJ_GCM2D_PERF_BACKEND_RECORD] +=
                    get_timestamp_ns() - tj_gcm2d_record_start;
                tj_gcm2d_perf_call.record_called = 1;
                tj_gcm2d_perf_call.record_ok = gpu_record_ok;
            }
        }
        if (!gpu_snapshot_ready || !gpu_record_ok) {
            if (!gpu_snapshot_ready) {
                rsx_d3d12_blit_snapshot_release(gpu.src_snapshot,
                                                  gpu.src_snapshot_bytes);
                rsx_d3d12_blit_snapshot_release(gpu.dst_snapshot,
                                                  gpu.dst_snapshot_bytes);
                rsx_d3d12_blit_snapshot_release(gpu.dst_post_snapshot,
                                                  gpu.dst_post_snapshot_bytes);
            }
            static unsigned gpu_capture_faults = 0;
            if (gpu_capture_faults++ < 16)
                fprintf(stderr, "[GCM2D-GPU] fault: CPU exact copy was not mirrored\n");
            rsx_d3d12_backend_note_exact_blit_fault();
            gpu_exact_fault = 1;
        }
        if (!gpu_exact_fault) {
            u64 tj_gcm2d_prefix_start = tj_gcm2d_perf ? get_timestamp_ns() : 0;
            int prefix_rc = rsx_d3d12_backend_complete_local_main_prefix();
            if (tj_gcm2d_perf) {
                tj_gcm2d_perf_call.phase_ns[TJ_GCM2D_PERF_BACKEND_COMPLETE] +=
                    get_timestamp_ns() - tj_gcm2d_prefix_start;
                tj_gcm2d_perf_call.complete_called = 1;
                tj_gcm2d_perf_call.complete_local_main =
                    gpu.src_local && !gpu.dst_local;
                tj_gcm2d_perf_call.complete_failed = prefix_rc != 0;
            }
            if (prefix_rc != 0) gpu_exact_fault = 1;
        }
        if (gpu_main_write_gate_token) {
            u64 tj_gcm2d_gate_start = tj_gcm2d_perf ? get_timestamp_ns() : 0;
            int gate_ok = spu_main_write_gate_end(gpu_main_write_gate_token);
            if (tj_gcm2d_perf) {
                tj_gcm2d_perf_call.phase_ns[TJ_GCM2D_PERF_GATE_END] +=
                    get_timestamp_ns() - tj_gcm2d_gate_start;
                tj_gcm2d_perf_call.gate_called = 1;
                tj_gcm2d_perf_call.gate_failed = !gate_ok;
            }
            if (!gate_ok) {
                rsx_d3d12_backend_note_exact_blit_fault();
                gpu_exact_fault = 1;
            }
        }
        if (gpu_exact_fault) {
            return_status = -1;
            goto gcm2d_out;
        }
    } else {
        rsx_d3d12_blit_snapshot_release(gpu.src_snapshot,
                                          gpu.src_snapshot_bytes);
        rsx_d3d12_blit_snapshot_release(gpu.dst_snapshot,
                                          gpu.dst_snapshot_bytes);
        rsx_d3d12_blit_snapshot_release(gpu.dst_post_snapshot,
                                          gpu.dst_post_snapshot_bytes);
        if (gpu_main_write_gate_token) {
            u64 tj_gcm2d_gate_start = tj_gcm2d_perf ? get_timestamp_ns() : 0;
            int gate_ok = spu_main_write_gate_end(gpu_main_write_gate_token);
            if (tj_gcm2d_perf) {
                tj_gcm2d_perf_call.phase_ns[TJ_GCM2D_PERF_GATE_END] +=
                    get_timestamp_ns() - tj_gcm2d_gate_start;
                tj_gcm2d_perf_call.gate_called = 1;
                tj_gcm2d_perf_call.gate_failed = !gate_ok;
            }
            if (!gate_ok) {
                rsx_d3d12_backend_note_exact_blit_fault();
                return_status = -1;
                goto gcm2d_out;
            }
        }
    }
#endif

    static int traced = 0;
    if (traced++ < 16 || result != RSX_SCALED_BLIT_OK) {
        printf("[GCM2D] scaled-image src=%08X dst=%08X dma=%08X->%08X "
               "in=%ux%u/%u out=%ux%u/%u clip=%u,%u %ux%u fmt=%u->%u "
               "step=%08X,%08X surface=%08X op=%u result=%d\n",
               src_ea, dst_ea,
               s_gcm2d.scaled.src_dma, swizzled_destination ? s_gcm2d.swizzled.dma :
                                                       s_gcm2d.dst_dma,
               desc.in_width, desc.in_height, desc.src_pitch,
               desc.out_width, desc.out_height, desc.dst_pitch,
               desc.clip_x, desc.clip_y, desc.clip_width, desc.clip_height,
               desc.src_format, desc.dst_format,
               s_gcm2d.scaled.ds_dx, s_gcm2d.scaled.dt_dy,
               s_gcm2d.scaled.context_surface, s_gcm2d.scaled.operation,
               (int)result);
    }
gcm2d_out:
    if (tj_gcm2d_perf) {
        tj_gcm2d_perf_call.result = (unsigned)result;
        tj_gcm2d_perf_call.return_failed = return_status != 0;
        tj_gcm2d_perf_call.phase_ns[TJ_GCM2D_PERF_TOTAL] =
            get_timestamp_ns() - tj_gcm2d_perf_start;
        tj_gcm2d_perf_record(&tj_gcm2d_perf_call, get_timestamp_ns());
    }
    free(swizzled_staging);
    return return_status;
}

/* Non-3D FIFO subchannel dispatch. Kept separate from rsx_process_method so
 * the NV406E/NV30xx paths retain their existing state handling. */
int cellGcmFifoSubchannelMethod(u32 subch, u32 method, u32 data)
{
    /* Isolated command replay uses the same object bindings as the walkers. */
    if (method == 0) { gcm_fifo_bind_object(subch, data); return 0; }
    /* SCE_DRIVER_INTERRUPT (subchannel 7/local 0x0B00) is intercepted by the
     * FIFO walkers: they commit guest GET before queue publication. */
    if (subch == 7 && method == 0x0B00u) return 0;
    GcmFifoObjectClass object_class = subch < 8
        ? s_gcm_fifo_object_class[subch] : GCM_FIFO_OBJECT_UNKNOWN;
    if (object_class == GCM_FIFO_OBJECT_NV0039) {
        switch (method) {
        case 0x0180: return 0; /* DMA_NOTIFIES */
        case 0x0184: s_gcm2d.m2mf.dma_in      = data; return 0;
        case 0x0188: s_gcm2d.m2mf.dma_out     = data; return 0;
        case 0x030C: s_gcm2d.m2mf.offset_in   = data; return 0;
        case 0x0310: s_gcm2d.m2mf.offset_out  = data; return 0;
        case 0x0314: s_gcm2d.m2mf.pitch_in    = (s32)data; return 0;
        case 0x0318: s_gcm2d.m2mf.pitch_out   = (s32)data; return 0;
        case 0x031C: s_gcm2d.m2mf.line_length = data; return 0;
        case 0x0320: s_gcm2d.m2mf.line_count  = data; return 0;
        case 0x0324: s_gcm2d.m2mf.format      = data; return 0;
        case 0x0328: return gcm_m2mf_buffer_notify(data);
        }
    }
    if (object_class == GCM_FIFO_OBJECT_NV3089) {
        switch (method) {
        case 0x0184: s_gcm2d.scaled.src_dma         = data; return 0;
        case 0x0198: s_gcm2d.scaled.context_surface = data; return 0;
        case 0x02FC: s_gcm2d.scaled.conversion      = data; return 0;
        case 0x0300: s_gcm2d.scaled.src_format      = data; return 0;
        case 0x0304: s_gcm2d.scaled.operation       = data; return 0;
        case 0x0308: s_gcm2d.scaled.clip_point      = data; return 0;
        case 0x030C: s_gcm2d.scaled.clip_size       = data; return 0;
        case 0x0310: s_gcm2d.scaled.out_point       = data; return 0;
        case 0x0314: s_gcm2d.scaled.out_size        = data; return 0;
        case 0x0318: s_gcm2d.scaled.ds_dx           = data; return 0;
        case 0x031C: s_gcm2d.scaled.dt_dy           = data; return 0;
        case 0x0400: s_gcm2d.scaled.in_size         = data; return 0;
        case 0x0404: s_gcm2d.scaled.in_format       = data; return 0;
        case 0x0408: s_gcm2d.scaled.in_offset       = data; return 0;
        case 0x040C: return gcm_scaled_image_in(data);
        }
    }
    if (object_class == GCM_FIFO_OBJECT_NV3062) {
        switch (method) {
        case 0x0184: s_gcm2d.src_dma    = data; return 0;
        case 0x0188: s_gcm2d.dst_dma    = data; return 0;
        case 0x0300: s_gcm2d.color_fmt  = data; return 0;
        case 0x0304: s_gcm2d.pitch      = data; return 0;
        case 0x030C: s_gcm2d.dst_offset = data; return 0;
        }
        return 0;
    }
    if (object_class == GCM_FIFO_OBJECT_NV309E) {
        switch (method) {
        case 0x0180: return 0; /* DMA_NOTIFIES */
        case 0x0184: s_gcm2d.swizzled.dma    = data; return 0;
        case 0x0300: s_gcm2d.swizzled.format = data; return 0;
        case 0x0304: s_gcm2d.swizzled.offset = data; return 0;
        }
        return 0;
    }
    if (object_class == GCM_FIFO_OBJECT_NV308A) {
        switch (method) {
        case 0x0304: s_gcm2d.point    = data; return 0;
        case 0x0308: s_gcm2d.size_out = data; return 0;
        case 0x030C: return 0;              /* SIZE_IN */
        }
        if (method >= 0x0400 && method <= 0x1FFC) {
            /* COLOR data word: write into the destination surface. Inline
             * transfers emit ARGB8 words; NV3062 selects either a 32-bit
             * bit-copy destination or per-word R5G6B5 conversion.
             * DMA 0xFEED0000 = local memory, 0xFEED0001 = main memory. */
            u32 idx = (method - 0x0400) >> 2;
            u32 width = s_gcm2d.size_out & 0xFFFF;
            if (!width || idx >= width)
                return 0;
            u32 px  = (s_gcm2d.point & 0xFFFF) + idx;
            u32 py  = s_gcm2d.point >> 16;
            u32 dst_pitch = s_gcm2d.pitch >> 16;
            u32 dst_format = (u8)s_gcm2d.color_fmt;
            u32 dst_bpp = dst_format == 4u ? 2u :
                          (dst_format == 10u || dst_format == 11u) ? 4u : 0u;
            int local = (s_gcm2d.dst_dma != 0xFEED0001u);
            u32 base;
            extern u32 ppu_vm_size;
            u64 cap = ppu_vm_size ? (u64)ppu_vm_size : (UINT64_C(1) << 32);
            if (!dst_bpp || !dst_pitch || px >= dst_pitch / dst_bpp ||
                !cellGcmResolveLocatedChecked(local, s_gcm2d.dst_offset, &base))
                return 0;
            u64 address = (u64)base + (u64)py * dst_pitch + (u64)px * dst_bpp;
            if ((u64)base > cap || address > cap || dst_bpp > cap - address)
                return 0;
            { static int _it = 0;
              if (_it++ < 6)
                  printf("[GCM2D] inline write dst=0x%08X (off=0x%X dma=0x%X pt=%u,%u pitch=%u fmt=%u) = 0x%08X\n",
                         (u32)address, s_gcm2d.dst_offset,
                         s_gcm2d.dst_dma, px, py, dst_pitch, dst_format, data); }
            if (dst_bpp == 4u) {
                vm_write32((u32)address, data);
            } else {
                u32 r = (data >> 16) & 0xFFu;
                u32 g = (data >> 8) & 0xFFu;
                u32 b = data & 0xFFu;
                u16 packed = (u16)((((r * 32u) / 255u) << 11) |
                                   (((g * 64u) / 255u) << 5) |
                                   ((b * 32u) / 255u));
                vm_write16((u32)address, packed);
            }
            return 0;
        }
        return 0;
    }
    /* Unknown object/method: preserve FIFO progress but retain bounded proof. */
    static int warned = 0;
    if (warned++ < 8)
        printf("[cellGcmSys] FIFO subch %u method 0x%04X (unhandled 2D engine)\n",
               subch, method);
    return 0;
}

/* Present gate for the ticker: a flip is ready once the drain has consumed
 * everything up to put. The guest blocks in its own WaitFlip right after
 * flipping, so the FIFO holds EXACTLY the completed frame -- no guest-side
 * blocking needed (the old spin serialized two vsync-class waits per frame
 * and halved the frame rate). Call AFTER draining. */
int cellGcm_take_flip_pending_synced(void)
{
    gcm_flip_publication_lock();
    if (!s_flip_publication.pending_valid) {
        gcm_flip_publication_unlock();
        return 0;
    }
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    u32 get = vm_read32(GCM_CONTROL_GUEST_ADDR + 4);
    if (get != put) {
        gcm_flip_publication_unlock();
        return 0;   /* frame not fully drained yet */
    }
    s_flip_publication.consumed = s_flip_publication.pending;
    s_flip_publication.consumed_timing = s_flip_publication.pending_timing;
    s_flip_publication.pending_valid = 0;
    gcm_flip_publication_unlock();
    return 1;
}

/* ---- fence (SET_REFERENCE) observability ---------------------------------
 * cellGcmFinish waits for ctrl->ref with EQUALITY. On hardware every fence
 * value persists in the register for the RSX-time until the next fence
 * (~a frame), so an equality poll cannot miss it. Our drain consumes a whole
 * backlog in microseconds; publishing only the final value made intermediate
 * fences invisible -- LBP's boot deadlocked with a thread spinning on a fence
 * the counter had already passed (waiting 0x13 while ref climbed 0x51->0x1F4).
 * Queue every fence as it is drained and publish ONE per tick: the GPU work
 * is never throttled (draw/recycle proceed at full speed), only the ref
 * register advances at most one fence per tick, so every value is observable
 * for >= 16 ms, like hardware. (Throttling the DRAIN at fence boundaries
 * instead starved wrap recycles and wedged the backend -- don't.) */
#define GCM_REF_QLEN 4096
static u32 s_ref_q[GCM_REF_QLEN];
static volatile u32 s_ref_qhead = 0, s_ref_qtail = 0;   /* single producer+consumer: the ticker */

static void gcm_ref_push_at(u32 v, u32 getoff)
{
    { static int _d = -1; if (_d < 0) _d = getenv("GCM_REFLOG") ? 1 : 0;
      if (_d) fprintf(stderr, "[refq] drained fence 0x%X at getoff=%08X\n", v, getoff); }
    u32 t = s_ref_qtail;
    if (t - s_ref_qhead >= GCM_REF_QLEN) {   /* overflow: drop oldest (keeps liveness) */
        s_ref_qhead++;
        static int _o = 0;
        if (_o++ < 4) fprintf(stderr, "[cellGcmSys] fence queue overflow -- oldest dropped\n");
    }
    s_ref_q[t % GCM_REF_QLEN] = v;
    s_ref_qtail = t + 1;
}

/* Global fence-publication counter: lets the lwmutex convoy trace correlate a
 * long lock hold with the number of paced fence publications inside it (the
 * 200us pacing x hundreds of one-ahead fences = the ~156ms holds). */
volatile long long g_gcm_ref_pub_count = 0;
static void gcm_ref_publish_one(void)
{
    u32 h = s_ref_qhead;
    if (h == s_ref_qtail) return;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 8, s_ref_q[h % GCM_REF_QLEN]);
    s_ref_qhead = h + 1;
    g_gcm_ref_pub_count++;
}

/* Read-driven fence publication. cellGcmFinish / FIFO-space waits spin-read the
 * ref register (GCM_CONTROL+8). The 60 Hz present-thread ticker publishes only
 * one queued fence per frame, so when present overruns during heavy boot load
 * the tick rate -- and thus fence publication -- collapses and an equality-
 * waiter stalls seconds per fence (the [finspin] boot crawl; the game is not
 * deadlocked, just starved). Let the spinning reader drive publication too:
 * this is called from vm_read32 on each ref read, advancing one queued fence,
 * globally paced to at most one per millisecond so every value stays visible
 * >= 1 ms to any equality-waiter (no skip-past). Decoupled from present, so a
 * wait makes progress even when the ticker is starved. Idempotent + additive:
 * it only ever publishes fences the ticker would eventually publish anyway. */
/* Serializes the FIFO walker between the 60 Hz present thread and the
 * poll-path below. TryAcquire on the poll path avoids re-entrancy (the walker
 * itself performs guest reads) and never blocks a spinning game thread. */

/* TJ_FIFO_PERF=1: aggregate-only FIFO timing. The wrapper holds the FIFO lock
 * while it updates this state, so reporting needs no extra synchronization. */
#define TJ_FIFO_PERF_GROUPS 1024u
typedef struct TjFifoPerfGroup {
    u64 groups, payloads, ns, max_ns;
    u32 subchannel, object_class, first_method, last_method;
} TjFifoPerfGroup;

typedef struct TjFifoPerf {
    u64 next_report_ns;
    u64 report_start_ms, report_end_ms;
    u64 lock_wait_ns, drain_ns, method_ns, method_max_ns;
    u64 drains, headers, words, controls, payloads, method_calls;
    u64 exit_flip, exit_idle, exit_ea, exit_budget, exit_caught_up, exit_not_initialized;
    u64 payload_ea_failures;
    u32 max_group_getoff, max_group_header_ea, max_group_raw;
    u32 max_group_type, max_group_subchannel, max_group_method, max_group_count;
    u32 max_group_first_method, max_group_last_method;
    TjFifoPerfGroup group[TJ_FIFO_PERF_GROUPS];
    u64 other_groups, other_payloads, other_ns;
} TjFifoPerf;

typedef struct TjFifoPerfCall {
    u64 headers, words, controls, payloads, method_calls;
    u64 method_ns, method_max_ns;
    unsigned exit_reason;
    unsigned payload_ea_failures;
    u32 max_group_getoff, max_group_header_ea, max_group_raw;
    u32 max_group_type, max_group_subchannel, max_group_method, max_group_count;
    u32 max_group_first_method, max_group_last_method;
} TjFifoPerfCall;

enum { TJ_FIFO_EXIT_CAUGHT_UP, TJ_FIFO_EXIT_FLIP, TJ_FIFO_EXIT_IDLE,
       TJ_FIFO_EXIT_EA, TJ_FIFO_EXIT_BUDGET, TJ_FIFO_EXIT_NOT_INITIALIZED };
static TjFifoPerf s_tj_fifo_perf = {0};
static atomic_int s_tj_fifo_perf_enabled = ATOMIC_VAR_INIT(-1);
static _Thread_local int s_tj_fifo_perf_thread_enabled = -1;

static void tj_fifo_perf_group_add(u32 subchannel, u32 object_class,
                                   u32 first, u32 last, u32 payloads, u64 ns)
{
    if (!payloads) return;
    u32 start = ((first >> 2) ^ (last * 17u) ^ (subchannel * 31u) ^
                 object_class) & (TJ_FIFO_PERF_GROUPS - 1u);
    for (u32 probe = 0; probe < TJ_FIFO_PERF_GROUPS; ++probe) {
        TjFifoPerfGroup* group = &s_tj_fifo_perf.group[
            (start + probe) & (TJ_FIFO_PERF_GROUPS - 1u)];
        if (!group->groups) {
            group->subchannel = subchannel;
            group->object_class = object_class;
            group->first_method = first;
            group->last_method = last;
        } else if (group->subchannel != subchannel ||
                   group->object_class != object_class ||
                   group->first_method != first || group->last_method != last) {
            continue;
        }
        ++group->groups;
        group->payloads += payloads;
        group->ns += ns;
        if (ns > group->max_ns) group->max_ns = ns;
        return;
    }
    /* Bounded diagnostics must account for overflow, never drop command cost. */
    ++s_tj_fifo_perf.other_groups;
    s_tj_fifo_perf.other_payloads += payloads;
    s_tj_fifo_perf.other_ns += ns;
}

static u32 tj_fifo_perf_top_group(const TjFifoPerf* perf, const u8* selected)
{
    u32 best = TJ_FIFO_PERF_GROUPS;
    for (u32 i = 0; i < TJ_FIFO_PERF_GROUPS; ++i) {
        if (selected[i] || !perf->group[i].groups) continue;
        if (best == TJ_FIFO_PERF_GROUPS || perf->group[i].ns > perf->group[best].ns ||
            (perf->group[i].ns == perf->group[best].ns &&
             perf->group[i].groups > perf->group[best].groups)) best = i;
    }
    return best;
}

static int tj_fifo_perf_enabled(void)
{
    if (s_tj_fifo_perf_thread_enabled >= 0)
        return s_tj_fifo_perf_thread_enabled;
    int enabled = atomic_load_explicit(&s_tj_fifo_perf_enabled, memory_order_acquire);
    if (enabled >= 0) return s_tj_fifo_perf_thread_enabled = enabled;
    int expected = -1;
    if (atomic_compare_exchange_strong_explicit(&s_tj_fifo_perf_enabled, &expected, -2,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        enabled = getenv("TJ_FIFO_PERF") ? 1 : 0;
        atomic_store_explicit(&s_tj_fifo_perf_enabled, enabled, memory_order_release);
        return s_tj_fifo_perf_thread_enabled = enabled;
    }
    do {
        enabled = atomic_load_explicit(&s_tj_fifo_perf_enabled, memory_order_acquire);
    } while (enabled == -2);
    return s_tj_fifo_perf_thread_enabled = enabled;
}

static int tj_fifo_perf_add(const TjFifoPerfCall* call, u64 lock_wait_ns,
                            u64 drain_ns, TjFifoPerf* snapshot)
{
    TjFifoPerf* perf = &s_tj_fifo_perf;
    perf->drains++;
    perf->lock_wait_ns += lock_wait_ns;
    perf->drain_ns += drain_ns;
    perf->headers += call->headers;
    perf->words += call->words;
    perf->controls += call->controls;
    perf->payloads += call->payloads;
    perf->method_calls += call->method_calls;
    perf->payload_ea_failures += call->payload_ea_failures;
    perf->method_ns += call->method_ns;
    if (call->method_max_ns > perf->method_max_ns) {
        perf->method_max_ns = call->method_max_ns;
        perf->max_group_getoff = call->max_group_getoff;
        perf->max_group_header_ea = call->max_group_header_ea;
        perf->max_group_raw = call->max_group_raw;
        perf->max_group_type = call->max_group_type;
        perf->max_group_subchannel = call->max_group_subchannel;
        perf->max_group_method = call->max_group_method;
        perf->max_group_count = call->max_group_count;
        perf->max_group_first_method = call->max_group_first_method;
        perf->max_group_last_method = call->max_group_last_method;
    }
    if (call->exit_reason == TJ_FIFO_EXIT_FLIP) perf->exit_flip++;
    else if (call->exit_reason == TJ_FIFO_EXIT_IDLE) perf->exit_idle++;
    else if (call->exit_reason == TJ_FIFO_EXIT_EA) perf->exit_ea++;
    else if (call->exit_reason == TJ_FIFO_EXIT_BUDGET) perf->exit_budget++;
    else if (call->exit_reason == TJ_FIFO_EXIT_NOT_INITIALIZED) perf->exit_not_initialized++;
    else perf->exit_caught_up++;

    u64 now = get_timestamp_ns();
    if (!perf->next_report_ns) {
        perf->next_report_ns = now + 1000000000ULL;
        perf->report_start_ms = ps3_ms_now();
    }
    if (now < perf->next_report_ns) return 0;
    perf->report_end_ms = ps3_ms_now();
    *snapshot = *perf;
    memset(perf, 0, sizeof(*perf));
    perf->next_report_ns = now + 1000000000ULL;
    perf->report_start_ms = snapshot->report_end_ms;
    return 1;
}

static void tj_fifo_perf_log(const TjFifoPerf* perf)
{
    fprintf(stderr,
            "[TJ-FIFO-PERF] start_ms=%llu end_ms=%llu drains=%llu lock-wait=%lluus drain=%lluus "
            "headers=%llu words=%llu controls=%llu payloads=%llu "
            "methods={calls=%llu groups-total=%lluus max-group=%lluus} "
            "max-group={getoff=0x%08X hea=0x%08X raw=0x%08X type=%u subch=%u method=0x%04X count=%u first=0x%04X last=0x%04X} "
            "exit={flip=%llu idle=%llu ea=%llu payload-ea=%llu budget=%llu caught-up=%llu not-initialized=%llu}\n",
            (unsigned long long)perf->report_start_ms,
            (unsigned long long)perf->report_end_ms,
            (unsigned long long)perf->drains,
            (unsigned long long)(perf->lock_wait_ns / 1000ULL),
            (unsigned long long)(perf->drain_ns / 1000ULL),
            (unsigned long long)perf->headers, (unsigned long long)perf->words,
            (unsigned long long)perf->controls, (unsigned long long)perf->payloads,
            (unsigned long long)perf->method_calls,
            (unsigned long long)(perf->method_ns / 1000ULL),
            (unsigned long long)(perf->method_max_ns / 1000ULL),
            perf->max_group_getoff, perf->max_group_header_ea,
            perf->max_group_raw, perf->max_group_type, perf->max_group_subchannel,
            perf->max_group_method, perf->max_group_count,
            perf->max_group_first_method, perf->max_group_last_method,
            (unsigned long long)perf->exit_flip, (unsigned long long)perf->exit_idle,
            (unsigned long long)perf->exit_ea,
            (unsigned long long)perf->payload_ea_failures,
            (unsigned long long)perf->exit_budget,
            (unsigned long long)perf->exit_caught_up,
            (unsigned long long)perf->exit_not_initialized);
    u8 selected[TJ_FIFO_PERF_GROUPS] = {0};
    for (u32 rank = 1; rank <= 6; ++rank) {
        u32 index = tj_fifo_perf_top_group(perf, selected);
        if (index == TJ_FIFO_PERF_GROUPS) break;
        selected[index] = 1;
        const TjFifoPerfGroup* group = &perf->group[index];
        fprintf(stderr,
                "[TJ-FIFO-GROUP-PERF] rank=%u subch=%u object_kind=%u "
                "first=0x%04X last=0x%04X groups=%llu payloads=%llu total_us=%llu max_us=%llu\n",
                rank, group->subchannel, group->object_class,
                group->first_method, group->last_method,
                (unsigned long long)group->groups, (unsigned long long)group->payloads,
                (unsigned long long)(group->ns / 1000ull),
                (unsigned long long)(group->max_ns / 1000ull));
    }
    if (perf->other_groups)
        fprintf(stderr, "[TJ-FIFO-GROUP-PERF] overflow groups=%llu payloads=%llu total_us=%llu\n",
                (unsigned long long)perf->other_groups,
                (unsigned long long)perf->other_payloads,
                (unsigned long long)(perf->other_ns / 1000ull));
    fflush(stderr);
}

static void gcm_rsx_process_fifo_unlocked_fast(void);
static void gcm_rsx_process_fifo_unlocked_profiled(TjFifoPerfCall* perf_call);

/* Kick event: a game thread whose fence wait finds the queue DRY signals the
 * present thread to run the FIFO walker NOW instead of on its next 16 ms
 * tick. A movie frame issues ~46 one-ahead cellGcmFinish waits; paying a
 * 16 ms tick per fence was ~0.75 s/frame (the 2 FPS intro). The walker (and
 * the D3D12 backend it feeds) stays on the present thread -- running it from
 * the polling game thread killed the process silently. */
static HANDLE s_gcm_kick_ev = NULL;
static _Atomic(void*) s_gcm_flip_kick_ev = ATOMIC_VAR_INIT(NULL);
void* cellGcm_fifo_kick_event(void)
{
    if (!s_gcm_kick_ev) s_gcm_kick_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    const char* flip_kick_setting = getenv("TJ_FIFO_KICK_WAIT");
    atomic_store_explicit(&s_gcm_flip_kick_ev,
        (flip_kick_setting && strcmp(flip_kick_setting, "1") == 0)
            ? (void*)s_gcm_kick_ev : NULL,
        memory_order_release);
    return (void*)s_gcm_kick_ev;
}

/* A pending guest flip is ready for the presenter to inspect. Signal only
 * after gcm_flip_publish has released its publication lock. */
static void gcm_flip_kick_presenter(void)
{
    HANDLE event = (HANDLE)atomic_load_explicit(&s_gcm_flip_kick_ev,
                                                memory_order_acquire);
    if (event) (void)SetEvent(event);
}

/* TJ_FLIP_WAIT_EVENT: a recycler about to block on the walker asks the
 * presenter to walk now rather than after its <= 1 ms nap. Same dry-queue
 * event cellGcm_ref_on_poll sets; NULL unless TJ_FIFO_KICK_WAIT=1. */
static void gcm_fifo_kick_presenter_for_wait(void)
{
    if (s_gcm_kick_ev) (void)SetEvent(s_gcm_kick_ev);
}

/* Serializes fence publication between the present ticker and every guest
 * thread poll-reading the ref register (REFPOLL default-on made this path
 * multi-threaded; the SPSC queue head raced and publication order broke --
 * boot froze with all threads parked on fences that were never published in
 * order). TryAcquire: a contended poll just reads the current ref. */
static SRWLOCK s_ref_pub_lock = SRWLOCK_INIT;

void cellGcm_ref_on_poll(void)
{
    /* Pace: leave each published value observable for >= ~200us of spins
     * (equality-waiters at any poll rate see every value; still ~5000/s so
     * loading's fence storms clear in seconds, not minutes). */
    static LARGE_INTEGER s_freq;
    static volatile LONGLONG s_last_pub;
    if (!TryAcquireSRWLockExclusive(&s_ref_pub_lock))
        return;
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG min_gap = s_freq.QuadPart / 5000;          /* ~200us */
    int dry = 0;
    if (now.QuadPart - s_last_pub >= min_gap) {
        gcm_ref_publish_one();
        s_last_pub = now.QuadPart;
        dry = (s_ref_qhead == s_ref_qtail);
        /* Tokyo Jungle: after the title's boot fences are all published its
         * per-frame pump (func_001F2D88) keeps polling ref for progress. With
         * an empty queue ref froze and the pump spent ~224 tick spins a frame.
         * libgcm's ref is "how far the RSX has consumed", so with nothing
         * queued publish the FIFO's own progress. A queued fence still wins. */
        if (dry)
            vm_write32(GCM_CONTROL_GUEST_ADDR + 8,
                       vm_read32(GCM_CONTROL_GUEST_ADDR + 0));
    }
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
    if (dry && s_gcm_kick_ev)
        SetEvent(s_gcm_kick_ev);       /* dry: ask the present thread to walk */
}

/* Keep the large diagnostic snapshot out of the normal dispatcher's compiled
 * stack frame as well as its source path. Clang otherwise inlines this helper. */
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void gcm_rsx_process_fifo_profiled(void)
{
    u64 wait_begin = get_timestamp_ns();
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    u64 drain_begin = get_timestamp_ns();
    TjFifoPerfCall call = {};
    gcm_rsx_process_fifo_unlocked_profiled(&call);
    rsx_d3d12_backend_flush_texture_labels();
    u64 drain_end = get_timestamp_ns();
    /* The full table is copied only when a report is ready. Keep this stack
     * reservation out of the default path and do not zero it on every drain. */
    TjFifoPerf snapshot;
    int report = tj_fifo_perf_add(&call, drain_begin - wait_begin,
                                                   drain_end - drain_begin, &snapshot);
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
    gcm_wait_signal(GCM_WAIT_CH_FIFO);     /* walker state now readable */
    if (report) tj_fifo_perf_log(&snapshot);
}

void cellGcm_rsx_process_fifo(void)
{
    if (tj_fifo_perf_enabled()) {
        gcm_rsx_process_fifo_profiled();
        return;
    }
    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    gcm_rsx_process_fifo_unlocked_fast();
    rsx_d3d12_backend_flush_texture_labels();
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
    gcm_wait_signal(GCM_WAIT_CH_FIFO);     /* walker state now readable */
}

/* Default path deliberately has no profiler pointer branches or QPC calls. */
/* Opt-in, bounded command/state correlation. Called under the FIFO lock before
 * dispatch, so a DEPTH_MASK line records both the old state and incoming value. */
static void tj_depth_pass_trace(const rsx_state* state, u32 method, u32 data,
                                u32 header_ea, u32 payload_ea)
{
    static int enabled = -1;
    static unsigned lines;
    if (enabled < 0) enabled = getenv("TJ_DEPTH_PASS_TRACE") != NULL;
    if (!enabled || lines >= 1024) return;
    if (method != NV4097_SET_DEPTH_MASK && method != NV4097_SET_DEPTH_FUNC &&
        method != NV4097_SET_DEPTH_TEST_ENABLE && method != NV4097_CLEAR_SURFACE &&
        method != 0x50u && !(method == NV4097_SET_BEGIN_END && data)) return;
#ifdef _WIN32
    /* Survival IO starts long before the world exists. Match the indexed
     * capture's image18-return gate so loading clears cannot consume the cap. */
    extern volatile LONG g_tj_geometry_trace_armed;
    if (InterlockedCompareExchange(&g_tj_geometry_trace_armed, 0, 0) < 2) return;
#endif
    if (!tj_geom_fifo_trace_arm_is_published() ||
        !atomic_load_explicit(&s_tj_survival_profile_armed, memory_order_acquire)) return;
    unsigned color = state->color_target == 2 ? 1 : 0;
    fprintf(stderr,
        "[TJ-DEPTH-PASS] n=%u header=%08X payload=%08X method=%04X data=%08X "
        "before={test=%d write=%d func=%04X} rt=%08X target=%X zeta=%08X pitch=%u fp=%08X cmask=%08X\n",
        ++lines, header_ea, payload_ea, method, data,
        state->depth_test_enable, state->depth_mask, state->depth_func,
        state->surface_color_offset[color], state->color_target,
        state->surface_zeta_offset, state->surface_zeta_pitch, state->fragment_program_addr,
        state->color_mask);
    if (method == NV4097_SET_BEGIN_END && data &&
        state->fragment_program_addr == 0x07608D40u) {
        fprintf(stderr,
            "[TJ-DEPTH-SKY] c250=(%g,%g,%g,%g) scale=(%g,%g,%g,%g) "
            "offset=(%g,%g,%g,%g) clip=(%g,%g)\n",
            state->vertex_constants[250][0], state->vertex_constants[250][1],
            state->vertex_constants[250][2], state->vertex_constants[250][3],
            state->viewport_scale[0], state->viewport_scale[1],
            state->viewport_scale[2], state->viewport_scale[3],
            state->viewport_offset[0], state->viewport_offset[1],
            state->viewport_offset[2], state->viewport_offset[3],
            state->clip_min, state->clip_max);
    }
}

/* These exact dispatcher cases only update host command state (or are no-ops).
 * They neither observe guest bytes nor execute a copy. Deferred texture labels
 * may cross them, but must retire before the next observer or drain boundary.
 * Unknown handles/methods/classes retain the conservative completion guard. */
static int gcm_fifo_method_is_metadata(u32 subch, u32 method, u32 data)
{
    if (subch >= 8) return 0;
    if (method == 0)
        return gcm_fifo_object_class_from_handle(data) != GCM_FIFO_OBJECT_UNKNOWN;
    /* An unknown bind keeps the compatibility class, but cannot authorize
     * metadata batching. Zero is also the unbound default handle sentinel. */
    if (!s_gcm_fifo_object_trusted[subch]) return 0;
    switch (s_gcm_fifo_object_class[subch]) {
    case GCM_FIFO_OBJECT_NV0039:
        switch (method) {
        case 0x0180: case 0x0184: case 0x0188: case 0x030C: case 0x0310:
        case 0x0314: case 0x0318: case 0x031C: case 0x0320: case 0x0324:
            return 1;
        }
        break;
    case GCM_FIFO_OBJECT_NV3089:
        switch (method) {
        case 0x0184: case 0x0198: case 0x02FC: case 0x0300: case 0x0304:
        case 0x0308: case 0x030C: case 0x0310: case 0x0314: case 0x0318:
        case 0x031C: case 0x0400: case 0x0404: case 0x0408:
            return 1;
        }
        break;
    case GCM_FIFO_OBJECT_NV3062:
        switch (method) {
        case 0x0184: case 0x0188: case 0x0300: case 0x0304: case 0x030C:
            return 1;
        }
        break;
    case GCM_FIFO_OBJECT_NV309E:
        switch (method) {
        case 0x0180: case 0x0184: case 0x0300: case 0x0304:
            return 1;
        }
        break;
    case GCM_FIFO_OBJECT_NV308A:
        switch (method) {
        case 0x0304: case 0x0308: case 0x030C:
            return 1;
        }
        break;
    default: break;
    }
    return 0;
}

/* Opt-in TJ_NON3D_LABEL_PASS=1: write-only CPU stores into LOCAL (NV308A
 * inline COLOR words) and the no-op NV3062 0x0308 may cross pending texture
 * labels. Batched label records read only CPU snapshots, so these stores
 * cannot change replayed input; labels still retire at the next real
 * observer, acquire or drain. MAIN destinations, copies, notifies,
 * interrupts and unknown/untrusted methods keep the flush. */
static int gcm_fifo_non3d_label_pass(u32 subch, u32 method)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* v = getenv("TJ_NON3D_LABEL_PASS");
        enabled = v && v[0] == '1' && v[1] == '\0';
        if (enabled) fprintf(stderr, "[tj-non3d-pass] enabled\n");
    }
    if (!enabled || subch >= 7 || !s_gcm_fifo_object_trusted[subch]) return 0;
    if (s_gcm_fifo_object_class[subch] == GCM_FIFO_OBJECT_NV3062)
        return method == 0x0308u;
    return s_gcm_fifo_object_class[subch] == GCM_FIFO_OBJECT_NV308A &&
           method >= 0x0400u && method <= 0x1FFCu &&
           s_gcm2d.dst_dma == RSX_CONTEXT_DMA_LOCAL;
}

/* Diagnostic: per-class counts of generic non-3D label flushes and passes,
 * one [TJ-NON3D-FLUSH] line per second while TJ_FPS_TRACE and TJ_VERBOSE_STATS are set. */
enum { NON3D_K_308A_LOCAL, NON3D_K_308A_MAIN, NON3D_K_3089, NON3D_K_0039,
       NON3D_K_3062, NON3D_K_309E, NON3D_K_SUBCH7, NON3D_K_METHOD0,
       NON3D_K_OTHER, NON3D_K_PASS, NON3D_K_COUNT };
static void gcm_fifo_non3d_count(u32 subch, u32 method, int passed)
{
    static int enabled = -1;
    static u64 counts[NON3D_K_COUNT];
    static ULONGLONG window_start;
    if (enabled < 0) enabled = getenv("TJ_FPS_TRACE") != NULL && getenv("TJ_VERBOSE_STATS") != NULL;
    if (!enabled) return;
    int k = NON3D_K_OTHER;
    if (passed) k = NON3D_K_PASS;
    else if (method == 0) k = NON3D_K_METHOD0;
    else if (subch >= 7) k = NON3D_K_SUBCH7;
    else switch (s_gcm_fifo_object_class[subch]) {
        case GCM_FIFO_OBJECT_NV308A:
            k = s_gcm2d.dst_dma == 0xFEED0001u ? NON3D_K_308A_MAIN : NON3D_K_308A_LOCAL; break;
        case GCM_FIFO_OBJECT_NV3089: k = NON3D_K_3089; break;
        case GCM_FIFO_OBJECT_NV0039: k = NON3D_K_0039; break;
        case GCM_FIFO_OBJECT_NV3062: k = NON3D_K_3062; break;
        case GCM_FIFO_OBJECT_NV309E: k = NON3D_K_309E; break;
        default: break;
    }
    counts[k]++;
    ULONGLONG now = GetTickCount64();
    if (!window_start) window_start = now;
    if (now - window_start >= 1000) {
        fprintf(stderr, "[TJ-NON3D-FLUSH] window_ms=%llu nv308a_local=%llu nv308a_main=%llu "
                "nv3089=%llu nv0039=%llu nv3062=%llu nv309e=%llu subch7=%llu method0=%llu "
                "other=%llu passed=%llu\n",
                (unsigned long long)(now - window_start),
                (unsigned long long)counts[NON3D_K_308A_LOCAL], (unsigned long long)counts[NON3D_K_308A_MAIN],
                (unsigned long long)counts[NON3D_K_3089], (unsigned long long)counts[NON3D_K_0039],
                (unsigned long long)counts[NON3D_K_3062], (unsigned long long)counts[NON3D_K_309E],
                (unsigned long long)counts[NON3D_K_SUBCH7], (unsigned long long)counts[NON3D_K_METHOD0],
                (unsigned long long)counts[NON3D_K_OTHER], (unsigned long long)counts[NON3D_K_PASS]);
        memset(counts, 0, sizeof(counts));
        window_start = now;
    }
}

/* 0: other method, 1: consumed, 2: waiting, -1: invalid/faulted.
 * A wait parks the walker at this payload, not in a spin while holding the
 * FIFO lock. Retained packet/CALL state lets the next drain resume exactly.
 * NV406E release values are NOT the byte-swizzled NV4097 backend values. */
static int gcm_fifo_semaphore_method(rsx_state* state, u32 subch, u32 method, u32 data)
{
    /* The generic non-NV4097 boundary can flush before semaphore dispatch.
     * Name its actual observation here without changing either flush point. */
    const RsxD3D12CompletionReason completion_reason =
        subch == 0 && method == 0x68u ? RSX_COMPLETION_ACQUIRE :
        subch == 0 && method == 0x6Cu ? RSX_COMPLETION_BACKEND_RELEASE :
        RSX_COMPLETION_OTHER;
    /* Other engines' observers can execute CPU copies or touch guest memory.
     * Exact metadata setters/known binds preserve FIFO order without forcing
     * an otherwise eligible batch to finish. Every unknown remains guarded. */
    if ((method == 0 || subch >= 8 ||
         s_gcm_fifo_object_class[subch] != GCM_FIFO_OBJECT_NV4097) &&
        !gcm_fifo_method_is_metadata(subch, method, data)) {
        if (completion_reason == RSX_COMPLETION_OTHER &&
            gcm_fifo_non3d_label_pass(subch, method)) {
            /* A pending merged draw must not read the post-store bytes. */
            rsx_flush_pending_draws(state);
            gcm_fifo_non3d_count(subch, method, 1);
        } else {
            if (completion_reason == RSX_COMPLETION_OTHER)
                gcm_fifo_non3d_count(subch, method, 0);
            /* Attribution only: an OTHER flush here is the generic non-3D edge. */
            if (completion_reason == RSX_COMPLETION_OTHER)
                rsx_d3d12_backend_note_prefix_trigger(RSX_PREFIX_TRIGGER_NON3D);
            const int flush_rc = rsx_d3d12_backend_flush_texture_labels_at(completion_reason);
            rsx_d3d12_backend_note_prefix_trigger(RSX_COMPLETION_OTHER);
            if (flush_rc != 0) return -1;
        }
    }
    if ((method == 0x1D70u || method == 0x1D74u) && subch < 8 &&
        s_gcm_fifo_object_class[subch] == GCM_FIFO_OBJECT_NV4097) {
        /* Both releases retain full GPU completion before publication. The
         * optional texture-label path only delays publication to batch work;
         * it never lets the guest recycle memory before the fence. */
        rsx_flush_pending_draws(state);
        if (method == 0x1D74u) {
            rsx_d3d12_backend_trace_texture_read_prefix();
            return rsx_d3d12_backend_prepare_texture_label() == 0 ? 0 : -1;
        }
        return rsx_d3d12_backend_complete_fifo_prefix_at(RSX_COMPLETION_BACKEND_RELEASE) == 0 ? 0 : -1;
    }
    if (subch != 0 || method < 0x60u || method > 0x6Cu) return 0;
    if (method == 0x60u) { s_fifo_semaphore_dma = data; return 1; }
    if (method == 0x64u) { s_fifo_semaphore_offset = data; return 1; }
    if (method != 0x68u && method != 0x6Cu) return 0;
    if ((s_fifo_semaphore_dma != 0x66606660u &&
         s_fifo_semaphore_dma != 0x66616661u) ||
        (s_fifo_semaphore_offset & 3u) ||
        s_fifo_semaphore_offset > CELL_GCM_MAX_LABEL_COUNT * GCM_LABEL_STRIDE - 4u) {
        static unsigned reports;
        if (reports++ < 8u)
            fprintf(stderr, "[GCM-SEMAPHORE] invalid dma=%08X offset=%08X method=%04X get=%08X\n",
                    s_fifo_semaphore_dma, s_fifo_semaphore_offset, method, s_fifo_getoff);
        return -1;
    }
    const u32 label_ea = GCM_LABEL_GUEST_BASE + s_fifo_semaphore_offset;
    rsx_flush_pending_draws(state);
    /* Retire before the initial comparison, even when the old value matches:
     * a preceding deferred release may change that same label. Opt-in mode 3
     * skips this only for an acquire with no texture label pending anywhere. */
    if (!(method == 0x68u && rsx_d3d12_backend_acquire_gpu_free()) &&
        rsx_d3d12_backend_flush_texture_labels_at(completion_reason) != 0) return -1;
    if (method == 0x68u) {
        if (vm_read32(label_ea) != data) {
            /* GPU work before the wait can itself be the label producer. */
            if (rsx_d3d12_backend_complete_fifo_prefix_at(RSX_COMPLETION_ACQUIRE) != 0) return -1;
            const u32 actual = vm_read32(label_ea);
            if (actual != data) {
                const u64 now = get_timestamp_ns();
                if (!s_fifo_semaphore_wait_since_ns) s_fifo_semaphore_wait_since_ns = now;
                if (s_fifo_semaphore_wait_since_ns != UINT64_MAX &&
                    now - s_fifo_semaphore_wait_since_ns >= 1000000000ULL) {
                    fprintf(stderr, "[GCM-SEMAPHORE] waiting dma=%08X offset=%08X expected=%08X actual=%08X get=%08X call=%08X\n",
                            s_fifo_semaphore_dma, s_fifo_semaphore_offset, data,
                            actual, s_fifo_getoff, s_fifo_calloff);
                    s_fifo_semaphore_wait_since_ns = UINT64_MAX;
                }
                return 2;
            }
        }
        s_fifo_semaphore_wait_since_ns = 0;
    } else if (vm_read32(label_ea) != data) {
        /* Label 1 is libgcm's reserved flip semaphore. Like the reference
         * frontend it must remain writable without a full pipeline sync. */
        if (s_fifo_semaphore_offset != GCM_LABEL_STRIDE &&
            rsx_d3d12_backend_complete_fifo_prefix_at(RSX_COMPLETION_BACKEND_RELEASE) != 0) return -1;
        vm_write32(label_ea, data);
    }
    return 1;
}

/* Walker debug switches are checked per drain and per CALL/RET/idle exit.
 * The launch environment is fixed, so each is read once (-1 = unread). */
static int gcm_rate_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("GCM_RATE") ? 1 : 0;
    return enabled;
}

static int gcm_drain_debug_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("GCM_DRAINDBG") ? 1 : 0;
    return enabled;
}

static int gcm_recdbg_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("GCM_RECDBG") ? 1 : 0;
    return enabled;
}

static void gcm_rsx_process_fifo_unlocked_fast(void)
{
    if (rsx_d3d12_backend_fifo_faulted()) return;
    { static unsigned n = 0; static unsigned long long t0 = 0;
      extern unsigned long long ps3_ms_now(void); n++;
      if (gcm_rate_trace_enabled()) { unsigned long long now = ps3_ms_now();
          if (!t0) t0 = now;
          if (now - t0 >= 2000) { fprintf(stderr, "[RATE] drains/sec=%.0f\n", n * 1000.0 / (now - t0)); n = 0; t0 = now; }
      } }
    static rsx_state state;
    static int inited = 0;
    static int depth_pass_trace = 0;
    extern u32 g_rsx_last_reference;
    if (!s_gcm_context_ea) return;
    if (!inited) { rsx_state_init(&state); depth_pass_trace = getenv("TJ_DEPTH_PASS_TRACE") != NULL; inited = 1; }
    /* Tokyo Jungle lays its command ring at io 0x01000000: the first word of
     * the region cellGcmSetupContext reserved is an idle JUMP to that base,
     * and the control block's put/get are offsets FROM that base. Learn the
     * base from that first jump so `put` becomes comparable with the absolute
     * io offsets the walker carries. */
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0) + s_fifo_io_base;
    if (gcm_drain_debug_enabled()) { static int n = 0; if (n++ % 60 == 0)
        printf("[DRAIN] getoff=%08X put=%08X ref=%08X ctx.current=%08X\n", s_fifo_getoff, put,
               vm_read32(GCM_CONTROL_GUEST_ADDR + 8), vm_read32(s_gcm_context_ea + 0x8)); }
    int budget = 0x100000;
    GcmIoMemo io_memo = { 0, 0, 0xFFFF };
    while (s_fifo_getoff != put && budget-- > 0) {
        if (!s_fifo_packet.remaining)
            rsx_d3d12_backend_pulse_fence_wait_service();
        int resume = s_fifo_packet.remaining != 0;
        u32 ea = resume ? s_fifo_packet.header_ea : gcm_io2ea_memo(&io_memo, s_fifo_getoff);
        if (!ea) break;
        u32 w = resume ? s_fifo_packet.raw : vm_read32(ea), type = w >> 29;
        if (!resume) {
          tj_geom_fifo_trace_outcome(s_fifo_getoff, ea, w);
          tj_geom_fifo_trace_relevant_header(s_fifo_getoff, ea, w);
        { static int side = -1; static unsigned side_count = 0;
          if (side < 0) side = getenv("GCM_SIDE_TRACE") ? 1 : 0;
          if (side && s_fifo_calloff && side_count < 512) { u32 count=(w>>18)&0x7FF, method=w&0x1FFCu, subch=(w>>13)&7; side_count++;
              fprintf(stderr, "[GCM-SIDE] getoff=0x%08X raw=0x%08X type=%u count=%u method=0x%04X subch=%u\n", s_fifo_getoff,w,type,count,method,subch); }
        }
        }
        if ((w & 0xFFFF0000u) == 0xFEAD0000u) {
            gcm_fifo_context_reset(&state);
            s_fifo_getoff += 4;
            rsx_d3d12_backend_snapshot_late_resolve(); /* TJ_SNAPSHOT_LATE_PROOF: before the flip request */
            cellGcmSetFlipCommand_source(w & 0xFFu, FRAME_PROF_FIFO_DISPATCH);
            break;
        }
        if (type == 1) {
            u32 target = w & 0x1FFFFFFCu;
            if (!s_fifo_io_base && s_fifo_getoff == 0 && target) {
                s_fifo_io_base = target;
                if (gcm_recdbg_enabled())
                    fprintf(stdout, "[FIFOBASE] ring io base = %08X\n", target);
            }
            { static int rd = -1; if (rd < 0) rd = getenv("GCM_RECDBG") ? 1 : 0; if (rd) fprintf(stdout,"[JMP] %08X -> %08X (put=%08X)\n",s_fifo_getoff,target,put); }
            if (target == s_fifo_getoff) {
                if (gcm_recdbg_enabled()) fprintf(stdout,"[IDLE] at=%08X calloff=%08X put=%08X\n",s_fifo_getoff,s_fifo_calloff,put);
                /* Park without consuming the saved CALL return. Advancing PUT
                 * does not prove that a secondary producer finished its work. */
                break;
            }
            s_fifo_getoff = target; continue;
        }
        if ((w & 3) == 2) { u32 target = w & 0x1FFFFFFCu;
            tj_geom_fifo_trace_call(target, s_fifo_getoff + 4, put);
            s_fifo_calloff=s_fifo_getoff+4; s_fifo_getoff=target;
            if (gcm_recdbg_enabled()) fprintf(stdout,"[CALL] target=%08X return=%08X put=%08X\n",s_fifo_getoff,s_fifo_calloff,put); continue; }
        if (w == 0x00020000u) { if (gcm_recdbg_enabled()) fprintf(stdout,"[RET] to=%08X put=%08X\n",s_fifo_calloff,put); s_fifo_getoff=s_fifo_calloff; s_fifo_calloff=0; continue; }
        if (type == 0 || type == 2) {
            u32 count=(w>>18)&0x7FF, method=w&0x1FFCu, subch=(w>>13)&7;
            if (!resume) {
                s_fifo_packet.raw = w;
                s_fifo_packet.header_ea = ea;
                s_fifo_packet.header_get = s_fifo_getoff;
                s_fifo_packet.next_index = 0;
                s_fifo_packet.remaining = count;
                s_fifo_getoff += 4;
            }
            while (s_fifo_packet.remaining && s_fifo_getoff != put) {
                u32 dea = gcm_io2ea_memo(&io_memo, s_fifo_getoff);
                if (!dea) break;
                u32 i = s_fifo_packet.next_index;
                u32 m = (type == 0) ? method + i * 4 : method;
                u32 data = vm_read32(dea), edge_get = s_fifo_getoff + 4;
                const int is_nv4097 = subch < 8 && s_gcm_fifo_object_class[subch] == GCM_FIFO_OBJECT_NV4097;
                int semaphore = (is_nv4097 && m != 0u && m != 0x1D70u && m != 0x1D74u &&
                                 (subch != 0 || m < 0x60u || m > 0x6Cu))
                    ? 0 : gcm_fifo_semaphore_method(&state, subch, m, data);
                if (semaphore < 0 || semaphore == 2) break;
                if (semaphore == 1) { /* Consumed by the FIFO frontend. */ }
                else if (subch == 7 && m == 0x0B00u) {
                    /* This is a FIFO completion edge, not a deferred group
                     * annotation.  MAIN writeback must be guest-visible
                     * before GET makes the handler runnable. */
                    rsx_flush_pending_draws(&state);
                    rsx_d3d12_backend_note_prefix_trigger(RSX_PREFIX_TRIGGER_INTERRUPT);
                    const int prefix_rc = rsx_d3d12_backend_complete_fifo_prefix();
                    rsx_d3d12_backend_note_prefix_trigger(RSX_COMPLETION_OTHER);
                    if (prefix_rc != 0) break;
                    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, edge_get);
                    cellGcmQueueUserInterrupt(data, edge_get);
                }
                else if (m == 0x0000u) { gcm_fifo_bind_object(subch, data); }
                else if (is_nv4097) { if (depth_pass_trace) tj_depth_pass_trace(&state,m,data,ea,dea); rsx_process_method(&state,m,data); if (m==0x50) {
                    rsx_d3d12_backend_note_prefix_trigger(RSX_PREFIX_TRIGGER_REFERENCE);
                    const int prefix_rc = rsx_d3d12_backend_complete_fifo_prefix();
                    rsx_d3d12_backend_note_prefix_trigger(RSX_COMPLETION_OTHER);
                    if (prefix_rc != 0) break;
                    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, edge_get);
                    gcm_ref_push_at(g_rsx_last_reference, edge_get);
                } }
                else if (cellGcmFifoSubchannelMethod(subch,m,data) != 0) return;
                s_fifo_getoff = edge_get;
                s_fifo_packet.next_index++;
                s_fifo_packet.remaining--;
            }
            if (s_fifo_packet.remaining) break;
            continue;
        }
        { static int uw = 0; if (uw++ < 16 && getenv("RTT_DUMP")) fprintf(stderr,"[FIFOUW] unknown word 0x%08X at getoff 0x%X\n",w,s_fifo_getoff); }
        s_fifo_getoff += 4;
    }
    rsx_d3d12_backend_snapshot_late_resolve(); /* TJ_SNAPSHOT_LATE_PROOF: before drained EA/GET */
    g_gcm_fifo_drained_ea = gcm_io2ea(s_fifo_getoff);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, s_fifo_getoff);
    AcquireSRWLockExclusive(&s_ref_pub_lock); gcm_ref_publish_one(); ReleaseSRWLockExclusive(&s_ref_pub_lock);
}

static void gcm_rsx_process_fifo_unlocked_profiled(TjFifoPerfCall* perf_call)
{
    if (rsx_d3d12_backend_fifo_faulted()) return;
    { static unsigned _n = 0; static unsigned long long _t0 = 0;
      extern unsigned long long ps3_ms_now(void);
      _n++;
      if (gcm_rate_trace_enabled()) {
          unsigned long long now = ps3_ms_now();
          if (_t0 == 0) _t0 = now;
          if (now - _t0 >= 2000) {
              fprintf(stderr, "[RATE] drains/sec=%.0f\n", _n * 1000.0 / (now - _t0));
              _n = 0; _t0 = now;
          }
      } }
    static rsx_state s_state;
    static int  s_inited = 0;
    extern u32 g_rsx_last_reference;

    if (!s_gcm_context_ea) {
        if (perf_call) perf_call->exit_reason = TJ_FIFO_EXIT_NOT_INITIALIZED;
        return;
    }
    if (!s_inited) { rsx_state_init(&s_state); s_inited = 1; }

    /* Walk the FIFO exactly like the RSX: chase the guest-written `put` (an IO
     * offset in the control register), decoding method headers and following
     * JUMP/CALL/RET control words. The old linear drain copied context->current
     * onward and treated a JUMP as end-of-buffer -- fine for titles that write
     * one flat ring (cellmark), but PSL1GHT/Tiny3D immediately JUMPs into its
     * own command ring, so everything after the jump (including the reference
     * writes its waits spin on) silently never executed. */
    u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);

    if (gcm_drain_debug_enabled()) {
        static int n = 0;
        if (n++ % 60 == 0)
            printf("[DRAIN] getoff=%08X put=%08X ref=%08X ctx.current=%08X\n",
                   s_fifo_getoff, put, vm_read32(GCM_CONTROL_GUEST_ADDR + 8),
                   vm_read32(s_gcm_context_ea + 0x8));
    }

    int budget = 0x100000;                    /* words per tick cap */
    while (s_fifo_getoff != put && budget-- > 0) {
        if (!s_fifo_packet.remaining)
            rsx_d3d12_backend_pulse_fence_wait_service();
        int resume = s_fifo_packet.remaining != 0;
        u32 ea = resume ? s_fifo_packet.header_ea : gcm_io2ea(s_fifo_getoff);
        if (!ea) {
            if (perf_call) perf_call->exit_reason = TJ_FIFO_EXIT_EA;
            break;
        }
        u32 w = resume ? s_fifo_packet.raw : vm_read32(ea);
        u32 type = w >> 29;
        if (!resume) {
        if (perf_call) {
            perf_call->headers++;
            perf_call->words++;
        }
        tj_geom_fifo_trace_outcome(s_fifo_getoff, ea, w);
        tj_geom_fifo_trace_relevant_header(s_fifo_getoff, ea, w);

        /* GCM_SIDE_TRACE=1: record command headers executed inside a CALLed
         * side buffer. The FIFO walker holds s_gcm_fifo_lock exclusively, so
         * this lazy state and cap are deterministic without another lock. */
        { static int s_side_trace = -1; static unsigned s_side_count = 0;
          if (s_side_trace < 0) s_side_trace = getenv("GCM_SIDE_TRACE") ? 1 : 0;
          if (s_side_trace && s_fifo_calloff != 0 && s_side_count < 512) {
              u32 count = (w >> 18) & 0x7FF;
              u32 method = w & 0x1FFCu;
              u32 subch = (w >> 13) & 7;
              s_side_count++;
              fprintf(stderr,
                      "[GCM-SIDE] getoff=0x%08X raw=0x%08X type=%u count=%u method=0x%04X subch=%u\n",
                      s_fifo_getoff, w, type, count, method, subch);
          }
        }
        }

        if ((w & 0xFFFF0000u) == 0xFEAD0000u) {
            /* lv1 driver flip: statically-linked libgcm cellGcmSetFlip writes
             * this word into the FIFO instead of calling any import. Fire the
             * flip (status/count/handler) and STOP this drain so the ticker's
             * flip-gated present shows exactly the completed frame -- draining
             * on would mix the next frame's head into the batch (wave: every
             * frame split across 4 presents, layout flashing/zooming). */
            gcm_fifo_context_reset(&s_state);
            s_fifo_getoff += 4;
            rsx_d3d12_backend_snapshot_late_resolve(); /* TJ_SNAPSHOT_LATE_PROOF */
            cellGcmSetFlipCommand_source(w & 0xFFu, FRAME_PROF_FIFO_DISPATCH);
            if (perf_call) perf_call->exit_reason = TJ_FIFO_EXIT_FLIP;
            break;
        }

        if (type == 1) {                       /* JUMP: 0x20000000 | offset */
            if (perf_call) perf_call->controls++;
            u32 target = w & 0x1FFFFFFCu;
            { static int _rd = -1; if (_rd < 0) _rd = getenv("GCM_RECDBG") ? 1 : 0;
              if (_rd) fprintf(stdout, "[JMP] %08X -> %08X (put=%08X)\n",
                               s_fifo_getoff, target, put); }
            /* A self-jump is the RSX idle sentinel used by the title's
             * secondary command ring. Hardware parks GET here until another
             * producer replaces the word. Following it inside this software
             * drain loops for the entire million-word budget while holding the
             * FIFO lock, starving the producer and the ring-recycle callback. */
            if (target == s_fifo_getoff) {
                if (gcm_recdbg_enabled())
                    fprintf(stdout, "[IDLE] at=%08X calloff=%08X put=%08X\n",
                            s_fifo_getoff, s_fifo_calloff, put);
                /* Keep the saved CALL return until an actual RET executes.
                 * PUT advancing cannot retire a pending secondary producer. */
                if (perf_call) perf_call->exit_reason = TJ_FIFO_EXIT_IDLE;
                break;
            }
            s_fifo_getoff = target;
            continue;
        }
        if ((w & 3) == 2) {                    /* CALL: offset | 2 */
            if (perf_call) perf_call->controls++;
            u32 target = w & 0x1FFFFFFCu;
            tj_geom_fifo_trace_call(target, s_fifo_getoff + 4, put);
            s_fifo_calloff = s_fifo_getoff + 4;
            s_fifo_getoff  = target;
            if (gcm_recdbg_enabled())
                fprintf(stdout, "[CALL] target=%08X return=%08X put=%08X\n",
                        s_fifo_getoff, s_fifo_calloff, put);
            continue;
        }
        if (w == 0x00020000u) {                /* RET */
            if (perf_call) perf_call->controls++;
            if (gcm_recdbg_enabled())
                fprintf(stdout, "[RET] to=%08X put=%08X\n", s_fifo_calloff, put);
            s_fifo_getoff = s_fifo_calloff;
            s_fifo_calloff = 0;
            continue;
        }
        if (type == 0 || type == 2) {          /* method (incrementing / NI) */
            u32 count  = (w >> 18) & 0x7FF;
            u32 method = w & 0x1FFCu;
            u32 subch  = (w >> 13) & 7;
            if (!resume) {
                s_fifo_packet.raw = w;
                s_fifo_packet.header_ea = ea;
                s_fifo_packet.header_get = s_fifo_getoff;
                s_fifo_packet.next_index = 0;
                s_fifo_packet.remaining = count;
                s_fifo_getoff += 4;
            }
            u32 first_index = s_fifo_packet.next_index;
            u64 method_group_begin = perf_call ? get_timestamp_ns() : 0;
            u32 method_group_calls = 0;
            while (s_fifo_packet.remaining && s_fifo_getoff != put) {
                u32 dea = gcm_io2ea(s_fifo_getoff);
                if (!dea) {
                    if (perf_call) {
                        perf_call->payload_ea_failures++;
                        perf_call->exit_reason = TJ_FIFO_EXIT_EA;
                    }
                    break;
                }
                u32 i = s_fifo_packet.next_index;
                u32 m = (type == 0) ? method + i * 4 : method;
                u32 data = vm_read32(dea);
                u32 edge_get = s_fifo_getoff + 4;
                int semaphore = gcm_fifo_semaphore_method(&s_state, subch, m, data);
                if (semaphore < 0 || semaphore == 2) {
                    if (perf_call) perf_call->exit_reason = TJ_FIFO_EXIT_IDLE;
                    break;
                }
                if (semaphore == 1) { /* Consumed by the FIFO frontend. */ }
                else if (subch == 7 && m == 0x0B00u) {
                    /* Publish the edge in payload order.  Deferring it to the
                     * end of this method group allows later FIFO work to race
                     * the guest interrupt handler's MAIN-memory observation. */
                    rsx_flush_pending_draws(&s_state);
                    rsx_d3d12_backend_note_prefix_trigger(RSX_PREFIX_TRIGGER_INTERRUPT);
                    const int prefix_rc = rsx_d3d12_backend_complete_fifo_prefix();
                    rsx_d3d12_backend_note_prefix_trigger(RSX_COMPLETION_OTHER);
                    if (prefix_rc != 0) {
                        if (perf_call) perf_call->exit_reason = TJ_FIFO_EXIT_IDLE;
                        break;
                    }
                    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, edge_get);
                    cellGcmQueueUserInterrupt(data, edge_get);
                } else if (m == 0x0000u) {
                    gcm_fifo_bind_object(subch, data);
                } else if (subch < 8 &&
                           s_gcm_fifo_object_class[subch] == GCM_FIFO_OBJECT_NV4097) {
                    tj_depth_pass_trace(&s_state, m, data, ea, dea);
                    rsx_process_method(&s_state, m, data);
                    /* NV406E_SET_REFERENCE: queue the fence value for PACED
                     * publication (gcm_ref_publish below) instead of letting a
                     * later fence in the same batch overwrite it. */
                    if (m == 0x50) {
                        rsx_d3d12_backend_note_prefix_trigger(RSX_PREFIX_TRIGGER_REFERENCE);
                        const int prefix_rc = rsx_d3d12_backend_complete_fifo_prefix();
                        rsx_d3d12_backend_note_prefix_trigger(RSX_COMPLETION_OTHER);
                        if (prefix_rc != 0) {
                            if (perf_call) perf_call->exit_reason = TJ_FIFO_EXIT_IDLE;
                            break;
                        }
                        vm_write32(GCM_CONTROL_GUEST_ADDR + 4, edge_get);
                        gcm_ref_push_at(g_rsx_last_reference, edge_get);
                    }
                } else {
                    if (cellGcmFifoSubchannelMethod(subch, m, data) != 0) return;
                }
                if (perf_call) {
                    perf_call->payloads++;
                    perf_call->words++;
                    method_group_calls++;
                }
                s_fifo_getoff = edge_get;
                s_fifo_packet.next_index++;
                s_fifo_packet.remaining--;
            }
            if (perf_call && method_group_calls) {
                u64 elapsed = get_timestamp_ns() - method_group_begin;
                tj_fifo_perf_group_add(subch, s_gcm_fifo_object_class[subch],
                    type == 0 ? method + first_index * 4 : method,
                    type == 0 ? method + (s_fifo_packet.next_index - 1) * 4 : method,
                    method_group_calls, elapsed);
                perf_call->method_calls += method_group_calls;
                perf_call->method_ns += elapsed;
                if (elapsed > perf_call->method_max_ns) {
                    perf_call->method_max_ns = elapsed;
                    perf_call->max_group_getoff = s_fifo_packet.header_get;
                    perf_call->max_group_header_ea = ea;
                    perf_call->max_group_raw = w;
                    perf_call->max_group_type = type;
                    perf_call->max_group_subchannel = subch;
                    perf_call->max_group_method = method;
                    perf_call->max_group_count = count;
                    perf_call->max_group_first_method =
                        type == 0 ? method + first_index * 4 : method;
                    perf_call->max_group_last_method =
                        type == 0 ? method + (s_fifo_packet.next_index - 1) * 4 : method;
                }
            }
            if (s_fifo_packet.remaining) break;
            continue;
        }
        { static int _uw = 0; if (_uw++ < 16 && getenv("RTT_DUMP"))
            fprintf(stderr, "[FIFOUW] unknown word 0x%08X at getoff 0x%X\n", w, s_fifo_getoff); }
        s_fifo_getoff += 4;                    /* unknown word: skip */
    }

    if (perf_call && perf_call->exit_reason == TJ_FIFO_EXIT_CAUGHT_UP &&
        s_fifo_getoff != put && budget < 0)
        perf_call->exit_reason = TJ_FIFO_EXIT_BUDGET;

    /* Publish progress: get chases put; ref advances at most ONE queued fence
     * per tick (gcm_ref_push/gcm_ref_publish_one above) so every SET_REFERENCE
     * value stays observable to equality-waiters. The wrap recycle path polls
     * the drained EA. */
    rsx_d3d12_backend_snapshot_late_resolve(); /* TJ_SNAPSHOT_LATE_PROOF: before drained EA/GET */
    g_gcm_fifo_drained_ea = gcm_io2ea(s_fifo_getoff);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 4, s_fifo_getoff);              /* get */
    AcquireSRWLockExclusive(&s_ref_pub_lock);                           /* ref */
    gcm_ref_publish_one();
    /* Tokyo Jungle: once the title's boot fences are all published, its
     * per-frame pump (func_001F2D88) keeps waiting on ref advancing toward the
     * offset it just wrote. The queue is empty by then, so ref froze and the
     * pump spent ~224 tick spins per frame. libgcm's ref is "how far the RSX
     * has consumed", so when no fence is queued, publish the FIFO's own
     * progress. Additive: a queued fence always wins. */
    if (s_ref_qhead == s_ref_qtail)
        vm_write32(GCM_CONTROL_GUEST_ADDR + 8,
                   vm_read32(GCM_CONTROL_GUEST_ADDR + 0));   /* put */
    ReleaseSRWLockExclusive(&s_ref_pub_lock);
}

/* FIFO command-buffer-full callback body. The title's inline gcmReserve calls
 * context->callback(context, count) (routed here via the synthetic OPD) when the
 * ring's `current` nears `end`. Real libgcm flushes, waits for the RSX `get` to
 * pass, then recycles `current` to `begin`. We mirror that: wait (bounded) for
 * the ticker drain to consume everything up to `current`, then reset current to
 * begin. Runs on the guest thread; the ticker owns all rendering, so we only
 * pointer-shuffle here (the drain's own wrap-detect handles s_get). Without this
 * the ring never recycles and the whole pipeline wedges once the FIFO wraps
 * (~200 frames at the 256KB default), which looked like a GPU/driver hang. */
static u32 gcm_ea2io(u32 ea)
{
    u32 page = ea >> 20;
    if (page >= 65536) return 0xFFFFFFFFu;
    AcquireSRWLockShared(&s_main_mapping_lock);
    u16 io_page = s_io_address_table[page];
    u32 io = io_page == 0xFFFF ? 0xFFFFFFFFu
                                : ((u32)io_page << 20) | (ea & 0xFFFFFu);
    ReleaseSRWLockShared(&s_main_mapping_lock);
    return io;
}

static int gcm_fifo_recycle_ready(u32 io_tail)
{
    /* An out-of-ring GET can be executing a CALL whose return still belongs
     * to the primary ring. The separately published drained EA may also be
     * stale. Read the actual walker state under its ownership lock. */
    if (!TryAcquireSRWLockShared(&s_gcm_fifo_lock)) return 0;
    int ready = s_fifo_getoff == io_tail && s_fifo_calloff == 0 &&
                s_fifo_packet.remaining == 0;
    ReleaseSRWLockShared(&s_gcm_fifo_lock);
    return ready;
}

/* Walker state the recycler may act on. GET (s_fifo_getoff) and the one-deep
 * CALL return (s_fifo_calloff, 0 = none) are IO offsets advanced only by the
 * FIFO walkers under s_gcm_fifo_lock. Never block on that lock here: a walker
 * may hold it for a whole drain. */
typedef struct GcmFifoWalkerView {
    u32 getoff, calloff, remaining;
} GcmFifoWalkerView;

static int gcm_fifo_walker_view(GcmFifoWalkerView* view)
{
    if (!TryAcquireSRWLockShared(&s_gcm_fifo_lock)) return 0;
    view->getoff = s_fifo_getoff;
    view->calloff = s_fifo_calloff;
    view->remaining = s_fifo_packet.remaining;
    ReleaseSRWLockShared(&s_gcm_fifo_lock);
    return 1;
}

/* First primary-ring word the walker has not consumed (IO offset), or
 * 0xFFFFFFFF when its state names none. A CALL stores calloff = CALL word + 4
 * and moves GET into the side buffer; RET reloads GET from calloff. Inside a
 * CALL the return is therefore the primary position; otherwise GET is (mid-
 * packet it is the next payload word, the header being held in s_fifo_packet).
 * A live return while GET itself lies in [io_begin, io_last] is no side-buffer
 * CALL (stale or nested return), so it gives no trustworthy position. */
static u32 gcm_fifo_walker_primary(const GcmFifoWalkerView* view,
                                   u32 io_begin, u32 io_last)
{
    if (!view->calloff) return view->getoff;
    if (view->getoff >= io_begin && view->getoff <= io_last) return 0xFFFFFFFFu;
    return view->calloff;
}

static void gcm_fifo_walker_describe(char* out, size_t size,
                                     const GcmFifoWalkerView* view, int seen)
{
    const u32 put = vm_read32(GCM_CONTROL_GUEST_ADDR + 0);
    if (!seen) {
        snprintf(out, size, "walker=busy put=0x%08X", put);
        return;
    }
    const u32 ea = gcm_io2ea(view->getoff);
    const u32 word = ea ? vm_read32(ea) : 0;
    /* The walker parks on a JUMP to itself without consuming it. Mid-packet,
     * the word at GET is payload rather than a control word. */
    const int self_jump = ea && !view->remaining && (word >> 29) == 1 &&
                          (word & 0x1FFFFFFCu) == view->getoff;
    snprintf(out, size,
             "get=0x%08X call=0x%08X remaining=%u put=0x%08X word@get=0x%08X%s self_jump=%d",
             view->getoff, view->calloff, view->remaining, put, word,
             ea ? "" : "(unmapped)", self_jump);
}

/* Open reuse window. One walker means one primary ring/context, and only its
 * producer thread calls back. While ctx->end < true_end the guest may write
 * only below end; the old lap's unread words and its tail JUMP (io_tail) lie
 * at or above the walker position the window was bounded by. */
typedef struct GcmFifoWindow {
    u32 ctx, begin, true_end, io_tail;
} GcmFifoWindow;
static GcmFifoWindow s_fifo_window;

#define GCM_FIFO_RECYCLE_SPINS 2000
#define GCM_FIFO_WINDOW_SPINS  25       /* strict-only grace before a window */
#define GCM_FIFO_WINDOW_WRAP   1        /* old lap retired; request needs a wrap */

/* EA(r) = begin + (r - io_begin) for every ring offset needs one linear IO
 * mapping of the ring, which the walker's linear GET already assumes. */
static int gcm_fifo_ring_linear(u32 begin, u32 end, u32 current,
                                u32 io_begin, u32 io_tail)
{
    return io_tail - io_begin == current - begin &&
           gcm_ea2io(end) - io_begin == end - begin;
}

/* A window bounded by unread position r holds count words from begin with
 * one spare word: begin + 4*count <= EA(r) - 8. */
static int gcm_fifo_window_fits(u32 io_begin, u32 r, u32 count)
{
    return (u64)r >= (u64)io_begin + 4ull * ((u64)count + 2);
}

/* Record the window and bound end; the caller then sets current = begin. */
static void gcm_fifo_window_set(u32 ctx_ea, u32 begin, u32 true_end,
                                u32 io_tail, u32 window_end)
{
    s_fifo_window.ctx = ctx_ea;
    s_fifo_window.begin = begin;
    s_fifo_window.true_end = true_end;
    s_fifo_window.io_tail = io_tail;
    vm_write32(ctx_ea + 0x4, window_end);
}

/* libgcm reuses command space once GET has left it; it does not wait for the
 * whole ring to retire. With the walker's primary position r inside the lap
 * being retired, reuse only [begin, r): end = EA(r) - 4, so the inline
 * reserve (it calls back whenever current + 4*count > end) never writes at or
 * above r, where every unread word and the old-tail JUMP live. The walker
 * finishes the old lap, takes the JUMP to io_begin and then consumes the new
 * lap below r. `view` is a snapshot taken under the walker lock. */
static int gcm_fifo_window_open(u32 ctx_ea, u32 begin, u32 end, u32 current,
                                u32 io_begin, u32 io_tail, u32 count,
                                const GcmFifoWalkerView* view)
{
    if (!gcm_fifo_ring_linear(begin, end, current, io_begin, io_tail)) return 0;
    const u32 r = gcm_fifo_walker_primary(view, io_begin, io_begin + (end - begin));
    /* PUT is io_tail, so a walker in this lap has r in [io_begin, io_tail].
     * Anything else (an earlier lap, a foreign buffer) bounds nothing. */
    if (r < io_begin || r > io_tail) return 0;
    /* A partial packet reaching the tail would consume the JUMP as payload. */
    if (!view->calloff && view->remaining &&
        (u64)r + 4ull * view->remaining > io_tail)
        return 0;
    if (!gcm_fifo_window_fits(io_begin, r, count)) return 0;
    const u32 window_end = begin + (r - io_begin) - 4;
    gcm_fifo_window_set(ctx_ea, begin, end, io_tail, window_end);  /* bound first */
    vm_write32(ctx_ea + 0x8, begin);                               /* then reuse */
    static int logged = 0;
    if (!logged) {
        char walker[160];
        logged = 1;
        gcm_fifo_walker_describe(walker, sizeof(walker), view, 1);
        fprintf(stderr,
                "[cellGcmSys] fifo recycle: window reuse below 0x%08X (tail=0x%08X end_ea=0x%08X count=%u %s)\n",
                r, io_tail, window_end, count, walker);
    }
    return 1;
}

/* Callback while a window is open. Flush like libgcm (PUT = current; all
 * words below it are complete commands) so the walker can leave the old lap.
 * Then grow end only to just below the walker's unread position, or restore
 * true_end once GET has crossed into this lap. Never writes the ring. */
static s32 gcm_fifo_window_extend(u32 ctx_ea, u32 begin, u32 current, u32 count)
{
    const u32 true_end = s_fifo_window.true_end;
    const u32 io_begin = gcm_ea2io(begin);
    const u32 io_cur = gcm_ea2io(current);
    if (io_begin == 0xFFFFFFFFu || io_cur == 0xFFFFFFFFu ||
        io_cur - io_begin != current - begin)
        return CELL_GCM_ERROR_INVALID_VALUE;
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_cur);
    GcmFifoWalkerView view = {0, 0, 0};
    int seen = 0;
    GcmPollWait wait;
    gcm_poll_wait_begin(&wait, GCM_WAIT_SITE_WINDOW, GCM_WAIT_CH_FIFO);
    wait.kick = gcm_fifo_kick_presenter_for_wait;
    for (int spins = 0;;) {
        if (gcm_fifo_walker_view(&view)) {
            seen = 1;
            const u32 r = gcm_fifo_walker_primary(&view, io_begin,
                                                  io_begin + (true_end - begin));
            /* Old lap: r > io_cur, because every window end stayed below the
             * position it was bounded by and r only advances there. This lap:
             * r <= PUT <= io_cur, and the old lap plus its JUMP are consumed. */
            if (r >= io_begin && r <= io_cur) {
                memset(&s_fifo_window, 0, sizeof(s_fifo_window));
                vm_write32(ctx_ea + 0x4, true_end);
                gcm_poll_wait_end(&wait, GCM_WAIT_OUT_READY);
                return (u64)current + 4ull * count <= true_end
                    ? CELL_OK : GCM_FIFO_WINDOW_WRAP;
            }
            if (r > io_cur && r <= s_fifo_window.io_tail &&
                (u64)begin + (r - io_begin) >= (u64)current + 4ull * ((u64)count + 1)) {
                vm_write32(ctx_ea + 0x4, begin + (r - io_begin) - 4);
                gcm_poll_wait_end(&wait, GCM_WAIT_OUT_ALT);
                return CELL_OK;
            }
        }
        if (spins >= GCM_FIFO_RECYCLE_SPINS) break;
        spins = gcm_poll_wait_pause(&wait, spins);    /* Sleep(1); ++spins */
    }
    gcm_poll_wait_end(&wait, GCM_WAIT_OUT_LIMIT);
    static int warned = 0;
    if (warned++ < 4) {
        char walker[160];
        gcm_fifo_walker_describe(walker, sizeof(walker), &view, seen);
        fprintf(stderr,
                "[cellGcmSys] fifo recycle: window not released (current=0x%08X end_ea=0x%08X count=%u); storage retained %s\n",
                io_cur, vm_read32(ctx_ea + 0x4), count, walker);
    }
    return CELL_GCM_ERROR_FAILURE;
}

s32 cellGcm_fifo_recycle_n(u32 ctx_ea, u32 count)
{
    if (!ctx_ea || (ctx_ea & 3u)) return CELL_GCM_ERROR_INVALID_VALUE;
    u32 begin   = vm_read32(ctx_ea + 0x0);
    u32 end     = vm_read32(ctx_ea + 0x4);
    u32 current = vm_read32(ctx_ea + 0x8);
    if (((begin | end | current) & 3u) || current < begin ||
        current > end || begin >= end)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* An end below the remembered true end is an open window, not a wrap.
     * Check it before the empty-ring return: a window starts at begin. */
    if (s_fifo_window.ctx == ctx_ea) {
        if (s_fifo_window.begin == begin && end < s_fifo_window.true_end) {
            const u32 true_end = s_fifo_window.true_end;
            const s32 rc = gcm_fifo_window_extend(ctx_ea, begin, current, count);
            if (rc != GCM_FIFO_WINDOW_WRAP) return rc;
            end = true_end;                     /* restored; wrap below */
        } else {
            memset(&s_fifo_window, 0, sizeof(s_fifo_window));
        }
    }
    if (current == begin) return CELL_OK;

    u32 io_begin = gcm_ea2io(begin);
    u32 io_tail = gcm_ea2io(current);
    if (io_begin == 0xFFFFFFFFu || io_tail == 0xFFFFFFFFu)
        return CELL_GCM_ERROR_INVALID_VALUE;

    static int s_blinkdbg = -1;
    if (s_blinkdbg < 0) s_blinkdbg = getenv("CELLMARK_BLINKDBG") ? 1 : 0;
    if (s_blinkdbg)
        printf("[RECYCLE] ring wrap: current=0x%08X -> begin=0x%08X\n", current, begin);

    static int s_recdbg = -1;
    if (s_recdbg < 0) s_recdbg = getenv("GCM_RECDBG") ? 1 : 0;
    if (s_recdbg)
        fprintf(stdout, "[REC>] tid=%lu begin=%08X cur=%08X put=%08X get=%08X drained=%08X\n",
                GetCurrentThreadId(), begin, current,
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea);

    /* This runtime recycles a whole ring, rather than rotating SDK segments.
     * Publish its old tail, not the wrapped head: PUT == old GET would make a
     * full ring look empty. Leave the reserved JUMP just beyond published PUT
     * until the producer's next flush publishes new commands at the head. */
    vm_write32(current, 0x20000000u | io_begin);
    vm_write32(GCM_CONTROL_GUEST_ADDR + 0, io_tail);

    /* Reuse only space GET has left. A fully retired lap is the common case;
     * after a short grace period, a window below the walker's unread position
     * opens as soon as it fits (re-checked every spin). On timeout retain the
     * old current and report failure to gcmReserve; never degrade a stalled
     * consumer into silently overwritten commands. */
    GcmFifoWalkerView view = {0, 0, 0};
    int seen = 0;
    int spins = 0;
    /* TJ_FLIP_WAIT_EVENT: spins are elapsed ms, so the 25-spin grace and the
     * 2000-spin timeout keep their wall-clock floors under early wakeups. */
    GcmPollWait wait;
    gcm_poll_wait_begin(&wait, GCM_WAIT_SITE_RECYCLE, GCM_WAIT_CH_FIFO);
    wait.kick = gcm_fifo_kick_presenter_for_wait;
    while (!gcm_fifo_recycle_ready(io_tail)) {
        if (spins >= GCM_FIFO_WINDOW_SPINS && gcm_fifo_walker_view(&view)) {
            seen = 1;
            if (gcm_fifo_window_open(ctx_ea, begin, end, current, io_begin,
                                     io_tail, count, &view)) {
                if (s_recdbg)
                    fprintf(stdout, "[REC<] tid=%lu window end=%08X cur-now=%08X put=%08X get=%08X spins=%d\n",
                            GetCurrentThreadId(), vm_read32(ctx_ea + 0x4),
                            vm_read32(ctx_ea + 0x8),
                            vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                            vm_read32(GCM_CONTROL_GUEST_ADDR + 4), spins);
                gcm_poll_wait_end(&wait, GCM_WAIT_OUT_ALT);
                return CELL_OK;
            }
        }
        if (spins >= GCM_FIFO_RECYCLE_SPINS) {
            static int warned = 0;
            if (warned++ < 4) {
                char walker[160];
                gcm_fifo_walker_describe(walker, sizeof(walker), &view, seen);
                fprintf(stderr,
                    "[cellGcmSys] fifo recycle: tail not retired (tail=0x%08X); storage retained %s\n",
                    io_tail, walker);
            }
            gcm_poll_wait_end(&wait, GCM_WAIT_OUT_LIMIT);
            return CELL_GCM_ERROR_FAILURE;
        }
        spins = gcm_poll_wait_pause(&wait, spins);    /* Sleep(1); spins++ */
    }
    gcm_poll_wait_end(&wait, GCM_WAIT_OUT_READY);

    /* Retired means GET stopped ON the old-tail JUMP (PUT == io_tail) without
     * consuming it. Keep the new lap below it, i.e. a window bounded by
     * r = io_tail, until the walker has taken it: the next callback flushes
     * PUT, sees the crossing and restores the true end (at most one extra
     * callback per lap, no wait once GET has crossed). A request that does
     * not fit below the JUMP (the NID path's unknown size) keeps the whole
     * ring, as before. */
    if (gcm_fifo_ring_linear(begin, end, current, io_begin, io_tail) &&
        gcm_fifo_window_fits(io_begin, io_tail, count))
        gcm_fifo_window_set(ctx_ea, begin, end, io_tail, current - 4);
    vm_write32(ctx_ea + 0x8, begin);                    /* recycle ring to base */

    if (s_recdbg)
        fprintf(stdout, "[REC<] tid=%lu cur-now=%08X put=%08X get=%08X drained=%08X spins=%d\n",
                GetCurrentThreadId(), vm_read32(ctx_ea + 0x8),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 0),
                vm_read32(GCM_CONTROL_GUEST_ADDR + 4),
                g_gcm_fifo_drained_ea, spins);
    return CELL_OK;
}

/* NID entry: no reserve size is known, so it never takes a partial window
 * and keeps the full-retirement contract. */
s32 cellGcm_fifo_recycle(u32 ctx_ea)
{
    return cellGcm_fifo_recycle_n(ctx_ea, 0xFFFFFFFFu);
}

/* NID: 0xDC09357E */
s32 cellGcmSetDisplayBuffer(u32 bufferId, u32 offset, u32 pitch,
                            u32 width, u32 height)
{
    printf("[cellGcmSys] SetDisplayBuffer(id=%u, offset=0x%X, pitch=%u, %ux%u)\n",
           bufferId, offset, pitch, width, height);

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    gcm_identity_write_begin();
    s_display_buffers[bufferId].offset = offset;
    s_display_buffers[bufferId].pitch  = pitch;
    s_display_buffers[bufferId].width  = width;
    s_display_buffers[bufferId].height = height;
    s_display_buffer_set[bufferId] = 1;
    gcm_identity_write_end();

    return CELL_OK;
}

/* Backend query (not a guest API): is this raw RSX offset a registered
 * display buffer? Used to split display draws (-> backbuffer) from
 * offscreen render-to-texture passes. */
int cellGcmOffsetIsDisplay(u32 offset)
{
    for (int i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i] && s_display_buffers[i].offset == offset)
            return 1;
    return 0;
}

/* Backend identity lookup for registered display surfaces and the one strict
 * history form used by titles that place a third equal-stride surface after a
 * matching registered pair. This is metadata-driven: no game offsets or
 * dimensions are baked into the rule. */
int cellGcmGetDisplaySurfaceInfo(u32 offset, u32 pitch, u32 width, u32 height,
                                 CellGcmDisplayInfo* out_info)
{
    for (int i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++)
        if (s_display_buffer_set[i] && s_display_buffers[i].offset == offset) {
            if (out_info) *out_info = s_display_buffers[i];
            return 1;
        }

    for (int first = 0; first < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; first++) {
        const CellGcmDisplayInfo* a = &s_display_buffers[first];
        if (!s_display_buffer_set[first] || a->pitch != pitch ||
            a->width != width || a->height != height)
            continue;
        for (int second = 0; second < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; second++) {
            const CellGcmDisplayInfo* b = &s_display_buffers[second];
            if (second == first || !s_display_buffer_set[second] ||
                b->pitch != pitch || b->width != width || b->height != height ||
                b->offset <= a->offset)
                continue;
            u64 stride = (u64)b->offset - a->offset;
            if ((u64)b->offset + stride == offset) {
                if (out_info) {
                    *out_info = *a;
                    out_info->offset = offset;
                }
                return 1;
            }
        }
    }
    return 0;
}

/* Backend-only identity query for the strict next-stride history form. Unlike
 * cellGcmGetDisplaySurfaceInfo, this returns both registered parents unchanged
 * so deferred replay can prove which parent was produced in FIFO order. */
int cellGcmGetDisplayHistoryAliasParents(u32 offset, u32 pitch, u32 width,
                                         u32 height,
                                         CellGcmDisplayInfo* out_first,
                                         CellGcmDisplayInfo* out_second)
{
    if (cellGcmOffsetIsDisplay(offset)) return 0;
    rsx_display_alias_surface alias = { offset, pitch, width, height };
    CellGcmDisplayInfo selected_first = {0}, selected_second = {0};
    u32 selected_first_raw = 0, selected_second_raw = 0;
    int have_selected = 0, ambiguous = 0;
    for (int first = 0; first < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; first++) {
        const CellGcmDisplayInfo* a = &s_display_buffers[first];
        if (!s_display_buffer_set[first]) continue;
        rsx_display_alias_surface first_meta = {
            a->offset, a->pitch, a->width, a->height
        };
        for (int second = 0; second < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; second++) {
            const CellGcmDisplayInfo* b = &s_display_buffers[second];
            if (second == first || !s_display_buffer_set[second]) continue;
            rsx_display_alias_surface second_meta = {
                b->offset, b->pitch, b->width, b->height
            };
            int was_selected = have_selected;
            int match = rsx_display_history_alias_select_unique_pair(
                &first_meta, &second_meta, &alias,
                &selected_first_raw, &selected_second_raw, &have_selected);
            if (match < 0) {
                ambiguous = 1;
                continue;
            }
            if (match > 0 && !was_selected) {
                selected_first = *a;
                selected_second = *b;
            }
        }
    }
    if (!have_selected || ambiguous) return 0;
    if (out_first) *out_first = selected_first;
    if (out_second) *out_second = selected_second;
    return 1;
}

/* Backend-only exact-backing query.  Unlike the identity function above this
 * recognizes a contained raw alias (for example base+0x1000) only when the
 * whole requested rectangle fits one registered display allocation. */
int cellGcmGetDisplaySurfaceContaining(u32 offset, u32 pitch, u32 width, u32 height,
                                       CellGcmDisplayInfo* out_info,
                                       u32* out_x, u32* out_y)
{
    for (int i = 0; i < CELL_GCM_MAX_DISPLAY_BUFFER_NUM; i++) {
        const CellGcmDisplayInfo* info = &s_display_buffers[i];
        if (!s_display_buffer_set[i] || info->pitch != pitch) continue;
        if (rsx_scaled_blit_alias_origin(info->offset, offset, pitch,
                                         info->width, info->height,
                                         width, height, out_x, out_y)) {
            if (out_info) *out_info = *info;
            return 1;
        }
    }
    return 0;
}

/* Tokyo Jungle keeps a third 1280x720 local surface immediately after its
 * two registered display slots (0x0, 0x3A0000, 0x740000). It is sampled as
 * display history, but is not registered with cellGcmSetDisplayBuffer. Do not
 * promote arbitrary local surfaces: require a pair of registered slots with
 * identical geometry/pitch and the exact next equal stride. */
int cellGcmOffsetIsDisplayHistoryAlias(u32 offset, u32 pitch, u32 width,
                                       u32 height)
{
    if (cellGcmOffsetIsDisplay(offset)) return 0;
    return cellGcmGetDisplaySurfaceInfo(offset, pitch, width, height, NULL);
}

/* NID: 0xEAA52F23 */
static s32 cellGcmSetFlipCommand_source(u32 bufferId, u32 frame_prof_source)
{
    { static int _n=0, _fd=-1; if (_fd<0) _fd=getenv("FLIP_DBG")?1:0; if (_fd && _n++ < 20)
        fprintf(stderr, "[FLIP] SetFlipCommand(buf=%u) set=%d\n",
                bufferId, bufferId < CELL_GCM_MAX_DISPLAY_BUFFER_NUM ? s_display_buffer_set[bufferId] : -1); }
    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;


    /* Flip requested but not yet shown. Only completion of the presented
     * ticket can release its WAITING status. */
    gcm_flip_publish(bufferId, 1, frame_prof_source);
    tj_flip_perf_set_waiting();
    if (s_tj_survival_flip_diag) tj_flip_trace("set-flip-waiting", bufferId);
    frame_prof_publish(frame_prof_source);
    /* Submission is not completion. The presenter queues the guest callback
     * after displaying this ticket; ppu_gcm_pump delivers it on a safe point. */

    return CELL_OK;
}

/* Isolated one-frame replay has no preceding guest flip to establish the
 * ordered-FIFO boundary policy. The complete input is already validated and
 * has one final flip. Claim that frame before dispatching its methods so a
 * mid-frame display clear cannot trigger the legacy boot-time present path.
 * Refuse this diagnostic entry point when a guest context/callback is active. */
int cellGcmReplayBeginFrame(u32 buffer_id, CellGcmFlipTicket* ticket)
{
    if (!ticket || !s_gcm_initialized || s_gcm_context_ea || s_flip_handler_opd)
        return 0;
    if (cellGcmSetFlipCommand_source(buffer_id, FRAME_PROF_FIFO_DISPATCH) != CELL_OK)
        return 0;
    return cellGcm_take_flip_ticket(ticket);
}

/* cellGcmSetFlip(context, buffer_id) — immediate flip request. PSL1GHT's
 * gcmSetFlip import (NID 0xDC09357E) lands here; without it vkcube's flip was
 * a no-op, GetFlipStatus never cleared, and init timed out into exit(-1). */
s32 cellGcmSetFlip(void* context, u32 bufferId)
{
    (void)context;   /* command context — flip is immediate in HLE */
    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xD01B570F */
s32 cellGcmSetFlipCommandWithWaitLabel(u32 bufferId, u32 labelIndex, u32 labelValue)
{
    if (labelIndex >= CELL_GCM_MAX_LABEL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Wait until the label reaches the expected value (instant in HLE). Labels
     * live in guest memory (see cellGcmGetLabelAddress) so the game can poll. */
    vm_write32(GCM_LABEL_GUEST_BASE + labelIndex * GCM_LABEL_STRIDE, labelValue);

    return cellGcmSetFlipCommand(bufferId);
}

/* NID: 0xA2478CA3 */
s32 cellGcmSetPrepareFlip(void* ctx, u32 bufferId)
{
    (void)ctx;  /* command buffer context -- not used in HLE */

    if (bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM)
        return CELL_GCM_ERROR_INVALID_VALUE;

    if (!s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetPrepareFlip(bufferId=%u)\n", bufferId);

    gcm_flip_publish(bufferId, 0, FRAME_PROF_DIRECT_HLE);
    frame_prof_publish(FRAME_PROF_DIRECT_HLE);
    s_last_flip_time = get_timestamp_ns();

    /* Invoke the guest flip handler via OPD resolution -- s_flip_handler holds
     * the raw guest OPD (e.g. 0x530D70); calling it as a host function pointer
     * jumps into guest code and crashes. */
    if (s_flip_handler_opd && g_ps3_guest_caller)
        g_ps3_guest_caller(s_flip_handler_opd, 0, 0, 0, 0, 0, 0, 0, 0);

    return CELL_OK;
}

/* NID: 0x1BFAB6EE */
CellGcmDisplayInfo* cellGcmGetDisplayBufferByFlipIndex(u32 index)
{
    u32 id = index % CELL_GCM_MAX_DISPLAY_BUFFER_NUM;
    return &s_display_buffers[id];
}

/* NID: 0x8BADE8BE */
u32 cellGcmGetCurrentDisplayBufferId(void)
{
    return atomic_load_explicit(&s_current_display_buffer_id, memory_order_acquire);
}

/* NID: 0xD9B7653E */
void cellGcmSetFlipHandler(CellGcmFlipHandler handler)
{
    /* `handler` is a guest OPD address — store as such and remember
     * the legacy host-typed value too for any callers still using
     * the old API style. Real dispatch goes through
     * cellGcmDispatchVBlank()/DispatchFlip() via g_ps3_guest_caller. */
    printf("[cellGcmSys] SetFlipHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    u32 opd = (u32)(size_t)handler;
    u32 code = opd ? vm_read32(opd) : 0;
    if (code) s_flip_handler_code = code;
    s_flip_handler_opd = opd;
    s_flip_handler = handler;
}

/* NID: 0xA547ADDE */
void cellGcmSetVBlankHandler(CellGcmVBlankHandler handler)
{
    printf("[cellGcmSys] SetVBlankHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    u32 opd = (u32)(size_t)handler;
    u32 code = opd ? vm_read32(opd) : 0;
    if (code) s_vblank_handler_code = code;
    s_vblank_handler_opd = opd;
    s_vblank_handler = handler;
}

/* NID: 0xF9BFCDA3 */
void cellGcmSetSecondVHandler(CellGcmSecondVHandler handler)
{
    printf("[cellGcmSys] SetSecondVHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    s_second_v_handler_opd = (u32)(size_t)handler;
    s_second_v_handler = handler;
}

/* NID: 0x0B4B62D5 */
void cellGcmSetUserHandler(CellGcmUserHandler handler)
{
    printf("[cellGcmSys] SetUserHandler(opd=0x%08X)\n", (unsigned)(size_t)handler);
    gcm_user_interrupt_lock();
    s_user_handler_opd = (u32)(size_t)handler;
    s_user_handler = handler;
    s_user_handler_code = s_user_handler_opd ? vm_read32(s_user_handler_opd) : 0;
    gcm_user_interrupt_unlock();
}

/* NID: 0x21AC3697 */
u64 cellGcmGetLastFlipTime(void)
{
    return s_last_flip_time;
}

/* ---------------------------------------------------------------------------
 * Address translation / IO mapping
 * -----------------------------------------------------------------------*/

/* NID: 0x0E6B0DFF */
s32 cellGcmGetOffsetTable(CellGcmOffsetTable* table)
{
    if (!table)
        return CELL_GCM_ERROR_INVALID_VALUE;

    AcquireSRWLockShared(&s_main_mapping_lock);
    *table = s_offset_table;
    ReleaseSRWLockShared(&s_main_mapping_lock);
    return CELL_OK;
}

/* NID: 0xDB769B32 */
static u32 gcm_io_alloc(u32 size);   /* defined below */

s32 cellGcmAddressToOffset(u32 address, u32* offset)
{
    /* `offset` is a GUEST address; write big-endian via vm_write32. */
    uint32_t off_ea = (uint32_t)(uintptr_t)offset;
    if (!off_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Local memory: offset = address - localBase. Record the offset page as
     * LOCAL-derived so cellGcmResolveOffset can disambiguate it from an IO
     * (main-memory) offset with the same page number -- the two offset spaces
     * overlap, and a title with a large IO region (gcm/cube: 16MB) otherwise
     * gets its VRAM vertex/texture/FP reads resolved into the empty command
     * buffer. */
    if (address >= s_config.localAddress &&
        address < s_config.localAddress + s_config.localSize) {
        u32 loff = address - s_config.localAddress;
        cellGcmMainMappingMarkLocalPage(loff >> 20);
        vm_write32(off_ea, loff);
        return CELL_OK;
    }

    /* IO-mapped main memory: consult offset table */
    u32 page = address >> 20;
    AcquireSRWLockShared(&s_main_mapping_lock);
    if (page < 65536 && s_io_address_table[page] != 0xFFFF) {
        u32 io_page = s_io_address_table[page];
        ReleaseSRWLockShared(&s_main_mapping_lock);
        vm_write32(off_ea, (io_page << 20) | (address & 0xFFFFF));
        return CELL_OK;
    }
    ReleaseSRWLockShared(&s_main_mapping_lock);

    /* Legacy fallback for initial IO region */
    if (s_config.ioAddress != 0 && address >= s_config.ioAddress &&
        address < s_config.ioAddress + s_config.ioSize) {
        vm_write32(off_ea, address - s_config.ioAddress);
        return CELL_OK;
    }

    /* Auto-map unmapped main memory. On real hardware PSL1GHT pre-maps its
     * whole RSX heap in gcmInitBody, so its libraries never call MapMainMemory
     * before handing an EA to the RSX (Tiny3D's command ring lives in an
     * sys_mmapper region). Mirror that by mapping the 1MB page on first use. */
    if (address < 0x40000000u) {
        u32 ea_page = address & ~0xFFFFFu;
        cellGcmMainMappingWriteBegin();
        u32 io      = gcm_io_alloc(0x100000u);
        populate_offset_table(ea_page, io, 0x100000u);
        cellGcmMainMappingWriteEnd();
        printf("[cellGcmSys] AddressToOffset: auto-mapped ea 0x%08X -> io 0x%08X\n",
               ea_page, io);
        vm_write32(off_ea, io | (address & 0xFFFFFu));
        return CELL_OK;
    }

    printf("[cellGcmSys] WARNING: AddressToOffset failed for 0x%08X\n", address);
    vm_write32(off_ea, 0);
    return CELL_GCM_ERROR_FAILURE;
}

/* IO-offset space bump allocator for main-memory mappings. Starts PAST the
 * Init command-buffer region: handing out io 0 (the old s_local_mem_allocated
 * bump, which begins at 0) remapped the FIFO's own io page -- demosaic's
 * cellGcmMapMainMemory(sys heap) got offset 0, io2ea(0) then resolved into its
 * heap (all zeros) and the RSX walker consumed every frame's commands as
 * no-ops (no draws, label fence never written, guest waited forever). */
static u32 s_io_alloc_next = 0;
static u32 gcm_io_alloc(u32 size)
{
    if (s_io_alloc_next == 0)
        s_io_alloc_next = (s_config.ioSize + 0xFFFFFu) & ~0xFFFFFu;
    if (s_io_alloc_next < 0x100000u) s_io_alloc_next = 0x100000u;
    u32 io = s_io_alloc_next;
    s_io_alloc_next += (size + 0xFFFFFu) & ~0xFFFFFu;
    return io;
}

/* NID: 0x2A6FBA9C */
s32 cellGcmMapMainMemory(u32 ea, u32 size, u32* offset)
{
    if (!offset)
        return CELL_GCM_ERROR_INVALID_VALUE;

    /* Size must be 1MB aligned */
    if (size == 0 || (size & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;

    /* EA must be 1MB aligned */
    if ((ea & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;

    printf("[cellGcmSys] MapMainMemory(ea=0x%08X, size=0x%X)\n", ea, size);

    cellGcmMainMappingWriteBegin();
    /* Allocate a fresh IO region past the Init command-buffer window. */
    u32 io_offset = gcm_io_alloc(size);
    printf("[cellGcmSys] MapMainMemory -> io=0x%08X\n", io_offset);

    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        cellGcmMainMappingWriteEnd();
        printf("[cellGcmSys] WARNING: no free IO mapping slots\n");
        return CELL_GCM_ERROR_FAILURE;
    }

    mapping->ea     = ea;
    mapping->io     = io_offset;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io_offset, size);
    cellGcmMainMappingWriteEnd();

    vm_write32((uint32_t)(uintptr_t)offset, io_offset);   /* guest out-param */
    return CELL_OK;
}

/* NID: 0x5A41C10F */
s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
{
    /* Both EA and IO must be 1MB aligned */
    if ((ea & 0xFFFFF) != 0 || (io & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;

    if (size == 0 || (size & 0xFFFFF) != 0)
        return CELL_GCM_ERROR_INVALID_ALIGNMENT;

    printf("[cellGcmSys] MapEaIoAddress(ea=0x%08X, io=0x%08X, size=0x%X)\n", ea, io, size);

    cellGcmMainMappingWriteBegin();
    /* Check for overlap with existing mappings */
    if (find_mapping_by_ea(ea) != NULL) {
        cellGcmMainMappingWriteEnd();
        return CELL_GCM_ERROR_ADDRESS_OVERWRAP;
    }

    IoMapping* mapping = find_free_mapping();
    if (!mapping) {
        cellGcmMainMappingWriteEnd();
        return CELL_GCM_ERROR_FAILURE;
    }

    mapping->ea     = ea;
    mapping->io     = io;
    mapping->size   = size;
    mapping->active = 1;
    s_io_mapping_count++;

    populate_offset_table(ea, io, size);
    cellGcmMainMappingWriteEnd();

    return CELL_OK;
}

s32 cellGcmSetFlipCommand(u32 bufferId)
{
    return cellGcmSetFlipCommand_source(bufferId, FRAME_PROF_DIRECT_HLE);
}

/* NID: 0x626E8518
 * Strict-ordering is an RSX mapping attribute; it does not change the guest
 * EA<->IO page-table translation maintained by this HLE runtime.  Tokyo Jungle
 * uses this variant to map a 1 MB notify area at IO 0x0F100000. */
s32 cellGcmMapEaIoAddressWithFlags(u32 ea, u32 io, u32 size, u32 flags)
{
    printf("[cellGcmSys] MapEaIoAddressWithFlags(ea=0x%08X, io=0x%08X, size=0x%X, flags=0x%X)\n",
           ea, io, size, flags);
    return cellGcmMapEaIoAddress(ea, io, size);
}

/* NID: 0xDB23E867 (disambiguation by context) */
s32 cellGcmUnmapEaIoAddress(u32 ea)
{
    cellGcmMainMappingWriteBegin();
    IoMapping* mapping = find_mapping_by_ea(ea);
    if (!mapping) {
        cellGcmMainMappingWriteEnd();
        return CELL_GCM_ERROR_FAILURE;
    }

    printf("[cellGcmSys] UnmapEaIoAddress(ea=0x%08X)\n", ea);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;
    cellGcmMainMappingWriteEnd();

    return CELL_OK;
}

/* NID: 0x3B9BD5BD */
s32 cellGcmUnmapIoAddress(u32 io)
{
    cellGcmMainMappingWriteBegin();
    IoMapping* mapping = find_mapping_by_io(io);
    if (!mapping) {
        cellGcmMainMappingWriteEnd();
        return CELL_GCM_ERROR_FAILURE;
    }

    printf("[cellGcmSys] UnmapIoAddress(io=0x%08X)\n", io);

    clear_offset_table(mapping->ea, mapping->io, mapping->size);
    mapping->active = 0;
    s_io_mapping_count--;
    cellGcmMainMappingWriteEnd();

    return CELL_OK;
}

/* NID: 0xC47D0812 */
s32 cellGcmIoOffsetToAddress(u32 ioOffset, u32* ea)
{
    uint32_t ea_ea = (uint32_t)(uintptr_t)ea;   /* guest out-param */
    if (!ea_ea)
        return CELL_GCM_ERROR_INVALID_VALUE;

    u32 page = ioOffset >> 20;
    AcquireSRWLockShared(&s_main_mapping_lock);
    if (page < 65536 && s_ea_address_table[page] != 0xFFFF) {
        u32 ea_page = s_ea_address_table[page];
        ReleaseSRWLockShared(&s_main_mapping_lock);
        vm_write32(ea_ea, (ea_page << 20) | (ioOffset & 0xFFFFF));
        return CELL_OK;
    }
    ReleaseSRWLockShared(&s_main_mapping_lock);

    printf("[cellGcmSys] WARNING: IoOffsetToAddress failed for 0x%08X\n", ioOffset);
    vm_write32(ea_ea, 0);
    return CELL_GCM_ERROR_FAILURE;
}

/* Resolve an RSX FIFO offset (from a vertex-array / texture / surface method)
 * to a guest effective address the backend can read via vm_base + addr.
 *
 * An RSX offset refers to either IO-mapped *main* memory or *local* video
 * memory, distinguished on hardware by a per-object location bit that the
 * command stream no longer carries by the time the backend sees it. Resolve by
 * table: if the offset's 1MB page is IO-mapped, it's main memory; otherwise it
 * is local video memory (localAddress + offset). IO mappings are sparse (the
 * command/IO window is a few MB) while local objects sit high in the local
 * heap, so this disambiguates every real case — only offsets inside the small
 * IO window are treated as main. */
u32 cellGcmResolveOffset(u32 offset)
{
    u32 page = offset >> 20;
    u32 result;
    AcquireSRWLockShared(&s_main_mapping_lock);
    /* A page the guest derived from a LOCAL EA resolves to VRAM even when the
     * IO table also covers that page number (overlapping offset spaces). */
    if (page < sizeof(s_local_offset_page) && s_local_offset_page[page])
        result = s_config.localAddress + offset;
    else if (page < 65536) {
        u16 ea_page = s_ea_address_table[page];
        if (ea_page != 0 && ea_page != 0xFFFF)
            result = ((u32)ea_page << 20) | (offset & 0xFFFFF);
        else
            result = s_config.localAddress + offset;
    } else
        result = s_config.localAddress + offset;
    ReleaseSRWLockShared(&s_main_mapping_lock);
    return result;
}

/* Location-aware resolve. RSX offsets are TWO overlapping number spaces --
 * LOCAL (VRAM, ea = localAddress + offset) and MAIN (IO-mapped, via the
 * offset table). cellGcmResolveOffset guesses table-first, which breaks when
 * a title's IO region is large enough to cover the same page numbers as its
 * local allocations (gcm/cube: ioSize 16MB, FP ucode at local 0xB90000 --
 * table hit returned the empty command-buffer EA instead of VRAM). Callers
 * that KNOW the location (texture format bits, SET_SHADER_PROGRAM bits) must
 * use this. `local` != 0 -> local memory. An explicit MAIN resolve ignores
 * local-page markers. */
int cellGcmResolveLocatedChecked(int local, u32 offset, u32* out)
{
    if (!out) return 0;
    if (local) {
        *out = s_config.localAddress + offset;
        return 1;
    }
    u16 ea_page = cellGcmMainEaPage(offset >> 20);
    if (ea_page == 0xFFFF) return 0;
    *out = ((u32)ea_page << 20) | (offset & 0xFFFFFu);
    return 1;
}

/* Legacy value-only ABI.  New location-aware callers must use the checked
 * form so a valid EA zero cannot be mistaken for an unmapped MAIN IO page. */
u32 cellGcmResolveLocated(int local, u32 offset)
{
    u32 result = 0;
    (void)cellGcmResolveLocatedChecked(local, offset, &result);
    return result;
}

/* ---------------------------------------------------------------------------
 * Label / report / timestamp
 * -----------------------------------------------------------------------*/

/* NID: 0xF80196C1 */
u32* cellGcmGetLabelAddress(u8 index)
{
    if (index >= CELL_GCM_MAX_LABEL_COUNT) {
        printf("[cellGcmSys] WARNING: GetLabelAddress index %u out of range\n", index);
        return NULL;
    }
    return (u32*)(uintptr_t)(GCM_LABEL_GUEST_BASE + (u32)index * GCM_LABEL_STRIDE);
}

/* NID: 0x8572ADE4 */
CellGcmReportData* cellGcmGetReportDataAddress(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddress index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* NID: 0x97FC4B73 */
u64 cellGcmGetTimeStamp(u32 index)
{
    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    /*
     * On real hardware this reads the RSX timestamp for a given report index.
     * We return the host timestamp in nanoseconds as a reasonable approximation.
     */
    s_report_data[index].timestamp = get_timestamp_ns();
    return s_report_data[index].timestamp;
}

/* ---------------------------------------------------------------------------
 * Tile / Zcull configuration
 * -----------------------------------------------------------------------*/

/* NID: 0x0B4B62D5 */
s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size,
                   u32 pitch, u8 comp, u16 base, u8 bank)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetTile(index=%u, loc=%u, offset=0x%X, size=0x%X, pitch=%u)\n",
           index, location, offset, size, pitch);

    gcm_identity_write_begin();
    s_tiles[index].offset = offset;
    s_tiles[index].size   = size;
    s_tiles[index].pitch  = pitch;
    s_tiles[index].format = comp;
    s_tiles[index].base   = base;
    s_tiles[index].bank   = bank;
    s_tiles[index].bound  = 0;

    /* Build tile register value (simplified) */
    s_tiles[index].tile  = (location << 0) | (pitch << 8);
    s_tiles[index].limit = offset + size - 1;
    gcm_identity_write_end();

    return CELL_OK;
}

/* NID: 0x06EDEA25 */
s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height,
                    u32 cullStart, u32 zFormat, u32 aaFormat,
                    u32 zcullDir, u32 zcullFormat,
                    u32 sFunc, u32 sRef, u32 sMask)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetZcull(index=%u, offset=0x%X, %ux%u)\n",
           index, offset, width, height);

    s_zcull[index].offset      = offset;
    s_zcull[index].width       = width;
    s_zcull[index].height      = height;
    s_zcull[index].cullStart   = cullStart;
    s_zcull[index].zFormat     = zFormat;
    s_zcull[index].aaFormat    = aaFormat;
    s_zcull[index].zcullDir    = zcullDir;
    s_zcull[index].zcullFormat = zcullFormat;
    s_zcull[index].sFunc       = sFunc;
    s_zcull[index].sRef        = sRef;
    s_zcull[index].sMask       = sMask;
    s_zcull[index].bound       = 0;

    return CELL_OK;
}

/* NID: 0x2BB1F1E5 */
s32 cellGcmBindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindTile(index=%u)\n", index);
    gcm_identity_write_begin();
    s_tiles[index].bound = 1;
    gcm_identity_write_end();
    const u32 location = s_tiles[index].tile & UINT32_C(0xFF);
    if (rsx_boundary_should_arm_survival_profile(
            index, location, s_tiles[index].offset, s_tiles[index].size,
            s_tiles[index].pitch) &&
        !atomic_exchange_explicit(&s_tj_survival_profile_armed, true,
                                  memory_order_acq_rel)) {
        s_tj_survival_flip_diag = tj_survival_flip_trace_enabled();
#ifdef _WIN32
        extern void tj_spu_geometry_trace_arm_scene(void);
        tj_spu_geometry_trace_arm_scene();
        rsx_d3d12_arm_survival_trace();
#endif
    }
    return CELL_OK;
}

/* NID: 0x1F61F9D3 */
s32 cellGcmBindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] BindZcull(index=%u)\n", index);
    s_zcull[index].bound = 1;
    return CELL_OK;
}

/* NID: 0x75843042 */
s32 cellGcmUnbindTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindTile(index=%u)\n", index);
    gcm_identity_write_begin();
    s_tiles[index].bound = 0;
    gcm_identity_write_end();
    return CELL_OK;
}

/* NID: 0xA093BE1C */
s32 cellGcmUnbindZcull(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] UnbindZcull(index=%u)\n", index);
    s_zcull[index].bound = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Misc
 * -----------------------------------------------------------------------*/

/* NID: 0x107BF789
 * SDK prototype (cell/gcm.h): uint32_t cellGcmGetTiledPitchSize(const uint32_t size)
 * -- returns the pitch BY VALUE in r3 (no out-pointer!). The old signature here
 * took (size, u32* pitch-out) and returned a status code, so every caller got 0
 * / an error code as its "pitch" -- gcm/cube's `bne` on the result then skipped
 * its entire config+framebuffer-alloc block (GetConfiguration never ran, local
 * memory base stayed 0, AddressToOffset(0) auto-mapped a bogus IO alias, and the
 * FIFO teleported into it). Returns 0 only for size > max, like hardware. */
u32 cellGcmGetTiledPitchSize(u32 size)
{
    u32 r = 0;
    /* Smallest valid tiled pitch >= size (RSX supports only specific pitches). */
    if (size) {
        for (int i = 0; i < s_valid_pitch_count; i++) {
            if (s_valid_pitches[i] >= size) { r = s_valid_pitches[i]; break; }
        }
    }
    { static int _n=0; if (_n++<4) fprintf(stderr, "[cellGcmSys] GetTiledPitchSize(0x%X) -> 0x%X\n", size, r); }
    return r;
}

/* NID: 0xBC982946 */
void cellGcmSetDebugOutputLevel(u32 level)
{
    printf("[cellGcmSys] SetDebugOutputLevel(level=%u)\n", level);
    s_debug_level = level;
}

/* ---------------------------------------------------------------------------
 * Additional functions needed by Tokyo Jungle
 * -----------------------------------------------------------------------*/

/* Notify data lives in main memory mapped at RSX IO page 0xF1.  The firmware
 * function returns a 32-bit GUEST effective address, not a host C pointer.
 * Each notify slot occupies 0x40 bytes. */
u32 cellGcmGetNotifyDataAddress(u32 index)
{
    const u32 io_page = 0x0F100000u >> 20;
    AcquireSRWLockShared(&s_main_mapping_lock);
    const u16 ea_page = s_ea_address_table[io_page];
    u32 result = (ea_page == 0xFFFF || index >= (0x100000u / 0x40u))
        ? 0 : ((u32)ea_page << 20) + index * 0x40u;
    ReleaseSRWLockShared(&s_main_mapping_lock);
    return result;
}

/* Timestamp read from local or IO-mapped main report memory.
 * The real ABI returns u64 and takes location by value. */
u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
    if (location == CELL_GCM_LOCATION_MAIN) {
        const u64 io64 = 0x0E000000ull + (u64)index * sizeof(CellGcmReportData);
        if (io64 > UINT32_MAX)
            return 0;

        const u32 io = (u32)io64;
        const u32 page = io >> 20;
        if (page >= 65536)
            return 0;
        AcquireSRWLockShared(&s_main_mapping_lock);
        const u16 ea_page = s_ea_address_table[page];
        const u32 ea = ea_page == 0xFFFF ? 0 : ((u32)ea_page << 20) |
                       (io & 0xFFFFF);
        ReleaseSRWLockShared(&s_main_mapping_lock);
        if (!ea) return 0;
        return vm_read64(ea);
    }

    return cellGcmGetTimeStamp(index);
}

/* SetTileInfo — alternative to SetTile with same parameters */
s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size,
                       u32 pitch, u8 comp, u16 base, u8 bank)
{
    /* Delegates to existing SetTile */
    return cellGcmSetTile(index, location, offset, size, pitch, comp, base, bank);
}

/* Default FIFO size — configures command buffer size before init */
static u32 s_default_fifo_size = 0x40000; /* 256KB default */

s32 cellGcmSetDefaultFifoSize(u32 size)
{
    printf("[cellGcmSys] SetDefaultFifoSize(0x%X)\n", size);
    s_default_fifo_size = size;
    return CELL_OK;
}

/* Internal flip commands — called directly by some games */
/* libgcm's internal exports pass the command context FIRST -- taking
 * bufferId as arg1 made the range check eat the context pointer, so the
 * flip counter never advanced (wave: presents free-ran 4x per frame,
 * layout flashing/zooming). */
/* NID 0x21397818 is gcmSetFlipCommand (PSL1GHT libgcm_sys exports.h) -- it
 * was misassigned to cellGcmGetLabelAddress (real NID 0xF80196C1), so every
 * import-based flip (LBP: cellGcmSetFlip -> stub 0x7482FC(ctx, id)) landed
 * in the label getter and no flip was EVER issued: the intro-movie pump ran,
 * drew, "flipped" into the wrong function, and the screen stayed black. */
/* NID: 0x21397818 */
s32 _cellGcmSetFlipCommand(void* ctx, u32 bufferId)
{
    /* This internal ABI appends to the supplied guest context; the SDK caller
     * reserves space and later flushes PUT. Publishing an immediate host flip
     * races the still-undrained UI tail against the next frame's clears.
     * Keep the two-word command size, using the walker's existing flip marker
     * plus NOP. Only consuming that marker publishes/completes the flip. */
    const uintptr_t address = (uintptr_t)ctx;
    if (!vm_base || !address || address > UINT32_MAX - 15u || (address & 3u) ||
        bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM ||
        !s_display_buffer_set[bufferId])
        return CELL_GCM_ERROR_INVALID_VALUE;
    const u32 context_ea = (u32)address;
    const u32 begin = vm_read32(context_ea);
    const u32 end = vm_read32(context_ea + 4u);
    const u32 current = vm_read32(context_ea + 8u);
    if ((current & 3u) || current < begin || current > end || end - current < 8u)
        return CELL_GCM_ERROR_FAILURE;
    vm_write32(current, 0xFEAD0000u | bufferId);
    vm_write32(current + 4u, 0u);
    vm_write32(context_ea + 8u, current + 8u);
    return CELL_OK;
}

/* NID: 0xD8F88E1A */
s32 _cellGcmSetFlipCommandWithWaitLabel(void* ctx, u32 bufferId,
                                        u32 labelIndex, u32 labelValue)
{
    /* The caller reserves and publishes this context. Queue OFFSET+ACQUIRE
     * before the flip; neither the awaited label nor host flip changes here. */
    const uintptr_t address = (uintptr_t)ctx;
    if (!vm_base || !address || address > UINT32_MAX - 15u || (address & 3u) ||
        bufferId >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM ||
        !s_display_buffer_set[bufferId] || labelIndex >= CELL_GCM_MAX_LABEL_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;
    const u32 context_ea = (u32)address;
    const u32 begin = vm_read32(context_ea);
    const u32 end = vm_read32(context_ea + 4u);
    const u32 current = vm_read32(context_ea + 8u);
    if ((current & 3u) || current < begin || current > end || end - current < 20u)
        return CELL_GCM_ERROR_FAILURE;
    vm_write32(current, 0x00080064u);
    vm_write32(current + 4u, labelIndex * GCM_LABEL_STRIDE);
    vm_write32(current + 8u, labelValue);
    vm_write32(current + 12u, 0xFEAD0000u | bufferId);
    vm_write32(current + 16u, 0u);
    vm_write32(context_ea + 8u, current + 20u);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Additional functions (RPCS3 parity)
 * -----------------------------------------------------------------------*/

/* FIFO command buffer callback — called when put pointer wraps.
 * In recomp we don't have a real GPU consuming FIFO, so this is a no-op. */
s32 cellGcmCallback(void* context, u32 count)
{
    (void)context;
    (void)count;
    return CELL_OK;
}

/* Map RSX local memory — returns the base address and size of local VRAM */
s32 cellGcmMapLocalMemory(u32* address, u32* size)
{
    if (!address || !size)
        return CELL_GCM_ERROR_INVALID_VALUE;

    *address = s_config.localAddress;
    *size    = s_config.localSize;

    printf("[cellGcmSys] MapLocalMemory(address=0x%08X, size=0x%X)\n",
           *address, *size);
    return CELL_OK;
}

/* Shutdown RSX — clear all state */
void cellGcmTerminate(void)
{
    printf("[cellGcmSys] Terminate\n");

    AcquireSRWLockExclusive(&s_gcm_fifo_lock);
    cellGcmMainMappingWriteBegin();

    s_gcm_initialized = 0;
    s_fifo_semaphore_offset = 0;
    s_fifo_semaphore_dma = 0x66606660u;
    s_fifo_semaphore_wait_since_ns = 0;
    s_flip_mode   = CELL_GCM_DISPLAY_VSYNC;
    s_tj_survival_flip_diag = 0;
    atomic_store_explicit(&s_tj_survival_profile_armed, false,
                          memory_order_release);
    flip_status_store(CELL_GCM_FLIP_STATUS_DONE);
    s_debug_level = CELL_GCM_DEBUG_LEVEL0;
    s_vblank_count = 0;
    s_io_map_reserved = 0;
    s_default_fifo_mode = 0;
    s_user_command = 0;

    gcm_identity_write_begin();
    memset(s_display_buffers, 0, sizeof(s_display_buffers));
    memset(s_display_buffer_set, 0, sizeof(s_display_buffer_set));
    memset(&s_config, 0, sizeof(s_config));
    memset(&s_control, 0, sizeof(s_control));
    memset(s_io_mappings, 0, sizeof(s_io_mappings));
    memset(s_tiles, 0, sizeof(s_tiles));
    gcm_identity_write_end();
    memset(s_zcull, 0, sizeof(s_zcull));
    memset(s_report_data, 0, sizeof(s_report_data));
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));

    s_flip_handler     = NULL;
    s_vblank_handler   = NULL;
    s_second_v_handler = NULL;
    s_flip_handler_opd = 0;
    s_vblank_handler_opd = 0;
    s_second_v_handler_opd = 0;
    s_flip_handler_code = 0;
    s_vblank_handler_code = 0;
    gcm_user_interrupt_reset_lifetime();
    s_graphics_handler = NULL;
    s_queue_handler    = NULL;
    s_local_mem_allocated = 0;
    s_io_mapping_count = 0;
    /* Unmapped = 0xFFFF. The tables are static (zero-initialized), so without
     * this every unmapped EA page silently aliased io page 0 (and vice versa),
     * producing plausible-but-wrong offsets instead of a translation failure. */
    memset(s_io_address_table, 0xFF, sizeof(s_io_address_table));
    memset(s_ea_address_table, 0xFF, sizeof(s_ea_address_table));
    memset(s_local_offset_page, 0, sizeof(s_local_offset_page));
    cellGcmMainMappingWriteEnd();
    gcm_flip_publication_reset();
    s_last_flip_time = 0;
    frame_prof_reset();
    tj_flip_perf_reset();
    ReleaseSRWLockExclusive(&s_gcm_fifo_lock);
    gcm_flip_status_done_notify();      /* status reset to DONE above */
    gcm_wait_signal(GCM_WAIT_CH_FIFO);
}

/* Return remaining IO map space (256MB total IO space minus already mapped) */
u32 cellGcmGetMaxIoMapSize(void)
{
    /* RSX has a 256MB IO address window */
    u32 total_io = 256 * 1024 * 1024;
    u32 used = 0;

    for (int i = 0; i < CELL_GCM_MAX_IO_MAPPINGS; i++) {
        if (s_io_mappings[i].active)
            used += s_io_mappings[i].size;
    }

    return (used < total_io) ? (total_io - used) : 0;
}

/* Reserve IO map space (pre-allocation for future mappings) */
s32 cellGcmReserveIoMapSize(u32 size)
{
    u32 max_size = cellGcmGetMaxIoMapSize();
    if (size > max_size - s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved += size;
    return CELL_OK;
}

/* Unreserve IO map space */
s32 cellGcmUnreserveIoMapSize(u32 size)
{
    if (size > s_io_map_reserved)
        return CELL_GCM_ERROR_FAILURE;

    s_io_map_reserved -= size;
    return CELL_OK;
}

/* Return incrementing VBlank counter */
u32 cellGcmGetVBlankCount(void)
{
    /* Approximate: increment each time queried (games use this for timing) */
    return s_vblank_count++;
}

/* Return tile info for a given index */
CellGcmTileInfo* cellGcmGetTileInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT) {
        printf("[cellGcmSys] WARNING: GetTileInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_tiles[index];
}

/* Return zcull info for a given index */
CellGcmZcullInfo* cellGcmGetZcullInfo(u8 index)
{
    if (index >= CELL_GCM_MAX_ZCULL_COUNT) {
        printf("[cellGcmSys] WARNING: GetZcullInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_zcull[index];
}

/* Return display buffer info by index */
CellGcmDisplayInfo* cellGcmGetDisplayInfo(u32 index)
{
    if (index >= CELL_GCM_MAX_DISPLAY_BUFFER_NUM) {
        printf("[cellGcmSys] WARNING: GetDisplayInfo index %u out of range\n", index);
        return NULL;
    }
    return &s_display_buffers[index];
}

/* Set default FIFO mode (before init) */
void cellGcmInitDefaultFifoMode(s32 mode)
{
    printf("[cellGcmSys] InitDefaultFifoMode(mode=%d)\n", mode);
    s_default_fifo_mode = mode;
}

/* Set command buffer to defaults (reset put/get pointers) */
void cellGcmSetDefaultCommandBuffer(void)
{
    printf("[cellGcmSys] SetDefaultCommandBuffer\n");
    s_control.put = 0;
    s_control.get = 0;
    s_control.ref = 0;
}

/* Debug dump — no-op in recomp */
void cellGcmDumpGraphicsError(void)
{
    printf("[cellGcmSys] DumpGraphicsError (no-op)\n");
}

/* Default command word size: 0x400 (1024) words */
u32 cellGcmGetDefaultCommandWordSize(void)
{
    return 0x400;
}

/* Default segment word size: 0x100 (256) words */
u32 cellGcmGetDefaultSegmentWordSize(void)
{
    return 0x100;
}

/* Immediate flip — perform flip right now */
s32 cellGcmSetFlipImmediate(u32 bufferId)
{
    printf("[cellGcmSys] SetFlipImmediate(bufferId=%u)\n", bufferId);
    return cellGcmSetFlipCommand(bufferId);
}

/* Set flip status directly */
void cellGcmSetFlipStatus(u32 status)
{
    flip_status_store(status);
    if (status != CELL_GCM_FLIP_STATUS_WAITING) gcm_flip_status_done_notify();
}

/* Store graphics handler callback */
void cellGcmSetGraphicsHandler(CellGcmGraphicsHandler handler)
{
    printf("[cellGcmSys] SetGraphicsHandler(%p)\n", (void*)(size_t)handler);
    s_graphics_handler = handler;
}

/* Store queue handler callback */
void cellGcmSetQueueHandler(CellGcmQueueHandler handler)
{
    printf("[cellGcmSys] SetQueueHandler(%p)\n", (void*)(size_t)handler);
    s_queue_handler = handler;
}

/* Set second V frequency */
void cellGcmSetSecondVFrequency(u32 freq)
{
    printf("[cellGcmSys] SetSecondVFrequency(%u)\n", freq);
    s_second_v_frequency = freq;
}

/* Set VBlank frequency */
void cellGcmSetVBlankFrequency(u32 freq)
{
    printf("[cellGcmSys] SetVBlankFrequency(%u)\n", freq);
    s_vblank_frequency = freq;
}

/* Store user command value */
void cellGcmSetUserCommand(u32 cmd)
{
    s_user_command = cmd;
}

/* Invalidate a tile region (unbind + clear) */
s32 cellGcmSetInvalidateTile(u8 index)
{
    if (index >= CELL_GCM_MAX_TILE_COUNT)
        return CELL_GCM_ERROR_INVALID_VALUE;

    printf("[cellGcmSys] SetInvalidateTile(index=%u)\n", index);

    gcm_identity_write_begin();
    s_tiles[index].bound = 0;
    memset(&s_tiles[index], 0, sizeof(CellGcmTileInfo));
    gcm_identity_write_end();
    return CELL_OK;
}

/* EA IO address remap — stub, sorting not needed in recomp */
void cellGcmSortRemapEaIoAddress(void)
{
    /* No-op: IO address remapping not required for recomp */
}

/* Report data with location parameter */
CellGcmReportData* cellGcmGetReportDataAddressLocation(u32 index, u32 location)
{
    (void)location;  /* We only have one report area */

    if (index >= CELL_GCM_MAX_REPORT_COUNT) {
        printf("[cellGcmSys] WARNING: GetReportDataAddressLocation index %u out of range\n", index);
        return NULL;
    }
    return &s_report_data[index];
}

/* Get report value at index with location */
u32 cellGcmGetReportDataLocation(u32 index, u32 location)
{
    (void)location;

    if (index >= CELL_GCM_MAX_REPORT_COUNT)
        return 0;

    return s_report_data[index].value;
}
