/*
 * ps3recomp - cellSpurs HLE implementation
 *
 * Provides the SPURS management API so games can call SPU task/workload
 * functions without crashing.  Full SPU execution requires recompiling
 * SPU programs; this layer provides the scheduling and management APIs.
 *
 * Tasks and workloads are tracked.  If a game provides PPU fallback
 * callbacks, those can be invoked through the task submission path.
 */

#include "cellSpurs.h"
#include "../../runtime/ps3_log.h"
#include "spu_workload.h"   /* SPU image -> lifted-entry dispatch (runtime/spu) */
#include "spurs_taskset.h"  /* REAL BE CellSpursTaskset layout builders (fork Option-B) */
#include "jobchain_run_gate.h"
#include "jobguard_line_gate.h"
#include "jobchain_joblist.h"
#include "../../runtime/spu/spu_reservation.h"
#include "../../runtime/memory/guest_world_rendezvous.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base (guest mem) */
#include "../../runtime/ppu/ppu_context.h"  /* active guest thread id for diagnostics */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif

static int spurs_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("PS3_SPURS_TRACE") ? 1 : 0;
    return enabled;
}

extern int sys_event_queue_push_by_id(uint32_t queue_id,
                                      uint64_t source, uint64_t data1,
                                      uint64_t data2, uint64_t data3);

/* Late-armed SPU atomic diagnostic.  The Survival completion wait happens
 * after hundreds of ordinary event-flag handshakes, so an environment-wide
 * atomic trace is both noisy and expensive.  The RSX marker is armed while
 * entering Survival, before this renderer's first 0x0100/0x0200 handshake. */
extern volatile uint32_t g_spu_event_flag_watch_ea;
extern volatile long g_rsx_survival_trace_armed;

static int tj_survival_trace_is_armed(void)
{
#ifdef _WIN32
    return InterlockedCompareExchange(&g_rsx_survival_trace_armed, 0, 0) != 0;
#else
    return __atomic_load_n(&g_rsx_survival_trace_armed, __ATOMIC_ACQUIRE) != 0;
#endif
}

/* Tokyo Jungle late-Survival diagnostic.  The watch EA is published only
 * after the failing completion waiter exists, so this adds no startup trace.
 * Windows sequence allocation is interlocked because PPU and job-chain host
 * threads can emit the same capture concurrently. */
static int tj_late_job_trace_requested(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TJ_LATE_JOB_TRACE") ? 1 : 0;
    return enabled;
}

static int tj_event_flag_trace_requested(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TJ_EVENTFLAG_TRACE") ? 1 : 0;
    return enabled;
}

static int tj_late_job_trace_enabled(void)
{
    return tj_late_job_trace_requested() &&
           g_spu_event_flag_watch_ea != 0;
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

static long tj_late_job_trace_load(volatile long* value)
{
    return _InterlockedCompareExchange(value, 0, 0);
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

static long tj_late_job_trace_load(volatile long* value)
{
    return *value;
}
#endif

/* Bridge the real (BE) taskset EA + selected taskId from CreateTask to the image-22
 * SPU dispatch (spu_workload.c), so spurs_pm_build_context can build the leaf's
 * SpursTasksetContext from the real taskset. Set right before dispatch_async (the PPU
 * create path is sequential here). Gated by YDKJ_REAL_TASKSET in the dispatch. */
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid     = 0;
uint32_t g_ydkj_real_spurs_ea   = 0;   /* real CellSpurs instance EA (for the taskset-policy handoff) */

/* Generic HLE adapter passes GUEST addresses; translate pointer args. CellSpurs
 * is treated opaquely by the game (passed back as a handle), so translating the
 * pointer is enough here. */
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#ifdef _WIN32
static volatile LONG s_tj_late_eventflag_receive_reported;
static uint64_t tj_late_eventflag_now_ms(void) { return (uint64_t)GetTickCount64(); }
static int tj_late_eventflag_take_receive_report(void)
{
    return InterlockedCompareExchange(&s_tj_late_eventflag_receive_reported, 1, 0) == 0;
}
#else
static volatile int s_tj_late_eventflag_receive_reported;
static uint64_t tj_late_eventflag_now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
static int tj_late_eventflag_take_receive_report(void)
{
    return __atomic_exchange_n(&s_tj_late_eventflag_receive_reported, 1,
                               __ATOMIC_ACQ_REL) == 0;
}
#endif

static void tj_late_eventflag_report(const char* state, uint32_t ea,
                                     u16 pattern, uint64_t started_ms,
                                     const TJLateEventFlagCounters* baseline);

/* Opt-in, early-safe timing diagnostic for every PPU EventFlag wait.  This is
 * deliberately separate from the Survival-only producer trace: menu pacing
 * can be dominated by host timeout granularity before Survival is armed. */
#ifdef _WIN32
static int tj_event_flag_wait_perf_requested(void)
{
    /* 0=uninitialized, 1=initializing, 2=disabled, 3=enabled. */
    static volatile LONG state = 0;
    LONG observed = InterlockedCompareExchange(&state, 1, 0);
    if (observed == 0) {
        int enabled = getenv("TJ_EVENTFLAG_WAIT_PERF") ? 1 : 0;
        InterlockedExchange(&state, enabled ? 3 : 2);
        return enabled;
    }
    while ((observed = InterlockedCompareExchange(&state, 0, 0)) == 1)
        Sleep(0);
    return observed == 3;
}

static uint64_t tj_event_flag_wait_qpc_us(LONGLONG begin, LONGLONG end,
                                           LONGLONG frequency)
{
    if (end <= begin || frequency <= 0) return 0;
    uint64_t delta = (uint64_t)(end - begin);
    uint64_t freq = (uint64_t)frequency;
    return (delta / freq) * 1000000ull +
           ((delta % freq) * 1000000ull) / freq;
}
#else
static pthread_once_t s_tj_event_flag_wait_perf_once = PTHREAD_ONCE_INIT;
static int s_tj_event_flag_wait_perf_enabled;

static void tj_event_flag_wait_perf_initialize(void)
{
    s_tj_event_flag_wait_perf_enabled =
        getenv("TJ_EVENTFLAG_WAIT_PERF") ? 1 : 0;
}

static int tj_event_flag_wait_perf_requested(void)
{
    pthread_once(&s_tj_event_flag_wait_perf_once,
                 tj_event_flag_wait_perf_initialize);
    return s_tj_event_flag_wait_perf_enabled;
}
#endif

/* TJ_JOBGUARD_TRACE: lock-free per-PPU-thread breadcrumbs. The thread sampler
 * reads these from a different host thread, so the suspected worker never has
 * to acquire stdio/debug locks while we determine the exact blocking stage. */
#define TJ_JOBGUARD_TRACE_SLOTS 64
volatile long g_tj_jobguard_stage[TJ_JOBGUARD_TRACE_SLOTS];
volatile u32  g_tj_jobguard_guard[TJ_JOBGUARD_TRACE_SLOTS];
volatile u32  g_tj_jobguard_old[TJ_JOBGUARD_TRACE_SLOTS];
volatile u32  g_tj_jobguard_zero[TJ_JOBGUARD_TRACE_SLOTS];
volatile u32  g_tj_jobguard_chain[TJ_JOBGUARD_TRACE_SLOTS];
#ifdef _WIN32
extern __declspec(thread) ppu_context* g_active_ctx;
#else
extern __thread ppu_context* g_active_ctx;
#endif
extern void ppu_poll_display_callbacks(ppu_context* ctx);

static unsigned tj_jobguard_trace_slot(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TJ_JOBGUARD_TRACE") ? 1 : 0;
    if (!enabled || !g_active_ctx || g_active_ctx->thread_id >= TJ_JOBGUARD_TRACE_SLOTS)
        return TJ_JOBGUARD_TRACE_SLOTS;
    return (unsigned)g_active_ctx->thread_id;
}

static void tj_jobguard_trace_stage(unsigned slot, long stage)
{
    if (slot >= TJ_JOBGUARD_TRACE_SLOTS) return;
#ifdef _WIN32
    InterlockedExchange(&g_tj_jobguard_stage[slot], stage);
#else
    g_tj_jobguard_stage[slot] = stage;
#endif
}

/* ---------------------------------------------------------------------------
 * Internal workload tracking
 * -----------------------------------------------------------------------*/

#define MAX_SPURS_INST 4

typedef enum {
    SPURS_WORKLOAD_POLICY = 0,
    SPURS_WORKLOAD_TASKSET,
} SpursWorkloadKind;

typedef struct {
    int         in_use;
    SpursWorkloadKind kind;
    const void* pm;            /* policy-module image EA (guest) */
    u32         sizePm;
    u64         data;          /* workload data (e.g. joblist EA) */
    u32         spurs_ea;      /* owning CellSpurs instance EA */
    u8          priority[CELL_SPURS_MAX_SPU];
    u32         minContention;
    u32         maxContention;
    u32         nameClass_ea;
    u32         nameInstance_ea;
} SpursWorkload;

typedef struct {
    spu_lifted_entry_fn fn;
    int                 image_id;
    int                 resolved;   /* 0=not tried, 1=found, -1=missing */
} WklPm;

typedef struct {
    int         in_use;
    u32         id;
    u32         taskset_ea;
    int         active;
    int         completed;
    s32         exitCode;
    void*       entryPoint;
    u64         argA;
} SpursTask;

/* Host bookkeeping is keyed by the owning CellSpurs instance, matching the
 * guest's per-instance WID namespace. */
static SpursWorkload s_workloads[MAX_SPURS_INST][CELL_SPURS_MAX_WORKLOAD];
static WklPm         s_wkl_pm[MAX_SPURS_INST][CELL_SPURS_MAX_WORKLOAD];
static SpursTask     s_tasks[CELL_SPURS_MAX_TASK];

/* ---------------------------------------------------------------------------
 * Event flag sync side table
 *
 * The CellSpursEventFlag itself is 128 bytes of GAME-owned guest memory in
 * the REAL big-endian kernel layout (see EF_* offsets below) — the SPU side
 * (lifted task-library code) reads/writes it with DMA + atomics, so the PPU
 * HLE must operate on the same guest bytes, never a host struct. This side
 * table (keyed by guest EA) only adds the host mutex + condvar used to block
 * and wake PPU waiters.
 * -----------------------------------------------------------------------*/
#define MAX_EVENT_FLAGS 64

typedef struct {
    uint32_t            ea;      /* guest EA of the 128-byte flag */
#ifdef _WIN32
    CRITICAL_SECTION    cs;
    CONDITION_VARIABLE  cv;
#else
    pthread_mutex_t     mtx;
    pthread_cond_t      cond;
#endif
    int                 initialized;
} EventFlagSync;

static EventFlagSync s_ef_sync[MAX_EVENT_FLAGS];

static EventFlagSync* ef_sync_find(uint32_t ea)
{
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (s_ef_sync[i].initialized && s_ef_sync[i].ea == ea)
            return &s_ef_sync[i];
    }
    return NULL;
}

static EventFlagSync* ef_sync_alloc(uint32_t ea)
{
    for (int i = 0; i < MAX_EVENT_FLAGS; i++) {
        if (!s_ef_sync[i].initialized) {
            s_ef_sync[i].ea = ea;
            s_ef_sync[i].initialized = 1;
#ifdef _WIN32
            InitializeCriticalSection(&s_ef_sync[i].cs);
            InitializeConditionVariable(&s_ef_sync[i].cv);
#else
            pthread_mutex_init(&s_ef_sync[i].mtx, NULL);
            pthread_cond_init(&s_ef_sync[i].cond, NULL);
#endif
            return &s_ef_sync[i];
        }
    }
    return NULL;
}

/* Lenient lookup: a flag touched before we saw its Initialize (alternate init
 * paths, e.g. taskset2) still gets a sync slot instead of an error. */
static EventFlagSync* ef_sync_get(uint32_t ea)
{
    EventFlagSync* s = ef_sync_find(ea);
    if (!s) {
        s = ef_sync_alloc(ea);
        if (s)
            fprintf(stderr, "[cellSpurs] event flag 0x%08X used before Initialize "
                            "-- sync slot auto-allocated\n", ea);
    }
    return s;
}

static void ef_sync_free(EventFlagSync* sync)
{
    if (!sync) return;
#ifdef _WIN32
    DeleteCriticalSection(&sync->cs);
    /* CONDITION_VARIABLE has no destroy on Windows */
#else
    pthread_mutex_destroy(&sync->mtx);
    pthread_cond_destroy(&sync->cond);
#endif
    sync->ea = 0;
    sync->initialized = 0;
}

static inline void ef_lock(EventFlagSync* s)
{
#ifdef _WIN32
    EnterCriticalSection(&s->cs);
#else
    pthread_mutex_lock(&s->mtx);
#endif
}

static inline void ef_unlock(EventFlagSync* s)
{
#ifdef _WIN32
    LeaveCriticalSection(&s->cs);
#else
    pthread_mutex_unlock(&s->mtx);
#endif
}

/* TJ_EF_SPIN_US (default 0): before its first sleep, the main PPU thread polls
 * the flag's pending byte for up to this many microseconds. Job batches it
 * waits on finish in ~0.1 ms, so this skips the condition-variable wake-up
 * latency (~15 us, ~55 waits per frame). Costs CPU only while waiting. */
static unsigned tj_ef_spin_us(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("TJ_EF_SPIN_US");
        int n = e && *e ? atoi(e) : 0;
        v = n < 0 ? 0 : n > 2000 ? 2000 : n;
        if (v) fprintf(stderr, "[tj-ef] main-thread spin before sleep: %d us\n", v);
    }
    return (unsigned)v;
}

/* Returns 1 if signaled, 0 if it timed out after `ms`. */
static inline int ef_wait_timed(EventFlagSync* s, unsigned ms)
{
#ifdef _WIN32
    return SleepConditionVariableCS(&s->cv, &s->cs, ms) ? 1 : 0;
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&s->cond, &s->mtx, &ts) == 0 ? 1 : 0;
#endif
}

static inline void ef_broadcast(EventFlagSync* s)
{
#ifdef _WIN32
    WakeAllConditionVariable(&s->cv);
#else
    pthread_cond_broadcast(&s->cond);
#endif
}

/* ---------------------------------------------------------------------------
 * REAL CellSpursEventFlag guest layout (128 bytes, big-endian; RPCS3
 * cellSpurs.h is the reference). The SPU task library manipulates this exact
 * layout with GETLLAR/PUTLLC, so every PPU-side access goes through vm_*.
 * -----------------------------------------------------------------------*/
#define EF_EVENTS            0x00u  /* be u16: the event bits */
#define EF_SPU_PENDING_RECV  0x02u  /* be u16: slot bits with met conditions */
#define EF_PPU_WAIT_MASK     0x04u  /* be u16: blocked PPU thread's mask */
#define EF_PPU_WAIT_SLOTMODE 0x06u  /* u8: hi4 = wait slot, lo4 = wait mode */
#define EF_PPU_PENDING_RECV  0x07u  /* u8: 1 when the PPU waiter's cond met */
#define EF_SPU_USED_SLOTS    0x08u  /* be u16: wait slots in use (bit 0x8000>>s) */
#define EF_SPU_WAIT_MODE     0x0Au  /* be u16: per-slot mode (1 = AND) */
#define EF_SPU_PORT          0x0Cu  /* u8 */
#define EF_IS_IWL            0x0Du  /* u8: addr is a wkl instead of taskset */
#define EF_DIRECTION         0x0Eu  /* u8: CELL_SPURS_EVENT_FLAG_* direction */
#define EF_CLEAR_MODE        0x0Fu  /* u8: 0 = AUTO, 1 = MANUAL */
#define EF_SPU_WAIT_MASK_ARR 0x10u  /* be u16 [16]: per-slot wait masks */
#define EF_PENDING_RECV_EVT  0x30u  /* be u16 [16]: events handed to waiters */
#define EF_WAITING_TASK_ID   0x50u  /* u8 [16]: waiting task ids */
#define EF_WAITING_WKL_ID    0x60u  /* u8 [16]: waiting workload ids */
#define EF_ADDR              0x70u  /* be u64: taskset EA (isIwl=0) */
#define EF_EVENT_PORT_ID     0x78u  /* be u32 */
#define EF_EVENT_QUEUE_ID    0x7Cu  /* be u32 */
#define EF_GUEST_SIZE        0x80u

/* First eight control bytes, interpreted in guest big-endian order. PPU-side
 * changes must use the same reservation lock as SPU GETLLAR/PUTLLC; ordinary
 * vm_write* calls can otherwise race a lifted SPU Set and lose completions. */
#define EF_CTRL_EVENTS_MASK       0xFFFF000000000000ull
#define EF_CTRL_PPU_WAIT_MASK     0x00000000FFFF0000ull
#define EF_CTRL_PPU_SLOT_MODE     0x000000000000FF00ull
#define EF_CTRL_PPU_PENDING       0x00000000000000FFull

static inline u16 ef_ctrl_events(u64 ctrl)    { return (u16)(ctrl >> 48); }
static inline u16 ef_ctrl_ppu_mask(u64 ctrl)  { return (u16)(ctrl >> 16); }
static inline u8  ef_ctrl_slot_mode(u64 ctrl) { return (u8)(ctrl >> 8); }
static inline u8  ef_ctrl_pending(u64 ctrl)   { return (u8)ctrl; }

static inline int ef_wait_satisfied(u16 events, u16 pattern, u32 mode)
{
    u16 relevant = (u16)(events & pattern);
    return (pattern & (u16)~relevant) == 0 ||
           (mode == CELL_SPURS_EVENT_FLAG_OR && relevant != 0);
}

/* Core Set protocol on the guest struct (sync lock held). Mirrors the real
 * kernel/RPCS3 semantics for SPU-task wait slots. Lifted SPU Set code handles
 * registered PPU waiters directly through the shared first-eight-byte control
 * word; cellSpursEventFlagWait registers that waiter below. */
static void spurs_ef_set_locked(uint32_t ea, u16 bits)
{
    uint32_t wake_taskset[CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS] = {0};
    u32 wake_task[CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS] = {0};
    unsigned wake_count = 0;

    /* Lifted SPU GETLLAR/PUTLLC and PPU Wait's control-word CAS use the
     * reservation lock.  Keep the complete 128-byte Set transaction under
     * that same lock so an SPU completion cannot be overwritten between one
     * of these reads and its matching write. */
    spu_reservation_lock();
    u16 events        = vm_read16(ea + EF_EVENTS);
    u16 used          = (u16)(vm_read16(ea + EF_SPU_USED_SLOTS) &
                              ~vm_read16(ea + EF_SPU_PENDING_RECV));
    u16 waitmode      = vm_read16(ea + EF_SPU_WAIT_MODE);
    u16 eventsToClear = 0;
    u16 pendingRecv   = 0;

    for (int s = 0; s < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS; s++) {
        u16 bit = (u16)(0x8000u >> s);
        if (!(used & bit)) continue;
        u16 mask = vm_read16(ea + EF_SPU_WAIT_MASK_ARR + 2u * s);
        u16 rel  = (u16)((events | bits) & mask);
        int mode_and = (waitmode & bit) != 0;
        if ((mask & ~rel) == 0 || (!mode_and && rel != 0)) {
            eventsToClear |= rel;
            pendingRecv   |= bit;
            vm_write16(ea + EF_PENDING_RECV_EVT + 2u * s, rel);
        }
    }

    events = (u16)(events | bits);
    if (pendingRecv) {
        vm_write16(ea + EF_SPU_PENDING_RECV,
                   (u16)(vm_read16(ea + EF_SPU_PENDING_RECV) | pendingRecv));
        if (vm_read8(ea + EF_CLEAR_MODE) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO)
            events = (u16)(events & ~eventsToClear);
    }
    vm_write16(ea + EF_EVENTS, events);

    for (int s = 0; s < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS; s++) {
        if (pendingRecv & (0x8000u >> s)) {
            wake_taskset[wake_count] = (uint32_t)vm_read64(ea + EF_ADDR);
            wake_task[wake_count] = vm_read8(ea + EF_WAITING_TASK_ID + s);
            wake_count++;
        }
    }
    spu_reservation_bump_locked(ea);
    spu_reservation_unlock();

    /* Signal after releasing the reservation lock: signal delivery updates a
     * different guest reservation line and takes the same global lock. */
    for (unsigned i = 0; i < wake_count; i++) {
        if (ps3_log_verbose())
            fprintf(stderr, "[cellSpurs] EventFlagSet 0x%08X satisfies SPU task "
                    "(taskset=0x%08X task=%u)\n", ea, wake_taskset[i], wake_task[i]);
        spu_taskset_signal_task(wake_taskset[i], wake_task[i]);
    }
}

/* SPU-side entry (Layer 2): a task's flag Set arriving via the taskset
 * syscall or a runtime bridge. Same protocol, takes the lock itself. */
void spurs_ef_set_from_spu(uint32_t flag_ea, uint16_t bits)
{
    EventFlagSync* sync = ef_sync_get(flag_ea);
    if (!sync) return;
    ef_lock(sync);
    spurs_ef_set_locked(flag_ea, (u16)bits);
    ef_broadcast(sync);
    ef_unlock(sync);
}

/* =========================================================================
 * SPURS core
 *
 * The CellSpurs instance (0x2000 bytes of GAME-owned guest memory) is real
 * shared state: the game's engine pokes it with INLINED atomics (readyCount
 * stores, signal bits) and the policy modules DMA it from the SPU side. So
 * it must hold the REAL big-endian kernel layout — never a host struct.
 * Verified offsets (RPCS3 cellSpurs.h contract):
 *   +0x00 wklReadyCount1[16] (u8/wid)   +0x80 wklState1[16] (u8: 2=runnable)
 *   +0x10 wklIdleSpuCount[16]           +0x90 wklStatus1[16]
 *   +0x20 wklCurrentContention[16]      +0xA0 wklEvent1[16]
 *   +0x40 wklMinContention[16]          +0xB0 wklEnabled (be u32, bit 31-wid)
 *   +0x50 wklMaxContention[16]          +0xBD sysSrvMsgUpdateWorkload (u8)
 *   +0x60 wklFlag (be u64)              +0xB00 wklInfo1[16] (32B each:
 *   +0x70 wklSignal1 (be u16)                  addr u64, arg u64, size u32,
 *   +0x76 nSpus (u8)                           uniqueId u8, prio[8] @+0x18)
 * Our own bookkeeping lives in a host side-table keyed by instance EA.
 * A host "kernel" thread per instance polls readyCount/signal and runs the
 * workload's policy module (spurs_policy.c) — the virtual SPU.
 * =====================================================================*/
enum {
    SPURS_WKL_READY1   = 0x00,
    SPURS_WKL_IDLE2    = 0x10,
    SPURS_WKL_CURCONT  = 0x20,
    SPURS_WKL_MINCONT  = 0x40,
    SPURS_WKL_MAXCONT  = 0x50,
    SPURS_WKL_FLAG     = 0x60,
    SPURS_WKL_SIGNAL1  = 0x70,
    SPURS_NSPUS        = 0x76,
    SPURS_WKL_STATE1   = 0x80,
    SPURS_WKL_STATUS1  = 0x90,
    SPURS_WKL_EVENT1   = 0xA0,
    SPURS_WKL_ENABLED  = 0xB0,
    SPURS_SYSSRV_MSG   = 0xBD,
    SPURS_WKL_INFO1    = 0xB00,
    SPURS_WKL_INFO_SZ  = 0x20,
    /* CELL_SPURS_SIZE = 4096 (SDK cell/spurs/types.h). The 8192-byte variant
     * is CellSpurs2 (cellSpursInitialize*2* NIDs) which LBP does not use —
     * clearing 0x2000 here overran the game's 4KB heap block and corrupted
     * the allocator (abort in the job pump's first object destruction). */
    SPURS_INST_SIZE    = 0x1000,
};

/* Exact 0x200-byte CellSpursAttribute guest layout. */
enum {
    SATTR_REVISION        = 0x00,
    SATTR_SDK_VERSION     = 0x04,
    SATTR_NSPUS           = 0x08,
    SATTR_SPU_PRIORITY    = 0x0C,
    SATTR_PPU_PRIORITY    = 0x10,
    SATTR_EXIT_NO_WORK    = 0x14,
    SATTR_PREFIX          = 0x15,
    SATTR_PREFIX_SIZE     = 0x24,
    SATTR_FLAGS           = 0x28,
    SATTR_SIZE            = 0x200,
    SATTR_FLAG_SPU_PRINTF = 0x10000000u,
};

static struct SpursInst {
    u32           ea;          /* 0 = free */
    u32           nspus;
    char          prefix[16];
    volatile long kernel_live; /* poll thread started */
#ifdef _WIN32
    HANDLE        kernel_thread; /* retained until Finalize joins it */
    volatile long kernel_stop;
#endif
    u32           event_queue[64]; /* SPU user-event port -> LV2 queue */
    u32           event_flag[64];  /* SPU user-event port -> event flag EA */
} s_inst[MAX_SPURS_INST];

static struct SpursInst* spurs_inst_find(u32 ea)
{
    if (!ea) return NULL;
    for (int i = 0; i < MAX_SPURS_INST; i++)
        if (s_inst[i].ea == ea) return &s_inst[i];
    return NULL;
}

/* readyCount is guest-owned shared state.  The host scheduler consumes it
 * while PPU HLE calls (and title-inlined stores) can update it concurrently,
 * so use byte atomics on the live guest field instead of a stale side-table
 * mirror. */
static u8 spurs_ready_load(u32 ea)
{
#ifdef _WIN32
    u32 word_ea = ea & ~3u;
    u32 shift = (ea & 3u) * 8u;
    u32 word = (u32)InterlockedCompareExchange(
        (volatile LONG*)(vm_base + word_ea), 0, 0);
    return (u8)(word >> shift);
#else
    return __atomic_load_n(vm_base + ea, __ATOMIC_ACQUIRE);
#endif
}

static u8 spurs_ready_exchange(u32 ea, u8 value)
{
#ifdef _WIN32
    u32 word_ea = ea & ~3u;
    u32 shift = (ea & 3u) * 8u;
    u32 mask = 0xFFu << shift;
    volatile LONG* target = (volatile LONG*)(vm_base + word_ea);
    for (;;) {
        u32 current = (u32)InterlockedCompareExchange(target, 0, 0);
        u32 next = (current & ~mask) | ((u32)value << shift);
        if ((u32)InterlockedCompareExchange(target, (LONG)next,
                                             (LONG)current) == current)
            return (u8)(current >> shift);
    }
#else
    return __atomic_exchange_n(vm_base + ea, value, __ATOMIC_ACQ_REL);
#endif
}

