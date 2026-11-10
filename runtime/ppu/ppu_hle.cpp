/*
 * ps3recomp - PPU HLE bridge (NID -> host function dispatch)
 *
 * Connects the recompiled game's firmware imports to our HLE C libraries.
 * The game calls an imported function through its import stub; the lifter
 * emits `ps3_hle_call(<NID>, ctx)` for those addresses (see ppu_lifter.py
 * --imports). This resolves the NID to a registered HLE handler and marshals
 * the PPC calling convention into a native C call.
 *
 * PPC64 ELFv1 integer/pointer ABI: arguments in r3..r10 (gpr[3..10]), return
 * value in r3. The generic adapter casts the handler to a uint64-in/uint64-out
 * function and passes the 8 GPR argument slots; this covers the large majority
 * of cellXxx APIs (integer args/handles, s32 return). Functions that take or
 * return *pointers* need host<->guest address translation and so require a
 * per-function wrapper -- the generic path passes the raw value through.
 *
 * Compiled as C++ (matches the lifted output). Game-agnostic.
 */
#include "ppu_recomp.h"   /* ppu_context */
#include "ps3emu/nid.h"   /* ps3_nid_table, ps3_nid_entry */
#include <stdlib.h>       /* getenv */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>       /* getenv, atoi (boot trace) */
#include <chrono>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Single flat NID -> handler table (all modules share it; resolution is by
 * NID which is globally unique). Sized for the firmware import surface. */
#define HLE_NID_CAP 4096
static ps3_nid_entry  g_hle_storage[HLE_NID_CAP];
static ps3_nid_table  g_hle_nids;
static int            g_hle_inited = 0;

extern "C" void ps3_hle_register(uint32_t nid, const char* name, void* handler)
{
    if (!g_hle_inited) { ps3_nid_table_init(&g_hle_nids, g_hle_storage, HLE_NID_CAP); g_hle_inited = 1; }
    /* Registration order is intentional: generic libraries register first,
     * then a title may install a compatibility override for the same NID.
     * The old append-only table used first-match lookup, so later overrides
     * were present but unreachable. Replace an existing entry in place. */
    if (ps3_nid_entry* existing = ps3_nid_table_find(&g_hle_nids, nid)) {
        existing->name = name;
        existing->handler = handler;
        return;
    }
    if (ps3_nid_table_add(&g_hle_nids, nid, name, handler) != 0) {
        fprintf(stderr,
                "[HLE] FATAL: generic NID registry full (%u entries), rejected 0x%08X %s\n",
                g_hle_nids.count, nid, name ? name : "<unnamed>");
        abort();
    }
}

extern "C" uint32_t ps3_hle_count(void) { return g_hle_inited ? g_hle_nids.count : 0; }

/* Context-aware handlers: functions that need the full ppu_context (to read
 * args beyond the generic ABI, set registers like r13, touch memory, etc.).
 * Registered separately and dispatched before the generic table. */
typedef void (*hle_ctx_fn)(ppu_context*);
#define HLE_CTX_CAP 1024
static struct { uint32_t nid; const char* name; hle_ctx_fn fn; } g_ctx[HLE_CTX_CAP];
static uint32_t g_ctx_count = 0;

extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn)
{
    /* Later registrations are title-specific overrides. Preserve their
     * precedence without accumulating duplicate entries. */
    for (uint32_t i = 0; i < g_ctx_count; ++i) {
        if (g_ctx[i].nid == nid) {
            g_ctx[i].name = name;
            g_ctx[i].fn = fn;
            return;
        }
    }
    if (g_ctx_count >= HLE_CTX_CAP) {
        fprintf(stderr,
                "[HLE] FATAL: context NID registry full (%u entries), rejected 0x%08X %s\n",
                g_ctx_count, nid, name ? name : "<unnamed>");
        abort();
    }
    g_ctx[g_ctx_count].nid = nid;
    g_ctx[g_ctx_count].name = name;
    g_ctx[g_ctx_count].fn = fn;
    ++g_ctx_count;
}

extern "C" uint32_t ps3_hle_ctx_count(void) { return g_ctx_count; }

extern "C" int ps3_hle_has(uint32_t nid)
{
    for (uint32_t i = 0; i < g_ctx_count; i++)
        if (g_ctx[i].nid == nid) return 1;
    return g_hle_inited && ps3_nid_table_find(&g_hle_nids, nid) != nullptr;
}

/* Generic PPC integer/pointer ABI adapter. */
typedef uint64_t (*hle_generic)(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t, uint64_t);

/* Host VM store (defined in ppu_loader.cpp) — used for the TOC save below. */
void vm_write64(uint64_t addr, uint64_t val);

/* Breadcrumb for the crash reporter: the last firmware import dispatched, so a
 * host AV inside an HLE handler names the culprit NID/function. */
extern "C" uint32_t    g_last_hle_nid  = 0;
extern "C" const char* g_last_hle_name = "";

/* Real-PRX bridge: a loaded system PRX (libsre = cellSpurs/cellSync) may export
 * this NID. If so, dispatch into the REAL recompiled Sony code (its OPD -> our
 * indirect dispatcher -> the registered lifted libsre function) instead of the
 * HLE stub. prx_resolve_export returns 0 when no PRX exports the NID, so this is
 * a no-op when no PRX is loaded. */
extern "C" uint32_t prx_resolve_export(uint32_t nid);
extern "C" void     ps3_indirect_call(ppu_context* ctx);
extern "C" uint32_t vm_read32(uint64_t a);
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" uint64_t ppu_guest_call(uint32_t opd,
                                    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
/* Weak: builds that don't link cellGcmSys.c (test harnesses) get a null and skip. */
extern "C" __attribute__((weak)) void ppu_gcm_pump(void);
extern "C" __attribute__((weak)) void ppu_poll_display_callbacks(ppu_context* ctx);
/* Weak, same reason: TJ_HLE_PERF companion line for cellGcmSys.c poll waits. */
extern "C" __attribute__((weak)) void cellGcm_wait_perf_report(void);
extern "C" void ppu_dispatch_display_callbacks(void)
{
    if (ppu_gcm_pump) ppu_gcm_pump();
}
static inline void* ps3_GetModuleHandleA(const char* m){ return (void*)GetModuleHandleA(m); }

/* An unresolved NID is a firmware import we never registered. Returning
 * CELL_OK "so the game keeps going" is the single biggest source of
 * days-long debugging in this project: the title reads back untouched
 * out-params and proceeds on fabricated state, then deadlocks or crashes
 * far from the actual gap. The unregistered mouse (a phantom device polled
 * forever) and six whole modules whose names never matched a source file
 * all reached the game this way.
 *
 * So: report every DISTINCT unresolved NID exactly once (not a global
 * first-40 cap that hides everything after the 40th unique import), count
 * how often each is hit, and make the fabricated return an explicit CHOICE
 * rather than the silent default.
 *
 * Recompiled titles must not continue from fabricated success with untouched
 * out-parameters.  Missing firmware behavior therefore defaults to CELL_ENOSYS.
 * A title still under initial bring-up may explicitly opt into the old behavior
 * while collecting its missing-NID ledger.
 *
 *   PS3_HLE_UNRESOLVED = fail (default): r3 = CELL_ENOSYS.
 *                        = ok: r3 = CELL_OK, reported as fabricated success.
 */
static void ps3_hle_unresolved(uint32_t nid, ppu_context* ctx)
{
    enum { CELL_ENOSYS = (int)0x80010003u };
    static int s_fake = -1;
    if (s_fake < 0) {
        const char* m = getenv("PS3_HLE_UNRESOLVED");
        s_fake = (m && (m[0]=='o' || m[0]=='O')) ? 1 : 0;
    }
    /* First-seen set: report each NID once, then count silently. Linear scan
     * over a small array -- the count of DISTINCT unresolved NIDs a title hits
     * is tens, not thousands (the log-flooders are all the SAME NID). */
    static uint32_t seen[512];
    static uint32_t hits[512];
    static uint32_t n_seen = 0;
    uint32_t i = 0;
    for (; i < n_seen; i++) if (seen[i] == nid) break;
    if (i == n_seen && n_seen < 512) { seen[n_seen] = nid; hits[n_seen] = 0; n_seen++; }
    uint32_t count = (i < 512) ? ++hits[i] : 0;
    (void)count;
    if (count == 1)
        fprintf(stderr, "[hle] UNRESOLVED NID 0x%08X -> %s%s\n",
                nid, s_fake ? "fabricated CELL_OK" : "CELL_ENOSYS",
                s_fake ? " (PS3_HLE_UNRESOLVED=ok)" : "");

    ctx->gpr[3] = s_fake ? 0u : (uint64_t)(uint32_t)CELL_ENOSYS;
}

extern "C" void ppu_prof_stamp(void* ctx, unsigned lr);
extern "C" uint32_t ppu_prof_resolve_host(void* ra);

/* TJ_HLE_PERF=1: fixed-table, main-PPU-only wall-duration profiler. HLE calls
 * are synchronous on this context, so this state needs no locks or allocation. */
enum { TJ_HLE_PERF_CAP = 256, TJ_HLE_PERF_TOP = 8 };
struct tj_hle_perf_site {
    uint32_t nid, lr;
    const char* name;
    uint64_t calls, total_ticks, max_ticks;
};
struct tj_hle_perf_state {
    int enabled;
    LARGE_INTEGER frequency;
    uint64_t next_report_ticks, dropped;
    tj_hle_perf_site sites[TJ_HLE_PERF_CAP];
    unsigned count;
};

static uint64_t tj_hle_perf_us(uint64_t ticks, uint64_t frequency)
{
    return (ticks / frequency) * 1000000ULL +
           ((ticks % frequency) * 1000000ULL) / frequency;
}

static const char* tj_hle_perf_name(uint32_t nid)
{
    for (uint32_t i = 0; i < g_ctx_count; ++i)
        if (g_ctx[i].nid == nid) return g_ctx[i].name ? g_ctx[i].name : "ctx-handler";
    ps3_nid_entry* e = g_hle_inited ? ps3_nid_table_find(&g_hle_nids, nid) : nullptr;
    return e && e->name ? e->name : "unregistered";
}

static void tj_hle_perf_report(tj_hle_perf_state& state)
{
    for (unsigned rank = 0; rank < TJ_HLE_PERF_TOP && rank < state.count; ++rank) {
        unsigned best = rank;
        for (unsigned i = rank + 1; i < state.count; ++i)
            if (state.sites[i].total_ticks > state.sites[best].total_ticks)
                best = i;
        tj_hle_perf_site selected = state.sites[best];
        state.sites[best] = state.sites[rank];
        state.sites[rank] = selected;
        fprintf(stderr,
                "[TJ-HLE-PERF] top=%u nid=0x%08X name=%s lr=0x%08X calls=%llu total=%lluus max=%lluus\n",
                rank + 1, selected.nid, selected.name ? selected.name : "?", selected.lr,
                (unsigned long long)selected.calls,
                (unsigned long long)tj_hle_perf_us(selected.total_ticks,
                                                    (uint64_t)state.frequency.QuadPart),
                (unsigned long long)tj_hle_perf_us(selected.max_ticks,
                                                    (uint64_t)state.frequency.QuadPart));
    }
    if (state.dropped)
        fprintf(stderr, "[TJ-HLE-PERF] dropped-calls=%llu\n",
                (unsigned long long)state.dropped);
    /* Same window and thread: GCM waits this table cannot see (the FIFO
     * recycle callback, SetWaitFlip, the guest's GetFlipStatus poll loop). */
    if (cellGcm_wait_perf_report) cellGcm_wait_perf_report();
    fflush(stderr);
    state.count = 0;
    state.dropped = 0;
}

static void tj_hle_perf_record(tj_hle_perf_state& state, uint32_t nid,
                               uint32_t lr, const char* name, uint64_t elapsed)
{
    for (unsigned i = 0; i < state.count; ++i) {
        tj_hle_perf_site& site = state.sites[i];
        if (site.nid == nid && site.lr == lr) {
            site.calls++;
            site.total_ticks += elapsed;
            if (elapsed > site.max_ticks) site.max_ticks = elapsed;
            return;
        }
    }
    if (state.count == TJ_HLE_PERF_CAP) { state.dropped++; return; }
    tj_hle_perf_site& site = state.sites[state.count++];
    site.nid = nid; site.lr = lr; site.name = name;
    site.calls = 1; site.total_ticks = elapsed; site.max_ticks = elapsed;
}

struct tj_hle_perf_guard {
    tj_hle_perf_state* state = nullptr;
    uint32_t nid = 0, lr = 0;
    const char* name = nullptr;
    uint64_t begin = 0;
    tj_hle_perf_guard(uint32_t call_nid, ppu_context* ctx) {
        if (!ctx || ctx->thread_id != 1) return;
        static tj_hle_perf_state s = {};
        if (!s.enabled) {
            s.enabled = getenv("TJ_HLE_PERF") ? 1 : -1;
            if (s.enabled > 0) QueryPerformanceFrequency(&s.frequency);
        }
        if (s.enabled < 0) return;
        state = &s; nid = call_nid; lr = (uint32_t)ctx->lr;
        name = tj_hle_perf_name(call_nid);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        begin = (uint64_t)now.QuadPart;
    }
    ~tj_hle_perf_guard() {
        if (!state) return;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        tj_hle_perf_record(*state, nid, lr, name, (uint64_t)now.QuadPart - begin);
        if (!state->next_report_ticks)
            state->next_report_ticks = (uint64_t)now.QuadPart +
                                       (uint64_t)state->frequency.QuadPart * 2ULL;
        if ((uint64_t)now.QuadPart >= state->next_report_ticks) {
            /* End timestamp is already captured: reporting itself is excluded. */
            tj_hle_perf_report(*state);
            state->next_report_ticks = (uint64_t)now.QuadPart +
                                       (uint64_t)state->frequency.QuadPart * 2ULL;
        }
    }
};

/* TJ_PAD_CALLER_TRACE=1: compact, diagnostic-only caller inventory for
 * cellPadGetData. Tokyo Jungle reaches the import from a shared pad wrapper,
 * so the import LR alone is not enough to classify a guest menu state. The
 * lifted wrapper saves its own caller at [r1+0xC0]; report that value together
 * with the live state registers. This is observation only: no pad state,
 * guest memory, scheduling, or input is changed. Each host thread owns its
 * small table, avoiding locks on the high-frequency pad path. */
enum { TJ_PAD_CALLER_CAP = 32, TJ_PAD_CALLER_REPORT_LIMIT = 8 };
struct tj_pad_input_snapshot {
    uint32_t data_ea;
    int32_t length, game_x, game_y, profile;
    uint16_t left_x, left_y;
};

/* At the next entry, the previous packet and its guest-decoded axes are both
 * available. Restrict the title-specific offsets to its verified pad wrapper. */
static bool tj_pad_read_input_snapshot(ppu_context* ctx, tj_pad_input_snapshot* out)
{
    if (!ctx || !out || !vm_base ||
        (ctx->lr != 0x001F7248 && ctx->lr != 0x001F7620 && ctx->lr != 0x001F7640))
        return false;
    const uint32_t object = (uint32_t)ctx->gpr[30];
    const uint32_t data_ea = (uint32_t)ctx->gpr[4];
    if (object < 0x10000u || object > 0xFFFFFFFFu - 0x5C4u || data_ea != object + 4u)
        return false;
    out->data_ea = data_ea;
    out->length = (int32_t)vm_read32(data_ea);
    out->left_x = vm_read16(data_ea + 4u + 6u * 2u);
    out->left_y = vm_read16(data_ea + 4u + 7u * 2u);
    out->game_x = (int32_t)vm_read32(object + 0x488u);
    out->game_y = (int32_t)vm_read32(object + 0x494u);
    const uint32_t profile_ea = vm_read32(object + 0x5C0u);
    out->profile = profile_ea >= 0x10000u && profile_ea <= 0xFFFFFFF8u
        ? (int32_t)vm_read32(profile_ea + 4u) : -1;
    return true;
}

struct tj_pad_action_snapshot {
    uint32_t ea, slot, input_type, scale_bits, bias_bits, deadzone_bits, value_bits;
    int32_t raw_value;
};

/* func_001F6464 updates the action pointers at pad+0x88. func_001F6200
 * selects an axis and func_001F6128 writes its scaled/dead-zoned value at+0x18.
 * Read only on observed input changes, not on every high-frequency pad poll. */