static u8 spurs_ready_compare_exchange(u32 ea, u8 compare, u8 value)
{
#ifdef _WIN32
    u32 word_ea = ea & ~3u;
    u32 shift = (ea & 3u) * 8u;
    u32 mask = 0xFFu << shift;
    volatile LONG* target = (volatile LONG*)(vm_base + word_ea);
    for (;;) {
        u32 current = (u32)InterlockedCompareExchange(target, 0, 0);
        u8 current_byte = (u8)(current >> shift);
        if (current_byte != compare) return current_byte;
        u32 next = (current & ~mask) | ((u32)value << shift);
        if ((u32)InterlockedCompareExchange(target, (LONG)next,
                                             (LONG)current) == current)
            return current_byte;
    }
#else
    u8 expected = compare;
    __atomic_compare_exchange_n(vm_base + ea, &expected, value, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return expected;
#endif
}

static u8 spurs_ready_consume_one(u32 ea)
{
    for (;;) {
        u8 current = spurs_ready_load(ea);
        if (!current) return 0;
        if (spurs_ready_compare_exchange(ea, current, (u8)(current - 1)) == current)
            return current;
    }
}

static int spurs_inst_index(u32 spurs_ea)
{
    struct SpursInst* si = spurs_inst_find(spurs_ea);
    return si ? (int)(si - s_inst) : -1;
}

static SpursWorkload* spurs_workload_find(u32 spurs_ea, u32 wid)
{
    int slot = spurs_inst_index(spurs_ea);
    if (slot < 0 || wid >= CELL_SPURS_MAX_WORKLOAD)
        return NULL;
    SpursWorkload* w = &s_workloads[slot][wid];
    return w->in_use && w->spurs_ea == spurs_ea ? w : NULL;
}

static void spurs_workload_cache_invalidate(u32 spurs_ea, u32 wid)
{
    int slot = spurs_inst_index(spurs_ea);
    if (slot >= 0 && wid < CELL_SPURS_MAX_WORKLOAD)
        memset(&s_wkl_pm[slot][wid], 0, sizeof(s_wkl_pm[slot][wid]));
}

static SpursWorkload* spurs_workload_claim(u32 spurs_ea,
                                            SpursWorkloadKind kind, u32* wid)
{
    int slot = spurs_inst_index(spurs_ea);
    if (slot < 0) return NULL;
    for (u32 i = 0; i < CELL_SPURS_MAX_WORKLOAD; i++) {
        SpursWorkload* w = &s_workloads[slot][i];
        if (!w->in_use) {
            memset(w, 0, sizeof(*w));
            spurs_workload_cache_invalidate(spurs_ea, i);
            w->in_use = 1;
            w->kind = kind;
            w->spurs_ea = spurs_ea;
            *wid = i;
            return w;
        }
    }
    return NULL;
}

static int spurs_workload_publish(u32 spurs_ea, u32 wid)
{
    SpursWorkload* w = spurs_workload_find(spurs_ea, wid);
    if (!w) return 0;

    u32 info = spurs_ea + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ;
    vm_write64(info + 0x00, (u64)(uintptr_t)w->pm);
    vm_write64(info + 0x08, w->data);
    vm_write32(info + 0x10, w->sizePm);
    vm_write32(info + 0x14, wid << 24);
    for (int b = 0; b < 8; b++)
        *(vm_base + info + 0x18 + b) = w->priority[b];
    *(vm_base + spurs_ea + SPURS_WKL_READY1 + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_IDLE2 + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_CURCONT + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_MINCONT + wid) = (u8)w->minContention;
    *(vm_base + spurs_ea + SPURS_WKL_MAXCONT + wid) = (u8)w->maxContention;
    *(vm_base + spurs_ea + SPURS_WKL_STATE1 + wid) = 2;
    vm_write32(spurs_ea + SPURS_WKL_ENABLED,
               vm_read32(spurs_ea + SPURS_WKL_ENABLED) | (0x80000000u >> wid));
    *(vm_base + spurs_ea + SPURS_SYSSRV_MSG) = 0xFF;
    return 1;
}

static void spurs_workload_release(u32 spurs_ea, u32 wid)
{
    SpursWorkload* w = spurs_workload_find(spurs_ea, wid);
    if (!w) return;

    vm_write32(spurs_ea + SPURS_WKL_ENABLED,
               vm_read32(spurs_ea + SPURS_WKL_ENABLED) & ~(0x80000000u >> wid));
    *(vm_base + spurs_ea + SPURS_WKL_READY1 + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_IDLE2 + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_CURCONT + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_MINCONT + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_MAXCONT + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_STATE1 + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_STATUS1 + wid) = 0;
    *(vm_base + spurs_ea + SPURS_WKL_EVENT1 + wid) = 0;
    vm_write32(spurs_ea + SPURS_WKL_SIGNAL1,
               vm_read32(spurs_ea + SPURS_WKL_SIGNAL1) &
               ~((0x8000u >> wid) << 16));
    u32 info = spurs_ea + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ;
    for (u32 off = 0; off < SPURS_WKL_INFO_SZ; off += 4)
        vm_write32(info + off, 0);
    spurs_workload_cache_invalidate(spurs_ea, wid);
    memset(w, 0, sizeof(*w));
}

#ifdef _WIN32
static DWORD WINAPI spurs_kernel_thread(LPVOID p);
#endif

static s32 spurs_initialize_common(u32 spurs_ea, u32 nspus, const char* prefix)
{
    struct SpursInst* si = spurs_inst_find(spurs_ea);
    if (si) return CELL_SPURS_CORE_ERROR_STAT;
    for (int i = 0; i < MAX_SPURS_INST; i++)
        if (!s_inst[i].ea) { si = &s_inst[i]; break; }
    if (!si) return CELL_SPURS_CORE_ERROR_NOMEM;

    int slot = (int)(si - s_inst);
    memset(s_workloads[slot], 0, sizeof(s_workloads[slot]));
    memset(s_wkl_pm[slot], 0, sizeof(s_wkl_pm[slot]));

    si->ea    = spurs_ea;
    si->nspus = (nspus > 0 && nspus <= CELL_SPURS_MAX_SPU) ? nspus : 1;
    memset(si->prefix, 0, sizeof(si->prefix));
    memset(si->event_queue, 0, sizeof(si->event_queue));
    memset(si->event_flag, 0, sizeof(si->event_flag));
    if (prefix) memcpy(si->prefix, prefix, 15);

    /* Real BE instance: zero it, then the few live fields. (The global
     * workload table is NOT wiped here — the title may init several SPURS
     * instances before adding workloads to any of them.) */
    memset(vm_base + spurs_ea, 0, SPURS_INST_SIZE);
    *(vm_base + spurs_ea + SPURS_NSPUS) = (u8)si->nspus;
    vm_write64(spurs_ea + SPURS_WKL_FLAG, 0xFFFFFFFFFFFFFFFFull); /* no receiver */

#ifdef _WIN32
    if (!si->kernel_live) {
        InterlockedExchange(&si->kernel_stop, 0);
        HANDLE kernel = CreateThread(NULL, 1u << 20, spurs_kernel_thread, si, 0, NULL);
        if (kernel) {
            si->kernel_thread = kernel;
            InterlockedExchange(&si->kernel_live, 1);
        }
    }
#endif
    printf("[cellSpurs] Initialize \"%s\" ea=0x%08X nSpus=%u (real BE instance + kernel poll)\n",
           si->prefix, spurs_ea, si->nspus);
    return CELL_OK;
}

s32 cellSpursInitialize(CellSpurs* spurs, s32 nSpus, s32 spuPriority,
                        s32 ppuPriority, u8 exitIfNoWork)
{
    (void)spuPriority; (void)ppuPriority; (void)exitIfNoWork;
    if (!spurs)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    return spurs_initialize_common((u32)(uintptr_t)spurs, (u32)nSpus, NULL);
}

s32 cellSpursInitializeWithAttribute(CellSpurs* spurs,
                                     const CellSpursAttribute* attr)
{
    const u32 spurs_ea = (u32)(uintptr_t)spurs;
    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (!spurs_ea || !attr_ea)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if ((spurs_ea & 0x7Fu) || (attr_ea & 7u))
        return CELL_SPURS_CORE_ERROR_ALIGN;
    if (vm_read32(attr_ea + SATTR_REVISION) > 2u)
        return CELL_SPURS_CORE_ERROR_INVAL;
    const u32 nspus = vm_read32(attr_ea + SATTR_NSPUS);
    if (!nspus || nspus > CELL_SPURS_MAX_SPU)
        return CELL_SPURS_CORE_ERROR_INVAL;

    char prefix[16] = {0};
    const u32 prefix_size = vm_read32(attr_ea + SATTR_PREFIX_SIZE);
    if (prefix_size > 15u) return CELL_SPURS_CORE_ERROR_INVAL;
    if (prefix_size)
        memcpy(prefix, vm_base + attr_ea + SATTR_PREFIX, prefix_size);

    return spurs_initialize_common(spurs_ea, nspus, prefix);
}

s32 cellSpursFinalize(CellSpurs* spurs)
{
    if (!spurs)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    struct SpursInst* si = spurs_inst_find((u32)(uintptr_t)spurs);
    if (!si)
        return CELL_SPURS_CORE_ERROR_STAT;
    int slot = (int)(si - s_inst);

    printf("[cellSpurs] Finalize(ea=0x%08X)\n", si->ea);
#ifdef _WIN32
    /* The poller continuously reads the guest instance.  Ask it to leave and
     * join before clearing any instance/workload memory it can observe. */
    InterlockedExchange(&si->kernel_stop, 1);
    if (si->kernel_thread && GetThreadId(si->kernel_thread) != GetCurrentThreadId()) {
        WaitForSingleObject(si->kernel_thread, INFINITE);
        CloseHandle(si->kernel_thread);
        si->kernel_thread = NULL;
    }
#endif
    memset(s_workloads[slot], 0, sizeof(s_workloads[slot]));
    memset(s_wkl_pm[slot], 0, sizeof(s_wkl_pm[slot]));
    si->ea = 0;
    return CELL_OK;
}

s32 cellSpursAttributeInitialize(CellSpursAttribute* attr, s32 nSpus,
                                 s32 spuPriority, s32 ppuPriority,
                                 u8 exitIfNoWork)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (!attr_ea)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (attr_ea & 7u)
        return CELL_SPURS_CORE_ERROR_ALIGN;
    memset(vm_base + attr_ea, 0, SATTR_SIZE);
    vm_write32(attr_ea + SATTR_REVISION, 1);
    vm_write32(attr_ea + SATTR_NSPUS, (u32)nSpus);
    vm_write32(attr_ea + SATTR_SPU_PRIORITY, (u32)spuPriority);
    vm_write32(attr_ea + SATTR_PPU_PRIORITY, (u32)ppuPriority);
    *(vm_base + attr_ea + SATTR_EXIT_NO_WORK) = exitIfNoWork ? 1 : 0;
    if (spurs_trace_enabled())
        printf("[cellSpurs] AttributeInitialize(nSpus=%d)\n", nSpus);
    return CELL_OK;
}

/* The SDK's cellSpursAttributeInitialize() macro imports this internal name
 * (NID 0x95180230). Forward to the implementation above. */
s32 _cellSpursAttributeInitialize(CellSpursAttribute* attr,
                                  u32 revision, u32 sdkVersion, u32 nSpus,
                                  s32 spuPriority, s32 ppuPriority,
                                  u8 exitIfNoWork)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (!attr_ea) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (attr_ea & 7u) return CELL_SPURS_CORE_ERROR_ALIGN;

    memset(vm_base + attr_ea, 0, SATTR_SIZE);
    vm_write32(attr_ea + SATTR_REVISION, revision);
    vm_write32(attr_ea + SATTR_SDK_VERSION, sdkVersion);
    vm_write32(attr_ea + SATTR_NSPUS, nSpus);
    vm_write32(attr_ea + SATTR_SPU_PRIORITY, (u32)spuPriority);
    vm_write32(attr_ea + SATTR_PPU_PRIORITY, (u32)ppuPriority);
    *(vm_base + attr_ea + SATTR_EXIT_NO_WORK) = exitIfNoWork ? 1 : 0;
    if (spurs_trace_enabled())
        printf("[cellSpurs] _AttributeInitialize(rev=%u sdk=0x%08X "
               "nSpus=%u spuPri=%d ppuPri=%d exit=%u)\n",
               revision, sdkVersion, nSpus, spuPriority, ppuPriority,
               exitIfNoWork ? 1u : 0u);
    return CELL_OK;
}

s32 cellSpursAttributeSetNamePrefix(CellSpursAttribute* attr,
                                    const char* prefix, u32 size)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    const u32 prefix_ea = (u32)(uintptr_t)prefix;
    if (!attr_ea || !prefix_ea)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (attr_ea & 7u) return CELL_SPURS_CORE_ERROR_ALIGN;
    if (size > 15u) return CELL_SPURS_CORE_ERROR_INVAL;
    memset(vm_base + attr_ea + SATTR_PREFIX, 0, 15);
    if (size)
        memcpy(vm_base + attr_ea + SATTR_PREFIX, vm_base + prefix_ea, size);
    vm_write32(attr_ea + SATTR_PREFIX_SIZE, size);
    return CELL_OK;
}

s32 cellSpursAttributeSetSpuThreadGroupType(CellSpursAttribute* attr,
                                            s32 type)
{
    (void)type;
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    return CELL_OK;
}

s32 cellSpursAttributeEnableSpuPrintfIfAvailable(CellSpursAttribute* attr)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (!attr_ea) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (attr_ea & 7u) return CELL_SPURS_CORE_ERROR_ALIGN;
    vm_write32(attr_ea + SATTR_FLAGS,
               vm_read32(attr_ea + SATTR_FLAGS) | SATTR_FLAG_SPU_PRINTF);
    return CELL_OK;
}

s32 cellSpursGetNumSpuThread(const CellSpurs* spurs, u32* nThreads)
{
    if (!spurs || !nThreads)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    struct SpursInst* si = spurs_inst_find((u32)(uintptr_t)spurs);
    u32* nThreads_h = GUEST_PTR(nThreads, u32*);

    if (!si)
        return CELL_SPURS_CORE_ERROR_STAT;

    /* out-param is guest BE */
    vm_write32((u32)(uintptr_t)nThreads, si->nspus);
    (void)nThreads_h;
    return CELL_OK;
}

s32 cellSpursSetMaxContention(CellSpurs* spurs, CellSpursWorkloadId wid,
                              u32 maxContention)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    u32 spurs_ea = (u32)(uintptr_t)spurs;
    SpursWorkload* w = spurs_workload_find(spurs_ea, wid);
    if (!w) return CELL_SPURS_CORE_ERROR_SRCH;

    w->maxContention = maxContention;
    *(vm_base + spurs_ea + SPURS_WKL_MAXCONT + wid) = (u8)maxContention;
    return CELL_OK;
}

s32 cellSpursSetPriorities(CellSpurs* spurs, CellSpursWorkloadId wid,
                           const u8* priorities)
{
    if (!spurs || !priorities) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    const u8* priorities_h = GUEST_PTR(priorities, const u8*);
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    u32 spurs_ea = (u32)(uintptr_t)spurs;
    SpursWorkload* w = spurs_workload_find(spurs_ea, wid);
    if (!w) return CELL_SPURS_CORE_ERROR_SRCH;

    memcpy(w->priority, priorities_h, CELL_SPURS_MAX_SPU);
    u32 info = spurs_ea + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ;
    for (int b = 0; b < 8; b++)
        *(vm_base + info + 0x18 + b) = w->priority[b];
    return CELL_OK;
}

s32 cellSpursAttachLv2EventQueue(CellSpurs* spurs, u32 queue, u8* port,
                                 s32 isDynamic)
{
    if (!spurs || !port) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    u32 spurs_ea = (u32)(uintptr_t)spurs;
    u32 port_ea = (u32)(uintptr_t)port;
    struct SpursInst* si = spurs_inst_find(spurs_ea);
    if (!si) return CELL_SPURS_CORE_ERROR_STAT;

    u32 selected = *(vm_base + port_ea);
    if (isDynamic) {
        selected = 0x10;
        while (selected <= 0x3F && si->event_queue[selected]) selected++;
        if (selected > 0x3F) return CELL_SPURS_CORE_ERROR_BUSY;
    } else if (selected > 0x3F) {
        return CELL_SPURS_CORE_ERROR_INVAL;
    }
    if (si->event_queue[selected] && si->event_queue[selected] != queue)
        return CELL_SPURS_CORE_ERROR_BUSY;

    si->event_queue[selected] = queue;
    *(vm_base + port_ea) = (u8)selected;
    printf("[cellSpurs] AttachLv2EventQueue(spurs=0x%08X queue=%u port=%u dynamic=%d)\n",
           spurs_ea, queue, selected, isDynamic);
    return CELL_OK;
}

s32 cellSpursDetachLv2EventQueue(CellSpurs* spurs, u8 port)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (port > 0x3F) return CELL_SPURS_CORE_ERROR_INVAL;
    struct SpursInst* si = spurs_inst_find((u32)(uintptr_t)spurs);
    if (!si) return CELL_SPURS_CORE_ERROR_STAT;
    si->event_queue[port] = 0;
    printf("[cellSpurs] DetachLv2EventQueue(port=%u)\n", port);
    return CELL_OK;
}

/* Called by a lifted SPURS job's interrupt-mailbox path. The mailbox command's
 * high byte selects the SPU user-event port; data0 is its low 24 bits and data1
 * is the preceding outbound-mailbox word. Returns 1 when delivered, 0 when no
 * route or permanently failed, and -2 when a connected queue is full. */
int cellSpursDeliverSpuEvent(u32 spurs_ea, u32 port, u32 data0, u32 data1)
{
    static const u64 user_event_key = 0xFFFFFFFF53505501ull;
    struct SpursInst* si = spurs_inst_find(spurs_ea);
    if (!si || port > 0x3F) return 0;
    if (si->event_flag[port]) {
        EventFlagSync* sync = ef_sync_get(si->event_flag[port]);
        if (sync) {
            ef_lock(sync);
            ef_broadcast(sync);
            ef_unlock(sync);
        }
        return 1;
    }
    if (!si->event_queue[port]) return 0;
    u32 queue = si->event_queue[port];
    int rc = sys_event_queue_push_by_id(queue, user_event_key, 0,
                                        ((u64)port << 32) | data0, data1);
    if (spurs_trace_enabled())
        printf("[cellSpurs] SPU event port=%u data0=0x%06X data1=0x%08X -> queue=%u rc=%d\n",
               port, data0, data1, queue, rc);
    return rc == 0 ? 1 : (rc == -2 ? -2 : 0);
}

/* =========================================================================
 * Taskset
 * =====================================================================*/

/* Exact CellSpursTasksetAttribute guest offsets.  This object is produced by
 * the PPU SDK and consumed by SPURS, so native little-endian struct accesses
 * silently corrupt every multi-byte field. */
enum {
    TSATTR_REVISION       = 0x00,
    TSATTR_SDK_VERSION    = 0x04,
    TSATTR_ARGS           = 0x08,
    TSATTR_PRIORITY       = 0x10,
    TSATTR_MAX_CONTENTION = 0x18,
    TSATTR_NAME           = 0x1C,
    TSATTR_TASKSET_SIZE   = 0x20,
    TSATTR_ENABLE_CLEAR_LS= 0x24,
    TSATTR_SIZE           = 0x200,
    SPURS_TASKSET_SIZE    = 0x1900,
    SPURS_TASKSET2_SIZE   = 0x2900,
};

/* Opaque CellSpursTaskAttribute storage used by this HLE pair.  Keeping the
 * metadata guest-endian means the object remains deterministic in guest RAM;
 * both initializer and consumer own these private offsets. */
enum {
    TATTR_REVISION       = 0x00,
    TATTR_SIZE_CONTEXT   = 0x04,
    TATTR_EA_CONTEXT     = 0x08,
    TATTR_EA_ELF         = 0x10,
    TATTR_LS_PATTERN_EA  = 0x18,
    TATTR_ARGUMENT_EA    = 0x1C,
    TATTR_SIZE           = 0x100,
};

static int spurs_taskset_guest_valid(u32 taskset_ea)
{
    if (!taskset_ea || (taskset_ea & 0x7Fu)) return 0;
    const u32 size = vm_read32(taskset_ea + CSTS_SIZE_FIELD);
    return vm_read64(taskset_ea + CSTS_SPURS) != 0 &&
           (size == SPURS_TASKSET_SIZE || size == SPURS_TASKSET2_SIZE);
}

static s32 cellSpursCreateTasksetInternal(CellSpurs* spurs,
                                          CellSpursTaskset* taskset,
                                          u64 args, u32 priority_ea,
                                          u32 maxContention, u32 name_ea,
                                          u32 taskset_size, s32 enable_clear_ls)
{
    const uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    const uint32_t spurs_ea   = (uint32_t)(uintptr_t)spurs;

    if (!spurs_ea || !taskset_ea) {
        fprintf(stderr, "[cellSpurs] CreateTaskset REJECT null "
                "(spurs=0x%08X taskset=0x%08X)\n", spurs_ea, taskset_ea);
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    }
    if ((spurs_ea & 0x7Fu) || (taskset_ea & 0x7Fu))
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (taskset_size != SPURS_TASKSET_SIZE &&
        taskset_size != SPURS_TASKSET2_SIZE)
        return CELL_SPURS_TASK_ERROR_INVAL;
    if (!priority_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (!maxContention || maxContention > CELL_SPURS_MAX_SPU)
        return CELL_SPURS_TASK_ERROR_INVAL;
    if (!spurs_inst_find(spurs_ea)) {
        fprintf(stderr, "[cellSpurs] CreateTaskset REJECT unregistered "
                "spurs=0x%08X (taskset=0x%08X)\n", spurs_ea, taskset_ea);
        return CELL_SPURS_CORE_ERROR_STAT;
    }

    /* Reusing a live taskset address without destroying it first would orphan
     * its previous WID after the object is cleared. */
    for (int slot = 0; slot < MAX_SPURS_INST; slot++) {
        for (u32 wid = 0; wid < CELL_SPURS_MAX_WORKLOAD; wid++) {
            SpursWorkload* existing = &s_workloads[slot][wid];
            if (existing->in_use && existing->kind == SPURS_WORKLOAD_TASKSET &&
                existing->data == taskset_ea)
                return CELL_SPURS_TASK_ERROR_STAT;
        }
    }

    u32 taskset_wid = 0;
    SpursWorkload* workload = spurs_workload_claim(spurs_ea,
                                                     SPURS_WORKLOAD_TASKSET,
                                                     &taskset_wid);
    if (!workload)
        return CELL_SPURS_TASK_ERROR_NOMEM;

    /* Clear the complete SDK-requested object.  Tokyo Jungle explicitly uses
     * CellSpursTaskset2 (0x2900); the old sizeof(host shim) clear left almost
     * all task records and extension state uninitialized. */
    memset(vm_base + taskset_ea, 0, taskset_size);
    spurs_taskset_init(taskset_ea, spurs_ea, args, taskset_wid,
                       taskset_size, /*evf1*/0, /*evf2*/0);
    *(vm_base + taskset_ea + CSTS_ENABLE_CLEAR_LS) = enable_clear_ls > 0 ? 1 : 0;
    *(vm_base + taskset_ea + CSTS_WKL_FLAG_WAIT) = 0x80;

    workload->pm = (const void*)(uintptr_t)0x200u;
    workload->sizePm = 0x1E40u;
    workload->data = taskset_ea;
    workload->minContention = 8;
    workload->maxContention = maxContention;
    workload->nameInstance_ea = name_ea;
    if (priority_ea)
        memcpy(workload->priority, vm_base + priority_ea, CELL_SPURS_MAX_SPU);
    if (!spurs_workload_publish(spurs_ea, taskset_wid)) {
        spurs_workload_release(spurs_ea, taskset_wid);
        memset(vm_base + taskset_ea, 0, taskset_size);
        return CELL_SPURS_TASK_ERROR_NOMEM;
    }
    spu_taskset_register_signal_state(taskset_ea);

    g_ydkj_real_taskset_ea = taskset_ea;
    g_ydkj_real_spurs_ea   = spurs_ea;

    printf("[cellSpurs] CreateTaskset ea=0x%08X spurs=0x%08X "
           "args=0x%016llX priority=0x%08X max=%u name=0x%08X "
           "size=0x%X clearLS=%d\n",
           taskset_ea, spurs_ea, (unsigned long long)args, priority_ea,
           maxContention, name_ea, taskset_size, enable_clear_ls > 0);
    return CELL_OK;
}

s32 cellSpursCreateTaskset(CellSpurs* spurs, CellSpursTaskset* taskset,
                           u64 args, const u8* priority, u32 maxContention)
{
    if (!priority) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    return cellSpursCreateTasksetInternal(
        spurs, taskset, args, (u32)(uintptr_t)priority, maxContention,
        0, SPURS_TASKSET_SIZE, 0);
}

s32 cellSpursCreateTasksetWithAttribute(CellSpurs* spurs,
                                        CellSpursTaskset* taskset,
                                        const CellSpursTasksetAttribute* attr)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (attr_ea & 7u) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (vm_read32(attr_ea + TSATTR_REVISION) != 1u)
        return CELL_SPURS_TASK_ERROR_INVAL;

    return cellSpursCreateTasksetInternal(
        spurs, taskset,
        vm_read64(attr_ea + TSATTR_ARGS),
        attr_ea + TSATTR_PRIORITY,
        vm_read32(attr_ea + TSATTR_MAX_CONTENTION),
        vm_read32(attr_ea + TSATTR_NAME),
        vm_read32(attr_ea + TSATTR_TASKSET_SIZE),
        (s32)vm_read32(attr_ea + TSATTR_ENABLE_CLEAR_LS));
}

s32 cellSpursDestroyTaskset(CellSpursTaskset* taskset)
{
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    if (!taskset_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (taskset_ea & 0x7Fu)
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!spurs_taskset_guest_valid(taskset_ea))
        return CELL_SPURS_TASK_ERROR_STAT;

    u32 spurs_ea = (u32)vm_read64(taskset_ea + CSTS_SPURS);
    u32 wid = vm_read32(taskset_ea + CSTS_WID);

    printf("[cellSpurs] DestroyTaskset(ea=0x%08X)\n", taskset_ea);
    spu_taskset_unregister_signal_state(taskset_ea);
    SpursWorkload* workload = spurs_workload_find(spurs_ea, wid);
    if (workload && workload->kind == SPURS_WORKLOAD_TASKSET &&
        workload->data == taskset_ea)
        spurs_workload_release(spurs_ea, wid);
    /* There is no `initialized` word in the guest ABI.  The old native write
     * hit the first RUNNING bitset word and could advertise a phantom task. */
    for (u32 off = CSTS_RUNNING; off <= CSTS_WAITING; off += 0x10) {
        vm_write64(taskset_ea + off, 0);
        vm_write64(taskset_ea + off + 8, 0);
    }
    vm_write64(taskset_ea + CSTS_SPURS, 0);
    vm_write64(taskset_ea + CSTS_ARGS, 0);
    *(vm_base + taskset_ea + CSTS_ENABLE_CLEAR_LS) = 0;
    *(vm_base + taskset_ea + CSTS_WKL_FLAG_WAIT) = 0;
    *(vm_base + taskset_ea + CSTS_LAST_SCHEDULED) = 0;
    vm_write32(taskset_ea + CSTS_WID, 0);
    vm_write32(taskset_ea + CSTS_SIZE_FIELD, 0);
    return CELL_OK;
}

s32 cellSpursShutdownTaskset(CellSpursTaskset* taskset)
{
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    if (!taskset_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (taskset_ea & 0x7Fu)
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!spurs_taskset_guest_valid(taskset_ea))
        return CELL_SPURS_TASK_ERROR_STAT;

    printf("[cellSpurs] ShutdownTaskset(ea=0x%08X)\n", taskset_ea);
    if (spu_taskset_request_shutdown(taskset_ea) != 0)
        return CELL_SPURS_TASK_ERROR_STAT;
    return CELL_OK;
}

s32 cellSpursJoinTaskset(CellSpursTaskset* taskset)
{
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    if (!taskset_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (taskset_ea & 0x7Fu)
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (!spurs_taskset_guest_valid(taskset_ea))
        return CELL_SPURS_TASK_ERROR_STAT;

    printf("[cellSpurs] JoinTaskset(ea=0x%08X)\n", taskset_ea);
    if (spu_taskset_wait_all_tasks_exit(taskset_ea) != 0)
        return CELL_SPURS_TASK_ERROR_STAT;
    return CELL_OK;
}

s32 cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (attr_ea & 7u) return CELL_SPURS_TASK_ERROR_ALIGN;
    memset(vm_base + attr_ea, 0, TSATTR_SIZE);
    vm_write32(attr_ea + TSATTR_REVISION, 1);
    vm_write32(attr_ea + TSATTR_TASKSET_SIZE, SPURS_TASKSET_SIZE);
    return CELL_OK;
}

s32 cellSpursTasksetAttributeSetName(CellSpursTasksetAttribute* attr,
                                      const char* name)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    const u32 name_ea = (u32)(uintptr_t)name;
    if (!attr_ea || !name_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (attr_ea & 7u) return CELL_SPURS_TASK_ERROR_ALIGN;
    vm_write32(attr_ea + TSATTR_NAME, name_ea);
    return CELL_OK;
}

/* =========================================================================
 * Task
 * =====================================================================*/

/* Real SDK ABI (verified against RPCS3 cellSpurs.cpp:370):
 *   cellSpursCreateTask(taskset, taskId, elf, context, sizeContext,
 *                       CellSpursTaskLsPattern* lsPattern,
 *                       CellSpursTaskArgument*  argument)
 * -- SEVEN args (r3..r9), not six. The old 6-arg form treated r8 as an opaque
 * `attr` and never read r9 (the argument), so the task's 16-byte work-descriptor
 * argument was dropped and TaskInfo.args stayed 0. LBP's audio task DMAs its work
 * from an EA computed out of r3 = that argument (spu_0003 task main @0x17e70:
 * `wrch $ch18, f(r3.word3)`), so a zero argument makes it GET from EA 0 and
 * stall. lsPattern/argument arrive as guest EAs (generic adapter forwards r3..r10). */