static unsigned tj_pad_read_action_snapshots(uint32_t object,
    tj_pad_action_snapshot* out, unsigned capacity)
{
    if (!vm_base || !out || !capacity || object < 0x10000u ||
        object > 0xFFFFFFFFu - 0x5C4u) return 0;
    unsigned count = 0;
    for (unsigned slot = 0; slot < 256 && count < capacity; ++slot) {
        const uint32_t ea = vm_read32(object + 0x88u + slot * 4u);
        if (ea < 0x10000u || ea > 0xFFFFFFFFu - 0x24u ||
            vm_read32(ea) != object) continue;
        const uint32_t type = vm_read32(ea + 8u);
        if (type > 9u) continue; /* digital actions use another representation */
        tj_pad_action_snapshot* action = &out[count++];
        action->ea = ea;
        action->slot = slot;
        action->input_type = type;
        action->scale_bits = vm_read32(ea + 0xCu);
        action->bias_bits = vm_read32(ea + 0x10u);
        action->deadzone_bits = vm_read32(ea + 0x14u);
        action->value_bits = vm_read32(ea + 0x18u);
        action->raw_value = (int32_t)vm_read32(ea + 0x1Cu);
    }
    return count;
}

struct tj_pad_caller_site {
    uint32_t tid, lr, saved_caller;
    uint64_t calls, first_ms, last_report_ms;
    unsigned reports;
    tj_pad_input_snapshot last_input;
    unsigned input_reports;
    bool input_seen;
};
struct tj_pad_caller_state {
    tj_pad_caller_site sites[TJ_PAD_CALLER_CAP];
    unsigned count;
    bool dropped_reported;
};