s32 cellSpursCreateTask(CellSpursTaskset* taskset, CellSpursTaskId* taskId,
                        void* elf, void* context, u32 sizeContext,
                        u32 lsPattern_ea, u32 argument_ea)
{
    (void)context; (void)sizeContext;

    /* Read the 16-byte CellSpursTaskArgument + optional LS pattern from guest mem. */
    uint32_t task_arg[4] = {0,0,0,0};
    uint32_t task_lsp[4] = {0,0,0,0};
    if (argument_ea)  for (int _i=0;_i<4;_i++) task_arg[_i] = vm_read32(argument_ea + _i*4);
    if (lsPattern_ea) for (int _i=0;_i<4;_i++) task_lsp[_i] = vm_read32(lsPattern_ea + _i*4);

    /* Capture guest EAs BEFORE host translation (the real BE taskset builder + the
     * SPU DMA use guest EAs). */
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    uint32_t task_id_ea = (uint32_t)(uintptr_t)taskId;
    uint32_t elf_ea     = (uint32_t)(uintptr_t)elf;
    uint32_t context_ea = (uint32_t)(uintptr_t)context;

    if (!taskset_ea || !task_id_ea || !elf_ea) {
        fprintf(stderr, "[cellSpurs] CreateTask REJECT null "
                "taskset=0x%08X taskId=0x%08X elf=0x%08X\n",
                taskset_ea, task_id_ea, elf_ea);
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    }
    if ((taskset_ea & 0x7Fu) || (task_id_ea & 3u))
        return CELL_SPURS_TASK_ERROR_ALIGN;

    if (!spurs_taskset_guest_valid(taskset_ea)) {
        fprintf(stderr, "[cellSpurs] CreateTask REJECT no-init (taskset=0x%08X elf=0x%08X)\n",
                taskset_ea, elf_ea);
        return CELL_SPURS_TASK_ERROR_STAT;
    }

    /* Task IDs are local to a taskset.  Keep the host bookkeeping in the
     * global side table, but allocate the lowest unused ID from this taskset's
     * enabled bitset.  Using the global host slot as the guest task ID made a
     * second taskset start at IDs 2/3 instead of 0/1, so its leaf tasks read
     * the wrong TaskInfo and scheduler lanes. */
    u32 local_task_id = CELL_SPURS_MAX_TASK;
    for (u32 id = 0; id < CELL_SPURS_MAX_TASK; id++) {
        if (!spurs_bitset_test(taskset_ea + CSTS_ENABLED, id)) {
            local_task_id = id;
            break;
        }
    }
    if (local_task_id == CELL_SPURS_MAX_TASK)
        return CELL_SPURS_TASK_ERROR_NOMEM;

    /* Find a free host bookkeeping slot. */
    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        if (!s_tasks[i].in_use) {
            s_tasks[i].in_use = 1;
            s_tasks[i].id = local_task_id;
            s_tasks[i].taskset_ea = taskset_ea;
            s_tasks[i].active = 1;
            s_tasks[i].completed = 0;
            s_tasks[i].exitCode = 0;
            s_tasks[i].entryPoint = elf;

            /* taskId is a guest u32 output.  A native pointer store writes
             * little-endian bytes on Windows, so guest ID 1 was later read as
             * 0x01000000.  Keep this output in guest byte order. */
            if (task_id_ea) vm_write32(task_id_ea, s_tasks[i].id);
            /* CellSpursTaskset is an opaque guest ABI object, not the simplified
             * host struct declared in cellSpurs.h.  In the real 0x1900-byte BE
             * layout, host `taskCount` offset +0x04 is the second word of the
             * RUNNING bitset.  Incrementing it for two tasks wrote 0x02000000
             * there, advertising a disabled task as running.  The lifted SPU
             * send-signal routine detects that invalid state and deliberately
             * performs a no-op, deadlocking Tokyo Jungle's Survival barrier.
             * s_tasks[] already owns the host-side task count/bookkeeping; never
             * write the fake native view into guest taskset memory. */

            /* Register the task in the REAL BE taskset: writes task_info[id]
             * (args/elf/context/ls_pattern) + sets enabled+ready bits so the PM's
             * SELECT_TASK picks it. */
            spurs_taskset_add_task(taskset_ea, local_task_id, (uint64_t)elf_ea,
                                   (uint64_t)context_ea, task_arg, task_lsp);
            spu_taskset_mark_task_started(taskset_ea, local_task_id);
            /* Bridge to the image-22 dispatch so build_context uses this taskset+task. */
            g_ydkj_real_taskset_ea = taskset_ea;
            g_ydkj_real_taskid     = local_task_id;

            printf("[cellSpurs] CreateTask(id=%u, entry=%p, arg=%08X %08X %08X %08X)\n",
                   s_tasks[i].id, elf, task_arg[0], task_arg[1], task_arg[2], task_arg[3]);

            /* One-shot: dump the memory the task argument points at, to find the
             * pointer that reads back 0 (the task GETs from EA 0 -> some field of
             * its work descriptor is null in our run). Each of the 4 arg words that
             * looks like a valid guest EA gets 64 bytes dumped as BE u32s. */
            if (getenv("LBP_TASKSET_TRACE")) {
                for (int a = 0; a < 4; a++) {
                    uint32_t p = task_arg[a];
                    if (p < 0x10000 || p >= 0x50000000u) continue;   /* not a plausible EA */
                    fprintf(stderr, "[argdump] arg[%d]=0x%08X ->", a, p);
                    for (int o = 0; o < 64; o += 4)
                        fprintf(stderr, " %08X", vm_read32(p + o));
                    fprintf(stderr, "\n");
                }
                /* Full heap object (arg[2], the audio engine object): the count the
                 * PPU validated is at +0xC8 (16-aligned, <=0x160 = stream count); the
                 * task's per-stream buffer pointers live at +0x124 (=arg[1]). Dump
                 * +0x00..+0x160 so we can see: 0 streams (task should yield) vs N
                 * streams with null data buffers (fill gap). */
                uint32_t ho = task_arg[2];
                if (ho >= 0x10000 && ho < 0x50000000u) {
                    fprintf(stderr, "[heapobj] 0x%08X count@+0xC8=0x%08X\n", ho, vm_read32(ho + 0xC8));
                    for (int o = 0; o < 0x160; o += 16)
                        fprintf(stderr, "  +%03X: %08X %08X %08X %08X\n", o,
                                vm_read32(ho+o), vm_read32(ho+o+4), vm_read32(ho+o+8), vm_read32(ho+o+12));
                }
                /* The descriptor block the SPU task actually DMAs + reads its buffer
                 * pointers from: v10[336..] at v10+1344 = arg[3]-64 (arg[3]=v10+1408).
                 * v10[344]=a1[141], v10[345]=a1[140] (the FMOD DSP buffers). If those
                 * words are 0 here, they are the null source (task GETs from EA 0). */
                uint32_t d = task_arg[3];
                if (d >= 0x10040 && d < 0x50000000u) {
                    uint32_t db = d - 64;   /* 0x0094F6C0 = v10+1344 */
                    fprintf(stderr, "[descblk] v10+1344=0x%08X (a1[141]@+0x20, a1[140]@+0x24):\n", db);
                    for (int o = 0; o < 0x40; o += 16)
                        fprintf(stderr, "  +%02X: %08X %08X %08X %08X\n", o,
                                vm_read32(db+o), vm_read32(db+o+4), vm_read32(db+o+8), vm_read32(db+o+12));
                }
                /* a1 (the FMOD object) = taskId_ea - 628 (sub_48420C passes the taskId
                 * out-param as (_DWORD)a1+628 for task 0). a1[140]/a1[141] (= a1+0x230/
                 * +0x234) are the null DSP-buffer fields. Log the EAs so the next run
                 * can YDKJ_WWATCH=<a1+0x230> to catch who should write it (or prove no
                 * one does). Only for task 0 (offset 628); task 1 uses +688. */
                uint32_t a1 = (uint32_t)(uintptr_t)taskId - 628u;
                if (a1 < 0x50000000u)
                    fprintf(stderr, "[a1obj] a1=0x%08X  a1+0x230(dsp0)=0x%08X val=0x%08X  "
                            "a1+0x234(dsp1)=0x%08X val=0x%08X\n", a1,
                            a1+0x230, vm_read32(a1+0x230), a1+0x234, vm_read32(a1+0x234));
                fflush(stderr);
            }

            /* Run the task's SPU program if a lifted build is registered for it.
             * The registry maps the task ELF (by content fingerprint) to its
             * pre-lifted native entry; dispatch loads the ELF into a local store
             * and runs it with the task arg in r3. INERT until the title
             * registers its lifted SPU set: an unregistered image MISSes and
             * returns 0, preserving the prior "track only" behaviour.
             *
             * NOTE: dispatch is synchronous (runs to completion inline). That
             * suits create+join task patterns; a workload/taskset whose SPU job
             * waits on concurrent PPU-side signals will want the async lv2
             * SPU-thread path instead — wired when a title exercises it. */
            if (elf) {
                /* elf/context are guest effective addresses; translate the image
                 * pointer to host memory for fingerprint+load, but keep context
                 * as the guest EA (the SPU job's DMA uses guest EAs / r3). */
                const uint8_t* host_elf = GUEST_PTR(elf, const uint8_t*);
                size_t sz = spu_elf_image_size(host_elf, 2u * 1024 * 1024);
                if (sz)
                    /* Async: SPURS tasks are persistent workers — running them
                     * inline would block this PPU thread forever (deadlock). */
                    spu_workload_dispatch_async_task(
                        host_elf, (uint32_t)sz,
                        (uint32_t)(uintptr_t)context,
                        taskset_ea, local_task_id);
            }
            return CELL_OK;
        }
    }

    return CELL_SPURS_TASK_ERROR_NOMEM;
}

/* The SDK's versioned task-attribute initializer. ABI (8 GPR args):
 *   r3=attr r4=revision r5=sdkVersion r6=eaElf r7=eaContext r8=sizeContext
 *   r9=lsPattern r10=argument
 * Stash the task ELF EA + context so cellSpursCreateTaskWithAttribute can
 * dispatch the SPU job. */
s32 _cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* attr, u32 revision,
                                      u32 sdkVersion, u64 eaElf, u64 eaContext,
                                      u32 sizeContext, const void* lsPattern,
                                      const void* argument)
{
    (void)sdkVersion;
    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (attr_ea & 0xFu) return CELL_SPURS_TASK_ERROR_ALIGN;
    memset(vm_base + attr_ea, 0, TATTR_SIZE);
    vm_write32(attr_ea + TATTR_REVISION, revision);
    vm_write32(attr_ea + TATTR_SIZE_CONTEXT, sizeContext);
    vm_write64(attr_ea + TATTR_EA_CONTEXT, eaContext);
    vm_write64(attr_ea + TATTR_EA_ELF, eaElf);
    /* lsPattern/argument are guest EAs of 16-byte blocks; carry them so
     * CreateTaskWithAttribute writes them into the TaskInfo. The SPU task
     * library refuses blocking waits for a task whose argument is zero or
     * whose lsPattern doesn't cover its stack (0x8041090F). */
    u32 ls_pattern_ea = (u32)(uintptr_t)lsPattern;
    u32 argument_ea = (u32)(uintptr_t)argument;
    /* Tokyo Jungle's embedded persistent tasks use this SDK initializer ABI
     * with r8 pointing at the per-task 16-byte CellSpursTaskArgument block.
     * The old prototype treated r8 as a numeric context size and r10 as the
     * argument pointer, leaving TaskInfo.args all zero.  The lifted leaf then
     * received r3={0,0,0,0}, woke, found no descriptor and parked again while
     * the PPU spun on its completion word.  Restrict the corrected handoff to
     * the two title-owned embedded task ELFs so older title paths keep their
     * existing attribute interpretation. */
    if ((u32)eaElf == 0x002C9A00u || (u32)eaElf == 0x002C7000u)
        argument_ea = sizeContext;
    vm_write32(attr_ea + TATTR_LS_PATTERN_EA, ls_pattern_ea);
    vm_write32(attr_ea + TATTR_ARGUMENT_EA, argument_ea);
    if (spurs_trace_enabled())
        printf("[cellSpurs] _TaskAttributeInitialize(eaElf=0x%08X "
               "ctx=0x%08X szctx=%u lsp=0x%08X arg=0x%08X)\n",
               (u32)eaElf, (u32)eaContext, sizeContext,
               ls_pattern_ea, argument_ea);
    return CELL_OK;
}

/* Create a task from a pre-initialized attribute (carries ELF EA + context).
 * Forwards to cellSpursCreateTask, which translates taskset/taskId and runs the
 * SPU image through spu_workload_dispatch. */
s32 cellSpursCreateTaskWithAttribute(CellSpursTaskset* taskset,
                                     CellSpursTaskId* taskId,
                                     CellSpursTaskAttribute* attr)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (attr_ea & 0xFu) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (spurs_trace_enabled()) {
      static int _n = 0; if (_n++ < 6) {
        fprintf(stderr, "[cellSpurs] CreateTaskWithAttr attr=0x%08X raw:", attr_ea);
        for (int o = 0; o < 0x40; o += 4) fprintf(stderr, " %08X", vm_read32(attr_ea + o));
        fprintf(stderr, "\n"); } }
    /* taskset/taskId forwarded raw (callee translates); elf/context are guest
     * EAs, as are lsPattern/argument (stored by _cellSpursTaskAttributeInitialize;
     * dropping them left the TaskInfo with a zero argument + zero lsPattern and
     * the SPU task library then refuses every blocking wait with 0x8041090F --
     * LBP's binkspu movie-IO task spun forever on that). */
    return cellSpursCreateTask(taskset, taskId,
                               (void*)(uintptr_t)(u32)vm_read64(attr_ea + TATTR_EA_ELF),
                               (void*)(uintptr_t)(u32)vm_read64(attr_ea + TATTR_EA_CONTEXT),
                               vm_read32(attr_ea + TATTR_SIZE_CONTEXT),
                               vm_read32(attr_ea + TATTR_LS_PATTERN_EA),
                               vm_read32(attr_ea + TATTR_ARGUMENT_EA));
}

/* SDK ABI: attr, revision, sdkVersion, args, priority[8] EA, maxContention. */
s32 _cellSpursTasksetAttributeInitialize(CellSpursTasksetAttribute* attr,
                                         u32 revision, u32 sdkVersion, u64 argTaskset,
                                         const u8* priority, u32 maxContention)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    const u32 priority_ea = (u32)(uintptr_t)priority;
    if (!attr_ea || !priority_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (attr_ea & 7u) return CELL_SPURS_TASK_ERROR_ALIGN;
    for (u32 i = 0; i < CELL_SPURS_MAX_SPU; ++i) {
        if (*(vm_base + priority_ea + i) >= CELL_SPURS_MAX_PRIORITY)
            return CELL_SPURS_TASK_ERROR_INVAL;
    }

    memset(vm_base + attr_ea, 0, TSATTR_SIZE);
    vm_write32(attr_ea + TSATTR_REVISION, revision);
    vm_write32(attr_ea + TSATTR_SDK_VERSION, sdkVersion);
    vm_write64(attr_ea + TSATTR_ARGS, argTaskset);
    memcpy(vm_base + attr_ea + TSATTR_PRIORITY,
           vm_base + priority_ea, CELL_SPURS_MAX_SPU);
    vm_write32(attr_ea + TSATTR_MAX_CONTENTION, maxContention);
    vm_write32(attr_ea + TSATTR_TASKSET_SIZE, SPURS_TASKSET_SIZE);
    printf("[cellSpurs] _TasksetAttributeInitialize(rev=%u sdk=0x%08X "
           "args=0x%016llX priority=0x%08X max=%u)\n",
           revision, sdkVersion, (unsigned long long)argTaskset,
           priority_ea, maxContention);
    return CELL_OK;
}

s32 cellSpursJoinTask(CellSpursTaskset* taskset, CellSpursTaskId taskId,
                      s32* exitCode)
{
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    uint32_t exit_code_ea = (uint32_t)(uintptr_t)exitCode;

    printf("[cellSpurs] JoinTask(id=%u)\n", taskId);

    /* Join observes SPU EXIT; it must not manufacture completion. */
    for (u32 i = 0; i < CELL_SPURS_MAX_TASK; i++) {
        if (s_tasks[i].in_use && s_tasks[i].taskset_ea == taskset_ea &&
            s_tasks[i].id == taskId) {
            int32_t code = 0;
            if (spu_taskset_wait_task_exit(taskset_ea, taskId, &code) != 0)
                return CELL_SPURS_TASK_ERROR_STAT;
            s_tasks[i].completed = 1;
            s_tasks[i].active = 0;
            s_tasks[i].exitCode = code;
            if (exit_code_ea)
                vm_write32(exit_code_ea, (uint32_t)code);
            s_tasks[i].in_use = 0;
            return CELL_OK;
        }
    }

    return CELL_SPURS_TASK_ERROR_SRCH;
}

s32 cellSpursSendSignal(CellSpursTaskset* taskset, CellSpursTaskId taskId)
{
    /* Capture the guest taskset EA before host translation -- the WAIT_SIGNAL
     * waiter (spu_taskset_wait_signal) keys on the guest EA + taskId. */
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;

    printf("[cellSpurs] SendSignal(taskset=0x%08X id=%u)\n", taskset_ea, taskId);

    /* Deliver the signal for real: set the task's bit in the guest taskset's
     * SIGNALLED bitset and wake its blocked host thread. (Was a documented
     * no-op from the pre-SPU-execution era -- a dropped signal deadlocked any
     * task parked in WAIT_SIGNAL waiting for it.) */
    if (taskset_ea) spu_taskset_signal_task(taskset_ea, taskId);
    return CELL_OK;
}

/* CellSpursBarrier is a 128-byte, 128-byte-aligned SPU-visible object.  The
 * lifted SDK code treats word +0 as the arrival counter and word +4 as the
 * configured total: Notify increments +0 and compares the result with +4.
 * Leaving the complete object zero (the old generic unresolved-import success
 * path) made Tokyo Jungle's two Survival workers reach arrived=2 against
 * total=0 and both sleep forever in WAIT_SIGNAL.
 *
 * The lifted SPU barrier code owns the remaining waiter masks/state.  Clear
 * the complete DMA line before publishing the initial counter. */
s32 cellSpursBarrierInitialize(CellSpursTaskset* taskset,
                               CellSpursBarrier* barrier, u32 total)
{
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    uint32_t barrier_ea = (uint32_t)(uintptr_t)barrier;

    if (!taskset_ea || !barrier_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if ((taskset_ea & 0x7Fu) || (barrier_ea & 0x7Fu))
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (total == 0 || total > CELL_SPURS_MAX_TASK)
        return CELL_SPURS_TASK_ERROR_INVAL;

    for (uint32_t off = 0; off < 0x80; off += 4)
        vm_write32(barrier_ea + off, 0);
    vm_write32(barrier_ea + 4, total);

    fprintf(stderr,
            "[cellSpurs] BarrierInitialize(taskset=0x%08X barrier=0x%08X "
            "arrived=0 total=%u)\n",
            taskset_ea, barrier_ea, total);
    fflush(stderr);
    return CELL_OK;
}

s32 cellSpursTaskAttributeInitialize(CellSpursTaskAttribute* attr)
{
    const u32 attr_ea = (u32)(uintptr_t)attr;
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (attr_ea & 0xFu) return CELL_SPURS_TASK_ERROR_ALIGN;
    memset(vm_base + attr_ea, 0, TATTR_SIZE);
    vm_write32(attr_ea + TATTR_REVISION, 1);
    return CELL_OK;
}

/* =========================================================================
 * Workload
 * =====================================================================*/

static s32 spurs_add_workload_internal(CellSpurs* spurs, CellSpursWorkloadId* wid,
                                       const void* pm, u32 sizePm, u64 data,
                                       const u8* priority, u32 minContention,
                                       u32 maxContention, u32 nameClass_ea,
                                       u32 nameInstance_ea)
{
    if (!spurs || !wid)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    /* spurs/wid/priority are guest EAs; pm stays a guest EA (it's the SPU
     * program address consumed later by the workload dispatch). */
    uint32_t spurs_ea = (uint32_t)(uintptr_t)spurs;
    struct SpursInst* si = spurs_inst_find(spurs_ea);
    const u8* priority_h = GUEST_PTR(priority, const u8*);

    if (!si)
        return CELL_SPURS_CORE_ERROR_STAT;

    u32 new_wid = 0;
    SpursWorkload* workload = spurs_workload_claim(spurs_ea,
                                                     SPURS_WORKLOAD_POLICY,
                                                     &new_wid);
    if (!workload)
        return CELL_SPURS_CORE_ERROR_NOMEM;

    workload->pm = pm;
    workload->sizePm = sizePm;
    workload->data = data;
    workload->minContention = minContention ? minContention : 1;
    workload->maxContention = maxContention ? maxContention : 1;
    workload->nameClass_ea = nameClass_ea;
    workload->nameInstance_ea = nameInstance_ea;
    if (priority_h)
        memcpy(workload->priority, priority_h, CELL_SPURS_MAX_SPU);

    if (!spurs_workload_publish(spurs_ea, new_wid)) {
        spurs_workload_release(spurs_ea, new_wid);
        return CELL_SPURS_CORE_ERROR_NOMEM;
    }

    /* wid out-param is guest BE */
    vm_write32((u32)(uintptr_t)wid, new_wid);
    printf("[cellSpurs] AddWorkload(wid=%u, pm=%p, size=%u)\n",
           new_wid, pm, sizePm);
    return CELL_OK;
}

s32 cellSpursAddWorkload(CellSpurs* spurs, CellSpursWorkloadId* wid,
                         const void* pm, u32 sizePm, u64 data,
                         const u8* priority, u32 minContention,
                         u32 maxContention)
{
    return spurs_add_workload_internal(spurs, wid, pm, sizePm, data, priority,
                                       minContention, maxContention, 0, 0);
}

/* Real (BE) CellSpursWorkloadAttribute offsets (libspurs layout; the game's
 * inlined SDK code writes the struct directly in guest memory, so it must be
 * read back big-endian at these offsets -- never through a native host struct
 * (see the BIG-ENDIAN WARNING in spurs_taskset.h). */
enum {
    WKATTR_REVISION   = 0x00,   /* be u32 */
    WKATTR_SDKVERSION = 0x04,   /* be u32 */
    WKATTR_PM         = 0x08,   /* be u32: policy-module image EA */
    WKATTR_SIZE       = 0x0C,   /* be u32: policy-module size */
    WKATTR_DATA       = 0x10,   /* be u64: workload data (jobchain/queue EA) */
    WKATTR_PRIORITY   = 0x18,   /* u8[8] */
    WKATTR_MIN_CONT   = 0x20,   /* be u32 */
    WKATTR_MAX_CONT   = 0x24,   /* be u32 */
    WKATTR_NAME_CLASS = 0x28,   /* be u32: char* EA */
    WKATTR_NAME_INST  = 0x2C,   /* be u32: char* EA */
    WKATTR_HOOK       = 0x30,   /* be u32 */
    WKATTR_HOOK_ARG   = 0x34,   /* be u32 */
};

static volatile u32 s_pmwatch_ea = 0, s_pmwatch_sz = 0;
static DWORD WINAPI pm_write_watch(LPVOID unused)
{
    (void)unused;
    if (!guest_world_register_current("spurs-pm-watch")) return 0;
    u32 ea = s_pmwatch_ea, sz = s_pmwatch_sz;
    u32 last_exe = vm_read32(ea + 0xAF4), last_hot = vm_read32(ea + 0x7E4); /* 0x14f4, 0x11e4 */
    for (int t = 0; t < 6000; t++) {          /* ~60s at 10ms */
        u32 exe = vm_read32(ea + 0xAF4), hot = vm_read32(ea + 0x7E4);
        if ((exe != last_exe && exe != 0) || (hot != last_hot && hot != 0xFFFFFFFFu)) {
            fprintf(stderr, "[pm-watch] PM ASSEMBLED at ~%dms: exe@0x14f4 %08X->%08X hot@0x11e4 %08X->%08X\n",
                    t * 10, last_exe, exe, last_hot, hot);
            FILE* pf = fopen("lbp_spu/pm_wwsjob_complete.bin", "wb");
            if (pf) { fwrite(vm_base + ea, 1, sz, pf); fclose(pf);
                fprintf(stderr, "[pm-watch] wrote lbp_spu/pm_wwsjob_complete.bin (0x%X bytes)\n", sz); }
            guest_world_unregister_current();
            return 0;
        }
        Sleep(10);
    }
    fprintf(stderr, "[pm-watch] PM NEVER assembled in 60s (exe@0x14f4=%08X hot@0x11e4=%08X) "
            "-> assembly is HLE'd/external\n", vm_read32(ea + 0xAF4), vm_read32(ea + 0x7E4));
    guest_world_unregister_current();
    return 0;
}

s32 cellSpursAddWorkloadWithAttribute(CellSpurs* spurs,
                                       CellSpursWorkloadId* wid,
                                       const CellSpursWorkloadAttribute* attr)
{
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    uint32_t attr_ea = (uint32_t)(uintptr_t)attr;

    /* Decode the REAL BE attribute from guest memory. */
    u32 pm_ea = vm_read32(attr_ea + WKATTR_PM);
    u32 pm_sz = vm_read32(attr_ea + WKATTR_SIZE);
    u64 data  = vm_read64(attr_ea + WKATTR_DATA);
    u32 minc  = vm_read32(attr_ea + WKATTR_MIN_CONT);
    u32 maxc  = vm_read32(attr_ea + WKATTR_MAX_CONT);
    u32 nmcls = vm_read32(attr_ea + WKATTR_NAME_CLASS);
    u32 nmins = vm_read32(attr_ea + WKATTR_NAME_INST);

    {   /* Layout ground truth: dump the raw attr words for the first few calls
         * (if the decode above prints nonsense, these bytes are the arbiter). */
        static int _n = 0;
        if (_n < 3) {
            printf("[cellSpurs] AddWorkloadWA attr=0x%08X raw:", attr_ea);
            for (int o = 0; o < 0x40; o += 4) {
                if ((o & 15) == 0) printf("\n    +%02X:", o);
                printf(" %08X", vm_read32(attr_ea + o));
            }
            printf("\n");
        } else if (_n == 3) {
            printf("[cellSpurs] AddWorkloadWA raw dumps suppressed from here\n");
        }
        printf("[cellSpurs] AddWorkloadWA: pm=0x%08X size=%u data=0x%016llX minC=%u maxC=%u name=%s/%s\n",
               pm_ea, pm_sz, (unsigned long long)data, minc, maxc,
               nmcls ? (const char*)(vm_base + nmcls) : "-",
               nmins ? (const char*)(vm_base + nmins) : "-");
        if (_n == 0 && pm_ea && pm_ea < 0x10000000u) {
            printf("[cellSpurs] PM@0x%08X first 96B:", pm_ea);
            for (int o = 0; o < 96; o += 4) {
                if ((o & 15) == 0) printf("\n    +%02X:", o);
                printf(" %08X", vm_read32(pm_ea + o));
            }
            printf("\n");
        }
        /* PM-COMPLETENESS PROBE (LBP_PM_DUMP): the wwsjob job-manager PM is
         * ASSEMBLED at runtime -- the embedded ELF (LS 0xA00) has zero HOLES at
         * LS 0xAEE..0x11B0 and the executeStage lives at LS 0x14f4, both filled
         * by SPURS setup. Report whether OUR runtime's PM has that code or the
         * holes, and (once) write the whole image out so we can re-lift it. */
        if (getenv("LBP_PM_DUMP") && pm_ea && pm_sz >= 0x2200 && pm_sz <= 0x4000) {
            u32 exe = vm_read32(pm_ea + 0xAF4);          /* LS 0x14f4 executeStage */
            int zeros = 0; for (u32 o = 0xEE; o < 0x7B0; o += 4)
                if (vm_read32(pm_ea + o) == 0) zeros += 4;
            fprintf(stderr, "[pm-dump] wid-PM ea=0x%08X sz=0x%X exe@+0xAF4=%08X (want 24F880ED) "
                    "holeZeros=%d/1730 -> %s\n", pm_ea, pm_sz, exe, zeros,
                    (exe == 0x24F880EDu ? "COMPLETE (we assemble it)"
                                        : zeros > 1000 ? "HOLEY (assembly skipped)" : "PARTIAL"));
            static int _dumped = 0;
            if (!_dumped) { _dumped = 1;
                FILE* pf = fopen("lbp_spu/pm_wwsjob_runtime.bin", "wb");
                if (pf) { fwrite(vm_base + pm_ea, 1, pm_sz, pf); fclose(pf);
                    fprintf(stderr, "[pm-dump] wrote lbp_spu/pm_wwsjob_runtime.bin (0x%X bytes)\n", pm_sz); }
                /* WRITE-WATCH: spawn a poller that reports if/when the PM's
                 * placeholder hot-loop (LS 0x11e4, currently 0xFFFFFFFF) and
                 * executeStage (LS 0x14f4, currently 0) get real code written by
                 * PPU code at runtime -> tells us whether our recompiled game
                 * assembles the PM (dump it) or the assembly is HLE'd away. */
                s_pmwatch_ea = pm_ea; s_pmwatch_sz = pm_sz;
                CreateThread(NULL, 1u << 18, pm_write_watch, NULL, 0, NULL);
            }
        }
        _n++;
    }

    /* Forward the BE-decoded values (priority as guest EA of the 8-byte table). */
    return spurs_add_workload_internal(
        spurs, wid, (const void*)(uintptr_t)pm_ea, pm_sz, data,
        (const u8*)(uintptr_t)(attr_ea + WKATTR_PRIORITY), minc, maxc,
        nmcls, nmins);
}

/* The SDK-versioned workload-attribute initializer (the import the game links;
 * NID differs from the non-underscore inline wrapper). Args arrive raw in
 * r3..r10; writes the REAL BE layout so AddWorkloadWithAttribute round-trips. */
s32 _cellSpursWorkloadAttributeInitialize(u64 attr_ea, u32 revision, u32 sdkVersion,
                                          u64 pm_ea, u32 size, u64 data,
                                          u64 prio_ea, u32 minContention)
{
    if (!attr_ea) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    vm_write32((u32)attr_ea + WKATTR_REVISION,   revision);
    vm_write32((u32)attr_ea + WKATTR_SDKVERSION, sdkVersion);
    vm_write32((u32)attr_ea + WKATTR_PM,         (u32)pm_ea);
    vm_write32((u32)attr_ea + WKATTR_SIZE,       size);
    vm_write64((u32)attr_ea + WKATTR_DATA,       data);
    for (int i = 0; i < 8; i++)
        *(vm_base + (u32)attr_ea + WKATTR_PRIORITY + i) =
            prio_ea ? *(vm_base + (u32)prio_ea + i) : 0;
    vm_write32((u32)attr_ea + WKATTR_MIN_CONT, minContention);
    vm_write32((u32)attr_ea + WKATTR_MAX_CONT, 1);   /* 9th arg is beyond the 8-GPR adapter */
    vm_write32((u32)attr_ea + WKATTR_NAME_CLASS, 0);
    vm_write32((u32)attr_ea + WKATTR_NAME_INST,  0);
    vm_write32((u32)attr_ea + WKATTR_HOOK,     0);
    vm_write32((u32)attr_ea + WKATTR_HOOK_ARG, 0);
    printf("[cellSpurs] _WorkloadAttributeInitialize(attr=0x%08X pm=0x%08X size=%u data=0x%llX minC=%u)\n",
           (u32)attr_ea, (u32)pm_ea, size, (unsigned long long)data, minContention);
    return CELL_OK;
}

s32 cellSpursWorkloadAttributeSetName(u64 attr_ea, u64 nameClass_ea, u64 nameInstance_ea)
{
    if (!attr_ea) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    vm_write32((u32)attr_ea + WKATTR_NAME_CLASS, (u32)nameClass_ea);
    vm_write32((u32)attr_ea + WKATTR_NAME_INST,  (u32)nameInstance_ea);
    printf("[cellSpurs] WorkloadAttributeSetName(attr=0x%08X, \"%s\", \"%s\")\n",
           (u32)attr_ea,
           nameClass_ea ? (const char*)(vm_base + (u32)nameClass_ea) : "-",
           nameInstance_ea ? (const char*)(vm_base + (u32)nameInstance_ea) : "-");
    return CELL_OK;
}

s32 cellSpursRemoveWorkload(CellSpurs* spurs, CellSpursWorkloadId wid)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD) return CELL_SPURS_CORE_ERROR_INVAL;
    u32 spurs_ea = (u32)(uintptr_t)spurs;
    if (!spurs_workload_find(spurs_ea, wid)) return CELL_SPURS_CORE_ERROR_SRCH;

    spurs_workload_release(spurs_ea, wid);
    printf("[cellSpurs] RemoveWorkload(wid=%u)\n", wid);
    return CELL_OK;
}

s32 cellSpursWorkloadAttributeInitialize(CellSpursWorkloadAttribute* attr,
                                         u32 revision, u32 sdkVersion,
                                         const void* pm, u32 sizePm,
                                         u64 data, const u8* priority,
                                         u32 minContention,
                                         u32 maxContention)
{
    if (!attr) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    attr = GUEST_PTR(attr, CellSpursWorkloadAttribute*);
    const u8* priority_h = GUEST_PTR(priority, const u8*);

    memset(attr, 0, sizeof(CellSpursWorkloadAttribute));
    attr->revision = revision;
    attr->sdkVersion = sdkVersion;
    attr->pm = (u64)(uintptr_t)pm;   /* pm kept as guest EA */
    attr->sizePm = sizePm;
    attr->data = data;
    attr->minContention = minContention;
    attr->maxContention = maxContention;

    if (priority_h)
        memcpy(attr->priority, priority_h, CELL_SPURS_MAX_SPU);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * The SPURS "kernel": one host poll thread per instance (the virtual SPU).
 *
 * The game kicks work by storing a nonzero wklReadyCount1[wid] byte or a
 * wklSignal1 bit into the instance — mostly with INLINED atomics (LBP never
 * calls an API for it beyond cellSpursReadyCountStore). The kernel thread
 * polls those real BE fields, consumes one ready unit (decrement / clear the
 * signal bit, like the real kernel's dispatch), and runs the workload's
 * policy module to completion via spu_run_policy_module.
 * -----------------------------------------------------------------------*/
static WklPm* spurs_resolve_pm(u32 spurs_ea, u32 wid)
{
    int slot = spurs_inst_index(spurs_ea);
    SpursWorkload* w = spurs_workload_find(spurs_ea, wid);
    if (slot < 0 || !w || w->kind != SPURS_WORKLOAD_POLICY)
        return NULL;
    WklPm* r = &s_wkl_pm[slot][wid];
    if (r->resolved) return r->resolved > 0 ? r : NULL;
    uint64_t fp = spu_workload_fingerprint(vm_base + (uint32_t)(uintptr_t)w->pm,
                                           w->sizePm);
    r->fn = spu_workload_find_img(fp, &r->image_id);
    r->resolved = r->fn ? 1 : -1;
    if (r->fn)
        printf("[cellSpurs] wid=%u PM resolved (fp=0x%016llX image=%d)\n",
               wid, (unsigned long long)fp, r->image_id);
    else
        printf("[cellSpurs] wid=%u PM NOT LIFTED (fp=0x%016llX size=%u) -- workload will not run\n",
               wid, (unsigned long long)fp, w->sizePm);
    return r->fn ? r : NULL;
}

#ifdef _WIN32
/* One virtual SPU running a workload's policy module. The WWS job manager
 * runs concurrently across N SPUs (RPCS3: jobmanagerCellSpursKernel0..N): each
 * claims jobs from the shared queue and advances its own lane of the sync
 * barrier. Running the SPUs SEQUENTIALLY deadlocks -- SPU 0 completes its job,
 * then busy-waits at the cross-SPU barrier for lane 1, which the sequential
 * loop can never advance (SPU 1 hasn't run). Concurrency is required, not an
 * optimization: SPU 0's barrier poll observes SPU 1's atomic lane update live. */
struct spurs_pm_worker_arg {
    spu_lifted_entry_fn fn; int image_id;
    const uint8_t* pm; uint32_t pm_size;
    uint64_t arg; uint32_t wid, ea, spu_num;
};
static DWORD WINAPI spurs_pm_worker(LPVOID p)
{
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
    if (!guest_world_register_current("spurs-pm")) return 0;
    struct spurs_pm_worker_arg* a = (struct spurs_pm_worker_arg*)p;
    spu_run_policy_module(a->fn, a->image_id, a->pm, a->pm_size,
                          a->arg, a->wid, a->ea, a->spu_num);
    guest_world_unregister_current();
    return 0;
}

static DWORD WINAPI spurs_kernel_thread(LPVOID p)
{
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }  /* let SO reach the reporter */
    if (!guest_world_register_current("spurs-kernel")) return 0;
    struct SpursInst* si = (struct SpursInst*)p;
    int instance_slot = (int)(si - s_inst);
    static volatile long s_pm_off = -1;
    if (s_pm_off < 0) s_pm_off = getenv("PS3_NO_SPURS_PM") ? 1 : 0;

    fprintf(stderr, "[spurs-kern] \"%s\" poll thread live: ea=0x%08X pm=%s\n",
            si->prefix, si->ea, s_pm_off ? "DISABLED (PS3_NO_SPURS_PM)" : "enabled");
    fflush(stderr);

    /* Instance change detector (SPURS_KERN_WATCH=1).
     *
     * We assumed the title kicks a workload by poking wklReadyCount/wklSignal
     * with inlined atomics -- but this thread watches exactly those bytes every
     * 1 ms and has never once seen them nonzero, across whole runs. Rather than
     * guess again, shadow the head of the instance and report EVERY byte the
     * title changes. Whatever the real kick is, it has to land in here. */
    static const u32 WATCH_LEN = 0xC0;
    unsigned char shadow[0xC0];
    int shadow_primed = 0;
    int watch = getenv("SPURS_KERN_WATCH") ? 1 : 0;
    int changes_logged = 0;

    while (!InterlockedCompareExchange(&si->kernel_stop, 0, 0)) {
        Sleep(1);
        u32 ea = si->ea;
        if (!ea || s_pm_off) continue;

        if (watch) {
            const unsigned char* live = (const unsigned char*)vm_base + ea;
            if (!shadow_primed) { memcpy(shadow, live, WATCH_LEN); shadow_primed = 1; }
            else if (memcmp(shadow, live, WATCH_LEN) != 0) {
                for (u32 o = 0; o < WATCH_LEN; o++) {
                    if (shadow[o] == live[o]) continue;
                    if (changes_logged < 200) {
                        changes_logged++;
                        fprintf(stderr, "[spurs-kern] \"%s\" INSTANCE +0x%02X: %02X -> %02X%s\n",
                                si->prefix, o, shadow[o], live[o],
                                o < 16                    ? "  (wklReadyCount1)" :
                                o >= 0x70 && o < 0x76     ? "  (wklSignal1)"     :
                                o >= 0x80 && o < 0x90     ? "  (wklState1)"      :
                                o >= 0xB0 && o < 0xB4     ? "  (wklEnabled)"     : "");
                    }
                }
                memcpy(shadow, live, WATCH_LEN);
                fflush(stderr);
            }
        }

        u32 enabled = vm_read32(ea + SPURS_WKL_ENABLED);

        /* Kick visibility (can't-miss): log ANY nonzero readyCount/signal state
         * even for wids the dispatch filter below would skip — the game pokes
         * these bytes with inlined atomics and this is our only tap. */
        {
            static int _seen[MAX_SPURS_INST][16];
            int slot = (int)(si - s_inst);
            u32 sig = vm_read32(ea + SPURS_WKL_SIGNAL1) >> 16;
            for (u32 w = 0; w < 16; w++) {
                u8 rc = *(vm_base + ea + SPURS_WKL_READY1 + w);
                if ((rc || (sig & (0x8000u >> w))) && _seen[slot][w] < 4) {
                    _seen[slot][w]++;
                    fprintf(stderr, "[spurs-kern] \"%s\" POKE wid=%u ready=%u sig=%u enabled=%d state=%u\n",
                            si->prefix, w, rc, (sig >> (15 - w)) & 1,
                            (enabled >> (31 - w)) & 1,
                            *(vm_base + ea + SPURS_WKL_STATE1 + w));
                }
            }
        }
        if (!enabled) continue;

        for (u32 wid = 0; wid < 16; wid++) {
            if (!(enabled & (0x80000000u >> wid))) continue;
            if (*(vm_base + ea + SPURS_WKL_STATE1 + wid) != 2) continue;
            SpursWorkload* workload = spurs_workload_find(ea, wid);
            /* Tasksets publish a real workload descriptor for guest ABI
             * consumers, but their leaf ELFs are dispatched directly by
             * cellSpursCreateTask; never resolve/run a second taskset PM. */
            if (!workload || workload->kind != SPURS_WORKLOAD_POLICY) continue;

            /* A SPURS policy module is a PERSISTENT SPU program. The real kernel
             * schedules an ENABLED, runnable workload onto an SPU and the module
             * then polls its OWN job queue in main memory; being enabled is the
             * trigger, not a per-job kick. That is why this title never writes
             * wklReadyCount, never sets a signal bit, and never calls
             * cellSpursReadyCountStore -- on hardware it does not have to. Gating
             * dispatch on a kick meant the module never ran at all, so nothing
             * ever called cellSpursEventFlagSet and the title's loading thread
             * blocked forever.
             *
             * Run one scheduling quantum per enabled workload per pass: the
             * module does its work, exits to the kernel (LS 0x9C0) when it has
             * none, and we re-enter it on the next pass -- which is exactly what
             * the real kernel's dispatch loop does.
             *
             * readyCount/wklSignal are still honoured when a title DOES use them:
             * consume one unit so a kick-driven title paces the same as before. */
            u32 ready_ea = ea + SPURS_WKL_READY1 + wid;
            volatile u8* rdy = vm_base + ready_ea;
            u32 sig = vm_read32(ea + SPURS_WKL_SIGNAL1) >> 16;    /* be u16 @0x70 */
            u8 ready_before = spurs_ready_consume_one(ready_ea);
            int kicked = (ready_before != 0) || ((sig & (0x8000u >> wid)) != 0);
            if (sig & (0x8000u >> wid))
                vm_write32(ea + SPURS_WKL_SIGNAL1,
                           (vm_read32(ea + SPURS_WKL_SIGNAL1) & ~((0x8000u >> wid) << 16)));

            WklPm* r = spurs_resolve_pm(ea, wid);
            if (!r) continue;

            /* Idle backoff: running EVERY enabled workload's module EVERY 1ms
             * pass (x N instance threads) burned ~5 host cores on modules that
             * immediately exit-to-kernel with no work, starving the actual
             * decode/render threads (LBP movie at ~1fps while 500% CPU).
             * A module that keeps finding nothing gets re-run every 2nd, 4th,
             * ... up to 16th pass; an explicit kick (readyCount/signal) resets
             * it to every pass, so kick-driven latency is unchanged. */
            {
                static u8 s_idle[MAX_SPURS_INST][CELL_SPURS_MAX_WORKLOAD];
                static u32 s_pass_no;                            /* shared pass counter is fine */
                if (wid == 0) s_pass_no++;
                if (kicked) s_idle[instance_slot][wid] = 0;
                u32 cad = 1u << (s_idle[instance_slot][wid] > 4 ? 4 : s_idle[instance_slot][wid]);
                if (!kicked && (s_pass_no & (cad - 1)) != 0) continue;
                extern volatile unsigned g_spurs_pm_polls;
                u32 polls_before = g_spurs_pm_polls;   /* heuristic only */
                (void)polls_before;

            {   static int _n = 0;
                if (_n < 8) { _n++;
                    fprintf(stderr, "[spurs-kern] \"%s\" dispatch wid=%u (enabled, state=2) "
                                    "image=%d ready=%u\n", si->prefix, wid, r->image_id, *rdy);
                    fflush(stderr); } }

            /* Live workload arg from the real wklInfo (the game may update it). */
            u64 arg = vm_read64(ea + SPURS_WKL_INFO1 + wid * SPURS_WKL_INFO_SZ + 8);
            /* Dispatch once per VIRTUAL SPU up to the workload's maxContention:
             * the WWS job manager keys its per-SPU ticket lane off the kernel
             * context's spuNum, and its command lists carry cross-lane BARRIER
             * commands -- with only spu 0 ever dispatched, lane 1 (pre-armed by
             * the PPU for a 2-SPU workload) never advanced and every barrier
             * deadlocked: LBP's post-intro loading froze with 8 ready jobs and
             * the lanes stuck at {1,0,...} against ticket 10+. Sequential
             * per-lane rounds converge where parallel SPUs would. */
            u8 maxcont = *(vm_base + ea + SPURS_WKL_MAXCONT + wid);
            if (maxcont < 1) maxcont = 1;
            if (maxcont > 6) maxcont = 6;
            /* Do NOT exceed the workload's own contention: per-SPU rows in
             * the WWS sync struct live at +0x40+16*spuNum, and dispatching
             * spuNum >= nSpus made virtual SPU 3 write its bookkeeping row
             * OVER the game's ticket row (row 3) -- observed as ticket values
             * jumping to garbage (141028, 9960...) during the savedata load.
             * The earlier "4 lanes drains the queue" result was partly that
             * scribble. Correct progress comes from PERSISTENT PM contexts
             * (spurs_policy.c), not extra lanes. */
            /* SPURS_FORCE_SPUS=<n>: dispatch every workload for n virtual SPUs
             * regardless of maxContention (A/B: LBP publishes its loading
             * tickets on sync row 3, and the PM's row index = spuNum).
             * 2026-07-23 FINDING: with the completion HLE (LBP_HLE_JOBDONE) in
             * place, forcing 2+ lanes is now COUNTERPRODUCTIVE -- two host
             * threads race on the shared jobIndex atomic in WwsJob_AllocateJob
             * (GETLLAR 0xD0/PUTLLC 0xB4 on the joblist header), making the boot
             * nondeterministic. Single-lane (leave SPURS_FORCE_SPUS unset) is
             * both correct (RPCS3 oracle: jobmanager runs one SPU at a time) and
             * far more stable -- it reaches the furthest boot yet (LBP renders to
             * finish#922: glyphthread+network+camera up). PREFER single-lane. */
            { static int s_fs = -2;
              if (s_fs == -2) { const char* e = getenv("SPURS_FORCE_SPUS");
                s_fs = e ? atoi(e) : -1; }
              if (s_fs > 0) maxcont = (u8)(s_fs > 6 ? 6 : s_fs); }
            *(vm_base + ea + SPURS_WKL_CURCONT + wid) = maxcont;
            {
                const uint8_t* pm = (const uint8_t*)vm_base + (uint32_t)(uintptr_t)workload->pm;
                uint32_t sz = workload->sizePm;
                if (maxcont <= 1) {
                    spu_run_policy_module(r->fn, r->image_id, pm, sz, arg, wid, ea, 0);
                } else {
                    /* Run the workload's virtual SPUs CONCURRENTLY (see
                     * spurs_pm_worker): each lane advances in parallel so the
                     * cross-SPU barrier resolves. Spawn maxcont-1 workers for
                     * lanes 1..N-1 and run lane 0 on this thread, then join. */
                    HANDLE th[8]; struct spurs_pm_worker_arg wa[8];
                    unsigned nth = 0;
                    for (u32 sn = 1; sn < maxcont && nth < 7; sn++, nth++) {
                        wa[nth].fn = r->fn; wa[nth].image_id = r->image_id;
                        wa[nth].pm = pm; wa[nth].pm_size = sz;
                        wa[nth].arg = arg; wa[nth].wid = wid; wa[nth].ea = ea;
                        wa[nth].spu_num = sn;
                        th[nth] = CreateThread(NULL, 1u << 20, spurs_pm_worker, &wa[nth], 0, NULL);
                    }
                    spu_run_policy_module(r->fn, r->image_id, pm, sz, arg, wid, ea, 0);
                    if (nth) {
                        WaitForMultipleObjects(nth, th, TRUE, INFINITE);
                        for (unsigned k = 0; k < nth; k++) CloseHandle(th[k]);
                    }
                }
            }
            *(vm_base + ea + SPURS_WKL_CURCONT + wid) = 0;
            /* "Found work" heuristic: a module that did something polls the
             * kernel for MORE work before exiting (selectWorkload calls >0);
             * an idle module exits immediately with polls==0. Grow the idle
             * streak on the latter, reset on the former. */
            if (g_spurs_pm_polls == 0) { if (s_idle[instance_slot][wid] < 8) s_idle[instance_slot][wid]++; }
            else s_idle[instance_slot][wid] = 0;
            }
        }
    }
    InterlockedExchange(&si->kernel_live, 0);
    guest_world_unregister_current();
    return 0;
}
#endif

s32 cellSpursReadyCountStore(CellSpurs* spurs, CellSpursWorkloadId wid,
                             u32 value)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    u32 spurs_ea = (u32)(uintptr_t)spurs;
    if (spurs_ea & 0x7Fu) return CELL_SPURS_CORE_ERROR_ALIGN;
    if (wid >= CELL_SPURS_MAX_WORKLOAD || value > 0xFFu)
        return CELL_SPURS_CORE_ERROR_INVAL;
    SpursWorkload* w = spurs_workload_find(spurs_ea, wid);
    if (!w) return CELL_SPURS_CORE_ERROR_SRCH;
    if (*(vm_base + spurs_ea + SPURS_WKL_STATE1 + wid) != 2)
        return CELL_SPURS_CORE_ERROR_STAT;

    /* The real store: the instance byte the kernel (poll thread) watches. */
    spurs_ready_exchange(spurs_ea + SPURS_WKL_READY1 + wid, (u8)value);
    {   static int _n = 0;
        if (_n < 32)
            printf("[cellSpurs] ReadyCountStore(wid=%u, value=%u)\n", wid, value);
        else if (_n == 32)
            printf("[cellSpurs] ReadyCountStore further logs suppressed\n");
        _n++;
    }
    return CELL_OK;
}

s32 cellSpursReadyCountSwap(CellSpurs* spurs, CellSpursWorkloadId wid,
                            u32* old, u32 value)
{
    if (!spurs || !old) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    u32 spurs_ea = (u32)(uintptr_t)spurs;
    u32 old_ea = (u32)(uintptr_t)old;
    if ((spurs_ea & 0x7Fu) || (old_ea & 3u)) return CELL_SPURS_CORE_ERROR_ALIGN;
    if (wid >= CELL_SPURS_MAX_WORKLOAD || value > 0xFFu)
        return CELL_SPURS_CORE_ERROR_INVAL;
    SpursWorkload* w = spurs_workload_find(spurs_ea, wid);
    if (!w) return CELL_SPURS_CORE_ERROR_SRCH;
    if (*(vm_base + spurs_ea + SPURS_WKL_STATE1 + wid) != 2)
        return CELL_SPURS_CORE_ERROR_STAT;

    u8 previous = spurs_ready_exchange(spurs_ea + SPURS_WKL_READY1 + wid,
                                       (u8)value);
    vm_write32(old_ea, previous);
    return CELL_OK;
}

s32 cellSpursReadyCountCompareAndSwap(CellSpurs* spurs,
                                       CellSpursWorkloadId wid,
                                       u32* old, u32 compare, u32 value)
{
    if (!spurs || !old) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    u32 spurs_ea = (u32)(uintptr_t)spurs;
    u32 old_ea = (u32)(uintptr_t)old;
    if ((spurs_ea & 0x7Fu) || (old_ea & 3u)) return CELL_SPURS_CORE_ERROR_ALIGN;
    if (wid >= CELL_SPURS_MAX_WORKLOAD || (compare | value) > 0xFFu)
        return CELL_SPURS_CORE_ERROR_INVAL;
    SpursWorkload* w = spurs_workload_find(spurs_ea, wid);
    if (!w) return CELL_SPURS_CORE_ERROR_SRCH;
    if (*(vm_base + spurs_ea + SPURS_WKL_STATE1 + wid) != 2)
        return CELL_SPURS_CORE_ERROR_STAT;

    u8 previous = spurs_ready_compare_exchange(
        spurs_ea + SPURS_WKL_READY1 + wid, (u8)compare, (u8)value);
    vm_write32(old_ea, previous);
    return CELL_OK;
}

s32 cellSpursWakeUp(CellSpurs* spurs)
{
    if (!spurs) return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    /* In a full implementation, wake the worker threads */
    return CELL_OK;
}

/* =========================================================================
 * Event flags
 * =====================================================================*/