static uint64_t tj_pad_caller_now_ms(void)
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static void tj_pad_caller_trace(ppu_context* ctx)
{
    static const bool enabled = getenv("TJ_PAD_CALLER_TRACE") != nullptr;
    if (!enabled || !ctx) return;

    const uint32_t tid = (uint32_t)ctx->thread_id;
    const uint32_t lr = (uint32_t)ctx->lr;
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    /* A 32-bit guest stack pointer near wrap cannot have a valid +0xC0 slot.
     * Do not probe it; normal PPU stacks are well inside this bound. */
    const uint32_t saved_caller = (vm_base && sp <= 0xFFFFFF3Fu)
        ? (uint32_t)vm_read64(sp + 0xC0u) : 0u;
    const uint64_t now_ms = tj_pad_caller_now_ms();

    static thread_local tj_pad_caller_state state = {};
    tj_pad_caller_site* site = nullptr;
    for (unsigned i = 0; i < state.count; ++i) {
        if (state.sites[i].tid == tid && state.sites[i].lr == lr &&
            state.sites[i].saved_caller == saved_caller) {
            site = &state.sites[i];
            break;
        }
    }
    if (!site) {
        if (state.count == TJ_PAD_CALLER_CAP) {
            if (!state.dropped_reported) {
                fprintf(stderr, "[TJ-PAD-CALLER] dropped=1 reason=site-cap cap=%u tid=%u\n",
                        TJ_PAD_CALLER_CAP, tid);
                fflush(stderr);
                state.dropped_reported = true;
            }
            return;
        }
        site = &state.sites[state.count++];
        site->tid = tid;
        site->lr = lr;
        site->saved_caller = saved_caller;
        site->calls = 0;
        site->first_ms = now_ms;
        site->last_report_ms = now_ms;
        site->reports = 0;
        fprintf(stderr,
                "[TJ-PAD-CALLER] phase=first tid=%u lr=0x%08X saved_caller=0x%08X "
                "sp=0x%08X r3=0x%08X r4=0x%08X r27=0x%08X r30=0x%08X r31=0x%08X\n",
                tid, lr, saved_caller, sp, (uint32_t)ctx->gpr[3],
                (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[27],
                (uint32_t)ctx->gpr[30], (uint32_t)ctx->gpr[31]);
        fflush(stderr);
    }
    site->calls++;
    tj_pad_input_snapshot input = {};
    if (site->input_reports < 32 && tj_pad_read_input_snapshot(ctx, &input) &&
        (!site->input_seen || input.data_ea != site->last_input.data_ea ||
         input.length != site->last_input.length || input.profile != site->last_input.profile ||
         input.left_x != site->last_input.left_x || input.left_y != site->last_input.left_y ||
         input.game_x != site->last_input.game_x || input.game_y != site->last_input.game_y)) {
        fprintf(stderr,
            "[TJ-PAD-CALLER] phase=input tid=%u lr=%08X saved_caller=%08X data=%08X "
            "previous_len=%d previous_left=%u,%u previous_game=%d,%d profile=%d\n",
            tid, lr, saved_caller, input.data_ea, input.length,
            (unsigned)input.left_x, (unsigned)input.left_y,
            input.game_x, input.game_y, input.profile);
        tj_pad_action_snapshot actions[16] = {};
        const unsigned action_count = tj_pad_read_action_snapshots(
            input.data_ea - 4u, actions, 16);
        for (unsigned i = 0; i < action_count; ++i) {
            const tj_pad_action_snapshot& action = actions[i];
            fprintf(stderr,
                "[TJ-PAD-ACTION] previous=1 pad=%08X action=%08X slot=%u type=%u "
                "scale=%08X bias=%08X deadzone=%08X value=%08X raw=%d\n",
                input.data_ea - 4u, action.ea, action.slot, action.input_type,
                action.scale_bits, action.bias_bits, action.deadzone_bits,
                action.value_bits, action.raw_value);
        }
        site->last_input = input;
        site->input_seen = true;
        ++site->input_reports;
        fflush(stderr);
    }
    if (site->reports < TJ_PAD_CALLER_REPORT_LIMIT &&
        (site->calls == 256 || now_ms - site->last_report_ms >= 1000)) {
        fprintf(stderr,
                "[TJ-PAD-CALLER] phase=sample tid=%u lr=0x%08X saved_caller=0x%08X "
                "calls=%llu elapsed_ms=%llu sp=0x%08X r3=0x%08X r4=0x%08X "
                "r27=0x%08X r30=0x%08X r31=0x%08X\n",
                tid, lr, saved_caller, (unsigned long long)site->calls,
                (unsigned long long)(now_ms - site->first_ms), sp,
                (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[27], (uint32_t)ctx->gpr[30],
                (uint32_t)ctx->gpr[31]);
        fflush(stderr);
        site->last_report_ms = now_ms;
        site->reports++;
    }
}

extern "C" 
/* Per-thread name of the HLE handler in flight, for host-side diagnostics
 * (dispatch_glue.cpp prints it when a guest access faults inside a call). */
#define PS3_HLE_INFLIGHT_MAX 64
extern "C" const char* g_hle_inflight[PS3_HLE_INFLIGHT_MAX] = { nullptr };

extern "C" void ps3_hle_call(uint32_t nid, ppu_context* ctx)
{
    /* Preserve the caller TOC (r2) across the HLE call. ELFv1 makes r2 caller-saved
     * across a cross-module call: the glink stub does `std r2,40(r1)` before jumping and
     * the caller does `ld r2,40(r1)` after. Our HLE import stubs (`ps3_hle_call(nid);
     * return;`) skip that, AND some handlers (cellSpurs* -> 0xAA6269A8 etc.) dispatch
     * into lifted libsre_ns code that returns with r2 = libsre's TOC (0x30039AB0) AND
     * clobbers the caller's reserved TOC slot at 0x28(r1) to 0. The caller's post-call
     * `ld r2,0x28(r1)` then loads 0 -> r2=0 -> every later TOC-relative load reads a null
     * base (e.g. func_002B03AC's `*(r2-0x4B34)` FMOD list head -> null -> infinite walk
     * -> no draws). Snapshot the caller r2+sp now and restore BOTH the live r2 and the
     * ABI slot on every exit path (offset 40 = 0x28 is the reserved TOC doubleword). */
    tj_hle_perf_guard _perf{ nid, ctx };
    struct _TocGuard { ppu_context* c; uint64_t toc, sp;
        ~_TocGuard(){ c->gpr[2] = toc; vm_write64(sp + 0x28, toc); } } _tg{ ctx, ctx->gpr[2], ctx->gpr[1] };
    /* Deliver pending display callbacks only on the registered main PPU thread.
     * Running this on every HLE boundary let arbitrary workers consume the edge
     * and synchronously enter guest vblank/flip code. A worker could then block
     * inside that callback forever (Tokyo Jungle: sgx-audio-thr), so its real
     * producer loop never resumed. The ticker still publishes scalar flip status
     * immediately; only guest callback execution is restricted here. */
    if (ppu_poll_display_callbacks) ppu_poll_display_callbacks(ctx);
    else if (ppu_gcm_pump && ctx->thread_id == 1) ppu_gcm_pump();

    /* YDKJ_TUNERFIX (at the top so it wins over any registered handler): the
     * profiler-presence query sysPrxForUser 0xE0998DBF, called by libsre
     * _cellSpursIsLaunchedFromTuner (0x3000D318), must return 0x8001112E ("profiler not
     * loaded") on a normal run. Anything else trips the usertrace.c:123 assert AND makes
     * the fn report "launched from tuner", so the caller runs tuner/trace setup that fails
     * with CELL_SPURS_CORE_ERROR_STAT -> the SPURS task workload never attaches. */
    if (nid == 0xE0998DBFu && getenv("YDKJ_TUNERFIX")) {
        ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001112E;
        static int _tn = 0; if (_tn++ < 3)
            fprintf(stderr, "[TUNERFIX] sysPrxForUser 0xE0998DBF -> 0x8001112E (profiler not loaded)\n");
        return;
    }
    /* Guest-PC breadcrumb for the sampling profiler (see lv2_syscall). */
    ppu_prof_stamp(ctx, ppu_prof_resolve_host(__builtin_return_address(0)));
    g_last_hle_nid = nid;
    if (nid == 0x8B72CDA1u)
        tj_pad_caller_trace(ctx);
    static int s_frame_state = -1;
    if (s_frame_state < 0) s_frame_state = getenv("TJ_FRAME_STATE") ? 1 : 0;
    if (nid == 0x72A577CEu && s_frame_state) {
        static unsigned frame_wait_calls = 0;
        unsigned call = ++frame_wait_calls;
        if ((call % 600u) == 0u) {
            const uint32_t object = (uint32_t)ctx->gpr[28];
            const uint32_t saved_caller = (uint32_t)vm_read64(ctx->gpr[1] + 0xA0);
            fprintf(stderr,
                    "[TJ-FRAME-STATE] n=%u caller=0x%08X object=0x%08X "
                    "r30=%08X r31=%08X f00=%08X f04=%08X f08=%08X "
                    "f0c=%08X f10=%08X f14=%08X f18=%08X f1c=%08X\n",
                    call, saved_caller, object, (uint32_t)ctx->gpr[30],
                    (uint32_t)ctx->gpr[31], vm_read32(object + 0x00),
                    vm_read32(object + 0x04), vm_read32(object + 0x08),
                    vm_read32(object + 0x0C), vm_read32(object + 0x10),
                    vm_read32(object + 0x14), vm_read32(object + 0x18),
                    vm_read32(object + 0x1C));
            fflush(stderr);
        }
    }
    /* GFX-SCAN: is the menu .gfx ever inflated into guest RAM? (magic 'GFX'=47 46 58) */
    { static long _c=0; static int _gs=-1; if(_gs<0)_gs=getenv("YDKJ_GFXSCAN")?1:0;
      if(_gs && (++_c % 200000)==0){ extern uint8_t* vm_base;
        int gfx=0,cfx=0,swf=0,dds=0,png=0; for(uint32_t a=0x10000; a<0x0FF00000u; a++){
          uint8_t m0=vm_base[a],m1=vm_base[a+1],m2=vm_base[a+2],vv=vm_base[a+3];
          /* uncompressed GFx: 'GFX'/'GFC' ver 4..20 */
          if(m0==0x47&&m1==0x46&&(m2==0x58||m2==0x43)&&(vv>=4&&vv<=20)){ if(gfx<3)fprintf(stderr,"[GFX-SCAN] GFX @0x%08X %c%c%c ver=%d\n",a,m0,m1,m2,vv); gfx++; }
          /* compressed GFx: 'CFX' (zlib) */
          else if(m0==0x43&&m1==0x46&&m2==0x58&&(vv>=4&&vv<=20)){ if(cfx<3)fprintf(stderr,"[GFX-SCAN] CFX(zlib) @0x%08X ver=%d\n",a,vv); cfx++; }
          /* SWF: 'FWS'(raw) / 'CWS'(zlib) / 'ZWS'(lzma) */
          else if((m0==0x46||m0==0x43||m0==0x5A)&&m1==0x57&&m2==0x53&&(vv>=4&&vv<=20)){ if(swf<3)fprintf(stderr,"[GFX-SCAN] SWF @0x%08X %c%c%c ver=%d\n",a,m0,m1,m2,vv); swf++; }
          else if(m0==0x44&&m1==0x44&&m2==0x53&&vv==0x20){ dds++; }
          else if(m0==0x89&&m1==0x50&&m2==0x4E&&vv==0x47){ png++; } }
        fprintf(stderr,"[GFX-SCAN #%ld] GFX=%d CFX=%d SWF=%d DDS=%d PNG=%d\n",_c/200000,gfx,cfx,swf,dds,png); fflush(stderr); } }

    /* Boot trace: log the first N HLE calls (PS3_HLE_TRACE=N). Invaluable for
     * new-SDK bring-up (e.g. PSL1GHT) where the failure is "nothing happens". */
    static int s_trace = -2;
    if (s_trace == -2) { const char* e = getenv("PS3_HLE_TRACE"); s_trace = e ? atoi(e) : 0; }
    if (s_trace > 0) {
        s_trace--;
        fprintf(stderr, "[HLETRACE] nid=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X lr=0x%08X\n",
                nid, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                (uint32_t)ctx->gpr[5], (uint32_t)ctx->lr);
    }
    /* Tokyo Jungle SPURS setup probe: capture the short import sequence after
     * taskset creation without enabling the extremely noisy global HLE trace.
     * The Survival workers park on the object immediately following taskset 0;
     * this proves whether cellSpursBarrierInitialize (0x7FDF4FEF) is actually
     * called, with which object/count, and what calls surround it. */
    { static int s_spurs_setup_left = 0;
      static int s_spurs_setup_trace = -1;
      if (s_spurs_setup_trace < 0)
          s_spurs_setup_trace = getenv("YDKJ_SPURS_SETUP_TRACE") ? 1 : 0;
      if (s_spurs_setup_trace) {
          if (nid == 0xC10931CBu || nid == 0x52CC6C82u)
              s_spurs_setup_left = 96;
          if (s_spurs_setup_left > 0) {
              fprintf(stderr,
                      "[SPURS-SETUP] nid=0x%08X r3=0x%08X r4=0x%08X "
                      "r5=0x%08X r6=0x%08X lr=0x%08X\n",
                      nid, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
                      (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6],
                      (uint32_t)ctx->lr);
              s_spurs_setup_left--;
          }
      } }
    static int s_boot_state = -1;
    if (s_boot_state < 0) s_boot_state = getenv("TJ_BOOT_STATE") ? 1 : 0;
    if (nid == 0x189A74DAu && s_boot_state) {
        const uint32_t holder = vm_read32(0x0035A8B0u);
        const uint32_t manager = holder ? vm_read32(holder) : 0;
        const uint32_t vtable = manager ? vm_read32(manager) : 0;
        const uint32_t init_opd = vtable ? vm_read32(vtable + 8) : 0;
        fprintf(stderr,
                "[TJ:boot-state] object=0x02000000 vtable=0x%08X flags=%u,%u state=0x%08X "
                "holder=0x%08X manager=0x%08X mgr_vtable=0x%08X init_opd=0x%08X init_code=0x%08X\n",
                vm_read32(0x02000000u), vm_read8(0x02000005u),
                vm_read8(0x02000006u), vm_read32(0x02000010u), holder,
                manager, vtable, init_opd, init_opd ? vm_read32(init_opd) : 0);
    }
    /* Bad-lock locator (FLOW_BADLOCK): sys_lwmutex_lock (0x1573DC3F) on a garbage/null
     * object (r3 < 0x10000 or high-bit set) in m_InitEntityHierarchy — host backtrace
     * to find the null global it dereferences. Map RVAs via flow.map. */
    { static int _bl=-1; if(_bl<0)_bl=getenv("FLOW_BADLOCK")?1:0;
      if(_bl && nid==0x1573DC3Fu){ uint32_t r3=(uint32_t)ctx->gpr[3];
        if(r3<0x00010000u || r3>=0x80000000u){ static int _n=0; if(_n++<3){
#ifdef _WIN32
          extern unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long,unsigned long,void**,unsigned long*);
          void* bt[44]; unsigned short fr=RtlCaptureStackBackTrace(0,44,bt,0);
          (void)bt;(void)fr;
          /* Guest-stack code-ptr scan (EXACT: a lifted func name IS its guest addr) —
           * the accurate way to recover the caller chain (host-bt nearest-symbol lies). */
          char ln[1500]; int p=snprintf(ln,sizeof ln,"[BADLOCK] r3=0x%08X guest-codeptrs:",r3);
          uint32_t sp=(uint32_t)ctx->gpr[1]; uint32_t prev=0; int found=0;
          for(uint32_t a=sp; a<sp+0x1800 && a<0x0FF00000u && found<40; a+=4){ uint32_t v=vm_read32(a);
            if(((v>=0x00010000u&&v<0x00900000u)||(v>=0x30000000u&&v<0x30100000u)) && v!=0x008969A8u){
              if(v!=prev){ p+=snprintf(ln+p,sizeof(ln)-p," %08X",v); prev=v; found++; } } }
          fprintf(stderr,"%s\n",ln);
          /* dump the allocator pool table: base ptr at TOC-0x315C (0x008969A8-0x315C=0x0089384C),
           * entries table[0..4]. Tells us if pool[1] alone is null (corruption) or many (init-miss). */
          { uint32_t tbase=vm_read32(0x0089384Cu); uint32_t cnt_ptr=vm_read32(0x00893848u);
            fprintf(stderr,"[BADLOCK] pooltable base=0x%08X state=0x%08X count=0x%08X entries:",
              tbase,cnt_ptr,cnt_ptr?vm_read32(cnt_ptr):0);
            for(int i=0;i<5;i++) fprintf(stderr," [%d]=0x%08X",i,tbase?vm_read32(tbase+i*4):0);
            fprintf(stderr,"\n"); }
#endif
        } } } }
    /* Spin-loop locator: print the guest LR for any HLE call whose r3 lands in the
     * lwmutex-spin object region (0x0275E000-0x02761000). Env FLOW_SPINLR. */
    { static int _sl=-1; if(_sl<0)_sl=getenv("FLOW_SPINLR")?1:0;
      if(_sl){ uint32_t r3=(uint32_t)ctx->gpr[3];
        if(nid==0x2F85C0EFu && r3>=0x02740000u && r3<0x02780000u){ static int _n=0; if(_n++<3){
          /* The DRAIN/fragment model leaves the standard LR slots zero, so scan the
           * guest stack for any word in the lifted code range (game 0x10000-0x900000,
           * libsre 0x30000000-0x30100000) — those are saved return addresses, and a
           * lifted func name IS its guest addr (func_XXXXXXXX), so they map directly. */
          char buf[1400]; int p=snprintf(buf,sizeof buf,"[SPINLR] lwmutex_create r3=0x%08X sp=0x%08X codeptrs:",r3,(uint32_t)ctx->gpr[1]);
          uint32_t sp=(uint32_t)ctx->gpr[1]; uint32_t prev=0; int found=0;
          for(uint32_t a=sp; a<sp+0x2400 && a<0x0FF00000u && found<48; a+=4){ uint32_t v=vm_read32(a);
            if((v>=0x00010000u&&v<0x00900000u)||(v>=0x30000000u&&v<0x30100000u)){
              if(v!=prev){ p+=snprintf(buf+p,sizeof(buf)-p," %08X",v); prev=v; found++; } } }
          fprintf(stderr,"%s\n",buf); } } } }
    { static int tr=-1; if(tr<0){const char*e=getenv("FLOW_HLETRACE"); tr=e?1:0;}
      if(tr) fprintf(stderr,"[hletrace] nid=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
          nid,(uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],(uint32_t)ctx->gpr[6]); }
    /* sys_process_exit (abort path): dump the guest back-chain so we see WHO aborted.
     * Always-on for a NONZERO exit code (error/abort) -- the rare loader-thread
     * abort in LBP is a race we can't reliably reproduce, so capture it whenever
     * it fires. Clean code=0 exits (normal shutdown) stay quiet. */
    if (nid == 0xE6F2C1E7u && ((uint32_t)ctx->gpr[3] != 0 || getenv("FLOW_EXITCHAIN"))) {
        uint32_t sp = (uint32_t)ctx->gpr[1];
        fprintf(stderr, "[exit-chain] code=0x%X lr=0x%08X sp=0x%08X\n", (uint32_t)ctx->gpr[3], (uint32_t)ctx->lr, sp);
        /* Scan the guest stack for words in the lifted code range — saved return
         * addresses; a lifted func name IS its guest addr. (back-chain LR slots are
         * 0 under the DRAIN/fragment model, so scan instead of walk.) */
        { uint32_t prev=0; int found=0; char b[1600]; int p=snprintf(b,sizeof b,"[exit-chain] codeptrs:");
          for(uint32_t a=sp; a<sp+0x3000 && a<0x0FF00000u && found<60; a+=4){ uint32_t v=vm_read32(a);
            if(((v>=0x00010000u&&v<0x00900000u)||(v>=0x30000000u&&v<0x30100000u)) && v!=0x008969A8u){
              if(v!=prev){ p+=snprintf(b+p,sizeof(b)-p," %08X",v); prev=v; found++; } } }
          fprintf(stderr,"%s\n",b); }
#ifdef _WIN32
        { extern unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long,unsigned long,void**,unsigned long*);
          void* bt[40]; unsigned short fr=RtlCaptureStackBackTrace(0,40,bt,0);
          char* mb=(char*)GetModuleHandleA(0); char line[1100]; int p=snprintf(line,sizeof line,"[exit-chain] HOST-bt rva:");
          for(int i=0;i<fr;i++) p+=snprintf(line+p,sizeof(line)-p," %llX",(unsigned long long)((char*)bt[i]-mb));
          fprintf(stderr,"%s\n",line); }
#endif
    }
    /* PPC64 ELFv1 cross-module ABI: the caller restores its TOC right after the
     * call with `ld r2, 0x28(r1)`, expecting the import stub to have saved the
     * caller's r2 into that slot. The real .lib.stub trampoline did this; the
     * lifted --hle-stubs body (ps3_hle_call) doesn't, so without this every
     * import call leaves the caller with a garbage r2 -> all later TOC-relative
     * loads (the C++ ctor list, globals, ...) read garbage -> boot corruption. */
    { static int _h=-1; if(_h<0)_h=getenv("FLOW_HLETOC")?1:0;
      if(_h && (uint32_t)(ctx->gpr[1]+0x28)==0x0FEFF268u){ static int _n=0; if(_n++<6){
        char* mb=(char*)ps3_GetModuleHandleA(0); void* ra0=__builtin_return_address(0); void* ra1=__builtin_return_address(1);
        fprintf(stderr,"[HLETOC] corrupt nid=0x%08X name=%s r1=0x%08X  host_ra0_rva=0x%llX ra1_rva=0x%llX\n",
          nid,g_last_hle_name?g_last_hle_name:"?",(uint32_t)ctx->gpr[1],
          (unsigned long long)((char*)ra0-mb),(unsigned long long)((char*)ra1-mb)); } } }
    /* FLOW_NOSPILL: with the lifted code using a constant main TOC (--main-toc/TOCFIX),
     * the caller no longer reads [r1+0x28] to restore r2, so this ABI TOC-spill is
     * unnecessary — and in the frameless-cascade it can clobber a caller frame slot
     * (e.g. the deserializer's this-pointer). Skip it to test that theory. */
    { static int _ns=-1; if(_ns<0)_ns=getenv("FLOW_NOSPILL")?1:0;
      if(!_ns) vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]); }

    /* Real libsre (loaded PRX) takes priority over the HLE stub -- EXCEPT for NIDs
     * listed in YDKJ_FORCE_HLE (comma-separated hex). Some LLE entry points fail
     * against our partially-modelled SPURS state and the CALLER ASSERTS on it:
     * YDKJ's CRI calls _cellSpursEventFlagInitialize (0x5EF96465), libsre returns
     * 0x80410910 (CELL_SPURS_TASK_ERROR_STAT), and CRI's own
     * "CellSpursTaskset.cc:423: ret == CELL_OK -- assertion failed" kills its
     * taskset init => the movie decode task is never created. Forcing the HLE
     * (which succeeds) lets the caller proceed. */
    {
        uint32_t opd = prx_resolve_export(nid);
        if (opd) {
            static const char* s_force = (const char*)1;
            if (s_force == (const char*)1) s_force = getenv("YDKJ_FORCE_HLE");
            if (s_force && *s_force) {
                char want[16]; snprintf(want, sizeof want, "%08X", nid);
                /* case-insensitive substring match on the 8-hex-digit NID */
                for (const char* p2 = s_force; *p2; p2++) {
                    int k = 0;
                    while (k < 8 && p2[k] &&
                           (p2[k] == want[k] ||
                            (p2[k] >= 'a' && p2[k] - 32 == want[k]))) k++;
                    if (k == 8) {
                        /* CRI exception: cellSpursEventFlagInitialize (0x5EF96465) is
                         * force-HLE'd because libsre's version STAT'd and tripped CRI's
                         * CellSpursTaskset.cc:423 assertion -- but that was before the
                         * taskset/attr fixes. The HLE stub does NOT stamp the eventFlag's
                         * +0xC=0xFF magic that the create-task pre-check (0x300158C4) then
                         * requires, so the CRI create STATs. Route ONLY the cri event flag
                         * (r4 == YDKJ_CRI_EVFLAG, default 0x006B4600) to real libsre so it
                         * stamps +0xC=0xFF and the create passes; keep the audio flags on
                         * the HLE stub (their create STAT is tolerated -- routing them makes
                         * the game wait for the audio SPU task we don't dispatch -> hang). */
                        if (nid == 0x5EF96465u) {
                            const char* ce = getenv("YDKJ_CRI_EVFLAG");
                            uint32_t crif = ce ? (uint32_t)strtoul(ce, 0, 16) : 0x006B4600u;
                            static int _cd=0; if(getenv("YDKJ_SPURSTRACE") && _cd++<4) fprintf(stderr,
                                "[EVFLAG] 0x5EF96465 r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
                                (uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],(uint32_t)ctx->gpr[6]);
                            /* the eventFlag EA can be r3/r4/r5 depending on the (public vs _internal)
                             * variant behind this NID; the cri flag is a unique address so match any. */
                            if ((uint32_t)ctx->gpr[3] == crif || (uint32_t)ctx->gpr[4] == crif ||
                                (uint32_t)ctx->gpr[5] == crif) {
                                static int _c=0; if(_c++<2) fprintf(stderr,
                                    "[FORCE-HLE] cri eventflag 0x%08X -> REAL libsre (stamps +0xC=0xFF)\n", crif);
                                break;   /* keep opd (libsre); do NOT force HLE */
                            }
                        }
                        static int _n = 0;
                        if (_n++ < 8)
                            fprintf(stderr, "[FORCE-HLE] nid=0x%08X: bypassing libsre, using HLE\n", nid);
                        opd = 0;   /* fall through to the HLE handler below */
                        break;
                    }
                }
            }
        }
        if (opd) {
            uint32_t code = vm_read32(opd);
            uint32_t toc  = vm_read32(opd + 4);
            { static int64_t st=-2; if(st==-2){const char*e=getenv("YDKJ_SPURSTRACE"); st=e?1:0;}
              if (st) fprintf(stderr, "[SPURSTRACE] nid=0x%08X -> libsre code=0x%08X  r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X lr=0x%08X\n",
                  nid, code, (uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],(uint32_t)ctx->gpr[6],(uint32_t)ctx->lr);
              /* Dump the struct state at the failing task-attach calls: libsre 0x300158C4
               * (nid 0x87630976) STATs unless struct+0xC==0xFF & +0xE in{1,3}; 0x30015AA4
               * (0x22AAB31D) validates the same struct. Show what our recomp left there. */
              if (st && (nid==0x87630976u || nid==0x22AAB31Du)) {
                  extern void ppu_guest_callstack(const char*);
                  ppu_guest_callstack(nid==0x87630976u?"CREATE-nid":"INIT-nid");
                  extern uint8_t* vm_base; uint32_t s3=(uint32_t)ctx->gpr[3];
                  #define _RW(o) ((vm_base[s3+(o)]<<24)|(vm_base[s3+(o)+1]<<16)|(vm_base[s3+(o)+2]<<8)|vm_base[s3+(o)+3])
                  /* Full task-descriptor dump: +0xC state(need FF), +0xE type, +0x74/+0x7C
                   * handles (the helper 0x3000b140 does an atomic on *(+0x74)+0xda8 -> if 0,
                   * garbage), +0x4/+0x7 stamp preconds. */
                  fprintf(stderr, "  [state] struct@0x%08X:", s3);
                  for (uint32_t o=0; o<0x80; o+=16) {
                      fprintf(stderr, " |+%02X:", o);
                      for (uint32_t k=0;k<16;k+=4) fprintf(stderr, "%08X ", _RW(o+k));
                  }
                  fprintf(stderr, "\n    +0C=%02X(need FF) +0E=%02X +74=%08X +7C=%08X\n",
                      vm_base[s3+0xC], vm_base[s3+0xE], _RW(0x74), _RW(0x7C));
                  /* The helper 0x3000b140 does a bitmap atomic on taskset(+0x74)+0xda8 --
                   * the task-slot free bitmap CreateTaskset should have initialized. Dump it. */
                  uint32_t ts = _RW(0x74);
                  if (ts >= 0x40000000u && ts < 0x50000000u) {
                      #define _RT(o) ((vm_base[ts+(o)]<<24)|(vm_base[ts+(o)+1]<<16)|(vm_base[ts+(o)+2]<<8)|vm_base[ts+(o)+3])
                      fprintf(stderr, "    taskset@0x%08X +0xda8 slot-bitmap: %08X %08X %08X %08X | +0x00:%08X +0x08:%08X\n",
                          ts, _RT(0xda8), _RT(0xdac), _RT(0xdb0), _RT(0xdb4), _RT(0x00), _RT(0x08));
                      #undef _RT
                  }
                  #undef _RW
              }
            }
            /* YDKJ_FORCEATTR (test): the create-task nid 0x87630976 (libsre 0x300158C4)
             * STATs because the task-attr struct(r3)+0xC != 0xFF -- the game runs create
             * before the attr-init (0x22AAB31D) that stamps it (a game-side vtable ordering
             * mis-lift). Force +0xC=0xFF here to test whether the task then creates + the
             * workload attaches + the cri decode runs (papering the ordering to probe the
             * NEXT gate). */
            if (nid==0x87630976u && getenv("YDKJ_FORCEATTR")) {
                extern uint8_t* vm_base; uint32_t s3=(uint32_t)ctx->gpr[3];
                if (vm_base[s3+0xC]!=0xFF) { vm_base[s3+0xC]=0xFF;
                    static int _f=0; if(_f++<4) fprintf(stderr,"[FORCEATTR] forced struct 0x%08X +0xC=0xFF before create-task\n",s3); }
            }
            /* YDKJ_GUESTINIT: the create-task (0x87630976) runs BEFORE the game's own
             * init of its task descriptor (r3), so descriptor+0xC != 0xFF -> STAT. Eagerly
             * invoke the REAL init (0x22AAB31D) on the descriptor first via the guest-caller
             * (ppu_guest_call runs it in a scratch ctx, so the create's args survive). This
             * does the FULL init incl. the task-slot alloc syscall -- unlike the raw +0xC
             * force (FORCEATTR) that hung -- then create proceeds against a valid descriptor. */
            if (nid==0x87630976u && getenv("YDKJ_GUESTINIT")) {
                extern uint8_t* vm_base; uint32_t desc=(uint32_t)ctx->gpr[3];
                /* YDKJ_GUESTINIT_CRI: scope the eager-init to the CRI/movie create only
                 * (taskset r4=0x40131000); leave the audio create (r4=0x4000C900) to its
                 * tolerated STAT so the legal screen still renders while we test the movie. */
                uint32_t _ts=(uint32_t)ctx->gpr[4];
                if (getenv("YDKJ_GUESTINIT_CRI") && _ts!=0x40131000u) { /* skip audio */ }
                else if (vm_base[desc+0xC]!=0xFF) {
                    uint32_t iopd = prx_resolve_export(0x22AAB31Du);
                    static int _g=0; if(_g++<6) fprintf(stderr,"[GUESTINIT] pre-init descriptor 0x%08X via 0x22AAB31D (opd=0x%08X) before create-task\n", desc, iopd);
                    if (iopd) { ppu_guest_call(iopd, desc, 1, 8, 0x398, 0, 0, 0, 0);
                        if(_g<=6) fprintf(stderr,"[GUESTINIT] after pre-init: descriptor 0x%08X +0xC=0x%02X\n", desc, vm_base[desc+0xC]); }
                }
            }
            /* Arm a page-guard on the CellSpurs struct at cellSpursInitializeWithAttribute2
             * ENTRY (nid 0xAA6269A8, r3=&spurs) — before the init writes it — to catch
             * where its struct stores actually land. Env YDKJ_GUARD_INST. */
            if (nid == 0xAA6269A8u && getenv("YDKJ_GUARD_INST")) {
                extern void ppu_guard_page(uint32_t);
                fprintf(stderr, "[GUARD] arming on CellSpurs &spurs=0x%08X at init entry\n", (uint32_t)ctx->gpr[3]);
                ppu_guard_page((uint32_t)ctx->gpr[3]);
            }
            /* CRI_ATTR: the real task-add libsre_func_30012310 (nid 0x1D46FEDF) STATs
             * 0x80410902 unless *(u32*)(r5+0)==1 -- r5 is the task-attribute struct the
             * game builds on the stack. Dump it to see how the game left it. */
            if (nid==0x1D46FEDFu && getenv("YDKJ_SPURSTRACE")) {
                extern uint8_t* vm_base; uint32_t a5=(uint32_t)ctx->gpr[5], a4=(uint32_t)ctx->gpr[4];
                #define _RD(b,o) ((vm_base[(b)+(o)]<<24)|(vm_base[(b)+(o)+1]<<16)|(vm_base[(b)+(o)+2]<<8)|vm_base[(b)+(o)+3])
                static int _a=0; if(_a++<2) fprintf(stderr,
                    "[CRI_ATTR] task-add r3(taskset)=0x%08X r4=0x%08X r5(attr)=0x%08X r6(elf)=0x%08X\n"
                    "           attr[0..0x1C]= %08X %08X %08X %08X %08X %08X %08X %08X  (attr[0] must be 1)\n",
                    (uint32_t)ctx->gpr[3],a4,a5,(uint32_t)ctx->gpr[6],
                    _RD(a5,0),_RD(a5,4),_RD(a5,8),_RD(a5,0xC),_RD(a5,0x10),_RD(a5,0x14),_RD(a5,0x18),_RD(a5,0x1C));
                #undef _RD
            }
            uint32_t _cap_ts = (nid==0x87630976u) ? (uint32_t)ctx->gpr[4] : 0; /* taskset EA before the call clobbers r4 */
            ctx->gpr[2] = toc;            /* libsre's own TOC */
            ctx->ctr    = code;
            /* FLOW_REENTRY_ISO: the libsre fn re-enters lifted guest code at the PPU
             * caller's r1; if it (or a frameless mid-entry inside it) runs at an
             * overlapping frame, its stores clobber the caller's live frame (the
             * deserializer this-ptr corruption). Drop r1 to a fresh guard slice so the
             * re-entered frame can never overlap the caller. Args are in regs (ABI);
             * results go via r3 / guest pointers, so isolating the frame is safe. */
            /* Default ON: without isolation the libsre fn runs at the caller's r1 and
             * its prologue `std r2,40(r1)` clobbers the caller's reserved TOC slot at
             * 0x28(r1) -> caller reloads r2=0 -> null TOC -> func_002B03AC spin -> no
             * draws. Isolating r1 to a fresh slice keeps the caller frame intact.
             * Opt out with YDKJ_NO_REENTRY_ISO=1. */
            { static int _ri=-1; if(_ri<0){ const char* e=getenv("YDKJ_NO_REENTRY_ISO"); _ri = (e && e[0]=='1') ? 0 : 1; }
              if(_ri){ uint64_t _sr1=ctx->gpr[1]; ctx->gpr[1]=(ctx->gpr[1]-0x1000)&~0xFull;
                ps3_indirect_call(ctx); ctx->gpr[1]=_sr1; }
              else ps3_indirect_call(ctx); }       /* -> registered lifted libsre fn; r3=ret */
            { static int64_t st=-2; if(st==-2){const char*e=getenv("YDKJ_SPURSTRACE"); st=e?1:0;}
              if (st) fprintf(stderr, "[SPURSTRACE] nid=0x%08X RETURNED r3=0x%08X\n",
                  nid, (uint32_t)ctx->gpr[3]);
              /* After a create-task (0x87630976), scan the taskset TaskInfo array to see
               * WHICH taskId the real libsre create actually populated (elf!=0). Our
               * CRI_INTERP dispatch hardcodes task 0; if create used a different slot we'd
               * read task 0's empty elf=0. _cap_ts was the r4 taskset before the call. */
              /* YDKJ_CRI_CREATE_OK: the cri caller func_00331DA4 checks create's return
               * and BAILS on the STAT (0x8041090F) -> it never calls the ELF-carrying init
               * (0x22AAB31D, r6=0x004F5F80 = movie image 22) that actually sets up the task.
               * Force ONLY create's return value to success -- leave the descriptor pristine
               * (unlike FORCEATTR/GUESTINIT, which mutate +0xC and make the real ELF-init
               * STAT as already-init) -- so the game proceeds to the ELF-init on a clean
               * descriptor. Scoped to the cri taskset (0x40131000). */
              if (nid==0x87630976u && _cap_ts==0x40131000u && (uint32_t)ctx->gpr[3]==0x8041090Fu
                  && getenv("YDKJ_CRI_CREATE_OK")) {
                  ctx->gpr[3]=0; static int _co=0;
                  if(_co++<2) fprintf(stderr,"[CRI_CREATE_OK] forced cri create return 0 (descriptor pristine) so ELF-init can run\n");
              }
              extern uint64_t vm_read64(uint64_t a);
              if (st && nid==0x87630976u && _cap_ts) {
                  fprintf(stderr,"[TSDUMP] taskset 0x%08X after create (r3=0x%08X): ready=%08X.%08X run=%08X.%08X\n",
                      _cap_ts,(uint32_t)ctx->gpr[3],
                      vm_read32(_cap_ts+0x10),vm_read32(_cap_ts+0x14),
                      vm_read32(_cap_ts+0x00),vm_read32(_cap_ts+0x04));
                  for (uint32_t t=0;t<16;t++){ uint32_t ti=_cap_ts+0x80+t*48;
                      uint64_t elf=vm_read64(ti+0x10);
                      if (elf) fprintf(stderr,"[TSDUMP]   task %u: elf=0x%llX args=%08X %08X %08X %08X ctx=0x%llX\n",
                          t,(unsigned long long)elf,vm_read32(ti+0),vm_read32(ti+4),vm_read32(ti+8),vm_read32(ti+12),
                          (unsigned long long)vm_read64(ti+0x18)); }
              } }
            /* cellSpursInitialize (0xACFC8DBC) just returned -> the CellSpurs
             * instance is now populated; spawn the lifted SPURS SPU kernel against
             * it (flОw libsre never calls group_start; spawning during init reads
             * an empty context and recurses to a stack overflow). Weak so a build
             * without flow_spurs_kernel.c still links. */
            if (nid == 0xACFC8DBCu) {
                extern void flow_spurs_kernel_spawn_postinit(void) __attribute__((weak));
                if (getenv("FLOW_SPURSKERNEL") && flow_spurs_kernel_spawn_postinit)
                    flow_spurs_kernel_spawn_postinit();
            }
            return;
        }
    }

    for (uint32_t i = 0; i < g_ctx_count; i++)
        if (g_ctx[i].nid == nid) { g_ctx[i].fn(ctx); return; }

    ps3_nid_entry* e = g_hle_inited ? ps3_nid_table_find(&g_hle_nids, nid) : nullptr;
    if (!e || !e->handler) {
        /* YDKJ_TUNERFIX: sysPrxForUser 0xE0998DBF is the profiler-presence query called
         * by libsre _cellSpursIsLaunchedFromTuner (0x3000D318). On a normal (non-tuner)
         * run it must return 0x8001112E ("profiler not loaded"); an unresolved-NID error
         * instead trips the usertrace.c:123 assert AND makes the fn return "launched=1",
         * so the caller does tuner/trace setup that fails with CELL_SPURS_CORE_ERROR_STAT
         * -> the SPURS task workload never attaches -> no movie decode. Return not-loaded. */
        if (nid == 0xE0998DBFu && getenv("YDKJ_TUNERFIX")) {
            ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)0x8001112E;
            static int _tn = 0; if (_tn++ < 3)
                fprintf(stderr, "[TUNERFIX] sysPrxForUser 0xE0998DBF -> 0x8001112E (profiler not loaded)\n");
            return;
        }
        /* cellSaveData call-shape capture: dump r3-r10 + resolve r7/r8 as OPD
         * callbacks (statCallback/fileCallback) to learn the cellSaveDataAutoLoad2
         * protocol so it can be implemented. */
        /* YDKJ_SAVEDATA: minimal cellSaveDataAutoLoad2 (NID 0xCDC6AEFD). The game
         * auto-loads its "BLUS30569-AUTO-" save; unimplemented = null completion =
         * state never transitions. Implement the no-save/new-user path: invoke the
         * game's statCallback (r8 OPD) with a CellSaveDataStatGet{isNewData=1}, honor
         * its result, return CELL_SAVEDATA_RET_OK(0). Legit HLE (real first-run behavior). */
        if (nid==0xCDC6AEFDu) { static int _sdi=-1; if(_sdi<0)_sdi=getenv("YDKJ_SAVEDATA")?1:0;
          if(_sdi){ static int _once=0; if(_once++<2){
            uint32_t statCb=(uint32_t)ctx->gpr[8];               /* statCallback OPD */
            uint32_t SC=0x02000000u, SG=0x02000100u, SS=0x02000900u; /* scratch structs */
            for(uint32_t a=0x02000000u;a<0x02001000u;a+=4) vm_write32(a,0);
            /* CellSaveDataStatGet: hddFreeSizeKB@0, isNewData@4, dir@8(dirStat), getParam@0x40 */
            vm_write32(SG+0x00, 0x100000);   /* hddFreeSizeKB */
            vm_write32(SG+0x04, 1);          /* isNewData = 1 (no existing save) */
            /* copy dirName ("BLUS30569-AUTO-") into dir.dirName @ 0x08+0x18=0x20 */
            { uint32_t src=(uint32_t)ctx->gpr[5]; for(int i=0;i<31;i++){ uint32_t w=vm_read32((src+i)&~3u); uint8_t b=(w>>((3-((src+i)&3))*8))&0xFF; vm_write32((SG+0x20+i)&~3u, (vm_read32((SG+0x20+i)&~3u) & ~(0xFFu<<((3-((SG+0x20+i)&3))*8))) | ((uint32_t)b<<((3-((SG+0x20+i)&3))*8))); if(!b)break; } }
            fprintf(stderr,"[SAVEDATA-HLE] cellSaveDataAutoLoad2: calling game statCallback OPD=0x%08X (isNewData=1)\n",statCb);
            uint64_t r=ppu_guest_call(statCb, SC, SG, SS, 0, 0, 0, 0, 0);
            int32_t cbres=(int32_t)vm_read32(SC+0x00);
            fprintf(stderr,"[SAVEDATA-HLE] statCallback returned r3=0x%llX cbResult->result=%d -> returning OK\n",(unsigned long long)r,cbres);
            ctx->gpr[3]=0; /* CELL_SAVEDATA_RET_OK */
            return;
          } } }
        if (nid==0xCDC6AEFDu || nid==0x27CB8BC2u) { static int _sd=0; if(_sd++<3){
            extern uint32_t vm_read32(uint64_t);
            uint32_t r7=(uint32_t)ctx->gpr[7], r8=(uint32_t)ctx->gpr[8];
            uint32_t r7c=r7?vm_read32(r7):0, r8c=r8?vm_read32(r8):0;
            fprintf(stderr,"[SAVEDATA] nid=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7(statCb OPD)=0x%08X->code=func_%08X r8(fileCb OPD)=0x%08X->code=func_%08X r9=0x%08X r10=0x%08X\n",
                nid,(uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],(uint32_t)ctx->gpr[6],r7,r7c,r8,r8c,(uint32_t)ctx->gpr[9],(uint32_t)ctx->gpr[10]);
            /* if r4 is a dirName string, dump it */
            uint32_t r4=(uint32_t)ctx->gpr[4]; if(r4>0x10000&&r4<0x10000000){ char nm[32]; for(int i=0;i<31;i++){uint32_t b=vm_read32((r4+i)&~3u); nm[i]=(char)((b>>((3-((r4+i)&3))*8))&0xFF); if(!nm[i])break;} nm[31]=0; fprintf(stderr,"[SAVEDATA]   r4 dirName='%s'\n",nm); } }
        }
        /* YDKJ_NETOFFLINE: unresolved network NIDs (sys_net/cellHttp/cellSysutil)
         * default r3=0 (fake success) -> the game stores a NULL handle as an object
         * and later virtual-calls it -> the 0xC708C708 null-vtable crash. Return a
         * negative error instead so the game's online-content check takes its
         * graceful OFFLINE-FAILURE path and proceeds with the local Persistent.zip. */
        { static int _no=-1; if(_no<0)_no=getenv("YDKJ_NETOFFLINE")?1:0;
          if(_no){ switch(nid){
            case 0x139A9E9Bu: case 0x9FB6228Eu: case 0x05893E7Cu:
            case 0x52AAC4FAu: case 0x9638F766u: case 0x522180BCu:
              ctx->gpr[3] = 0x80010002u; /* generic failure (net unavailable) */
              return;
          } } }
        ps3_hle_unresolved(nid, ctx);
        return;
    }
    g_last_hle_name = e->name;
    if (nid == 0xD0B1D189u /*cellGcmSetTile*/ || nid == 0xDC09357Eu /*SetDisplayBuffer*/) {
        static int _g=0; if (_g++ < 8)
            fprintf(stderr, "[hle-trace] %s lr=0x%08X cia=0x%08X r3..r8=%08X %08X %08X %08X %08X %08X\n",
                    e->name, (uint32_t)ctx->lr, (uint32_t)ctx->cia,
                    (uint32_t)ctx->gpr[3],(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],
                    (uint32_t)ctx->gpr[6],(uint32_t)ctx->gpr[7],(uint32_t)ctx->gpr[8]);
    }
    hle_generic fn = (hle_generic)e->handler;
    uint64_t r = fn(ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6],
                    ctx->gpr[7], ctx->gpr[8], ctx->gpr[9], ctx->gpr[10]);
    ctx->gpr[3] = r;   /* PPC return value */
    /* PTR-LEAK detector (FLOW_PTRLEAK): the game stores HLE return values as
     * guest pointers; if an HLE returns a value out of guest RAM (>= 0x10000000)
     * that isn't a CELL_ERROR code (0x8001xxxx), it's leaking a truncated HOST
     * pointer -> becomes an unresolved guest call later (e.g. 0xC708C708). */
    { static int _pl=-1; if(_pl<0)_pl=getenv("FLOW_PTRLEAK")?1:0;
      if(_pl){ uint32_t rv=(uint32_t)r;
        if(rv>=0x10000000u && (rv & 0xFFFF0000u)!=0x80010000u && (rv & 0xFFFF0000u)!=0x80020000u){
          static int _n=0; if(_n++<40) fprintf(stderr,"[PTRLEAK] NID 0x%08X %s returned 0x%08X (out-of-guest-RAM host ptr?)\n",
            nid, e->name?e->name:"?", rv); } } }
}

/* Populated by the generated registration unit (gen_hle_nids.py). Weak so a
 * build without it still links (no HLE registered -> imports log + return 0). */
extern "C" void ppu_hle_register_all(void) __attribute__((weak));
extern "C" void ppu_hle_register_all(void) {}

extern "C" void ppu_hle_init(void) { ppu_hle_register_all(); }