static s32 spurs_event_flag_initialize_common(uint32_t spurs_ea,
                                              uint32_t taskset_ea,
                                              uint32_t eventFlag_ea,
                                              u32 clearMode, u32 direction)
{
    if (!eventFlag_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (eventFlag_ea & 0x7F)
        return CELL_SPURS_TASK_ERROR_ALIGN;
    if (clearMode > CELL_SPURS_EVENT_FLAG_CLEAR_MANUAL ||
        direction > CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_INVAL;
    if (!taskset_ea && !spurs_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    /* Re-initialization: recycle the sync slot. */
    EventFlagSync* old = ef_sync_find(eventFlag_ea);
    if (old) ef_sync_free(old);

    for (uint32_t o = 0; o < EF_GUEST_SIZE; o += 8)
        vm_write64(eventFlag_ea + o, 0);
    vm_write8(eventFlag_ea + EF_DIRECTION,  (uint8_t)direction);
    vm_write8(eventFlag_ea + EF_CLEAR_MODE, (uint8_t)clearMode);
    vm_write8(eventFlag_ea + EF_SPU_PORT, 0xFFu);
    if (taskset_ea) {
        vm_write8(eventFlag_ea + EF_IS_IWL, 0);
        vm_write64(eventFlag_ea + EF_ADDR, (uint64_t)taskset_ea);
    } else {
        vm_write8(eventFlag_ea + EF_IS_IWL, 1);
        vm_write64(eventFlag_ea + EF_ADDR, (uint64_t)spurs_ea);
    }
    spu_reservation_notify_write(eventFlag_ea, EF_GUEST_SIZE);

    EventFlagSync* sync = ef_sync_alloc(eventFlag_ea);
    if (!sync) {
        printf("[cellSpurs] EventFlagInitialize: no free sync slots!\n");
        return CELL_SPURS_TASK_ERROR_NOMEM;
    }

    printf("[cellSpurs] EventFlagInitialize(clearMode=%u, direction=%u) "
           "flagEA=0x%08X owner=0x%08X isIwl=%u\n",
           clearMode, direction, eventFlag_ea,
           taskset_ea ? taskset_ea : spurs_ea, taskset_ea ? 0u : 1u);
    return CELL_OK;
}

s32 cellSpursEventFlagInitialize(CellSpursTaskset* taskset,
                                 CellSpursEventFlag* eventFlag,
                                 u32 clearMode, u32 direction)
{
    return spurs_event_flag_initialize_common(
        0, (uint32_t)(uintptr_t)taskset, (uint32_t)(uintptr_t)eventFlag,
        clearMode, direction);
}

s32 cellSpursEventFlagAttachLv2EventQueue(CellSpursEventFlag* eventFlag)
{
    uint32_t ea = (uint32_t)(uintptr_t)eventFlag;
    if (!ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (ea & 0x7Fu) return CELL_SPURS_TASK_ERROR_ALIGN;
    u8 direction = vm_read8(ea + EF_DIRECTION);
    if (direction != CELL_SPURS_EVENT_FLAG_SPU2PPU &&
        direction != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    if (vm_read8(ea + EF_SPU_PORT) != 0xFFu)
        return CELL_SPURS_TASK_ERROR_STAT;

    uint32_t owner = (uint32_t)vm_read64(ea + EF_ADDR);
    uint32_t spurs_ea = vm_read8(ea + EF_IS_IWL)
                      ? owner : (uint32_t)vm_read64(owner + CSTS_SPURS);
    struct SpursInst* si = spurs_inst_find(spurs_ea);
    if (!si) return CELL_SPURS_TASK_ERROR_STAT;

    u32 port = 0x10u;
    while (port <= 0x3Fu && (si->event_flag[port] || si->event_queue[port]))
        port++;
    if (port > 0x3Fu) return CELL_SPURS_TASK_ERROR_BUSY;
    si->event_flag[port] = ea;
    vm_write8(ea + EF_SPU_PORT, (u8)port);
    spu_reservation_notify_write(ea + EF_SPU_PORT, sizeof(u8));
    printf("[cellSpurs] EventFlagAttachLv2EventQueue(flag=0x%08X spurs=0x%08X port=%u)\n",
           ea, spurs_ea, port);
    return CELL_OK;
}

s32 cellSpursEventFlagDetachLv2EventQueue(CellSpursEventFlag* eventFlag)
{
    uint32_t ea = (uint32_t)(uintptr_t)eventFlag;
    if (!ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if (ea & 0x7Fu) return CELL_SPURS_TASK_ERROR_ALIGN;
    u32 port = vm_read8(ea + EF_SPU_PORT);
    if (port > 0x3Fu) return CELL_SPURS_TASK_ERROR_STAT;
    uint32_t owner = (uint32_t)vm_read64(ea + EF_ADDR);
    uint32_t spurs_ea = vm_read8(ea + EF_IS_IWL)
                      ? owner : (uint32_t)vm_read64(owner + CSTS_SPURS);
    struct SpursInst* si = spurs_inst_find(spurs_ea);
    if (si && si->event_flag[port] == ea) si->event_flag[port] = 0;
    vm_write8(ea + EF_SPU_PORT, 0xFFu);
    spu_reservation_notify_write(ea + EF_SPU_PORT, sizeof(u8));
    printf("[cellSpurs] EventFlagDetachLv2EventQueue(flag=0x%08X port=%u)\n",
           ea, port);
    return CELL_OK;
}

s32 cellSpursEventFlagSet(CellSpursEventFlag* eventFlag, u16 bits)
{
    uint32_t ea = (uint32_t)(uintptr_t)eventFlag;
    if (!ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    EventFlagSync* sync = ef_sync_get(ea);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    { static int _n=0; if (_n++ < 40 || (_n%1000)==0)
        fprintf(stderr, "[cellSpurs] EventFlagSet#%d flagEA=0x%08X bits=0x%04X "
                "events=0x%04X used=0x%04X pend=0x%04X mode=0x%04X\n",
                _n, ea, (unsigned)bits,
                vm_read16(ea + EF_EVENTS), vm_read16(ea + EF_SPU_USED_SLOTS),
                vm_read16(ea + EF_SPU_PENDING_RECV), vm_read16(ea + EF_SPU_WAIT_MODE)); }

    ef_lock(sync);
    spurs_ef_set_locked(ea, bits);
    ef_broadcast(sync);
    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagWait(CellSpursEventFlag* eventFlag, u16* bits,
                           u32 mode)
{
    uint32_t ea      = (uint32_t)(uintptr_t)eventFlag;
    uint32_t bits_ea = (uint32_t)(uintptr_t)bits;
    if (!ea || !bits_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (ea & 0x7Fu) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (mode > CELL_SPURS_EVENT_FLAG_AND) return CELL_SPURS_TASK_ERROR_INVAL;
    u8 direction = vm_read8(ea + EF_DIRECTION);
    if (direction != CELL_SPURS_EVENT_FLAG_SPU2PPU &&
        direction != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;
    if (vm_read8(ea + EF_SPU_PORT) == 0xFFu)
        return CELL_SPURS_TASK_ERROR_STAT;

    EventFlagSync* sync = ef_sync_get(ea);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;

    u16 pattern = vm_read16(bits_ea);
    u16 received = 0;
    u8 wait_slot = 0;
    int registered = 0;
    unsigned waits = 0;
    unsigned late_checks = 0;
    int late_waiter_active = 0;
    int late_blocked_reported = 0;
    uint64_t late_waiter_started_ms = 0;
    TJLateEventFlagCounters late_baseline = {0};
#ifdef _WIN32
    const int wait_perf = tj_event_flag_wait_perf_requested();
    LARGE_INTEGER wait_perf_start = {0};
    LARGE_INTEGER wait_perf_frequency = {0};
    uint64_t wait_poll_total_us = 0;
    uint64_t wait_poll_min_us = UINT64_MAX;
    uint64_t wait_poll_max_us = 0;
    unsigned wait_poll_calls = 0;
    if (wait_perf) {
        QueryPerformanceFrequency(&wait_perf_frequency);
        QueryPerformanceCounter(&wait_perf_start);
    }
#endif

    ef_lock(sync);
    for (;;) {
        u64 ctrl = vm_read64(ea);
        if (ef_ctrl_ppu_mask(ctrl) || ef_ctrl_pending(ctrl)) {
            ef_unlock(sync);
            return CELL_SPURS_TASK_ERROR_BUSY;
        }

        u16 events = ef_ctrl_events(ctrl);
        u16 relevant = (u16)(events & pattern);
        u64 desired = ctrl;
        int registered_this_attempt = 0;

        if (direction == CELL_SPURS_EVENT_FLAG_ANY2ANY) {
            u16 relevant_slots = (u16)(vm_read16(ea + EF_SPU_USED_SLOTS) &
                                       ~(u16)(ctrl >> 32));
            if (mode == CELL_SPURS_EVENT_FLAG_OR)
                relevant_slots &= vm_read16(ea + EF_SPU_WAIT_MODE);
            for (int slot = CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS - 1;
                 relevant_slots; slot--, relevant_slots >>= 1) {
                if ((relevant_slots & 1u) &&
                    (vm_read16(ea + EF_SPU_WAIT_MASK_ARR + 2u * slot) & pattern) &&
                    vm_read16(ea + EF_SPU_WAIT_MASK_ARR + 2u * slot) != pattern) {
                    ef_unlock(sync);
                    return CELL_SPURS_TASK_ERROR_AGAIN;
                }
            }
        }

        if (ef_wait_satisfied(events, pattern, mode)) {
            received = relevant;
            if (vm_read8(ea + EF_CLEAR_MODE) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO) {
                desired &= ~EF_CTRL_EVENTS_MASK;
                desired |= (u64)(u16)(events & ~relevant) << 48;
            }
        } else {
            if (direction == CELL_SPURS_EVENT_FLAG_ANY2ANY) {
                u16 used = vm_read16(ea + EF_SPU_USED_SLOTS);
                int i = 0;
                while (i < CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS && (used & 1u)) {
                    used >>= 1;
                    i++;
                }
                if (i == CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS) {
                    ef_unlock(sync);
                    return CELL_SPURS_TASK_ERROR_BUSY;
                }
                wait_slot = (u8)(CELL_SPURS_EVENT_FLAG_MAX_WAIT_SLOTS - 1 - i);
            }
            u8 slot_mode = (u8)((wait_slot << 4) | mode);
            desired &= ~(EF_CTRL_PPU_WAIT_MASK | EF_CTRL_PPU_SLOT_MODE |
                         EF_CTRL_PPU_PENDING);
            desired |= (u64)pattern << 16;
            desired |= (u64)slot_mode << 8;
            registered_this_attempt = 1;
        }

        u64 expected = ctrl;
#ifdef PS3RECOMP_SPURS_EVENT_FLAG_TEST
        extern void ps3recomp_test_before_eventflag_wait_cas(uint32_t, uint64_t);
        ps3recomp_test_before_eventflag_wait_cas(ea, ctrl);
#endif
        if (spu_reservation_compare_exchange64(ea, &expected, desired)) {
            /* A producer may win between our read and CAS.  The retry can then
             * consume its queued event immediately, so durable wait state must
             * describe only the iteration whose CAS actually succeeded. */
            registered = registered_this_attempt;
            if (registered_this_attempt)
                tj_late_eventflag_trace_note_ppu_registration(ea, ctrl, desired);
            break;
        }
    }

    if (registered && tj_survival_trace_is_armed() &&
        tj_event_flag_trace_requested() && (pattern & 0x0300u)) {
        /* Tokyo Jungle's Survival renderer alternates 0x0100/0x0200
         * SPU-to-PPU completions on this flag. Arm the existing bounded atomic
         * watcher as soon as either waiter registers so a missing producer,
         * wrong bit, or failed reservation is visible without forcing the
         * completion. */
        g_spu_event_flag_watch_ea = ea & ~0x7Fu;
        fprintf(stderr, "[cellSpurs] EventFlagWait REGISTER flagEA=0x%08X "
                        "pattern=0x%04X slot=%u mode=%u\n",
                ea, pattern, wait_slot, mode);
    }
    if (registered && tj_survival_trace_is_armed() &&
        pattern == 0x0001u && tj_late_job_trace_requested()) {
        g_spu_event_flag_watch_ea = ea & ~0x7Fu;
        fprintf(stderr,
                "[cellSpurs] EventFlagWait WATCH flagEA=0x%08X raw=0x%016llX "
                "slot=%u mode=%u\n",
                ea, (unsigned long long)vm_read64(ea), wait_slot, mode);
        fflush(stderr);
    }
    if (registered && tj_survival_trace_is_armed() &&
        tj_late_eventflag_trace_matches(ea)) {
        /* Baseline precedes activation: producer counters can only contribute
         * after exact waiter has registered its PPU mask. */
        tj_late_eventflag_trace_snapshot(&late_baseline);
        late_waiter_active = tj_late_eventflag_trace_arm(ea);
        if (late_waiter_active)
            late_waiter_started_ms = tj_late_eventflag_now_ms();
    }

    int spun = 0;
    while (registered) {
        /* A GPU interrupt can arrive after the HLE entry safe point. Its guest
         * handler can start the SPU work that satisfies this very wait. Keep
         * delivery on the main PPU, outside the EventFlag lock; then re-read
         * real pending state. The loader also excludes nested callbacks. */
        if (g_active_ctx && g_active_ctx->thread_id == 1) {
            ef_unlock(sync);
            ppu_poll_display_callbacks(g_active_ctx);
            ef_lock(sync);
        }
        u64 ctrl = vm_read64(ea);
        if (ef_ctrl_pending(ctrl)) {
            wait_slot = (u8)(ef_ctrl_slot_mode(ctrl) >> 4);
            received = vm_read16(ea + EF_PENDING_RECV_EVT + 2u * wait_slot);
            for (;;) {
                u64 desired = ctrl & ~EF_CTRL_PPU_PENDING;
                u64 expected = ctrl;
                if (spu_reservation_compare_exchange64(ea, &expected, desired)) {
                    tj_late_eventflag_trace_note_ppu_pending_clear(ea, ctrl, desired);
                    break;
                }
                ctrl = expected;
                if (!ef_ctrl_pending(ctrl))
                    break;
            }
            break;
        }

#ifdef _WIN32
        if (!spun && g_active_ctx && g_active_ctx->thread_id == 1 && tj_ef_spin_us()) {
            spun = 1;                  /* once per wait, then sleep as before */
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            const LONGLONG limit = f.QuadPart * (LONGLONG)tj_ef_spin_us() / 1000000;
            ef_unlock(sync);
            for (;;) {
                if (ef_ctrl_pending(vm_read64(ea))) break;
                QueryPerformanceCounter(&t1);
                if (t1.QuadPart - t0.QuadPart >= limit) break;
                YieldProcessor();
            }
            ef_lock(sync);
            continue;                  /* re-check under the lock */
        }
#endif
        int signaled;
#ifdef _WIN32
        LARGE_INTEGER wait_poll_start = {0};
        LARGE_INTEGER wait_poll_end = {0};
        if (wait_perf) QueryPerformanceCounter(&wait_poll_start);
#endif
        signaled = ef_wait_timed(sync, 2);
#ifdef _WIN32
        if (wait_perf) {
            QueryPerformanceCounter(&wait_poll_end);
            uint64_t poll_us = tj_event_flag_wait_qpc_us(
                wait_poll_start.QuadPart, wait_poll_end.QuadPart,
                wait_perf_frequency.QuadPart);
            wait_poll_total_us += poll_us;
            if (poll_us < wait_poll_min_us) wait_poll_min_us = poll_us;
            if (poll_us > wait_poll_max_us) wait_poll_max_us = poll_us;
            ++wait_poll_calls;
        }
#endif
        if (late_waiter_active && !late_blocked_reported &&
            ++late_checks % 128u == 0 &&
            tj_late_eventflag_now_ms() - late_waiter_started_ms >= 3000u) {
            tj_late_eventflag_report("BLOCKED", ea, pattern,
                                     late_waiter_started_ms, &late_baseline);
            late_blocked_reported = 1;
        }
        if (!signaled && ++waits % 500 == 0 && waits <= 12000 &&
            (!(pattern & 0x0300u) || tj_event_flag_trace_requested())) {
            fprintf(stderr, "[cellSpurs] EventFlagWait BLOCKED %us pattern=0x%04X "
                            "events=0x%04X ppuMask=0x%04X pending=%u flagEA=0x%08X\n",
                    waits / 500, pattern, ef_ctrl_events(ctrl),
                    ef_ctrl_ppu_mask(ctrl), ef_ctrl_pending(ctrl), ea);
        }
    }

    vm_write16(bits_ea, received);
    spu_reservation_notify_write(bits_ea, sizeof(u16));
    if (late_waiter_active) {
        uint64_t elapsed = tj_late_eventflag_now_ms() - late_waiter_started_ms;
        if (elapsed >= 3000u || tj_late_eventflag_take_receive_report())
            tj_late_eventflag_report("RECEIVE", ea, pattern,
                                     late_waiter_started_ms, &late_baseline);
        tj_late_eventflag_trace_disarm();
    }
    if ((pattern & 0x0300u) && tj_event_flag_trace_requested()) {
        fprintf(stderr, "[cellSpurs] EventFlagWait RECEIVE flagEA=0x%08X "
                        "pattern=0x%04X received=0x%04X waits=%u\n",
                ea, pattern, received, waits);
    }
    ef_unlock(sync);
#ifdef _WIN32
    if (wait_perf) {
        LARGE_INTEGER wait_perf_end;
        static volatile LONG wait_perf_sequence = 0;
        QueryPerformanceCounter(&wait_perf_end);
        LONG sequence = InterlockedIncrement(&wait_perf_sequence);
        if (sequence <= 512) {
            uint64_t elapsed_us = tj_event_flag_wait_qpc_us(
                wait_perf_start.QuadPart, wait_perf_end.QuadPart,
                wait_perf_frequency.QuadPart);
            fprintf(stderr,
                    "[TJ-EVENTFLAG-WAIT-PERF] seq=%ld flag=0x%08X "
                    "pattern=0x%04X received=0x%04X registered=%d "
                    "timeouts=%u polls=%u poll_total_us=%llu "
                    "poll_min_us=%llu poll_max_us=%llu elapsed_us=%llu\n",
                    sequence, ea, pattern, received, registered, waits,
                    wait_poll_calls,
                    (unsigned long long)wait_poll_total_us,
                    (unsigned long long)(wait_poll_calls ? wait_poll_min_us : 0),
                    (unsigned long long)wait_poll_max_us,
                    (unsigned long long)elapsed_us);
        }
    }
#endif
    return CELL_OK;
}

s32 cellSpursEventFlagTryWait(CellSpursEventFlag* eventFlag, u16* bits,
                              u32 mode)
{
    uint32_t ea      = (uint32_t)(uintptr_t)eventFlag;
    uint32_t bits_ea = (uint32_t)(uintptr_t)bits;
    if (!ea || !bits_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    if (ea & 0x7Fu) return CELL_SPURS_TASK_ERROR_ALIGN;
    if (mode > CELL_SPURS_EVENT_FLAG_AND) return CELL_SPURS_TASK_ERROR_INVAL;
    u8 direction = vm_read8(ea + EF_DIRECTION);
    if (direction != CELL_SPURS_EVENT_FLAG_SPU2PPU &&
        direction != CELL_SPURS_EVENT_FLAG_ANY2ANY)
        return CELL_SPURS_TASK_ERROR_PERM;

    EventFlagSync* sync = ef_sync_get(ea);
    if (!sync) return CELL_SPURS_TASK_ERROR_STAT;

    u16 pattern = vm_read16(bits_ea);
    u16 received;
    ef_lock(sync);
    for (;;) {
        u64 ctrl = vm_read64(ea);
        if (ef_ctrl_ppu_mask(ctrl) || ef_ctrl_pending(ctrl)) {
            ef_unlock(sync);
            return CELL_SPURS_TASK_ERROR_BUSY;
        }
        u16 events = ef_ctrl_events(ctrl);
        if (!ef_wait_satisfied(events, pattern, mode)) {
            ef_unlock(sync);
            return CELL_SPURS_TASK_ERROR_BUSY;
        }
        received = (u16)(events & pattern);
        u64 desired = ctrl;
        if (vm_read8(ea + EF_CLEAR_MODE) == CELL_SPURS_EVENT_FLAG_CLEAR_AUTO) {
            desired &= ~EF_CTRL_EVENTS_MASK;
            desired |= (u64)(u16)(events & ~received) << 48;
        }
        u64 expected = ctrl;
        if (spu_reservation_compare_exchange64(ea, &expected, desired))
            break;
    }

    vm_write16(bits_ea, received);
    spu_reservation_notify_write(bits_ea, sizeof(u16));
    ef_unlock(sync);
    return CELL_OK;
}

s32 cellSpursEventFlagClear(CellSpursEventFlag* eventFlag, u16 bits)
{
    uint32_t ea = (uint32_t)(uintptr_t)eventFlag;
    if (!ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    EventFlagSync* sync = ef_sync_get(ea);
    if (!sync)
        return CELL_SPURS_TASK_ERROR_STAT;

    ef_lock(sync);
    spu_reservation_lock();
    vm_write16(ea + EF_EVENTS, (u16)(vm_read16(ea + EF_EVENTS) & ~bits));
    spu_reservation_bump_locked(ea);
    spu_reservation_unlock();
    ef_unlock(sync);

    return CELL_OK;
}

s32 cellSpursEventFlagGetDirection(CellSpursEventFlag* eventFlag,
                                   u32* direction)
{
    uint32_t ea     = (uint32_t)(uintptr_t)eventFlag;
    uint32_t dir_ea = (uint32_t)(uintptr_t)direction;
    if (!ea || !dir_ea)
        return CELL_SPURS_TASK_ERROR_NULL_POINTER;

    vm_write32(dir_ea, vm_read8(ea + EF_DIRECTION));
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Additional functions needed by Tokyo Jungle (from RPCS3 audit)
 * -----------------------------------------------------------------------*/

/* _cellSpursEventFlagInitialize — internal init with more parameters */
s32 _cellSpursEventFlagInitialize(void* spurs, void* taskset,
                                    CellSpursEventFlag* eventFlag,
                                    u32 clearMode, u32 direction)
{
    printf("[cellSpurs] _EventFlagInitialize(clearMode=%u, dir=%u)\n",
           clearMode, direction);
    return spurs_event_flag_initialize_common(
        (uint32_t)(uintptr_t)spurs, (uint32_t)(uintptr_t)taskset,
        (uint32_t)(uintptr_t)eventFlag, clearMode, direction);
}

/* _cellSpursSendSignal — internal signal delivery */
s32 _cellSpursSendSignal(void* taskset, u32 taskId)
{
    uint32_t taskset_ea = (uint32_t)(uintptr_t)taskset;
    printf("[cellSpurs] _SendSignal(taskset=0x%08X id=%u)\n", taskset_ea, taskId);
    /* Real delivery now that SPU tasks execute (comment was stale). */
    if (taskset_ea) spu_taskset_signal_task(taskset_ea, taskId);
    return CELL_OK;
}

/* =========================================================================
 * Job chains (LBP's render path: the game emits SPURS job descriptors and
 * chains them via u64 command words; the jobchain policy module walks the
 * chain on SPU). Facts-first bring-up: decode + log the REAL BE guest
 * structures at the SDK offsets; execution wiring lands once the logged
 * shapes confirm the descriptor formats.
 * =====================================================================*/

/* Real (BE) CellSpursJobChainAttribute offsets. */
enum {
    JCATTR_REVISION   = 0x00,   /* be u32 */
    JCATTR_SDKVERSION = 0x04,   /* be u32 */
    JCATTR_ENTRY      = 0x08,   /* be u32: EA of the first jobchain command word */
    JCATTR_SIZE_DESC  = 0x0C,   /* be u16: sizeJobDescriptor */
    JCATTR_MAX_GRAB   = 0x0E,   /* be u16: maxGrabbedJob */
    JCATTR_PRIORITY   = 0x10,   /* u8[8] */
    JCATTR_MAX_CONT   = 0x18,   /* be u32 */
    JCATTR_AUTO_RDY   = 0x1C,   /* u8 bool: autoReadyCount */
    JCATTR_TAG1       = 0x20,   /* be u32 */
    JCATTR_TAG2       = 0x24,   /* be u32 */
    JCATTR_FIXED_MEM  = 0x28,   /* u8 bool */
    JCATTR_MAX_SIZE_D = 0x2C,   /* be u32 */
    JCATTR_INIT_SPU   = 0x30,   /* be u32 */
    JCATTR_HALT_ERROR = 0x34,   /* be u32 */
    JCATTR_NAME       = 0x38,   /* be u32: char* EA (SetName) */
    JCATTR_JOB_CHECK  = 0x3C,   /* u8 bit */
};

/* Host-side jobchain registry (the CellSpursJobChain guest struct is opaque
 * to the game; we track what we need beside it). */
#define MAX_JOBCHAINS 32
static struct {
    u32 jc_ea;        /* CellSpursJobChain EA (0 = free) */
    u32 spurs_ea;     /* owning CellSpurs instance EA */
    u32 entry_ea;     /* first command word EA */
    u32 resume_pc, resume_return_pc; /* parked GUARD continuation, owned by walker */
    u16 size_desc;
    u16 max_grab;
    u32 max_cont;
    /* SPU request (ready) count: with autoRequestSpuCount off the kernel puts
     * at most this many SPUs on the chain. Set from initialRequestSpuCount on
     * Run, from numReadyCount on Kick, from the guard's requestSpuCount when a
     * JobGuard restarts the chain. Only TJ_JOBCHAIN_LANES reads it. */
    u8 auto_request;
    u8 init_request;
    volatile u8 request_spus;
    u32 generation;   /* changes on every in-place recreate; guards Join cleanup */
    int run_count;
    volatile long running;   /* 1 while a host thread walks this chain */
    volatile long worker_count; /* live and retiring host workers for this row */
    volatile long run_requested; /* latched rerun across worker shutdown */
    volatile long shutdown_requested;
    volatile int64_t snapshot_pc, snapshot_command, snapshot_job;
    volatile int64_t perf_request_qpc;
    volatile int64_t perf_pending_request_qpc;
    volatile int64_t perf_complete_qpc;
    volatile int64_t perf_jobs_current;
    volatile int64_t perf_runs, perf_jobs, perf_total_us, perf_total_max_us;
    volatile int64_t perf_start_us, perf_start_max_us;
    volatile int64_t perf_exec_us, perf_exec_max_us;
    volatile int64_t perf_threads, perf_coalesced;
    volatile int64_t perf_join_calls, perf_join_wait_us, perf_join_wait_max_us;
    volatile int64_t perf_join_overshoot_us, perf_join_overshoot_max_us;
} s_jobchains[MAX_JOBCHAINS];
static u32 s_jobchain_next_generation;

/* Focused, opt-in job-chain evidence.  Unlike PS3_SPURS_TRACE this has a small
 * global budget and can be filtered to one chain by setting TJ_JOBCHAIN_TRACE
 * to its EA.  Target fingerprint detection remains in the workload dispatcher
 * after this diagnostic budget is exhausted. */
#define TJ_JOBCHAIN_TRACE_CAP 160
#define TJ_JOBCHAIN_TRACE_CRITICAL_CAP 128
#define TJ_JOBCHAIN_LIFECYCLE_TRACE_CAP 96
static u32 s_tj_jobchain_trace_jc;
static int s_tj_jobchain_trace_after_scene;
static volatile long s_tj_jobchain_trace_scene_armed;
static int tj_jobchain_trace_enabled(void)
{
    static volatile long state;
    static int enabled;
    long observed;
    if (state == 2) return enabled;   /* initialized: plain read per job */
#ifdef _WIN32
    observed = InterlockedCompareExchange(&state, 1, 0);
    if (observed == 0) {
        const char* text = getenv("TJ_JOBCHAIN_TRACE");
        enabled = text && *text ? 1 : 0;
        if (enabled && strcmp(text, "1") != 0)
            s_tj_jobchain_trace_jc = (u32)strtoul(text, NULL, 0);
        s_tj_jobchain_trace_after_scene =
            getenv("TJ_JOBCHAIN_TRACE_AFTER_SCENE") ? 1 : 0;
        InterlockedExchange(&state, 2);
        return enabled;
    }
    while (observed == 1) {
        Sleep(0);
        observed = InterlockedCompareExchange(&state, 0, 0);
    }
#else
    observed = __sync_val_compare_and_swap(&state, 0, 1);
    if (observed == 0) {
        const char* text = getenv("TJ_JOBCHAIN_TRACE");
        enabled = text && *text ? 1 : 0;
        if (enabled && strcmp(text, "1") != 0)
            s_tj_jobchain_trace_jc = (u32)strtoul(text, NULL, 0);
        s_tj_jobchain_trace_after_scene =
            getenv("TJ_JOBCHAIN_TRACE_AFTER_SCENE") ? 1 : 0;
        __sync_synchronize();
        __sync_lock_test_and_set(&state, 2);
        return enabled;
    }
    while (observed == 1)
        observed = __sync_val_compare_and_swap(&state, 0, 0);
#endif
    return enabled;
}

static int tj_jobchain_trace_matches(u32 jc_ea)
{
    return tj_jobchain_trace_enabled() &&
           (!s_tj_jobchain_trace_after_scene ||
#ifdef _WIN32
            InterlockedCompareExchange(&s_tj_jobchain_trace_scene_armed, 0, 0)
#else
            __atomic_load_n(&s_tj_jobchain_trace_scene_armed, __ATOMIC_ACQUIRE)
#endif
           ) &&
           (!s_tj_jobchain_trace_jc || s_tj_jobchain_trace_jc == jc_ea);
}

/* Keep lifecycle evidence separate from hot command/job/guard tracing, but
 * honor the same optional post-scene gate so startup cannot exhaust its cap. */
static void tj_jobchain_lifecycle_trace(const char* op, u32 jc_ea, u32 spurs_ea,
                                        u32 attr_ea, u32 entry_ea,
                                        const char* decision, int result)
{
    static volatile long count;
    long n;
    if (!tj_jobchain_trace_matches(jc_ea)) return;
#ifdef _WIN32
    n = InterlockedIncrement(&count);
#else
    n = __sync_add_and_fetch(&count, 1);
#endif
    if (n > TJ_JOBCHAIN_LIFECYCLE_TRACE_CAP) return;
    fprintf(stderr,
            "[TJ-JC-LIFECYCLE] op=%s jc=0x%08X spurs=0x%08X attr=0x%08X entry=0x%08X decision=%s result=%d tid=%lu lr=0x%08llX cia=0x%08llX\n",
            op, jc_ea, spurs_ea, attr_ea, entry_ea, decision, result,
#ifdef _WIN32
            (unsigned long)GetCurrentThreadId(),
#else
            0ul,
#endif
            (unsigned long long)(g_active_ctx ? g_active_ctx->lr : 0),
            (unsigned long long)(g_active_ctx ? g_active_ctx->cia : 0));
    fflush(stderr);
}

/* Called by the title's semantic scene-ready hook.  The release store makes
 * all later jobchain trace reads eligible without changing chain behaviour. */
void tj_jobchain_trace_arm_after_scene(void)
{
    (void)tj_jobchain_trace_enabled();
    if (!s_tj_jobchain_trace_after_scene) return;
#ifdef _WIN32
    InterlockedExchange(&s_tj_jobchain_trace_scene_armed, 1);
#else
    __atomic_store_n(&s_tj_jobchain_trace_scene_armed, 1, __ATOMIC_RELEASE);
#endif
}

static int tj_jobchain_trace_take(int critical)
{
    static volatile long count, critical_count;
    long n;
    if (!tj_jobchain_trace_enabled()) return 0;
#ifdef _WIN32
    n = InterlockedIncrement(critical ? &critical_count : &count);
#else
    n = __sync_add_and_fetch(critical ? &critical_count : &count, 1);
#endif
    return n <= (critical ? TJ_JOBCHAIN_TRACE_CRITICAL_CAP : TJ_JOBCHAIN_TRACE_CAP);
}

static void tj_jobchain_trace(const char* phase, u32 jc_ea, u32 entry_ea,
                              u32 command_ea, u32 descriptor_ea,
                              uint64_t fingerprint, const char* decision)
{
    const int target = fingerprint == UINT64_C(0x463CEB5F46CA2F82);
    if (!tj_jobchain_trace_matches(jc_ea) ||
        !tj_jobchain_trace_take(strcmp(phase, "TERMINAL") == 0)) {
        return;
    }
    fprintf(stderr,
            "[TJ-JC-TRACE] phase=%s jc=0x%08X entry=0x%08X cmd=0x%08X desc=0x%08X fp=%016llX target=%u decision=%s\n",
            phase, jc_ea, entry_ea, command_ea, descriptor_ea,
            (unsigned long long)fingerprint,
            target, decision);
    fflush(stderr);
}

static void tj_jobchain_trace_job(u32 jc_ea, u32 entry_ea,
                                  u32 descriptor_ea, u32 binary_ea,
                                  u32 binary_size)
{
    if (!tj_jobchain_trace_matches(jc_ea) || !tj_jobchain_trace_take(0)) return;
    fprintf(stderr,
            "[TJ-JC-TRACE] phase=JOB jc=0x%08X entry=0x%08X cmd=0x%08X desc=0x%08X binary=0x%08X size=%u decision=dispatch\n",
            jc_ea, entry_ea, 0u, descriptor_ea, binary_ea, binary_size);
    fflush(stderr);
}

static void tj_jobguard_trace(u32 jc_ea, u32 entry_ea, u32 command_ea,
                              u32 guard_ea, u32 count, u32 parked,
                              const char* decision)
{
    if (!tj_jobchain_trace_matches(jc_ea) || !tj_jobchain_trace_take(1)) return;
    fprintf(stderr,
            "[TJ-JC-TRACE] phase=GUARD jc=0x%08X entry=0x%08X cmd=0x%08X guard=0x%08X count=%u parked=%u decision=%s\n",
            jc_ea, entry_ea, command_ea, guard_ea, count, parked, decision);
    fflush(stderr);
}

static void tj_joblist_trace(u32 jc_ea, u32 entry_ea, u32 command_ea,
                             u32 header_ea, uint64_t guest_limit)
{
    uint8_t header[JOBCHAIN_JOBLIST_HEADER_SIZE];
    jobchain_joblist list;
    if (!tj_jobchain_trace_matches(jc_ea) || !tj_jobchain_trace_take(0)) return;
    if (header_ea && (header_ea & 15u) == 0 && vm_base &&
        guest_limit >= sizeof(header) &&
        (uint64_t)header_ea <= guest_limit - sizeof(header) &&
        (memcpy(header, vm_base + header_ea, sizeof(header)), 1) &&
        jobchain_joblist_decode(header, guest_limit, &list)) {
        fprintf(stderr,
                "[TJ-JC-TRACE] phase=JOBLIST jc=0x%08X entry=0x%08X cmd=0x%08X header=0x%08X num_jobs=%u size_desc=%u jobs_ea=0x%08X decision=decoded\n",
                jc_ea, entry_ea, command_ea, header_ea, list.num_jobs,
                list.size_desc, list.jobs_ea);
    } else {
        fprintf(stderr,
                "[TJ-JC-TRACE] phase=JOBLIST jc=0x%08X entry=0x%08X cmd=0x%08X header=0x%08X decision=invalid\n",
                jc_ea, entry_ea, command_ea, header_ea);
    }
    fflush(stderr);
}

static int s_tj_jobchain_perf_enabled;
#ifdef _WIN32
static uint64_t s_tj_jobchain_perf_frequency;
static volatile LONG s_tj_jobchain_perf_init_state;
static SRWLOCK s_tj_jobchain_perf_lock = SRWLOCK_INIT;
static SRWLOCK s_tj_jobchain_registry_lock = SRWLOCK_INIT;
static __declspec(thread) int s_tj_jobchain_perf_tls = -1;
#else
static pthread_once_t s_tj_jobchain_perf_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t s_tj_jobchain_perf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_tj_jobchain_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int s_tj_jobchain_perf_tls = -1;
#endif

static void tj_jobchain_perf_initialize(void)
{
    s_tj_jobchain_perf_enabled = getenv("TJ_JOBCHAIN_PERF") ? 1 : 0;
#ifdef _WIN32
    if (s_tj_jobchain_perf_enabled) {
        LARGE_INTEGER frequency;
        if (QueryPerformanceFrequency(&frequency))
            s_tj_jobchain_perf_frequency = (uint64_t)frequency.QuadPart;
        else
            s_tj_jobchain_perf_enabled = 0;
    }
#endif
}

/* Configure before a chain can publish a worker. Each host thread pays the
 * safe global-init check once, then reads only its immutable TLS gate. */
static void tj_jobchain_perf_configure(void)
{
#ifdef _WIN32
    LONG state = InterlockedCompareExchange(&s_tj_jobchain_perf_init_state, 1, 0);
    if (state == 0) {
        tj_jobchain_perf_initialize();
        InterlockedExchange(&s_tj_jobchain_perf_init_state,
                            s_tj_jobchain_perf_enabled ? 3 : 2);
        return;
    }
    while ((state = InterlockedCompareExchange(
                &s_tj_jobchain_perf_init_state, 0, 0)) == 1)
        Sleep(0);
#else
    pthread_once(&s_tj_jobchain_perf_once, tj_jobchain_perf_initialize);
#endif
}

static int tj_jobchain_perf_enabled(void)
{
    if (s_tj_jobchain_perf_tls < 0) {
        tj_jobchain_perf_configure();
        s_tj_jobchain_perf_tls = s_tj_jobchain_perf_enabled;
    }
    return s_tj_jobchain_perf_tls;
}

static void tj_jobchain_perf_lock(void)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_tj_jobchain_perf_lock);
#else
    pthread_mutex_lock(&s_tj_jobchain_perf_lock);
#endif
}

static void tj_jobchain_perf_unlock(void)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_tj_jobchain_perf_lock);
#else
    pthread_mutex_unlock(&s_tj_jobchain_perf_lock);
#endif
}

static void tj_jobchain_registry_lock(void)
{
#ifdef _WIN32
    AcquireSRWLockExclusive(&s_tj_jobchain_registry_lock);
#else
    pthread_mutex_lock(&s_tj_jobchain_registry_lock);
#endif
}

static void tj_jobchain_registry_unlock(void)
{
#ifdef _WIN32
    ReleaseSRWLockExclusive(&s_tj_jobchain_registry_lock);
#else
    pthread_mutex_unlock(&s_tj_jobchain_registry_lock);
#endif
}

static uint64_t tj_jobchain_perf_qpc(void)
{
#ifdef _WIN32
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static uint64_t tj_jobchain_perf_us(uint64_t begin, uint64_t end)
{
    if (end < begin) return 0;
#ifdef _WIN32
    uint64_t ticks = end - begin;
    return (ticks / s_tj_jobchain_perf_frequency) * 1000000ull +
           (ticks % s_tj_jobchain_perf_frequency) * 1000000ull /
           s_tj_jobchain_perf_frequency;
#else
    return (end - begin) / 1000ull;
#endif
}

static void tj_jobchain_perf_add(volatile int64_t* value, int64_t amount)
{
#ifdef _WIN32
    InterlockedAdd64((volatile LONG64*)value, (LONG64)amount);
#else
    __atomic_add_fetch(value, amount, __ATOMIC_RELAXED);
#endif
}

static int64_t tj_jobchain_perf_load(volatile int64_t* value)
{
#ifdef _WIN32
    return (int64_t)InterlockedCompareExchange64((volatile LONG64*)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static void tj_jobchain_perf_store(volatile int64_t* value, int64_t next)
{
#ifdef _WIN32
    InterlockedExchange64((volatile LONG64*)value, (LONG64)next);
#else
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
#endif
}

static int64_t tj_jobchain_perf_exchange(volatile int64_t* value, int64_t next)
{
#ifdef _WIN32
    return (int64_t)InterlockedExchange64((volatile LONG64*)value, (LONG64)next);
#else
    return __atomic_exchange_n(value, next, __ATOMIC_ACQ_REL);
#endif
}

static void tj_jobchain_perf_max(volatile int64_t* value, int64_t next)
{
#ifdef _WIN32
    LONG64 old = InterlockedCompareExchange64((volatile LONG64*)value, 0, 0);
    while (old < (LONG64)next &&
           InterlockedCompareExchange64((volatile LONG64*)value, (LONG64)next, old) != old)
        old = InterlockedCompareExchange64((volatile LONG64*)value, 0, 0);
#else
    int64_t old = __atomic_load_n(value, __ATOMIC_ACQUIRE);
    while (old < next && !__atomic_compare_exchange_n(value, &old, next, 0,
                                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
#endif
}

static int tj_jobchain_perf_compare_exchange(volatile int64_t* value,
                                              int64_t expected, int64_t next)
{
#ifdef _WIN32
    return InterlockedCompareExchange64((volatile LONG64*)value, (LONG64)next,
                                        (LONG64)expected) == (LONG64)expected;
#else
    return __atomic_compare_exchange_n(value, &expected, next, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
}

/* One [TJ-JOBCHAIN-PERF] row, copied under s_tj_jobchain_perf_lock. */
typedef struct tj_jobchain_perf_row {
    u32 jc_ea, max_cont;
    uint64_t runs, jobs, total, total_max, start, start_max, exec, exec_max;
    uint64_t threads, coalesced, joins, wait, wait_max, over, over_max;
} tj_jobchain_perf_row;

/* Caller holds s_tj_jobchain_perf_lock, so every copied row is one coherent
 * cumulative snapshot rather than a mix of partially updated run fields.
 * Returns a heap copy of the rows to print, or NULL when no report is due.
 * The caller releases its perf/registry locks, then tj_jobchain_perf_print
 * prints and frees it: chain workers and joins take those locks, and an
 * fprintf that blocked under them (full launcher stderr pipe) stalled every
 * job chain in the 2026-10-17 traced run. Same rows, values and line format
 * as before; the heap copy keeps the worker/join frames their old size. */
static tj_jobchain_perf_row* tj_jobchain_perf_snapshot_locked(unsigned* count_out)
{
    static volatile int64_t last_qpc;
    *count_out = 0;
    uint64_t now = tj_jobchain_perf_qpc();
    int64_t previous = tj_jobchain_perf_load(&last_qpc);
    if (previous && tj_jobchain_perf_us((uint64_t)previous, now) < 2000000ull) return NULL;
    if (!tj_jobchain_perf_compare_exchange(&last_qpc, previous, (int64_t)now)) return NULL;
    tj_jobchain_perf_row* rows =
        (tj_jobchain_perf_row*)malloc(MAX_JOBCHAINS * sizeof(*rows));
    if (!rows) return NULL;
    unsigned count = 0;
    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (!s_jobchains[i].jc_ea) continue;
        uint64_t runs = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_runs);
        if (!runs) continue;
        tj_jobchain_perf_row* row = &rows[count++];
        row->jc_ea = s_jobchains[i].jc_ea;
        row->max_cont = s_jobchains[i].max_cont;
        row->runs = runs;
        row->jobs = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_jobs);
        row->total = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_total_us);
        row->start = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_start_us);
        row->exec = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_exec_us);
        row->joins = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_join_calls);
        row->wait = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_join_wait_us);
        row->over = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_join_overshoot_us);
        row->total_max = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_total_max_us);
        row->start_max = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_start_max_us);
        row->exec_max = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_exec_max_us);
        row->threads = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_threads);
        row->coalesced = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_coalesced);
        row->wait_max = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_join_wait_max_us);
        row->over_max = (uint64_t)tj_jobchain_perf_load(&s_jobchains[i].perf_join_overshoot_max_us);
    }
    *count_out = count;
    return rows;
}

/* Call with no job-chain lock held; prints and frees the snapshot. */
static void tj_jobchain_perf_print(tj_jobchain_perf_row* rows, unsigned count)
{
    if (!rows) return;
    for (unsigned n = 0; n < count; n++) {
        const tj_jobchain_perf_row* row = &rows[n];
        fprintf(stderr,
                "[TJ-JOBCHAIN-PERF] jc=0x%08X maxCont=%u runs=%llu jobs=%llu complete={total_us=%llu avg_us=%llu max_us=%llu} "
                "startup={total_us=%llu avg_us=%llu max_us=%llu} walk_dispatch={total_us=%llu avg_us=%llu max_us=%llu} "
                "threads=%llu coalesced=%llu join={calls=%llu wait_total=%llu wait_avg=%llu wait_max=%llu overshoot_total=%llu overshoot_avg=%llu overshoot_max=%llu}\n",
                row->jc_ea, row->max_cont,
                (unsigned long long)row->runs, (unsigned long long)row->jobs,
                (unsigned long long)row->total, (unsigned long long)(row->total / row->runs),
                (unsigned long long)row->total_max,
                (unsigned long long)row->start, (unsigned long long)(row->start / row->runs),
                (unsigned long long)row->start_max,
                (unsigned long long)row->exec, (unsigned long long)(row->exec / row->runs),
                (unsigned long long)row->exec_max,
                (unsigned long long)row->threads,
                (unsigned long long)row->coalesced,
                (unsigned long long)row->joins, (unsigned long long)row->wait,
                (unsigned long long)(row->joins ? row->wait / row->joins : 0),
                (unsigned long long)row->wait_max,
                (unsigned long long)row->over,
                (unsigned long long)(row->joins ? row->over / row->joins : 0),
                (unsigned long long)row->over_max);
    }
    free(rows);
}

static void tj_late_jobchain_trace(const char* phase, int slot, u32 jc_ea,
                                   u32 job_ea, int rc)
{
    if (!tj_late_job_trace_enabled()) return;
    fprintf(stderr,
            "[TJ-LATE-JC] seq=%lu tid=%lu phase=%s jc=0x%08X slot=%d job=0x%08X rc=%d running=%ld requested=%ld shutdown=%ld\n",
            tj_late_job_trace_next_sequence(), tj_late_job_trace_thread_id(),
            phase, jc_ea, slot, job_ea, rc,
            tj_late_job_trace_load(&s_jobchains[slot].running),
            tj_late_job_trace_load(&s_jobchains[slot].run_requested),
            tj_late_job_trace_load(&s_jobchains[slot].shutdown_requested));
    fflush(stderr);
}

static const char* tj_late_eventflag_transition_name(uint8_t kind)
{
    switch (kind) {
    case TJ_LATE_EVENTFLAG_TRANSITION_PPU_REGISTER: return "PPU_REGISTER";
    case TJ_LATE_EVENTFLAG_TRANSITION_GETLLAR: return "GETLLAR";
    case TJ_LATE_EVENTFLAG_TRANSITION_PUTLLC_SUCCESS: return "PUTLLC_OK";
    case TJ_LATE_EVENTFLAG_TRANSITION_PUTLLC_FAIL: return "PUTLLC_FAIL";
    case TJ_LATE_EVENTFLAG_TRANSITION_PPU_PENDING_CLEAR: return "PPU_CLEAR";
    default: return "UNKNOWN";
    }
}

static void tj_late_eventflag_report(const char* state, uint32_t ea,
                                     u16 pattern, uint64_t started_ms,
                                     const TJLateEventFlagCounters* baseline)
{
    TJLateEventFlagCounters current;
    TJLateEventFlagTransition transitions[TJ_LATE_EVENTFLAG_TRANSITION_CAPACITY];
    int chain_slot = -1;
    long running = 0, requested = 0, shutdown = 0;
    uint32_t chain_ea = ea + 0x80u;
    uint64_t elapsed = tj_late_eventflag_now_ms() - started_ms;

    tj_late_eventflag_trace_snapshot(&current);
    uint32_t transition_count =
        tj_late_eventflag_trace_transition_snapshot(transitions);
    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (s_jobchains[i].jc_ea != chain_ea) continue;
        chain_slot = i;
        running = tj_late_job_trace_load(&s_jobchains[i].running);
        requested = tj_late_job_trace_load(&s_jobchains[i].run_requested);
        shutdown = tj_late_job_trace_load(&s_jobchains[i].shutdown_requested);
        break;
    }
    fprintf(stderr,
            "[TJ-LATE-EVENTFLAG] state=%s flag=0x%08X pattern=0x%04X elapsed_ms=%llu raw=0x%016llX "
            "delta={image13_begin=%llu image13_end=%llu atomics=%llu chain_requests=%llu chain_start_worker=%llu chain_thread_created=%llu chain_thread_create_failed=%llu chain_worker_register_failed=%llu} "
            "last={image13_job=0x%08X image13_pc=0x%05X atomic_pc=0x%05X} "
            "chain={ea=0x%08X slot=%d running=%ld requested=%ld shutdown=%ld} "
            "guest_world={registered=%u poisoned=%d}\n",
            state, ea, pattern, (unsigned long long)elapsed,
            (unsigned long long)vm_read64(ea),
            (unsigned long long)(current.image13_dispatch_begin - baseline->image13_dispatch_begin),
            (unsigned long long)(current.image13_dispatch_end - baseline->image13_dispatch_end),
            (unsigned long long)(current.committed_atomics - baseline->committed_atomics),
            (unsigned long long)(current.jobchain_requests - baseline->jobchain_requests),
            (unsigned long long)(current.jobchain_start_workers - baseline->jobchain_start_workers),
            (unsigned long long)(current.jobchain_thread_created - baseline->jobchain_thread_created),
            (unsigned long long)(current.jobchain_thread_create_failed - baseline->jobchain_thread_create_failed),
            (unsigned long long)(current.jobchain_worker_register_failed - baseline->jobchain_worker_register_failed),
            current.last_image13_job, current.last_image13_pc, current.last_atomic_pc,
            chain_ea, chain_slot, running, requested, shutdown,
            guest_world_registered_count(), guest_world_is_poisoned());
    if (transition_count) {
        fprintf(stderr, "[TJ-LATE-EVENTFLAG-RING] flag=0x%08X records=%u", ea,
                transition_count);
        for (uint32_t i = 0; i < transition_count; i++) {
            const TJLateEventFlagTransition* record = &transitions[i];
            fprintf(stderr,
                    " {seq=%llu kind=%s ok=%u mem=0x%016llX resv=0x%016llX "
                    "store=0x%016llX ver=%llu/%llu valid=%u img=%u pc=0x%05X}",
                    (unsigned long long)record->sequence,
                    tj_late_eventflag_transition_name(record->kind),
                    record->success,
                    (unsigned long long)record->memory,
                    (unsigned long long)record->reservation,
                    (unsigned long long)record->store,
                    (unsigned long long)record->line_version,
                    (unsigned long long)record->reservation_version,
                    record->reservation_valid, record->image_id, record->pc);
        }
        fputc('\n', stderr);
    }
    fflush(stderr);
}

/* Called only by the title's opt-in, one-shot late-stall diagnostic. */
void cellSpurs_jobchain_snapshot(FILE* out, u32 flag_ea)
{
    struct Snapshot {
        u32 ea, entry, pc, job;
        long running, workers, requested, shutdown;
        u64 command, words[8];
    } rows[MAX_JOBCHAINS] = {0};
    unsigned count = 0;
    tj_jobchain_registry_lock();
    for (int i = 0; i < MAX_JOBCHAINS; ++i) {
        if (!s_jobchains[i].jc_ea) continue;
        struct Snapshot* r = &rows[count++];
        r->ea = s_jobchains[i].jc_ea;
        r->entry = s_jobchains[i].entry_ea;
        r->running = tj_late_job_trace_load(&s_jobchains[i].running);
        r->workers = tj_late_job_trace_load(&s_jobchains[i].worker_count);
        r->requested = tj_late_job_trace_load(&s_jobchains[i].run_requested);
        r->shutdown = tj_late_job_trace_load(&s_jobchains[i].shutdown_requested);
        r->pc = (u32)tj_jobchain_perf_load(&s_jobchains[i].snapshot_pc);
        r->command = (u64)tj_jobchain_perf_load(&s_jobchains[i].snapshot_command);
        r->job = (u32)tj_jobchain_perf_load(&s_jobchains[i].snapshot_job);
        if (r->entry && r->entry < 0x0FFFFFC0u)
            for (int j = 0; j < 8; ++j) r->words[j] = vm_read64(r->entry + j * 8u);
    }
    tj_jobchain_registry_unlock();
    fprintf(out, "[SPURS-JC-SNAPSHOT] flag=0x%08X ctrl=0x%016llX chains=%u\n",
            flag_ea, (unsigned long long)(flag_ea ? vm_read64(flag_ea) : 0), count);
    for (unsigned i = 0; i < count; ++i) {
        const struct Snapshot* r = &rows[i];
        fprintf(out, "[SPURS-JC-SNAPSHOT] jc=%08X entry=%08X running=%ld workers=%ld requested=%ld shutdown=%ld pc=%08X cmd=%016llX job=%08X words=",
                r->ea, r->entry, r->running, r->workers, r->requested, r->shutdown,
                r->pc, (unsigned long long)r->command, r->job);
        for (int j = 0; j < 8; ++j) fprintf(out, " %016llX", (unsigned long long)r->words[j]);
        fputc('\n', out);
    }
}

static void jc_dump_commands(const char* tag, u32 ea, int max_words)
{
    printf("[cellSpurs] %s chain@0x%08X commands:", tag, ea);
    for (int i = 0; i < max_words; i++) {
        u64 cmd = vm_read64(ea + (u32)i * 8);
        printf("\n    [%2d] 0x%016llX", i, (unsigned long long)cmd);
        if (cmd == 0) { printf(" (halt/empty)"); break; }
    }
    printf("\n");
}

/* SDK ABI (cell/spurs/job_chain.h): the REVISIONS come first -- attr is r5.
 *   _cellSpursJobChainAttributeInitialize(jmRevision, sdkRevision, attr,
 *       jobChainEntry, sizeJobDescriptor, maxGrabbedJob, priorityTable,
 *       maxContention, [stack: autoRequestSpuCount, tag1, tag2,
 *       isFixedMemAlloc, maxSizeJobDescriptor, initialRequestSpuCount]) */
s32 cellSpursJobChainAttributeInitializeFull(
    u32 jmRevision, u32 sdkRevision, u64 attr_ea, u64 entry_ea,
    u32 sizeJobDescriptor, u32 maxGrabbedJob, u64 prio_ea,
    u32 maxContention, u32 autoRequestSpuCount, u32 tag1, u32 tag2,
    u32 isFixedMemAlloc, u32 maxSizeJobDescriptor,
    u32 initialRequestSpuCount)
{
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    vm_write32((u32)attr_ea + JCATTR_REVISION,   jmRevision);
    vm_write32((u32)attr_ea + JCATTR_SDKVERSION, sdkRevision);
    vm_write32((u32)attr_ea + JCATTR_ENTRY,      (u32)entry_ea);
    vm_write32((u32)attr_ea + JCATTR_SIZE_DESC,
               ((sizeJobDescriptor & 0xFFFFu) << 16) | (maxGrabbedJob & 0xFFFFu));
    for (int i = 0; i < 8; i++)
        *(vm_base + (u32)attr_ea + JCATTR_PRIORITY + i) =
            prio_ea ? *(vm_base + (u32)prio_ea + i) : 0;
    vm_write32((u32)attr_ea + JCATTR_MAX_CONT, maxContention);
    *(vm_base + (u32)attr_ea + JCATTR_AUTO_RDY) =
        autoRequestSpuCount ? 1u : 0u;
    vm_write32((u32)attr_ea + JCATTR_TAG1, tag1);
    vm_write32((u32)attr_ea + JCATTR_TAG2, tag2);
    *(vm_base + (u32)attr_ea + JCATTR_FIXED_MEM) =
        isFixedMemAlloc ? 1u : 0u;
    vm_write32((u32)attr_ea + JCATTR_MAX_SIZE_D, maxSizeJobDescriptor);
    vm_write32((u32)attr_ea + JCATTR_INIT_SPU, initialRequestSpuCount);
    vm_write32((u32)attr_ea + JCATTR_HALT_ERROR, 0);
    vm_write32((u32)attr_ea + JCATTR_NAME,       0);
    vm_write32((u32)attr_ea + JCATTR_JOB_CHECK,  0);
    if (spurs_trace_enabled())
        printf("[cellSpurs] _JobChainAttributeInitialize(attr=0x%08X entry=0x%08X "
               "sizeDesc=%u maxGrab=%u maxCont=%u auto=%u tags=%u/%u fixed=%u "
               "maxSize=%u initial=%u)\n",
               (u32)attr_ea, (u32)entry_ea, sizeJobDescriptor, maxGrabbedJob,
               maxContention, autoRequestSpuCount, tag1, tag2,
               isFixedMemAlloc, maxSizeJobDescriptor, initialRequestSpuCount);
    return CELL_OK;
}

s32 _cellSpursJobChainAttributeInitialize(u32 jmRevision, u32 sdkRevision,
                                          u64 attr_ea, u64 entry_ea,
                                          u32 sizeJobDescriptor,
                                          u32 maxGrabbedJob, u64 prio_ea,
                                          u32 maxContention)
{
    /* Generic HLE callers expose only r3-r10. Titles using the complete SDK
     * form install a context-aware wrapper and call the full helper above. */
    return cellSpursJobChainAttributeInitializeFull(
        jmRevision, sdkRevision, attr_ea, entry_ea, sizeJobDescriptor,
        maxGrabbedJob, prio_ea, maxContention, 0, 0, 0, 0, 0, 0);
}

s32 cellSpursJobChainAttributeSetName(u64 attr_ea, u64 name_ea)
{
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    vm_write32((u32)attr_ea + JCATTR_NAME, (u32)name_ea);
    if (spurs_trace_enabled())
        printf("[cellSpurs] JobChainAttributeSetName(attr=0x%08X, \"%s\")\n",
               (u32)attr_ea, name_ea ? (const char*)(vm_base + (u32)name_ea) : "-");
    return CELL_OK;
}

s32 cellSpursJobChainAttributeSetHaltOnError(u64 attr_ea, u32 halt_on_error)
{
    if (!attr_ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    vm_write32((u32)attr_ea + JCATTR_HALT_ERROR, halt_on_error ? 1u : 0u);
    if (spurs_trace_enabled())
        printf("[cellSpurs] JobChainAttributeSetHaltOnError(attr=0x%08X, halt=%u)\n",
               (u32)attr_ea, halt_on_error ? 1u : 0u);
    return CELL_OK;
}

s32 cellSpursCreateJobChainWithAttribute(u64 spurs_ea, u64 jc_ea, u64 attr_ea)
{
    if (!spurs_ea || !jc_ea || !attr_ea)
    {
        tj_jobchain_lifecycle_trace("CREATE", (u32)jc_ea, (u32)spurs_ea,
                                    (u32)attr_ea, 0, "null", CELL_SPURS_JOB_ERROR_NULL_POINTER);
        return (s32)CELL_SPURS_JOB_ERROR_NULL_POINTER;
    }
    if ((spurs_ea & 0x7Fu) || (jc_ea & 0x7Fu) || (attr_ea & 7u))
    {
        tj_jobchain_lifecycle_trace("CREATE", (u32)jc_ea, (u32)spurs_ea,
                                    (u32)attr_ea, 0, "align", CELL_SPURS_JOB_ERROR_ALIGN);
        return (s32)CELL_SPURS_JOB_ERROR_ALIGN;
    }
    u32 revision = vm_read32((u32)attr_ea + JCATTR_REVISION);
    if (revision != 2 && revision != 3)
    {
        tj_jobchain_lifecycle_trace("CREATE", (u32)jc_ea, (u32)spurs_ea,
                                    (u32)attr_ea, 0, "revision", CELL_SPURS_JOB_ERROR_INVAL);
        return (s32)CELL_SPURS_JOB_ERROR_INVAL;
    }
    if (!spurs_inst_find((u32)spurs_ea))
    {
        tj_jobchain_lifecycle_trace("CREATE", (u32)jc_ea, (u32)spurs_ea,
                                    (u32)attr_ea, 0, "spurs-missing", CELL_SPURS_JOB_ERROR_STAT);
        return (s32)CELL_SPURS_JOB_ERROR_STAT;
    }
    int perf = tj_jobchain_perf_enabled();
    u32 entry   = vm_read32((u32)attr_ea + JCATTR_ENTRY);
    u32 sd_mg   = vm_read32((u32)attr_ea + JCATTR_SIZE_DESC);
    u32 max_cont = vm_read32((u32)attr_ea + JCATTR_MAX_CONT);
    u32 name_ea = vm_read32((u32)attr_ea + JCATTR_NAME);
    u8 auto_request = *(vm_base + (u32)attr_ea + JCATTR_AUTO_RDY) ? 1u : 0u;
    u32 init_request = vm_read32((u32)attr_ea + JCATTR_INIT_SPU);
    if (init_request > 255u) init_request = 255u;

    if (spurs_trace_enabled()) {   /* Layout ground truth (same rationale as AddWorkloadWA). */
        static int _n = 0;
        if (_n < 3) {
            printf("[cellSpurs] CreateJobChainWA jc=0x%08X attr=0x%08X raw:", (u32)jc_ea, (u32)attr_ea);
            for (int o = 0; o < 0x40; o += 4) {
                if ((o & 15) == 0) printf("\n    +%02X:", o);
                printf(" %08X", vm_read32((u32)attr_ea + o));
            }
            printf("\n");
            if (entry && entry < 0x10000000u) jc_dump_commands("CreateJobChainWA", entry, 16);
        } else if (_n == 3) {
            printf("[cellSpurs] CreateJobChainWA raw dumps suppressed from here\n");
        }
        _n++;
    }
    if (spurs_trace_enabled())
        printf("[cellSpurs] CreateJobChainWithAttribute(jc=0x%08X entry=0x%08X sizeDesc=%u maxGrab=%u maxCont=%u name=\"%s\")\n",
               (u32)jc_ea, entry, sd_mg >> 16, sd_mg & 0xFFFFu, max_cont,
               name_ea ? (const char*)(vm_base + name_ea) : "-");

retry_duplicate:
    tj_jobchain_registry_lock();
    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (s_jobchains[i].jc_ea != (u32)jc_ea) continue;
        if (tj_late_job_trace_load(&s_jobchains[i].running) ||
            tj_late_job_trace_load(&s_jobchains[i].worker_count)) {
            int retiring = tj_late_job_trace_load(
                &s_jobchains[i].shutdown_requested) != 0;
            tj_jobchain_registry_unlock();
            if (retiring) {
#ifdef _WIN32
                Sleep(1);
#else
                struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
#endif
                goto retry_duplicate;
            }
            tj_jobchain_trace("CREATE", (u32)jc_ea, entry, 0, (u32)attr_ea,
                              0, "duplicate-busy");
            tj_jobchain_lifecycle_trace("CREATE", (u32)jc_ea, (u32)spurs_ea,
                                        (u32)attr_ea, entry, "duplicate-busy", CELL_SPURS_JOB_ERROR_STAT);
            return (s32)CELL_SPURS_JOB_ERROR_STAT;
        }
        memset(&s_jobchains[i], 0, sizeof(s_jobchains[i]));
        s_jobchains[i].jc_ea     = (u32)jc_ea;
        s_jobchains[i].spurs_ea  = (u32)spurs_ea;
        s_jobchains[i].entry_ea  = entry;
        s_jobchains[i].size_desc = (u16)(sd_mg >> 16);
        s_jobchains[i].max_grab  = (u16)(sd_mg & 0xFFFFu);
        s_jobchains[i].max_cont  = max_cont;
        s_jobchains[i].auto_request = auto_request;
        s_jobchains[i].init_request = (u8)init_request;
        s_jobchains[i].request_spus = (u8)init_request;
        s_jobchains[i].generation = ++s_jobchain_next_generation;
        vm_write32((u32)jc_ea + 0x74, (u32)i);
        tj_jobchain_registry_unlock();
        tj_jobchain_trace("CREATE", (u32)jc_ea, entry, 0, (u32)attr_ea, 0,
                          "reinitialized");
        tj_jobchain_lifecycle_trace("CREATE", (u32)jc_ea, (u32)spurs_ea,
                                    (u32)attr_ea, entry, "reinitialized", CELL_OK);
        return CELL_OK;
    }
    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (!s_jobchains[i].jc_ea) {
            if (perf) tj_jobchain_perf_lock();
            s_jobchains[i].jc_ea     = (u32)jc_ea;
            s_jobchains[i].spurs_ea  = (u32)spurs_ea;
            s_jobchains[i].entry_ea  = entry;
            s_jobchains[i].size_desc = (u16)(sd_mg >> 16);
            s_jobchains[i].max_grab  = (u16)(sd_mg & 0xFFFFu);
            s_jobchains[i].max_cont  = max_cont;
            s_jobchains[i].auto_request = auto_request;
            s_jobchains[i].init_request = (u8)init_request;
            s_jobchains[i].request_spus = (u8)init_request;
            s_jobchains[i].generation = ++s_jobchain_next_generation;
            s_jobchains[i].run_count = 0;
            s_jobchains[i].worker_count = 0;
            s_jobchains[i].run_requested = 0;
            s_jobchains[i].shutdown_requested = 0;
            s_jobchains[i].perf_request_qpc = 0;
            s_jobchains[i].perf_pending_request_qpc = 0;
            s_jobchains[i].perf_complete_qpc = 0;
            s_jobchains[i].perf_jobs_current = 0;
            s_jobchains[i].perf_runs = 0;
            s_jobchains[i].perf_jobs = 0;
            s_jobchains[i].perf_total_us = 0;
            s_jobchains[i].perf_total_max_us = 0;
            s_jobchains[i].perf_start_us = 0;
            s_jobchains[i].perf_start_max_us = 0;
            s_jobchains[i].perf_exec_us = 0;
            s_jobchains[i].perf_exec_max_us = 0;
            s_jobchains[i].perf_threads = 0;
            s_jobchains[i].perf_coalesced = 0;
            s_jobchains[i].perf_join_calls = 0;
            s_jobchains[i].perf_join_wait_us = 0;
            s_jobchains[i].perf_join_wait_max_us = 0;
            s_jobchains[i].perf_join_overshoot_us = 0;
            s_jobchains[i].perf_join_overshoot_max_us = 0;
            vm_write32((u32)jc_ea + 0x74, (u32)i);
            if (perf) tj_jobchain_perf_unlock();
            tj_jobchain_registry_unlock();
            tj_jobchain_trace("CREATE", (u32)jc_ea, entry, 0, (u32)attr_ea,
                              0, "registered");
            tj_jobchain_lifecycle_trace("CREATE", (u32)jc_ea, (u32)spurs_ea,
                                        (u32)attr_ea, entry, "registered", CELL_OK);
            return CELL_OK;
        }
    }
    tj_jobchain_registry_unlock();
    printf("[cellSpurs] CreateJobChainWithAttribute: registry full\n");
    tj_jobchain_lifecycle_trace("CREATE", (u32)jc_ea, (u32)spurs_ea,
                                (u32)attr_ea, entry, "registry-full", CELL_SPURS_JOB_ERROR_NOMEM);
    return (s32)CELL_SPURS_JOB_ERROR_NOMEM;
}

/* ---------------------------------------------------------------------------
 * Job-chain execution
 *
 * A job chain is a u64 command stream (SDK cell/spurs/job_commands.h): the low
 * 3 bits select the opcode, and a nonzero word whose low 3 bits are 0 IS a job
 * pointer. The real jobchain policy module walks this on an SPU, fetching each
 * CellSpursJobHeader and running its binary.
 *
 * LBP's draw pipeline is built on this -- the jobs emit the GCM commands. With
 * the chain unimplemented the FIFO starves, the RSX `ref` fence stops advancing
 * and the game spins on it forever (the boot hang: ref frozen at 0x2B3 while
 * the chain sat un-run).
 *
 * We walk the stream on a host thread (one per chain; the real thing is async
 * on SPUs, so RunJobChain must not block the PPU) and push each job binary
 * through the same fingerprint -> lifted-SPU dispatch that already runs this
 * title's workload images.
 * -----------------------------------------------------------------------*/
enum {                              /* CellSpursJobHeader (48 B) */
    JH_EA_BINARY     = 0x00,        /* be u64: job binary EA (low 3 bits = flags) */
    JH_SIZE_BINARY   = 0x08,        /* be u16: binary size >> 4                   */
    JH_JOB_TYPE      = 0x2C,        /* u8                                         */
    JH_SIZE          = 0x30,
};

/* CellSpursJobGuard is a 128-byte, 128-byte-aligned guest object. Keep its
 * counters and pointers in guest big-endian memory because lifted SPU/PPU code
 * can inspect the same object directly. */
enum {
    JG_COUNT       = 0x00,
    JG_RESET_COUNT = 0x04,
    JG_JOB_CHAIN   = 0x0C,
    JG_SPU_COUNT   = 0x10,
    JG_AUTO_RESET  = 0x20,
    JG_ZERO        = 0x30,
    JG_SIZE        = 0x80,
};

static void jc_run_one_job(u32 job_ea, int idx, u32 size_desc, u32 spurs_ea,
                           int slot, u32 jc_ea)
{
    u64 ea_bin_raw = vm_read64(job_ea + JH_EA_BINARY);
    u32 ea_bin     = (u32)(ea_bin_raw & ~7ull);          /* low 3 bits = flags */
    u32 size_bin   = ((vm_read32(job_ea + JH_SIZE_BINARY) >> 16) & 0xFFFFu) << 4;
    u8  job_type   = *(vm_base + job_ea + JH_JOB_TYPE);

    { static int _n = 0;
      if (_n < 8 && spurs_trace_enabled()) {
          printf("[cellSpurs]   job[%d] @0x%08X eaBinary=0x%08X size=%u type=0x%02X hdr:",
                 idx, job_ea, ea_bin, size_bin, job_type);
          for (int o = 0; o < (int)(size_desc < 0x100u ? size_desc : 0x100u); o += 4) {
              if ((o & 15) == 0) printf("\n      +%02X:", o);
              printf(" %08X", vm_read32(job_ea + o));
          }
          printf("\n");
      }
      if (_n < 8) _n++; }

    if (!ea_bin || !size_bin || ea_bin >= 0x10000000u) {
        static int _b = 0;
        if (_b++ < 4)
            printf("[cellSpurs]   job[%d]: implausible binary (ea=0x%08X size=%u) -- skipped\n",
                   idx, ea_bin, size_bin);
        tj_late_jobchain_trace("JOB_SKIP", slot, jc_ea, job_ea, 0);
        return;
    }
    tj_jobchain_trace_job(jc_ea, s_jobchains[slot].entry_ea, job_ea,
                          ea_bin, size_bin);
    /* A jobchain job is NOT a SPURS task: Sony's jm2 stages the whole working
     * set in local store and enters the job's CRT with r3 = CellSpursJobContext2*
     * and r4 = the job descriptor, both LS pointers. Dispatching it on the task
     * ABI (arg EA in r3) left it reading a zeroed context and parking with no
     * DMA traffic at all. spu_workload_dispatch_job reproduces jm2's staging.
     * Synchronous by design: a job runs to completion, and the chain's own
     * NEXT/CALL/RET commands are what order them. With TJ_JOBCHAIN_LANES the
     * caller may be a lane helper, inside one barrier window (see below). */
    tj_late_jobchain_trace("JOB_BEGIN", slot, jc_ea, job_ea, 0);
    int rc = spu_workload_dispatch_job(vm_base + ea_bin, size_bin, job_ea,
                                       size_desc, spurs_ea);
    tj_late_jobchain_trace("JOB_END", slot, jc_ea, job_ea, rc);
}

typedef struct jc_lanes jc_lanes;

typedef struct jc_dispatch_context {
    int slot;
    u32 jc_ea;
    u32 spurs_ea;
    int snapshot;
    jc_lanes* lanes;    /* NULL: the plain serial walk */
} jc_dispatch_context;

static int jc_read_joblist_header(void* opaque, uint32_t ea,
                                  uint8_t out[JOBCHAIN_JOBLIST_HEADER_SIZE])
{
    memcpy(out, (const uint8_t*)opaque + ea, JOBCHAIN_JOBLIST_HEADER_SIZE);
    return 1;
}

static int jc_stop_requested(void* opaque)
{
    int slot = *(const int*)opaque;
    return s_jobchains[slot].shutdown_requested != 0;
}

/* ---------------------------------------------------------------------------
 * Opt-in job-chain lanes: TJ_JOBCHAIN_LANES=2..4. Unset or 1 keeps the plain
 * serial walk (jc_lanes_begin returns NULL and none of this runs).
 *
 * SPURS may run the jobs between two barriers on up to
 * min(maxContention, nSpus) SPUs at once. With lanes the walker collects such
 * a window -- consecutive JOB commands and JOBLIST entries, in order -- and,
 * at the next non-job command or when the batch fills, runs it on itself plus
 * up to width-1 persistent helper threads owned by this chain slot. Every
 * non-job command (SYNC, LWSYNC as a full barrier, labels, GUARD, NEXT, CALL,
 * RET, END, FLUSH, NOP, ...) first waits for the whole window, so it sees each
 * earlier job finished exactly as in the serial walk: a GUARD never takes the
 * reservation lock, and the walker never returns, with a job in flight. Each
 * running job still leases its own jm2 worker number (spurs_job_lane_acquire),
 * which also caps live jobs per CellSpurs at nSpus across all chains.
 *
 * TJ_JOBCHAIN_LANES_TRACE=1 prints per slot, about once a second: jobs run,
 * jobs that started while another job of the chain ran, barriers that drained
 * a window, and average width (job time / window wall time). Without
 * TJ_JOBCHAIN_LANES it only counts the serial walk.
 * TJ_JOBCHAIN_LANES_SPIN_US=0..200 (default 0): an idle helper polls that
 * long for the next window before it blocks.
 * -----------------------------------------------------------------------*/
#define JC_LANES_MAX           4
#define JC_LANES_BATCH         128  /* a longer window drains once per batch */
#define JC_LANES_THREAD_CAP    8    /* helpers per process (rendezvous slots) */
#define JC_LANES_DRAIN_SPIN_US 20   /* walker polls this long at a barrier */
#define JC_LANES_TRACE         0x100

#ifdef _WIN32
struct jc_lanes {
    /* generation 63..32 | admitted helpers 31..24 | count 23..12 | next 11..0.
     * One compare-exchange claims a job of exactly this batch, so a helper
     * that wakes late can never run a job of a batch it was not admitted to. */
    volatile LONG64 claim;
    volatile LONG done;               /* finished jobs of the current batch */
    jc_dispatch_context context;      /* fixed while a batch runs */
    u32 job_ea[JC_LANES_BATCH];
    u32 job_size[JC_LANES_BATCH];
    u32 job_index[JC_LANES_BATCH];
    int count;                        /* collected, not yet run (walker) */
    int width;                        /* lanes for this walk (walker) */
    int window;                       /* jobs since the last barrier (walker) */
    int helpers;                      /* helper threads started for the slot */
    volatile LONG helper_failed;
    SRWLOCK lock;
    CONDITION_VARIABLE work, idle;
    volatile LONG inflight, t_max_inflight;            /* trace only */
    volatile LONG64 t_jobs, t_concurrent, t_helper_jobs;
    volatile LONG64 t_barriers, t_busy, t_wall;
    uint64_t t_last;                  /* walker */
};

static jc_lanes s_jc_lanes[MAX_JOBCHAINS];
static volatile LONG s_jc_lanes_config = -1;  /* lanes | TRACE | spin_us << 16 */
static volatile LONG s_jc_lanes_threads;
static uint64_t s_jc_lanes_frequency;

static LONG jc_lanes_config(void)
{
    LONG config = s_jc_lanes_config;
    if (config < 0) {
        const char* lanes_text = getenv("TJ_JOBCHAIN_LANES");
        const char* trace_text = getenv("TJ_JOBCHAIN_LANES_TRACE");
        const char* spin_text = getenv("TJ_JOBCHAIN_LANES_SPIN_US");
        long lanes = lanes_text && *lanes_text ? strtol(lanes_text, NULL, 0) : 1;
        long spin_us = spin_text && *spin_text ? strtol(spin_text, NULL, 0) : 0;
        LARGE_INTEGER frequency;
        if (lanes < 2) lanes = 1;
        if (lanes > JC_LANES_MAX) lanes = JC_LANES_MAX;
        if (spin_us < 0) spin_us = 0;
        if (spin_us > 200) spin_us = 200;
        QueryPerformanceFrequency(&frequency);
        s_jc_lanes_frequency = (uint64_t)frequency.QuadPart;
        config = (LONG)lanes | (LONG)(spin_us << 16) |
                 (trace_text && *trace_text && strcmp(trace_text, "0") != 0
                      ? JC_LANES_TRACE : 0);
        if (InterlockedCompareExchange(&s_jc_lanes_config, config, -1) == -1 &&
            (lanes > 1 || (config & JC_LANES_TRACE)))
            fprintf(stderr, "[TJ-JOBCHAIN-LANES] lanes=%ld trace=%d spin_us=%ld\n",
                    lanes, (config & JC_LANES_TRACE) != 0, spin_us);
    }
    return config;
}
#endif

/* Run one job on the calling thread: the walker, or a lane helper inside one
 * barrier window. */
static void jc_run_job(const jc_dispatch_context* context, uint32_t job_ea,
                       uint32_t size_desc, uint32_t job_index, int helper)
{
#ifdef _WIN32
    jc_lanes* lanes = context->lanes;
    const int trace = lanes && (jc_lanes_config() & JC_LANES_TRACE);
    /* Width >= 2: sibling jobs may overlap, so this job's plain PUTs are
     * ordered against PUTLLC commits (spu_reservation.h). Serial: untouched. */
    const int ordered = lanes && lanes->width >= 2;
    int ordered_old = 0;
    uint64_t begin = 0;
    if (ordered) ordered_old = spu_reservation_set_put_ordered(1);
    if (trace) {
        LONG inflight = InterlockedIncrement(&lanes->inflight);
        LONG seen = lanes->t_max_inflight;
        if (inflight > 1) InterlockedIncrement64(&lanes->t_concurrent);
        while (inflight > seen &&
               InterlockedCompareExchange(&lanes->t_max_inflight, inflight,
                                          seen) != seen)
            seen = lanes->t_max_inflight;
        begin = tj_jobchain_perf_qpc();
    }
#else
    (void)helper;
#endif
    if (context->snapshot)
        tj_jobchain_perf_store(&s_jobchains[context->slot].snapshot_job, job_ea);
    jc_run_one_job(job_ea, (int)job_index, size_desc, context->spurs_ea,
                   context->slot, context->jc_ea);
    if (context->snapshot)
        tj_jobchain_perf_store(&s_jobchains[context->slot].snapshot_job, 0);
    if (tj_jobchain_perf_enabled())
        tj_jobchain_perf_add(&s_jobchains[context->slot].perf_jobs_current, 1);
#ifdef _WIN32
    if (ordered) spu_reservation_set_put_ordered(ordered_old);
    if (trace) {
        LONG64 ticks = (LONG64)(tj_jobchain_perf_qpc() - begin);
        InterlockedAdd64(&lanes->t_busy, ticks);
        if (lanes->width < 2) InterlockedAdd64(&lanes->t_wall, ticks);
        InterlockedIncrement64(&lanes->t_jobs);
        if (helper) InterlockedIncrement64(&lanes->t_helper_jobs);
        InterlockedDecrement(&lanes->inflight);
    }
#endif
}

#ifdef _WIN32
/* min(TJ_JOBCHAIN_LANES, maxContention, nSpus, request count when
 * autoRequestSpuCount is off); 1 = serial walk. */
/* TJ_JOBCHAIN_LANES_ONLY=ea[,ea...]: lanes only for these job chains (e.g.
 * chains of long jobs); every other chain stays serial. Unset = all chains. */
static int jc_lanes_chain_allowed(u32 jc_ea)
{
    static volatile LONG parsed;
    static u32 list[8];
    static int count = -1;
    if (InterlockedCompareExchange(&parsed, 1, 0) == 0) {
        const char* text = getenv("TJ_JOBCHAIN_LANES_ONLY");
        int n = 0;
        while (text && *text && n < 8) {
            char* end = NULL;
            unsigned long v = strtoul(text, &end, 0);
            if (end == text) break;
            list[n++] = (u32)v;
            text = *end == ',' ? end + 1 : end;
        }
        count = text ? n : -1;
        if (count >= 0)
            fprintf(stderr, "[TJ-JOBCHAIN-LANES] only %d listed chain(s)\n", count);
        InterlockedExchange(&parsed, 2);
    }
    while (parsed != 2) YieldProcessor();
    if (count < 0) return 1;
    for (int i = 0; i < count; ++i)
        if (list[i] == jc_ea) return 1;
    return 0;
}

static int jc_lanes_width(int slot, LONG config)
{
    u32 width = (u32)(config & 0xFF);
    u32 max_cont = s_jobchains[slot].max_cont;
    u32 spurs_ea = s_jobchains[slot].spurs_ea;
    u32 nspus;
    if (width < 2 || max_cont < 2 || !spurs_ea || !vm_base) return 1;
    if (!jc_lanes_chain_allowed(s_jobchains[slot].jc_ea)) return 1;
    /* The kernel adds an SPU to a workload only while its contention is below
     * the ready count. Without auto-request that count is the chain's request
     * (0 treated as 1): jobs the game expects on one SPU stay serial. */
    if (!s_jobchains[slot].auto_request) {
        u32 request = s_jobchains[slot].request_spus;
        if (request < 2) return 1;
        if (width > request) width = request;
    }
    /* nSpus, read where the jm2 lane lease (spurs_job.c) reads it. */
    nspus = vm_base[spurs_ea + 0x76u];
    if (nspus < 2 || nspus > 8) return 1;
    if (width > max_cont) width = max_cont;
    if (width > nspus) width = nspus;
    return (int)width;
}

/* Claim and run jobs of batch `generation` until none is left. */
static void jc_lanes_work(jc_lanes* lanes, u32 generation, int helper)
{
    for (;;) {
        LONG64 claim = lanes->claim;
        u32 next = (u32)claim & 0xFFFu;
        u32 count = ((u32)claim >> 12) & 0xFFFu;
        if ((u32)((uint64_t)claim >> 32) != generation || next >= count) return;
        if (InterlockedCompareExchange64(&lanes->claim, claim + 1, claim) != claim)
            continue;
        jc_run_job(&lanes->context, lanes->job_ea[next], lanes->job_size[next],
                   lanes->job_index[next], helper);
        if (InterlockedIncrement(&lanes->done) == (LONG)count && helper) {
            /* Last job of the batch: the walker may be blocked at the barrier. */
            AcquireSRWLockExclusive(&lanes->lock);
            ReleaseSRWLockExclusive(&lanes->lock);
            WakeConditionVariable(&lanes->idle);
        }
    }
}

/* Persistent helper of one chain slot. Idle, it blocks on `work` (after the
 * optional TJ_JOBCHAIN_LANES_SPIN_US poll). Registered like the walker, so a
 * guest-world rendezvous suspends it too. */
static DWORD WINAPI jc_lane_thread(LPVOID p)
{
    const int slot = (int)((intptr_t)p >> 4);
    const int index = (int)((intptr_t)p & 15);
    jc_lanes* lanes = &s_jc_lanes[slot];
    const uint64_t spin_ticks =
        s_jc_lanes_frequency * (uint64_t)((jc_lanes_config() >> 16) & 0xFF) / 1000000u;
    u32 seen = 0;
    if (!guest_world_register_current("spurs-jobchain-lane")) {
        InterlockedExchange(&lanes->helper_failed, 1);
        fprintf(stderr, "[TJ-JOBCHAIN-LANES] slot %d helper not registered; slot runs serially\n",
                slot);
        return 0;
    }
    for (;;) {
        LONG64 claim = lanes->claim;
        if ((u32)((uint64_t)claim >> 32) == seen && spin_ticks) {
            uint64_t limit = tj_jobchain_perf_qpc() + spin_ticks;
            while ((u32)((uint64_t)(claim = lanes->claim) >> 32) == seen &&
                   tj_jobchain_perf_qpc() < limit)
                YieldProcessor();
        }
        if ((u32)((uint64_t)claim >> 32) == seen) {
            AcquireSRWLockExclusive(&lanes->lock);
            while ((u32)((uint64_t)(claim = lanes->claim) >> 32) == seen)
                SleepConditionVariableSRW(&lanes->work, &lanes->lock, INFINITE, 0);
            ReleaseSRWLockExclusive(&lanes->lock);
        }
        seen = (u32)((uint64_t)claim >> 32);
        if (index < (int)((claim >> 24) & 0xFF))
            jc_lanes_work(lanes, seen, 1);
    }
}

/* Walker: start helper threads up to `wanted`; returns how many may join. */
static int jc_lanes_helpers(jc_lanes* lanes, int wanted)
{
    while (lanes->helpers < wanted && !lanes->helper_failed) {
        HANDLE thread;
        if (InterlockedIncrement(&s_jc_lanes_threads) > JC_LANES_THREAD_CAP) {
            InterlockedDecrement(&s_jc_lanes_threads);
            break;
        }
        thread = CreateThread(NULL, 1u << 20, jc_lane_thread,
                              (LPVOID)(intptr_t)((lanes->context.slot << 4) |
                                                 lanes->helpers), 0, NULL);
        if (!thread) {
            InterlockedDecrement(&s_jc_lanes_threads);
            break;
        }
        CloseHandle(thread);
        lanes->helpers++;
    }
    if (lanes->helper_failed) return 0;
    return lanes->helpers < wanted ? lanes->helpers : wanted;
}

static void jc_lanes_report(jc_lanes* lanes)
{
    uint64_t now = tj_jobchain_perf_qpc();
    if (!lanes->t_last) lanes->t_last = now;
    if (now - lanes->t_last < s_jc_lanes_frequency) return;
    lanes->t_last = now;
    LONG64 jobs = InterlockedExchange64(&lanes->t_jobs, 0);
    LONG64 concurrent = InterlockedExchange64(&lanes->t_concurrent, 0);
    LONG64 helper_jobs = InterlockedExchange64(&lanes->t_helper_jobs, 0);
    LONG64 barriers = InterlockedExchange64(&lanes->t_barriers, 0);
    LONG64 busy = InterlockedExchange64(&lanes->t_busy, 0);
    LONG64 wall = InterlockedExchange64(&lanes->t_wall, 0);
    LONG max_inflight = InterlockedExchange(&lanes->t_max_inflight, 0);
    fprintf(stderr,
            "[TJ-JOBCHAIN-LANES] slot=%d jc=0x%08X lanes=%d auto_req=%u req=%u "
            "init_req=%u max_cont=%u jobs=%lld concurrent=%lld "
            "helper_jobs=%lld barriers=%lld jobs_per_barrier=%.1f avg_width=%.2f "
            "max_inflight=%ld\n",
            lanes->context.slot, lanes->context.jc_ea, lanes->width,
            (unsigned)s_jobchains[lanes->context.slot].auto_request,
            (unsigned)s_jobchains[lanes->context.slot].request_spus,
            (unsigned)s_jobchains[lanes->context.slot].init_request,
            (unsigned)s_jobchains[lanes->context.slot].max_cont,
            (long long)jobs, (long long)concurrent, (long long)helper_jobs,
            (long long)barriers, barriers ? (double)jobs / (double)barriers : 0.0,
            wall ? (double)busy / (double)wall : 0.0, (long)max_inflight);
}

/* Walker: run the collected jobs and wait until every one has finished.
 * `barrier` is 0 only when a full batch drains in the middle of a window.
 * Returns the number of jobs run. */
static int jc_lanes_flush(jc_lanes* lanes, int barrier)
{
    if (!lanes) return 0;
    const int trace = (jc_lanes_config() & JC_LANES_TRACE) != 0;
    const int count = lanes->count;
    if (count) {
        uint64_t begin = trace ? tj_jobchain_perf_qpc() : 0;
        /* A Kick or guard restart may lower the request count mid-walk. */
        int width = jc_lanes_width(lanes->context.slot, jc_lanes_config());
        int helpers = (width < lanes->width ? width : lanes->width) - 1;
        lanes->count = 0;
        if (helpers > count - 1) helpers = count - 1;
        if (helpers > 0) helpers = jc_lanes_helpers(lanes, helpers);
        if (helpers <= 0) {
            for (int i = 0; i < count; i++)
                jc_run_job(&lanes->context, lanes->job_ea[i], lanes->job_size[i],
                           lanes->job_index[i], 0);
        } else {
            u32 generation = (u32)((uint64_t)lanes->claim >> 32) + 1;
            if (!generation) generation = 1;   /* 0 is a new helper's "seen" */
            InterlockedExchange(&lanes->done, 0);
            AcquireSRWLockExclusive(&lanes->lock);
            InterlockedExchange64(&lanes->claim,
                                  (LONG64)(((uint64_t)generation << 32) |
                                           ((uint64_t)helpers << 24) |
                                           ((uint64_t)count << 12)));
            ReleaseSRWLockExclusive(&lanes->lock);
            WakeAllConditionVariable(&lanes->work);
            jc_lanes_work(lanes, generation, 0);
            /* Barrier: wait for the jobs helpers still run. */
            if (lanes->done != count) {
                uint64_t limit = tj_jobchain_perf_qpc() +
                    s_jc_lanes_frequency * JC_LANES_DRAIN_SPIN_US / 1000000u;
                while (lanes->done != count && tj_jobchain_perf_qpc() < limit)
                    YieldProcessor();
                AcquireSRWLockExclusive(&lanes->lock);
                while (lanes->done != count)
                    SleepConditionVariableSRW(&lanes->idle, &lanes->lock, INFINITE, 0);
                ReleaseSRWLockExclusive(&lanes->lock);
            }
        }
        if (trace)
            InterlockedAdd64(&lanes->t_wall, (LONG64)(tj_jobchain_perf_qpc() - begin));
    }
    if (barrier && lanes->window) {
        lanes->window = 0;
        if (trace) {
            InterlockedIncrement64(&lanes->t_barriers);
            jc_lanes_report(lanes);
        }
    }
    return count;
}

/* Walker: add a job to the open window. Returns 0 for a serial (width 1)
 * walk, whose caller then runs the job right away. */
static int jc_lanes_enqueue(jc_lanes* lanes, u32 job_ea, u32 size_desc,
                            u32 job_index)
{
    lanes->window++;
    if (lanes->width < 2) return 0;
    if (lanes->count == JC_LANES_BATCH) jc_lanes_flush(lanes, 0);
    lanes->job_ea[lanes->count] = job_ea;
    lanes->job_size[lanes->count] = size_desc;
    lanes->job_index[lanes->count] = job_index;
    lanes->count++;
    return 1;
}

/* NULL (the plain serial walk) unless lanes or their trace are enabled. */
static jc_lanes* jc_lanes_begin(int slot)
{
    LONG config = jc_lanes_config();
    int width = jc_lanes_width(slot, config);
    jc_lanes* lanes;
    if (width < 2 && !(config & JC_LANES_TRACE)) return NULL;
    lanes = &s_jc_lanes[slot];
    lanes->width = width;
    lanes->count = 0;
    lanes->window = 0;
    return lanes;
}
#else
static jc_lanes* jc_lanes_begin(int slot) { (void)slot; return NULL; }
static int jc_lanes_flush(jc_lanes* lanes, int barrier) { (void)lanes; (void)barrier; return 0; }
#endif

static int jc_dispatch_job(void* opaque, uint32_t job_ea, uint32_t size_desc,
                           uint32_t job_index)
{
    const jc_dispatch_context* context = (const jc_dispatch_context*)opaque;
#ifdef _WIN32
    if (context->lanes &&
        jc_lanes_enqueue(context->lanes, job_ea, size_desc, job_index))
        return 0;
#endif
    jc_run_job(context, job_ea, size_desc, job_index, 0);
    return 0;
}

/* Walk one chain's command stream. Bounded: a malformed or self-looping stream
 * must not spin a host thread forever. */
static void jc_walk(int slot, jc_lanes* lanes)
{
    u32 entry_ea = s_jobchains[slot].entry_ea;
    u32 jc_ea = s_jobchains[slot].jc_ea;
    u32 size_desc = s_jobchains[slot].size_desc;
    u32 spurs_ea = s_jobchains[slot].spurs_ea;
    u32 pc = s_jobchains[slot].resume_pc ? s_jobchains[slot].resume_pc : entry_ea;
    u32 ret_pc = s_jobchains[slot].resume_return_pc;
    s_jobchains[slot].resume_pc = 0;
    s_jobchains[slot].resume_return_pc = 0;
    u32 jobs = 0;
    int malformed_joblists = 0;
    static int s_snapshot = -1;
    if (s_snapshot < 0) s_snapshot = getenv("TJ_SPU_COMPLETION_WAIT_TRACE") != NULL;
    const int snapshot = s_snapshot;
    jc_dispatch_context dispatch_context = { slot, jc_ea, spurs_ea, snapshot, lanes };
#ifdef _WIN32
    if (lanes) lanes->context = dispatch_context;
#endif
    uint64_t guest_limit = UINT64_C(1) << 32;
    extern uint32_t ppu_vm_size;
    if (ppu_vm_size) guest_limit = ppu_vm_size;
    for (int step = 0; step < 4096; step++) {
        if (s_jobchains[slot].shutdown_requested) {
            tj_jobchain_trace("TERMINAL", jc_ea, entry_ea, pc, 0, 0,
                              "shutdown");
            if (spurs_trace_enabled())
                printf("[cellSpurs] chain 0x%08X: shutdown after %u job(s)\n",
                       jc_ea, jobs);
            return;
        }
        u64 cmd = vm_read64(pc);
        if (snapshot) {
            tj_jobchain_perf_store(&s_jobchains[slot].snapshot_pc, pc);
            tj_jobchain_perf_store(&s_jobchains[slot].snapshot_command, (int64_t)cmd);
        }
        u32 op  = (u32)(cmd & 7);
        u32 ext = (u32)(cmd & 127);

        if ((cmd != 0 && op == 0) || op == 6) {
            /* vm_init reserves the full 32-bit guest address space and
             * commits pages on demand. guest_limit is deliberately a numeric
             * guard only: it prevents wrap/out-of-VM arithmetic but cannot
             * prove a particular reserved page is committed. The title-owned
             * command streams reside in guest allocations; vm_base must still
             * be live before their headers are copied. */
            if (op == 6)
                tj_joblist_trace(jc_ea, entry_ea, pc,
                                 (u32)(cmd & ~7ull), guest_limit);
            tj_jobchain_trace("COMMAND", jc_ea, entry_ea, pc,
                              (u32)(cmd & ~7ull), 0,
                              op == 6 ? "joblist" : "job");
            int result = vm_base ? jobchain_dispatch_command(
                cmd, size_desc, guest_limit, &jobs, JOBCHAIN_JOBCHAIN_JOB_BUDGET,
                jc_read_joblist_header, vm_base, jc_stop_requested, &slot,
                jc_dispatch_job, &dispatch_context) : JOBCHAIN_JOB_INVALID;
            if (result == JOBCHAIN_JOB_DISPATCHED || result == JOBCHAIN_JOB_NOOP) {
                pc += 8; continue;
            }
            if (result == JOBCHAIN_JOB_STOPPED) {
                tj_jobchain_trace("TERMINAL", jc_ea, entry_ea, pc, 0, 0,
                                  "stopped");
                if (spurs_trace_enabled())
                    printf("[cellSpurs] chain 0x%08X: shutdown after %u job(s)\n",
                           jc_ea, jobs);
                return;
            }
            if (result == JOBCHAIN_JOB_BUDGET_EXHAUSTED) {
                tj_jobchain_trace("TERMINAL", jc_ea, entry_ea, pc, 0, 0,
                                  "job-budget");
                printf("[cellSpurs] chain 0x%08X: job budget (%u) exhausted\n",
                       jc_ea, JOBCHAIN_JOBCHAIN_JOB_BUDGET);
                return;
            }
            /* Not a job: a barrier, as below. Nothing was queued for it, so
             * its header is simply read again once earlier jobs are done. */
            if (jc_lanes_flush(lanes, 1) && result == JOBCHAIN_JOB_INVALID) {
                step--; continue;
            }
            if (op == 6 && malformed_joblists++ < 4)
                printf("[cellSpurs] chain 0x%08X: invalid JOBLIST at 0x%08X -- skipped\n",
                       jc_ea, (u32)(cmd & ~7ull));
            pc += 8; continue;
        }
        /* Lanes: every non-job command is a barrier. Earlier jobs finish before
         * it is acted on -- a GUARD must not take the reservation lock, nor the
         * walker return, while a helper still runs one -- and the command is
         * then read again, as the serial walk reads it only after they ran.
         * The step is not counted twice. */
        if (jc_lanes_flush(lanes, 1)) { step--; continue; }
        if (op == 1) { pc = (u32)(cmd & ~7ull); continue; }   /* RESET_PC */
        if (op == 3) { pc = (u32)(cmd & ~7ull); continue; }   /* NEXT     */
        if (op == 4) { ret_pc = pc + 8; pc = (u32)(cmd & ~7ull); continue; }  /* CALL */
        if (op == 7) {
            if (ext == (7 | (1 << 3))) {                      /* GUARD */
                u32 guard = (u32)(cmd & ~127ull);
                /* Count and JG_ZERO share one SPU reservation line with the
                 * notifier's final decrement. Keep the decision and parked
                 * publication indivisible: otherwise Notify can see count=0
                 * before this worker publishes parked=1 and lose the rerun. */
                spu_reservation_lock();
                jobguard_line_state guard_state = {
                    vm_read32(guard + JG_COUNT), vm_read32(guard + JG_ZERO)
                };
                u32 count = guard_state.count;
                if (jobguard_line_try_park(&guard_state)) {
                    /* GUARD suspends this command stream; it does not restart
                     * it. Save before publishing parked so a notifier cannot
                     * wake a walker that revisits an earlier auto-reset guard. */
                    s_jobchains[slot].resume_pc = pc;
                    s_jobchains[slot].resume_return_pc = ret_pc;
                    /* The real jobchain policy marks the guard as parked. A
                     * PPU JobGuardNotify uses this word to decide whether it
                     * must signal/restart the chain after count reaches zero. */
                    vm_write32(guard + JG_ZERO, guard_state.parked);
                    spu_reservation_bump_locked(guard);
                    spu_reservation_unlock();
                    tj_jobguard_trace(jc_ea, entry_ea, pc, guard, count,
                                      guard_state.parked, "parked");
                    if (spurs_trace_enabled())
                        printf("[cellSpurs] chain 0x%08X: GUARD 0x%08X waiting (count=%u) after %d job(s)\n",
                               jc_ea, guard, count, jobs);
                    return;
                }
                vm_write32(guard + JG_ZERO, guard_state.parked);
                if (vm_read32(guard + JG_AUTO_RESET))
                    vm_write32(guard + JG_COUNT, vm_read32(guard + JG_RESET_COUNT));
                spu_reservation_bump_locked(guard);
                spu_reservation_unlock();
                tj_jobguard_trace(jc_ea, entry_ea, pc, guard,
                                  vm_read32(guard + JG_COUNT),
                                  guard_state.parked, "opened");
                if (spurs_trace_enabled())
                    printf("[cellSpurs] chain 0x%08X: GUARD 0x%08X opened after %d job(s)\n",
                           jc_ea, guard, jobs);
                pc += 8; continue;
            }
            if (ext == (7 | (15 << 3))) {                     /* END */
                tj_jobchain_trace("TERMINAL", jc_ea, entry_ea, pc, 0, 0,
                                  "end");
                if (spurs_trace_enabled())
                    printf("[cellSpurs] chain 0x%08X: END after %d job(s)\n", jc_ea, jobs);
                return;
            }
            if (ext == (7 | (14 << 3))) {                     /* RET */
                if (!ret_pc) {
                    tj_jobchain_trace("TERMINAL", jc_ea, entry_ea, pc, 0, 0,
                                      "bare-ret");
                    return;
                }
                pc = ret_pc; ret_pc = 0; continue;
            }
            if (ext == (7 | (0 << 3))) {                      /* ABORT */
                tj_jobchain_trace("TERMINAL", jc_ea, entry_ea, pc, 0, 0,
                                  "abort");
                printf("[cellSpurs] chain 0x%08X: ABORT\n", jc_ea);
                return;
            }
        }
        /* NOP(0) / SYNC+LWSYNC(2: nothing left to wait for -- the serial walk
         * has no job in flight, and lanes drained the window above) /
         * FLUSH(5) / SET_LABEL */
        pc += 8;
    }
    tj_jobchain_trace("TERMINAL", jc_ea, entry_ea, pc, 0, 0, "step-bound");
    printf("[cellSpurs] chain 0x%08X: hit the 4096-step bound after %u job(s) -- malformed?\n",
           jc_ea, jobs);
}

/* Every exit of the walk (END, park, shutdown, budget, ...) passes the final
 * drain, so the worker never reports a chain done or parked with a job of it
 * still running. */
static void jc_execute(int slot)
{
    jc_lanes* lanes = jc_lanes_begin(slot);
    jc_walk(slot, lanes);
    jc_lanes_flush(lanes, 1);
}

#ifdef _WIN32
static DWORD WINAPI jc_thread(LPVOID p)
{
    int slot = (int)(intptr_t)p;
    if (!guest_world_register_current("spurs-jobchain")) {
        _InterlockedExchange(&s_jobchains[slot].run_requested, 0);
        _InterlockedExchange(&s_jobchains[slot].running, 0);
        tj_late_eventflag_trace_note_jobchain_worker_register_failure(
            s_jobchains[slot].jc_ea);
        _InterlockedDecrement(&s_jobchains[slot].worker_count);
        return 0;
    }
    /* TIMING PROBE (LBP_JC_DELAY=ms): the real jm2 chain walker is async and
     * picks up jobs as the PPU appends them + fills their descriptors. Our walk
     * is one-shot; if it reads descriptors before the PPU populates the I/O
     * (n_dma=0, empty ioBuffer), deferring the walk should let real I/O appear.
     * Confirms timing-vs-never before committing to the async rewrite. */
    { static long long s_delay = -2;
      if (s_delay == -2) { const char* d = getenv("LBP_JC_DELAY");
          s_delay = (d && *d) ? (long long)(unsigned)atoi(d) : -1; }
      if (s_delay >= 0) Sleep((unsigned)s_delay); }

    for (;;) {
        int perf = tj_jobchain_perf_enabled();
        uint64_t worker_start = perf ? tj_jobchain_perf_qpc() : 0;
        uint64_t request = perf ? (uint64_t)tj_jobchain_perf_load(
            &s_jobchains[slot].perf_request_qpc) : 0;
        if (perf) {
            if (!request || request > worker_start) request = worker_start;
            tj_jobchain_perf_store(&s_jobchains[slot].perf_jobs_current, 0);
        }
        jobchain_worker_begin(&s_jobchains[slot].run_requested);
        tj_late_jobchain_trace("WORKER_BEGIN", slot, s_jobchains[slot].jc_ea,
                               0, 0);
        jc_execute(slot);
        if (perf) {
            uint64_t complete = tj_jobchain_perf_qpc();
            uint64_t startup_us = tj_jobchain_perf_us(request, worker_start);
            uint64_t exec_us = tj_jobchain_perf_us(worker_start, complete);
            uint64_t total_us = tj_jobchain_perf_us(request, complete);
            int64_t jobs = tj_jobchain_perf_load(&s_jobchains[slot].perf_jobs_current);
            unsigned perf_row_count = 0;
            tj_jobchain_perf_lock();
            tj_jobchain_perf_store(&s_jobchains[slot].perf_complete_qpc, (int64_t)complete);
            tj_jobchain_perf_add(&s_jobchains[slot].perf_runs, 1);
            tj_jobchain_perf_add(&s_jobchains[slot].perf_jobs, jobs);
            tj_jobchain_perf_add(&s_jobchains[slot].perf_start_us, (int64_t)startup_us);
            tj_jobchain_perf_add(&s_jobchains[slot].perf_exec_us, (int64_t)exec_us);
            tj_jobchain_perf_add(&s_jobchains[slot].perf_total_us, (int64_t)total_us);
            tj_jobchain_perf_max(&s_jobchains[slot].perf_start_max_us, (int64_t)startup_us);
            tj_jobchain_perf_max(&s_jobchains[slot].perf_exec_max_us, (int64_t)exec_us);
            tj_jobchain_perf_max(&s_jobchains[slot].perf_total_max_us, (int64_t)total_us);
            tj_jobchain_perf_row* perf_rows =
                tj_jobchain_perf_snapshot_locked(&perf_row_count);
            tj_jobchain_perf_unlock();
            tj_jobchain_perf_print(perf_rows, perf_row_count);
        }
        tj_late_jobchain_trace("EXECUTE_END", slot, s_jobchains[slot].jc_ea,
                               0, 0);
        if (jobchain_worker_should_continue(&s_jobchains[slot].running,
                                            &s_jobchains[slot].run_requested)) {
            if (perf) {
                /* The run gate has selected this worker for the rerun. Consume
                 * its pre-published timestamp exactly once; this is diagnostic
                 * state only and never feeds back into the gate. */
                int64_t pending = tj_jobchain_perf_exchange(
                    &s_jobchains[slot].perf_pending_request_qpc, 0);
                if (pending)
                    tj_jobchain_perf_store(&s_jobchains[slot].perf_request_qpc, pending);
            }
            tj_late_jobchain_trace("WORKER_RERUN", slot,
                                   s_jobchains[slot].jc_ea, 0, 0);
            continue;
        }
        tj_late_jobchain_trace("WORKER_RELEASE", slot, s_jobchains[slot].jc_ea,
                               0, 0);
        guest_world_unregister_current();
        _InterlockedDecrement(&s_jobchains[slot].worker_count);
        return 0;
    }
}
#endif

/* Record the SPU request count the next walk of this chain uses for its lane
 * width (jc_lanes_width). request < 0: the attribute's initialRequestSpuCount.
 * Bookkeeping only; nothing else reads it. */
static void jc_note_request(u32 jc_ea, int request)
{
    u32 slot;
    if (!jc_ea || (jc_ea & 0x7Fu) || !vm_base) return;
    slot = vm_read32(jc_ea + 0x74);
    if (slot >= MAX_JOBCHAINS || s_jobchains[slot].jc_ea != jc_ea) return;
    if (request < 0) request = s_jobchains[slot].init_request;
    if (request > 255) request = 255;
    s_jobchains[slot].request_spus = (u8)request;
}

static s32 jc_run_chain(u64 jc_ea);

/* cellSpursRunJobChain -- start job chain execution (async, like the real one). */
s32 cellSpursRunJobChain(u64 jc_ea)
{
    jc_note_request((u32)jc_ea, -1);
    return jc_run_chain(jc_ea);
}

/* Shared by Run, Kick and a JobGuard restart; each sets the request count. */
static s32 jc_run_chain(u64 jc_ea)
{
    static int s_off = -1;
    if (s_off < 0) s_off = getenv("PS3_NO_JOBCHAIN") ? 1 : 0;
    tj_jobchain_lifecycle_trace("RUN", (u32)jc_ea, 0, 0, 0, "attempt", CELL_OK);
    if (!jc_ea) {
        tj_jobchain_lifecycle_trace("RUN", 0, 0, 0, 0, "null", CELL_SPURS_JOB_ERROR_NULL_POINTER);
        return (s32)CELL_SPURS_JOB_ERROR_NULL_POINTER;
    }
    if (jc_ea & 0x7Fu) {
        tj_jobchain_lifecycle_trace("RUN", (u32)jc_ea, 0, 0, 0, "align", CELL_SPURS_JOB_ERROR_ALIGN);
        return (s32)CELL_SPURS_JOB_ERROR_ALIGN;
    }
    if (vm_read32((u32)jc_ea + 0x74) >= 32) {
        tj_jobchain_lifecycle_trace("RUN", (u32)jc_ea, 0, 0, 0, "wid-invalid", CELL_SPURS_JOB_ERROR_INVAL);
        return (s32)CELL_SPURS_JOB_ERROR_INVAL;
    }

    tj_jobchain_registry_lock();
    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (s_jobchains[i].jc_ea != (u32)jc_ea) continue;
        if (s_jobchains[i].shutdown_requested) {
            tj_jobchain_lifecycle_trace("RUN", (u32)jc_ea, s_jobchains[i].spurs_ea,
                                        0, s_jobchains[i].entry_ea, "shutdown", CELL_OK);
            tj_jobchain_registry_unlock();
            return CELL_OK;
        }
        s_jobchains[i].run_count++;
        tj_jobchain_trace("RUN", (u32)jc_ea, s_jobchains[i].entry_ea, 0, 0,
                          0, "requested");
        if (spurs_trace_enabled() && s_jobchains[i].run_count <= 3) {
            printf("[cellSpurs] RunJobChain(jc=0x%08X) run#%d entry=0x%08X\n",
                   (u32)jc_ea, s_jobchains[i].run_count, s_jobchains[i].entry_ea);
            jc_dump_commands("RunJobChain", s_jobchains[i].entry_ea, 16);
        }
        if (s_off || !s_jobchains[i].entry_ea) {
            tj_late_eventflag_trace_note_jobchain_request((u32)jc_ea, 0);
            tj_jobchain_lifecycle_trace("RUN", (u32)jc_ea, s_jobchains[i].spurs_ea,
                                        0, s_jobchains[i].entry_ea, "disabled", CELL_OK);
            tj_jobchain_registry_unlock();
            return CELL_OK;
        }
#ifdef _WIN32
        int perf = tj_jobchain_perf_enabled();
        uint64_t request_qpc = perf ? tj_jobchain_perf_qpc() : 0;
        /* Publish before jobchain_request_run raises run_requested. A worker
         * that observes the gate can therefore consume this diagnostic data
         * without participating in the run-gate protocol. */
        if (perf)
            tj_jobchain_perf_store(&s_jobchains[i].perf_pending_request_qpc,
                                   (int64_t)request_qpc);
        int start_worker = jobchain_request_run(&s_jobchains[i].running,
                                                &s_jobchains[i].run_requested);
        u32 trace_spurs_ea = s_jobchains[i].spurs_ea;
        u32 trace_entry_ea = s_jobchains[i].entry_ea;
        tj_jobchain_registry_unlock();
        tj_jobchain_lifecycle_trace("RUN", (u32)jc_ea, trace_spurs_ea,
                                    0, trace_entry_ea,
                                    start_worker ? "start-worker" : "coalesced", CELL_OK);
        tj_late_eventflag_trace_note_jobchain_request((u32)jc_ea, start_worker);
        if (perf) {
            if (start_worker)
                tj_jobchain_perf_store(&s_jobchains[i].perf_request_qpc,
                                       (int64_t)request_qpc);
            else {
                tj_jobchain_perf_lock();
                tj_jobchain_perf_add(&s_jobchains[i].perf_coalesced, 1);
                tj_jobchain_perf_unlock();
            }
        }
        tj_late_jobchain_trace("RUN_REQUEST", i, (u32)jc_ea, 0, start_worker);
        if (start_worker) {
            if (perf) tj_jobchain_perf_lock();
            _InterlockedIncrement(&s_jobchains[i].worker_count);
            HANDLE th = CreateThread(NULL, 1u << 20, jc_thread, (LPVOID)(intptr_t)i, 0, NULL);
            if (th) {
                tj_late_eventflag_trace_note_jobchain_thread_result((u32)jc_ea, 1);
                if (perf) tj_jobchain_perf_add(&s_jobchains[i].perf_threads, 1);
                CloseHandle(th);
            }
            else {
                tj_late_eventflag_trace_note_jobchain_thread_result((u32)jc_ea, 0);
                _InterlockedExchange(&s_jobchains[i].running, 0);
                tj_late_jobchain_trace("THREAD_CREATE_FAIL", i, (u32)jc_ea,
                                       0, 0);
                _InterlockedDecrement(&s_jobchains[i].worker_count);
            }
            if (perf) tj_jobchain_perf_unlock();
        }
#endif
        
#ifndef _WIN32
        tj_jobchain_registry_unlock();
#endif
        return CELL_OK;
    }
    tj_jobchain_registry_unlock();
    printf("[cellSpurs] RunJobChain(jc=0x%08X) -- UNKNOWN chain (no Create seen)\n", (u32)jc_ea);
    tj_jobchain_lifecycle_trace("RUN", (u32)jc_ea, 0, 0, 0, "unknown", CELL_SPURS_JOB_ERROR_STAT);
    return (s32)CELL_SPURS_JOB_ERROR_STAT;
}

/* Request graceful termination. The worker checks between command-stream
 * entries; JoinJobChain remains the synchronization point, matching the SDK's
 * Shutdown-then-Join lifecycle. A parked chain is already quiescent, and the
 * flag also prevents a JobGuard notification from restarting it. */
s32 cellSpursShutdownJobChain(u64 jc_ea)
{
    if (!jc_ea) return (s32)CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (jc_ea & 0x7Fu) return (s32)CELL_SPURS_JOB_ERROR_ALIGN;
    if (vm_read32((u32)jc_ea + 0x74) >= 32)
        return (s32)CELL_SPURS_JOB_ERROR_INVAL;
    tj_jobchain_registry_lock();
    for (int i = 0; i < MAX_JOBCHAINS; i++) {
        if (s_jobchains[i].jc_ea != (u32)jc_ea) continue;
#ifdef _WIN32
        _InterlockedExchange(&s_jobchains[i].shutdown_requested, 1);
#else
        s_jobchains[i].shutdown_requested = 1;
#endif
        tj_jobchain_registry_unlock();
        if (spurs_trace_enabled())
            printf("[cellSpurs] ShutdownJobChain(jc=0x%08X)\n", (u32)jc_ea);
        return CELL_OK;
    }
    tj_jobchain_registry_unlock();
    return (s32)CELL_SPURS_JOB_ERROR_STAT;
}

s32 cellSpursJoinJobChain(u64 jc_ea)
{
    static int _n = 0;
    if (!jc_ea) return (s32)CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (jc_ea & 0x7Fu) return (s32)CELL_SPURS_JOB_ERROR_ALIGN;
    if (vm_read32((u32)jc_ea + 0x74) >= 32)
        return (s32)CELL_SPURS_JOB_ERROR_INVAL;
    if (_n++ < 8) printf("[cellSpurs] JoinJobChain(jc=0x%08X)\n", (u32)jc_ea);
    int i = -1;
    u32 generation = 0;
    tj_jobchain_registry_lock();
    for (int slot = 0; slot < MAX_JOBCHAINS; slot++) {
        if (s_jobchains[slot].jc_ea == (u32)jc_ea) {
            generation = s_jobchains[slot].generation;
            i = slot;
            break;
        }
    }
    tj_jobchain_registry_unlock();
    if (i < 0) return (s32)CELL_SPURS_JOB_ERROR_STAT;
    {
        int perf = tj_jobchain_perf_enabled();
        uint64_t join_begin = perf ? tj_jobchain_perf_qpc() : 0;
        int waited = 0;
        int same_generation = 1;
        tj_jobchain_perf_row* perf_rows = NULL;
        unsigned perf_row_count = 0;
        for (;;) {
            int busy;
            tj_jobchain_registry_lock();
            if (s_jobchains[i].jc_ea != (u32)jc_ea ||
                s_jobchains[i].generation != generation) {
                same_generation = 0;
                tj_jobchain_registry_unlock();
                break;
            }
            busy = tj_late_job_trace_load(&s_jobchains[i].running) ||
                   tj_late_job_trace_load(&s_jobchains[i].worker_count);
            tj_jobchain_registry_unlock();
            if (!busy) break;
            waited = 1;
#ifdef _WIN32
            Sleep(1);
#else
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
#endif
        }
        tj_jobchain_registry_lock();
        if (s_jobchains[i].jc_ea == (u32)jc_ea &&
            s_jobchains[i].generation == generation &&
            !tj_late_job_trace_load(&s_jobchains[i].running) &&
            !tj_late_job_trace_load(&s_jobchains[i].worker_count)) {
            if (perf && same_generation) {
                uint64_t join_end = tj_jobchain_perf_qpc();
                uint64_t wait_us = tj_jobchain_perf_us(join_begin, join_end);
                uint64_t complete = (uint64_t)tj_jobchain_perf_load(
                    &s_jobchains[i].perf_complete_qpc);
                uint64_t overshoot_us = waited && complete >= join_begin && complete <= join_end
                    ? tj_jobchain_perf_us(complete, join_end) : 0;
                /* Lock order is registry -> perf.  The row cannot be
                 * recreated between these accounting writes and cleanup. */
                tj_jobchain_perf_lock();
                tj_jobchain_perf_add(&s_jobchains[i].perf_join_calls, 1);
                tj_jobchain_perf_add(&s_jobchains[i].perf_join_wait_us, (int64_t)wait_us);
                tj_jobchain_perf_add(&s_jobchains[i].perf_join_overshoot_us,
                                     (int64_t)overshoot_us);
                tj_jobchain_perf_max(&s_jobchains[i].perf_join_wait_max_us,
                                     (int64_t)wait_us);
                tj_jobchain_perf_max(&s_jobchains[i].perf_join_overshoot_max_us,
                                     (int64_t)overshoot_us);
                perf_rows = tj_jobchain_perf_snapshot_locked(&perf_row_count);
                tj_jobchain_perf_unlock();
            }
            vm_write32((u32)jc_ea + 0x74, 32);
            memset(&s_jobchains[i], 0, sizeof(s_jobchains[i]));
        }
        tj_jobchain_registry_unlock();
        tj_jobchain_perf_print(perf_rows, perf_row_count);
        return CELL_OK;
    }
}

s32 cellSpursJobChainGetError(u64 jc_ea, u64 cause_out_ea)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] JobChainGetError(jc=0x%08X)\n", (u32)jc_ea);
    if (cause_out_ea) vm_write32((u32)cause_out_ea, 0);
    return CELL_OK;
}

s32 cellSpursJobGuardInitialize(u64 jc_ea, u64 guard_ea, u32 notify_count,
                                u8 request_spu_count, u8 auto_reset)
{
    u32 jc = (u32)jc_ea, guard = (u32)guard_ea;
    if (!jc || !guard) return (s32)CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if ((jc & 0x7Fu) || (guard & 0x7Fu)) return (s32)CELL_SPURS_JOB_ERROR_ALIGN;
    if (vm_read32(jc + 0x74) >= 32) return (s32)CELL_SPURS_JOB_ERROR_INVAL;

    memset(vm_base + guard, 0, JG_SIZE);
    vm_write32(guard + JG_COUNT, notify_count);
    vm_write32(guard + JG_RESET_COUNT, notify_count);
    vm_write32(guard + JG_JOB_CHAIN, jc);
    vm_write32(guard + JG_SPU_COUNT, request_spu_count);
    vm_write32(guard + JG_AUTO_RESET, auto_reset);
    vm_write32(guard + JG_ZERO, 0);
    if (spurs_trace_enabled())
        printf("[cellSpurs] JobGuardInitialize(guard=0x%08X jc=0x%08X count=%u spus=%u auto=%u)\n",
               guard, jc, notify_count, request_spu_count, auto_reset);
    return CELL_OK;
}

s32 cellSpursJobGuardNotify(u64 guard_ea)
{
    u32 guard = (u32)guard_ea;
    unsigned trace_slot = tj_jobguard_trace_slot();
    if (trace_slot < TJ_JOBGUARD_TRACE_SLOTS) {
        g_tj_jobguard_guard[trace_slot] = guard;
        g_tj_jobguard_old[trace_slot] = 0;
        g_tj_jobguard_zero[trace_slot] = 0;
        g_tj_jobguard_chain[trace_slot] = 0;
        tj_jobguard_trace_stage(trace_slot, 1);       /* entered */
    }
    if (!guard) {
        tj_jobguard_trace_stage(trace_slot, -1);      /* null */
        return (s32)CELL_SPURS_JOB_ERROR_NULL_POINTER;
    }
    if (guard & 0x7Fu) {
        tj_jobguard_trace_stage(trace_slot, -2);      /* unaligned */
        return (s32)CELL_SPURS_JOB_ERROR_ALIGN;
    }
    tj_jobguard_trace_stage(trace_slot, 2);           /* validated */

    /* Serialize the final decrement with jc_execute's JG_ZERO publication;
     * both fields belong to this 128-byte reservation line. */
    spu_reservation_lock();
    jobguard_line_state guard_state = {
        vm_read32(guard + JG_COUNT), vm_read32(guard + JG_ZERO)
    };
    u32 old = guard_state.count;
    if (trace_slot < TJ_JOBGUARD_TRACE_SLOTS)
        g_tj_jobguard_old[trace_slot] = old;
    tj_jobguard_trace_stage(trace_slot, 3);           /* count observed */
    int restart_parked_chain = jobguard_line_notify(&guard_state);
    if (restart_parked_chain < 0) {
        spu_reservation_unlock();
        tj_jobguard_trace_stage(trace_slot, -3);      /* zero count */
        return (s32)CELL_SPURS_JOB_ERROR_STAT;
    }
    vm_write32(guard + JG_COUNT, guard_state.count);
    spu_reservation_bump_locked(guard);
    u32 jc = (guard_state.count == 0 || tj_jobchain_trace_enabled())
                 ? vm_read32(guard + JG_JOB_CHAIN) : 0;
    spu_reservation_unlock();
    tj_jobguard_trace(jc, 0, 0, guard, guard_state.count,
                      guard_state.parked,
                      restart_parked_chain < 0 ? "invalid" :
                      restart_parked_chain ? "restart" : "no-restart");
    tj_jobguard_trace_stage(trace_slot, 4);           /* decrement committed */
    tj_jobguard_trace_stage(trace_slot, 5);           /* entering reservation notify */
    tj_jobguard_trace_stage(trace_slot, 6);           /* reservation notified */
    if (old > 1) {
        tj_jobguard_trace_stage(trace_slot, 7);       /* ordinary return */
        return CELL_OK;
    }

    if (trace_slot < TJ_JOBGUARD_TRACE_SLOTS) {
        g_tj_jobguard_zero[trace_slot] = restart_parked_chain;
        g_tj_jobguard_chain[trace_slot] = jc;
    }
    tj_jobguard_trace_stage(trace_slot, 8);           /* zero-path fields read */
    if (spurs_trace_enabled())
        printf("[cellSpurs] JobGuardNotify(guard=0x%08X) reached zero; run=%u jc=0x%08X\n",
               guard, restart_parked_chain != 0, jc);
    if (jc && restart_parked_chain) {
        tj_jobguard_trace_stage(trace_slot, 9);       /* entering chain restart */
        /* The guard restarts the chain with its own requestSpuCount. */
        jc_note_request(jc, (int)(vm_read32(guard + JG_SPU_COUNT) & 0xFFu));
        jc_run_chain(jc);
        tj_jobguard_trace_stage(trace_slot, 10);      /* chain restart returned */
    }
    tj_jobguard_trace_stage(trace_slot, 11);          /* final return */
    return CELL_OK;
}

s32 cellSpursJobGuardReset(u64 guard_ea)
{
    u32 guard = (u32)guard_ea;
    if (!guard) return (s32)CELL_SPURS_JOB_ERROR_NULL_POINTER;
    if (guard & 0x7Fu) return (s32)CELL_SPURS_JOB_ERROR_ALIGN;
    vm_write32(guard + JG_COUNT, vm_read32(guard + JG_RESET_COUNT));
    return CELL_OK;
}

/* cellSpursKickJobChain(CellSpursJobChain*, u8 numReadyCount).
 * Kick is another chain-start entry point, not a two-pointer no-op. */
s32 cellSpursKickJobChain(u64 jc_ea, u32 numReadyCount)
{
    tj_jobchain_lifecycle_trace("KICK", (u32)jc_ea, 0, 0, 0,
                                "entry", CELL_OK);
    jc_note_request((u32)jc_ea, (int)(numReadyCount & 0xFFu));
    return jc_run_chain(jc_ea);
}

/* =========================================================================
 * SPURS queues (PPU<->SPU bounded FIFO; LBP's audio instance uses these).
 * Log-only bring-up: capture the shapes before wiring real state.
 * =====================================================================*/

/* SDK ABI (cell/spurs/queue.h):
 *   _cellSpursQueueInitialize(CellSpurs*, CellSpursTaskset*, CellSpursQueue*,
 *       const void* buffer, u32 size, u32 depth, CellSpursQueueDirection) */
s32 _cellSpursQueueInitialize(u64 spurs_ea, u64 taskset_ea, u64 queue_ea,
                              u64 buffer_ea, u32 size, u32 depth, u32 direction)
{
    (void)spurs_ea;
    static int _n = 0;
    if (_n++ < 8)
        printf("[cellSpurs] _QueueInitialize(taskset=0x%08X q=0x%08X buf=0x%08X size=%u depth=%u dir=%u)\n",
               (u32)taskset_ea, (u32)queue_ea, (u32)buffer_ea, size, depth, direction);
    return CELL_OK;
}

s32 cellSpursQueueClear(u64 queue_ea)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] QueueClear(q=0x%08X)\n", (u32)queue_ea);
    return CELL_OK;
}

/* SDK ABI: cellSpursQueuePushBody(CellSpursQueue*, const void* buffer, bool isBlocking) */
s32 cellSpursQueuePushBody(u64 queue_ea, u64 data_ea, u32 isBlocking)
{
    static int _n = 0;
    if (_n < 8)
        printf("[cellSpurs] QueuePushBody(q=0x%08X data=0x%08X blocking=%u)\n",
               (u32)queue_ea, (u32)data_ea, isBlocking);
    else if (_n == 8)
        printf("[cellSpurs] QueuePushBody further logs suppressed\n");
    _n++;
    return CELL_OK;
}

/* =========================================================================
 * Misc SPURS surface the title links (logged CELL_OK stubs).
 * =====================================================================*/

s32 cellSpursRequestIdleSpu(u64 spurs_ea)
{
    static int _n = 0;
    if (_n++ < 4) printf("[cellSpurs] RequestIdleSpu(spurs=0x%08X)\n", (u32)spurs_ea);
    return CELL_OK;
}

s32 cellSpursGetInfo(u64 spurs_ea, u64 info_ea)
{
    struct SpursInst* si = spurs_inst_find((u32)spurs_ea);
    static int _n = 0;
    if (_n++ < 4) printf("[cellSpurs] GetInfo(spurs=0x%08X info=0x%08X)\n", (u32)spurs_ea, (u32)info_ea);
    if (info_ea) {
        vm_write32((u32)info_ea, si ? si->nspus : 1);   /* nSpus */
        for (int o = 4; o < 0x28; o += 4) vm_write32((u32)info_ea + o, 0);
    }
    return CELL_OK;
}

s32 cellSpursSetExceptionEventHandler(u64 spurs_ea, u64 handler_ea, u64 arg_ea)
{
    static int _n = 0;
    if (_n++ < 4)
        printf("[cellSpurs] SetExceptionEventHandler(spurs=0x%08X handler=0x%08X)\n",
               (u32)spurs_ea, (u32)handler_ea);
    return CELL_OK;
}

s32 cellSpursGetWorkloadInfo(u64 spurs_ea, u32 wid, u64 info_ea)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] GetWorkloadInfo(wid=%u info=0x%08X)\n", wid, (u32)info_ea);

    if (!spurs_ea || !info_ea)
        return CELL_SPURS_CORE_ERROR_NULL_POINTER;
    if (wid >= CELL_SPURS_MAX_WORKLOAD)
        return CELL_SPURS_CORE_ERROR_INVAL;
    const SpursWorkload* w = spurs_workload_find((u32)spurs_ea, wid);
    if (!w)
        return CELL_SPURS_CORE_ERROR_SRCH;

    /* CellSpursWorkloadInfo is a GUEST out-buffer (cell/spurs/workload_types.h),
     * big-endian, 32-bit pointers. This used to return CELL_OK writing NOTHING,
     * so the guest read whatever stale bytes sat at info_ea as the descriptor.
     * Fill the header from our workload mirror + the live BE instance counters;
     * zero the pointer/name/hook fields we do not track. Byte fields are single
     * bytes (endian-neutral); multi-byte fields go through vm_write* (BE). */
    u32 base = (u32)info_ea;
    for (u32 o = 0; o < 0x30; o += 4) vm_write32(base + o, 0);   /* header clean */

    vm_write64(base + 0x00, w->data);                            /* data        */
    for (int b = 0; b < 8 && b < CELL_SPURS_MAX_SPU; b++)
        *(vm_base + base + 0x08 + b) = w->priority[b];           /* priority[8] */
    vm_write32(base + 0x10, (u32)(uintptr_t)w->pm);              /* policyModule (32-bit EA) */
    vm_write32(base + 0x14, w->sizePm);                          /* sizePolicyModule */
    vm_write32(base + 0x18, w->nameClass_ea);
    vm_write32(base + 0x1C, w->nameInstance_ea);

    u32 se = (u32)spurs_ea;
    *(vm_base + base + 0x20) = *(vm_base + se + SPURS_WKL_CURCONT + wid);  /* contention   */
    *(vm_base + base + 0x21) = (u8)w->minContention;                       /* minContention */
    *(vm_base + base + 0x22) = (u8)w->maxContention;                       /* maxContention */
    *(vm_base + base + 0x23) = *(vm_base + se + SPURS_WKL_READY1 + wid);   /* readyCount   */
    *(vm_base + base + 0x24) = *(vm_base + se + SPURS_WKL_IDLE2  + wid);   /* idleSpuRequest */
    u32 sig = vm_read32(se + SPURS_WKL_SIGNAL1) >> 16;
    *(vm_base + base + 0x25) = (sig & (0x8000u >> wid)) ? 1 : 0;           /* hasSignal    */
    return CELL_OK;
}

s32 cellSpursShutdownWorkload(u64 spurs_ea, u32 wid)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] ShutdownWorkload(wid=%u)\n", wid);
    return CELL_OK;
}

s32 cellSpursWaitForWorkloadShutdown(u64 spurs_ea, u32 wid)
{
    static int _n = 0;
    if (_n++ < 8) printf("[cellSpurs] WaitForWorkloadShutdown(wid=%u)\n", wid);
    return CELL_OK;
}
