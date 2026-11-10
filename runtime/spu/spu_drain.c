/* SPU_DRAIN trampoline model -- runtime state + stub hooks (faithful-adopt).
 *
 * Milestone 1 lands the execution-model plumbing; the per-transfer hooks
 * (lockstep gate, flight recorder, SPURS task-launch) are inert stubs here and
 * gain real bodies in their own milestones. Keeping them as real out-of-line
 * functions (not macros) means the SPU_DRAIN/SPU_RET call sites are already in
 * place -- later milestones only replace the bodies. */
#include "spu_context.h"
#include <stdlib.h>
#include <stdio.h>

/* Tokyo Jungle: once the late Survival event-flag waiter is armed, retain a
 * small image-13-only transfer trail.  This runs at trampoline transfers, not
 * in the interpreter's execution loop, and never mutates the guest context. */
extern volatile uint32_t g_spu_event_flag_watch_ea;

static int tj_late_job_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TJ_LATE_JOB_TRACE") ? 1 : 0;
    return enabled && g_spu_event_flag_watch_ea != 0;
}

#ifdef _WIN32
#include <Windows.h>
static volatile LONG s_tj_image13_trace_lines = 0;

/* Bounded image-17 checkpoints: 0=menu, 1=Survival IO mapping, 2=image18
 * return. Separate phase budgets retain pre-handoff geometry evidence.
 * Reuse existing transfer hooks; never bypass guest checks or change state. */
volatile LONG g_tj_geometry_trace_armed = 0;
void tj_spu_geometry_trace_arm_scene(void)
{
    if (getenv("TJ_GEOMETRY_STAGE_TRACE") &&
        InterlockedCompareExchange(&g_tj_geometry_trace_armed, 1, 0) == 0)
        fprintf(stderr, "[TJ-GEOMETRY-PHASE] phase=1 source=survival_io_mapping\n");
}
static const u32 s_tj_geometry_trace_pcs[] = {
    0xB0, 0x1D4, 0x288, 0x2E4, 0x340, 0x3AC,
    0x4A50, 0x540C, 0x4E8, 0x55C, 0x5D8, 0x1D48, 0x520, 0x9220
};
static volatile LONG s_tj_geometry_trace_counts[3][
    sizeof(s_tj_geometry_trace_pcs) / sizeof(s_tj_geometry_trace_pcs[0])];
unsigned tj_spu_geometry_packing_checkpoint_count(void)
{
    for (unsigned i = 0; i < sizeof(s_tj_geometry_trace_pcs) / sizeof(s_tj_geometry_trace_pcs[0]); ++i)
        if (s_tj_geometry_trace_pcs[i] == 0x5D8)
            return (unsigned)InterlockedCompareExchange(&s_tj_geometry_trace_counts[2][i], 0, 0);
    return 0;
}
static void tj_geometry_stage_trace(const spu_context* ctx)
{
    if (ctx->image_id != 17) return;
    LONG phase = InterlockedCompareExchange(&g_tj_geometry_trace_armed, 0, 0);
    if (phase < 1 || phase > 2) return;
    const unsigned count = sizeof(s_tj_geometry_trace_pcs) / sizeof(s_tj_geometry_trace_pcs[0]);
    const u32 pc = (u32)ctx->pc & SPU_LS_MASK;
    unsigned slot;
    for (slot = 0; slot < count && s_tj_geometry_trace_pcs[slot] != pc; ++slot) {}
    if (slot == count) return;
    LONG hit = InterlockedIncrement(&s_tj_geometry_trace_counts[phase][slot]);
    const int packing = pc == 0x55C || pc == 0x5D8;
    /* The first target draw can trail several producer jobs. The original
     * four-hit budget could not establish which output it consumed. */
    if (phase == 2 && packing) {
        static LONG after = -1;
        if (after < 0) {
            const char* value = getenv("TJ_GEOMETRY_CAPTURE_AFTER_PACKS");
            after = value ? (LONG)strtoul(value, NULL, 10) : 0;
            if (after < 0 || after > 1000000) after = 0;
        }
        /* Retain producer look-behind for draws already queued when the
         * indexed capture opens. This changes diagnostics only, not jobs,
         * allocator selection or their completion counter. */
        const LONG start = after > 16 ? after - 16 : 0;
        const LONG limit = after ? 64 : 32;
        if (hit <= start || hit > start + limit) return;
    } else if (hit > 4) return;
    fprintf(stderr, "[TJ-GEOMETRY] pc=%05X phase=%ld hit=%ld tid=%lu r3=%08X r4=%08X "
        "r5=%08X r6=%08X r7=%08X r80=%08X r81=%08X r82=%08X r88=%08X\n",
        pc, phase, hit, GetCurrentThreadId(), ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
        ctx->gpr[5]._u32[0], ctx->gpr[6]._u32[0], ctx->gpr[7]._u32[0],
        ctx->gpr[80]._u32[0], ctx->gpr[81]._u32[0], ctx->gpr[82]._u32[0],
        ctx->gpr[88]._u32[0]);
    if (packing || ((pc == 0x4A50 || pc == 0xB0 || pc == 0x3AC || pc == 0x9220) && hit == 1)) {
        const char* dir = getenv("TJ_GEOMETRY_STAGE_TRACE");
        if (dir && *dir) {
            char path[1024];
            int n = packing
                ? snprintf(path, sizeof(path), "%s\\geometry-%s-phase%ld-hit%ld.bin", dir,
                    pc == 0x55C ? "before-pack" : "after-pack", phase, hit)
                : snprintf(path, sizeof(path), "%s\\geometry-%s-phase%ld-input.bin", dir,
                pc == 0x4A50 ? "cull" : pc == 0x3AC ? "decode" :
                pc == 0x9220 ? "transform" : "entry", phase);
            FILE* f = n > 0 && n < sizeof(path) ? fopen(path, "wb") : NULL;
            int ok = f && fwrite(ctx->gpr, sizeof(ctx->gpr), 1, f) == 1 &&
                fwrite(ctx->ls, SPU_LS_SIZE, 1, f) == 1;
            if (f && fclose(f) != 0) ok = 0;
            fprintf(stderr, "[TJ-GEOMETRY] input_snapshot=%s path=%s\n",
                ok ? "saved" : "failed", path);
            if (pc == 0x5D8) {
                /* r7 retains the output start; the allocator cursor in r83
                 * has advanced past the writes. Capture the actual output
                 * before this job publishes DRAW
                 * commands. Separate hits identify reuse across render passes;
                 * no allocator result or synchronization is manufactured. */
                extern uint8_t* vm_base;
                extern u32 ppu_vm_size;
                const u128 allocation = spu_ls_read128(ctx, ctx->gpr[83]._u32[0]);
                const u32 begin = ctx->gpr[7]._u32[0];
                const u32 end = allocation._u32[3];
                const u64 cap = ppu_vm_size ? (u64)ppu_vm_size : (1ULL << 32);
                if (vm_base && begin >= 0x10000u && end > begin &&
                    (u64)end <= cap && end - begin <= SPU_LS_SIZE) {
                    n = snprintf(path, sizeof(path),
                        "%s\\geometry-output-phase%ld-hit%ld-ea%08X.bin", dir, phase, hit, begin);
                    f = n > 0 && n < sizeof(path) ? fopen(path, "wb") : NULL;
                    ok = f && fwrite(vm_base + begin, end - begin, 1, f) == 1;
                    if (f && fclose(f) != 0) ok = 0;
                    fprintf(stderr, "[TJ-GEOMETRY] output_snapshot=%s phase=%ld hit=%ld "
                        "allocation=%08X/%08X/%08X/%08X path=%s\n",
                        ok ? "saved" : "failed", phase, hit, begin, allocation._u32[1],
                        allocation._u32[2], end, path);
                } else {
                    fprintf(stderr, "[TJ-GEOMETRY] output_snapshot=invalid_span phase=%ld hit=%ld "
                        "allocation=%08X/%08X/%08X/%08X\n", phase, hit, begin,
                        allocation._u32[1], allocation._u32[2], end);
                }
            }
        }
    }
}

/* Capture the actual animation inputs at its main processing boundaries.
 * Same opt-in as geometry; first hit per stage/phase only, no guest writes. */
void tj_animation_stage_trace_at(const spu_context* ctx, u32 pc)
{
    if (ctx->image_id != 16) return;
    LONG phase = InterlockedCompareExchange(&g_tj_geometry_trace_armed, 0, 0);
    if (phase < 1 || phase > 2) return;
    static const u32 pcs[] = {0x180, 0x2AC, 0x2D30, 0x13C8, 0x580, 0x1850, 0xFF8, 0xC80};
    static volatile LONG counts[3][sizeof(pcs) / sizeof(pcs[0])];
    unsigned slot;
    for (slot = 0; slot < sizeof(pcs) / sizeof(pcs[0]) && pcs[slot] != pc; ++slot) {}
    if (slot == sizeof(pcs) / sizeof(pcs[0]) ||
        InterlockedIncrement(&counts[phase][slot]) != 1) return;
    const char* dir = getenv("TJ_GEOMETRY_STAGE_TRACE");
    if (!dir || !*dir) return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s\\animation-%05X-phase%ld-input.bin", dir, pc, phase);
    FILE* f = n > 0 && n < sizeof(path) ? fopen(path, "wb") : NULL;
    int ok = f && fwrite(ctx->gpr, sizeof(ctx->gpr), 1, f) == 1 &&
        fwrite(ctx->ls, SPU_LS_SIZE, 1, f) == 1;
    if (f && fclose(f) != 0) ok = 0;
    fprintf(stderr, "[TJ-ANIMATION] pc=%05X phase=%ld r3=%08X r4=%08X "
        "r5=%08X r6=%08X r7=%08X input_snapshot=%s path=%s\n", pc, phase,
        ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0], ctx->gpr[5]._u32[0],
        ctx->gpr[6]._u32[0], ctx->gpr[7]._u32[0], ok ? "saved" : "failed", path);
}

/* Opt-in image transfer histogram.  This reuses the existing per-transfer
 * task-hook site so normal builds gain only one cached image-id comparison.
 * TJ_SPU_PC_HIST=<image id>; image 17 reports every 160 completed jobs. */
static volatile LONG s_tj_pc_hist_image = -2;
static volatile LONG64 s_tj_pc_hist_total;
static volatile LONG64 s_tj_pc_hist_jobs;
static volatile LONG64 s_tj_pc_hist[SPU_LS_SIZE / 4];

static volatile LONG s_tj_pc_time_image = -2;
static LARGE_INTEGER s_tj_pc_time_frequency;
static volatile LONG64 s_tj_pc_time_jobs;
static volatile LONG64 s_tj_pc_time_calls[SPU_LS_SIZE / 4];
static volatile LONG64 s_tj_pc_time_ticks[SPU_LS_SIZE / 4];
static __declspec(thread) int s_tj_pc_time_active;
static __declspec(thread) uint64_t s_tj_pc_time_last_qpc;
static __declspec(thread) unsigned s_tj_pc_time_last_index;

static LONG tj_pc_hist_image(void)
{
    LONG image = s_tj_pc_hist_image;
    if (image != -2) return image;
    const char* value = getenv("TJ_SPU_PC_HIST");
    LONG configured = value && *value ? (LONG)strtol(value, NULL, 0) : -1;
    InterlockedCompareExchange(&s_tj_pc_hist_image, configured, -2);
    return s_tj_pc_hist_image;
}

static void tj_pc_hist_transfer(spu_context* ctx)
{
    if (ctx->image_id != tj_pc_hist_image()) return;
    unsigned index = (ctx->pc & SPU_LS_MASK) >> 2;
    InterlockedIncrement64(&s_tj_pc_hist[index]);
    InterlockedIncrement64(&s_tj_pc_hist_total);
}

static LONG tj_pc_time_image(void)
{
    LONG image = s_tj_pc_time_image;
    if (image != -2) return image;
    const char* value = getenv("TJ_SPU_PC_TIME");
    LONG configured = value && *value ? (LONG)strtol(value, NULL, 0) : -1;
    LARGE_INTEGER frequency;
    if (configured >= 0 && !QueryPerformanceFrequency(&frequency))
        configured = -1;
    if (configured >= 0) s_tj_pc_time_frequency = frequency;
    InterlockedCompareExchange(&s_tj_pc_time_image, configured, -2);
    return s_tj_pc_time_image;
}

static uint64_t tj_pc_time_qpc(void)
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static uint64_t tj_pc_time_ticks_to_ns(uint64_t ticks)
{
    uint64_t frequency = (uint64_t)s_tj_pc_time_frequency.QuadPart;
    if (!frequency) return 0;
    return (ticks / frequency) * 1000000000ull +
           ((ticks % frequency) * 1000000000ull) / frequency;
}

static void tj_pc_time_charge(uint64_t now)
{
    if (!s_tj_pc_time_last_qpc) return;
    uint64_t ticks = now - s_tj_pc_time_last_qpc;
    InterlockedIncrement64(&s_tj_pc_time_calls[s_tj_pc_time_last_index]);
    InterlockedAdd64(&s_tj_pc_time_ticks[s_tj_pc_time_last_index],
                     (LONG64)ticks);
}

void tj_spu_pc_time_job_begin(int image_id)
{
    s_tj_pc_time_active = image_id == tj_pc_time_image();
    s_tj_pc_time_last_qpc = 0;
    s_tj_pc_time_last_index = 0;
}

static void tj_pc_time_transfer(spu_context* ctx)
{
    if (!s_tj_pc_time_active) return;
    uint64_t now = tj_pc_time_qpc();
    tj_pc_time_charge(now);
    s_tj_pc_time_last_qpc = now;
    s_tj_pc_time_last_index = (ctx->pc & SPU_LS_MASK) >> 2;
}

void tj_spu_pc_time_job_complete(int image_id)
{
    if (!s_tj_pc_time_active) return;
    tj_pc_time_charge(tj_pc_time_qpc());
    s_tj_pc_time_last_qpc = 0;
    s_tj_pc_time_active = 0;

    LONG64 jobs = InterlockedIncrement64(&s_tj_pc_time_jobs);
    if ((jobs % 160) != 0) return;

    struct tj_pc_time_rank {
        unsigned index;
        LONG64 calls;
        LONG64 ticks;
    } top[16] = {0};
    for (unsigned index = 0; index < SPU_LS_SIZE / 4; ++index) {
        LONG64 ticks = InterlockedCompareExchange64(
            &s_tj_pc_time_ticks[index], 0, 0);
        if (ticks <= top[15].ticks) continue;
        LONG64 calls = InterlockedCompareExchange64(
            &s_tj_pc_time_calls[index], 0, 0);
        unsigned at = 15;
        while (at && ticks > top[at - 1].ticks) {
            top[at] = top[at - 1];
            --at;
        }
        top[at].index = index;
        top[at].calls = calls;
        top[at].ticks = ticks;
    }

    fprintf(stderr, "[TJ-SPU-PC-TIME] image=%d jobs=%lld top=",
            image_id, (long long)jobs);
    for (unsigned i = 0; i < 16 && top[i].ticks; ++i) {
        uint64_t total_ns = tj_pc_time_ticks_to_ns((uint64_t)top[i].ticks);
        uint64_t avg_ns = top[i].calls
            ? total_ns / (uint64_t)top[i].calls : 0;
        fprintf(stderr, "%s%05X:c%lld:t%lluus:a%lluns",
                i ? "," : "", top[i].index << 2,
                (long long)top[i].calls,
                (unsigned long long)(total_ns / 1000ull),
                (unsigned long long)avg_ns);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

void tj_spu_pc_hist_job_complete(int image_id)
{
    if (image_id != tj_pc_hist_image()) return;
    LONG64 jobs = InterlockedIncrement64(&s_tj_pc_hist_jobs);
    if ((jobs % 160) != 0) return;

    LONG64 total = InterlockedCompareExchange64(&s_tj_pc_hist_total, 0, 0);
    struct tj_pc_rank { unsigned index; LONG64 count; } top[16] = {0};
    for (unsigned index = 0; index < SPU_LS_SIZE / 4; ++index) {
        LONG64 count = InterlockedCompareExchange64(
            &s_tj_pc_hist[index], 0, 0);
        if (count <= top[15].count) continue;
        unsigned at = 15;
        while (at && count > top[at - 1].count) {
            top[at] = top[at - 1];
            --at;
        }
        top[at].index = index;
        top[at].count = count;
    }
    fprintf(stderr,
            "[TJ-SPU-PC-HIST] image=%d jobs=%lld transfers=%lld per_job=%.2f top=",
            image_id, (long long)jobs, (long long)total,
            jobs ? (double)total / (double)jobs : 0.0);
    for (unsigned i = 0; i < 16 && top[i].count; ++i)
        fprintf(stderr, "%s%05X:%lld", i ? "," : "",
                top[i].index << 2, (long long)top[i].count);
    fputc('\n', stderr);
    fflush(stderr);
}

static int tj_image13_trace_reserve_line(void)
{
    for (;;) {
        LONG used = InterlockedCompareExchange(&s_tj_image13_trace_lines, 0, 0);
        if (used >= 64) return 0;
        if (InterlockedCompareExchange(&s_tj_image13_trace_lines,
                                       used + 1, used) == used)
            return 1;
    }
}

static unsigned long tj_image13_trace_thread_id(void)
{
    return (unsigned long)GetCurrentThreadId();
}
#else
static unsigned s_tj_image13_trace_lines;

static int tj_image13_trace_reserve_line(void)
{
    return ++s_tj_image13_trace_lines <= 64;
}

static unsigned long tj_image13_trace_thread_id(void)
{
    return 0;
}
#endif

static void tj_late_image13_transfer_trace(spu_context* ctx)
{
    enum { TJ_TRAIL = 16 };
    static SPU_THREAD_LOCAL unsigned long long transfer_count;
    static SPU_THREAD_LOCAL unsigned same_pc_count;
    static SPU_THREAD_LOCAL uint32_t last_pc;
    static SPU_THREAD_LOCAL uint32_t pc_trail[TJ_TRAIL];

    if (!tj_late_job_trace_enabled() || ctx->image_id != 13) return;

    uint32_t pc = ctx->pc & SPU_LS_MASK;
    uint32_t link = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    unsigned long long n = ++transfer_count;
    pc_trail[(unsigned)(n - 1) % TJ_TRAIL] = pc;
    same_pc_count = (n > 1 && pc == last_pc) ? same_pc_count + 1 : 1;
    last_pc = pc;

    /* First 24 transfers establish the tail. Later output is a concise proof
     * of no progress rather than a per-transfer flood.  The shared cap is 64
     * lines across all image-13 host threads. */
    if (n <= 24 && tj_image13_trace_reserve_line()) {
        fprintf(stderr,
                "[TJ-LATE-IMG13] tid=%lu phase=TRANSFER n=%llu pc=0x%05X lr=0x%05X\n",
                tj_image13_trace_thread_id(), n, pc, link);
        fflush(stderr);
    }
    if ((same_pc_count == 64 || n == 4096 || n == 65536) &&
        tj_image13_trace_reserve_line()) {
        fprintf(stderr,
                "[TJ-LATE-IMG13] tid=%lu phase=PROGRESS n=%llu pc=0x%05X lr=0x%05X same_pc=%u tail=",
                tj_image13_trace_thread_id(), n, pc, link, same_pc_count);
        unsigned start = n < TJ_TRAIL ? 0 : (unsigned)n % TJ_TRAIL;
        unsigned count = n < TJ_TRAIL ? (unsigned)n : TJ_TRAIL;
        for (unsigned i = 0; i < count; i++)
            fprintf(stderr, "%s%05X", i ? "," : "", pc_trail[(start + i) % TJ_TRAIL]);
        fputc('\n', stderr);
        fflush(stderr);
    }
}

/* Pending cross-function transfer target for this host thread's SPU context. */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*) = 0;

/* yz_lockstep_tick now has its real body in spu_lockstep.c (milestone 2). */

/* SPURS task-launch interception at a trampoline hop (milestone: SPURS kernel).
 * No-op until then. */
SPU_THREAD_LOCAL int g_task9a00_transfer_diagnostics = -1;

/* PM flow trace (SPURS_PM_FLOW=1): record every cross-function transfer of ONE
 * policy-module run (the ctx spurs_policy.c arms) so the post-claim decision
 * path can be reconstructed offline. Written by the drain-loop hook below;
 * armed/dumped by spu_run_policy_module. */
uint32_t          g_pm_flow_buf[8192];
volatile unsigned g_pm_flow_n = 0;
void* volatile    g_pm_flow_ctx = 0;

#include <stdlib.h>
#include <stdio.h>

/* Sync-model ring-drain: the wwsjob PM's job ring drains only when jobs mark
 * their record's "done" bit (0x8000 in the record's first halfword @LS 0xDF0).
 * On hardware the async load-completion INTERRUPT sets it; our loads complete
 * synchronously, so a job whose code never got a ring slot stays "pending"
 * forever and the ring deadlocks (scan re-bails at LS 0x2318). When we observe
 * that bail spin, the in-flight loads HAVE in fact completed in our model, so
 * set the done bit on the pending records -- the faithful sync equivalent of
 * the completion interrupt. Env LBP_JOBDRAIN (default off while validating). */
void spu_task_launch_check(spu_context* ctx, void* fn)
{
    (void)fn;
    if (ctx->image_id == 15 && g_task9a00_transfer_diagnostics < 0) {
        const char* hist = getenv("TJ_SPU_PC_HIST");
        const char* time = getenv("TJ_SPU_PC_TIME");
        const char* trace_img = getenv("SPU_DRAIN_TRACE_IMG");
        g_task9a00_transfer_diagnostics =
            (hist && *hist) || (time && *time) || (trace_img && *trace_img);
    }
#ifdef _WIN32
    tj_geometry_stage_trace(ctx);
    tj_animation_stage_trace_at(ctx, (u32)ctx->pc & SPU_LS_MASK);
    tj_pc_hist_transfer(ctx);
    tj_pc_time_transfer(ctx);
#endif
    tj_late_image13_transfer_trace(ctx);
    /* Generic lifted-control-flow probe. Unlike SPU_JOBTRACE (which only arms
     * for WWS overlays), this sees direct SPURS jobs such as Tokyo Jungle's
     * image 10. Keep it fully opt-in because this hook is on every trampoline
     * transfer. SPU_DRAIN_TRACE_IMG=<image id> logs the first 4096 transfers,
     * then one sample per million so a stable spin remains observable.
     * SPU_DRAIN_TRACE_PC=<hex LS address> narrows output to one PC without
     * changing the transfer counter. */
    { static int s_trace_img = -2;
      static unsigned s_trace_pc = ~0u;
      static SPU_THREAD_LOCAL unsigned long long s_trace_n = 0;
      static SPU_THREAD_LOCAL unsigned long long s_trace_match_n = 0;
      if (s_trace_img == -2) {
          const char* e = getenv("SPU_DRAIN_TRACE_IMG");
          const char* p = getenv("SPU_DRAIN_TRACE_PC");
          s_trace_img = e ? atoi(e) : -1;
          s_trace_pc = p ? (unsigned)strtoul(p, NULL, 0) : ~0u;
      }
      if (ctx->image_id == s_trace_img) {
          unsigned long long n = ++s_trace_n;
          unsigned pc = ctx->pc & SPU_LS_MASK;
          int pc_match = (s_trace_pc == ~0u || pc == (s_trace_pc & SPU_LS_MASK));
          unsigned long long mn = pc_match ? ++s_trace_match_n : 0;
          if (pc_match && (mn <= 64 || (n % 1000000ull) == 0)) {
              fprintf(stderr,
                      "[spu-drain] n=%llu img=%d pc=0x%05X lr=0x%05X "
                      "status=0x%X events=0x%X&0x%X "
                      "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X "
                      "r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X "
                      "r15=%08X r17=%08X r19=%08X r33=%08X\n",
                      n, ctx->image_id, pc,
                      ctx->gpr[0]._u32[0] & SPU_LS_MASK, ctx->status,
                      ctx->event_status, ctx->event_mask,
                      ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                      ctx->gpr[5]._u32[0], ctx->gpr[6]._u32[0],
                      ctx->gpr[7]._u32[0], ctx->gpr[8]._u32[0],
                      ctx->gpr[9]._u32[0], ctx->gpr[10]._u32[0],
                      ctx->gpr[11]._u32[0], ctx->gpr[12]._u32[0],
                      ctx->gpr[15]._u32[0],
                      ctx->gpr[17]._u32[0], ctx->gpr[19]._u32[0],
                      ctx->gpr[33]._u32[0]);
              if (getenv("SPU_DRAIN_TRACE_FULL")) {
                  const unsigned regs[] = {3, 5, 8, 10, 11, 15, 17, 19, 74, 75, 84, 87};
                  for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
                      unsigned r = regs[i];
                      fprintf(stderr, "    r%-2u=%08X,%08X,%08X,%08X%s",
                              r, ctx->gpr[r]._u32[0], ctx->gpr[r]._u32[1],
                              ctx->gpr[r]._u32[2], ctx->gpr[r]._u32[3],
                              ((i + 1) % 3) ? " " : "\n");
                  }
                  fprintf(stderr, "\n");
              }
          }
      }
    }
    if (g_pm_flow_ctx == (void*)ctx && g_pm_flow_n < 8192)
        g_pm_flow_buf[g_pm_flow_n++] = ctx->pc;

    static int s_on = -1;
    if (s_on < 0) s_on = getenv("LBP_JOBDRAIN") ? 1 : 0;
    if (!s_on || ctx->image_id != 2 || !ctx->policy_mode) return;

    /* 0x2318 = the type-2 load's ring-full bail (resets scan, retries). */
    if ((ctx->pc & 0x3FFFF) != 0x2318) return;

    static unsigned s_bail = 0;
    if (++s_bail < 4096) return;      /* only after a real spin, not one pass */
    s_bail = 0;

    /* Set bit 31 (the 0x8000 first-halfword done flag) on each pending record
     * the release sweep scans (16 slots, 4-byte stride at LS 0xDF0), for any
     * record that is non-empty and not already done. */
    int marked = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t* r = &ctx->ls[0xDF0 + i * 4];
        uint32_t w = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                     ((uint32_t)r[2] << 8) | r[3];
        if (w == 0 || (w & 0x80000000u)) continue;
        w |= 0x80000000u;
        r[0] = (uint8_t)(w >> 24); r[1] = (uint8_t)(w >> 16);
        r[2] = (uint8_t)(w >> 8);  r[3] = (uint8_t)w;
        marked++;
    }
    static int _n = 0;
    if (marked && _n++ < 12)
        fprintf(stderr, "[pm-jobdrain] ring-full spin: marked %d pending records done\n", marked);
}

/* Restore the active image after a lifted call bracket. The brsl/bisl emission
 * saves image_id in a call-site local and hands it back here so an image
 * adopted inside the callee cannot leak into the caller's continuation. A
 * persistent LS-0xA00 workload module (module_img_a00) is re-applied since the
 * plain restore would undo it. */
void spu_img_restore(spu_context* ctx, int32_t saved_img)
{
    ctx->image_id = ctx->module_img_a00 ? ctx->module_img_a00 : saved_img;
}

/* spu_ch_wake now has its real body in spu_channels.c (milestone 3). */

/* --- SPU interrupt dispatch (drain-loop hook) ------------------------------
 * Called when int_enable && (event_status & event_mask): architectural SPU
 * interrupt. srr0 <- the interrupted continuation (ctx->pc already holds the
 * pending transfer target), interrupts disable, and control vectors through
 * the guest-planted branch instruction at LS 0 (the WWS jobmanager's entry
 * writes `bra 0xA2C` there; its handler drives the DMA-list job pipeline and
 * returns via iret). If LS 0 holds no br/bra, the interrupt is NOT taken --
 * we log once and resume normal flow rather than jump into zeros. */
#include <stdio.h>
#include <stdlib.h>

/* ---- Deferred DMA-list stall-and-notify delivery --------------------------
 * Real MFC hardware processes a queued list command (the WWS job manager's
 * barriered null list, MFC_GETLB) ASYNCHRONOUSLY: the stall-and-notify -- and
 * the interrupt it raises -- land after the SPU has run on past the code that
 * armed it. We execute the list synchronously inside `wrch MFC_Cmd`, so the
 * interrupt was delivered before the job manager had stored
 * g_WwsJob_loadJobState = kReadCommands (LS 0x12A0).
 *
 * Its interrupt handler gates the Load->Run advance on BOTH the ch25 stall mask
 * carrying tag 0 (kLoadJob_readCommands) AND loadJobState == kReadCommands. With
 * the state still kNone it correctly declined to advance -- and because
 * MFC_RdListStallStat is "accumulative, clear on read" that single stall was
 * consumed, so no later interrupt could ever advance it: the job never ran and
 * LBP's loading thread deadlocked on a completion that never came.
 *
 * Holding the interrupt off for N drain ticks was tried and DISPROVEN: a probe
 * at the stall site shows loadJobState is ALREADY 0 when the GETLB is issued, so
 * the arming store never ran on this path at all -- it is not an interrupt-timing
 * race. (The pm takes trychangefreetoloadjob's early-exit `ceqhi jobHeader,1 /
 * brhz cond1,Exit` to LS 0x2D30, skipping the Load setup.) Default OFF; kept
 * env-gated for experiments: SPU_SN_DEFER=<n> ticks (0 = immediate, faithful
 * to the current synchronous list execution). */
void* volatile   g_sn_defer_ctx = 0;
volatile unsigned g_sn_defer     = 0;

unsigned spu_sn_defer_ticks(void)
{
    static int s_n = -1;
    if (s_n < 0) { const char* e = getenv("SPU_SN_DEFER"); s_n = e ? atoi(e) : 0; }
    return (unsigned)(s_n < 0 ? 0 : s_n);
}

/* ---- Interrupt register preservation --------------------------------------
 * Hardware contract: an SPU interrupt handler preserves every register (the
 * WWS jobmanager guarantees it with a save/restore shim it installs at the
 * TOP of LS, ~0x3FEC0 -- interrupthandlerasm.spu.s). That shim lies OUTSIDE
 * the PM image our lifter processed, so the lifted handler runs WITHOUT the
 * save/restore and eats the interrupted code's live registers. Measured
 * mid-GetBufferTag: the fsmbi mask registers (r13/r15) arrived zeroed at the
 * packing selbs, the BufferTag's lanes collapsed, the job saw a NULL buffer
 * and bailed -- LBP's loading freeze.
 *
 * Enforce the contract host-side: snapshot the full GPR file when the
 * interrupt is taken, restore it when the handler irets (detected in
 * spu_indirect_branch: pc == saved srr0 with interrupts re-enabled -- during
 * the handler int_enable stays 0, so the first enabled dispatch at srr0 IS
 * the irete). State the handler legitimately publishes lives in LS/channels
 * and is untouched. Per-ctx slots; no nesting (interrupts stay disabled
 * until iret). */
#define SPU_IRQ_SLOTS 8
struct spu_irq_save {
    spu_context* ctx;          /* NULL = free */
    uint32_t     resume_pc;    /* srr0 at take */
    u128         gpr[128];
};
static struct spu_irq_save g_irq_save[SPU_IRQ_SLOTS];

static void spu_irq_regs_save(spu_context* ctx)
{
    struct spu_irq_save* s = 0;
    for (int i = 0; i < SPU_IRQ_SLOTS; i++)
        if (g_irq_save[i].ctx == ctx) { s = &g_irq_save[i]; break; }
    if (!s) for (int i = 0; i < SPU_IRQ_SLOTS; i++)
        if (!g_irq_save[i].ctx) { s = &g_irq_save[i]; break; }
    if (!s) return;                        /* out of slots: behave as before */
    s->ctx = ctx;
    s->resume_pc = ctx->srr0;
    memcpy(s->gpr, ctx->gpr, sizeof s->gpr);
}

/* Called from spu_indirect_branch on every dispatch. Restores + clears when
 * the iret lands. Returns 1 if a restore happened (diagnostic). */
int spu_irq_regs_maybe_restore(spu_context* ctx)
{
    for (int i = 0; i < SPU_IRQ_SLOTS; i++) {
        if (g_irq_save[i].ctx == ctx) {
            if ((ctx->pc & SPU_LS_MASK) == (g_irq_save[i].resume_pc & SPU_LS_MASK) &&
                ctx->int_enable) {
                memcpy(ctx->gpr, g_irq_save[i].gpr, sizeof g_irq_save[i].gpr);
                g_irq_save[i].ctx = 0;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

void (*spu_take_interrupt(spu_context* ctx,
                          void (*tf)(spu_context*)))(spu_context*)
{
    /* Stall-and-notify still settling: let the SPU run on (see above). */
    if (g_sn_defer && g_sn_defer_ctx == (void*)ctx) {
        if (--g_sn_defer == 0) g_sn_defer_ctx = 0;
        return tf;
    }
    const uint8_t* v = ctx->ls;      /* vector word at LS 0, big-endian */
    uint32_t w = ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) |
                 ((uint32_t)v[2] << 8) | v[3];
    uint32_t op9 = w >> 23;          /* top 9 opcode bits */
    uint32_t target;
    if (op9 == 0x060) {              /* bra i16 (absolute) */
        target = ((w >> 7) & 0xFFFF) << 2;
    } else if (op9 == 0x064) {       /* br i16 (relative to LS 0) */
        target = (((w >> 7) & 0xFFFF) << 2) & SPU_LS_MASK;
    } else {
        static int _w = 0;
        if (_w++ < 4)
            fprintf(stderr, "[spu-int] pending (st=0x%X mask=0x%X) but LS0 word "
                    "0x%08X is no branch -- not taken\n",
                    ctx->event_status, ctx->event_mask, w);
        return tf;
    }
    ctx->srr0 = ctx->pc;             /* resume point for iret */
    ctx->int_enable = 0;
    spu_irq_regs_save(ctx);          /* hardware contract: handler preserves regs */
    { static int s_it = -1; if (s_it < 0) s_it = getenv("SPU_IRQTRACE") ? 1 : 0;
      if (s_it) { static int _n = 0; if (_n++ < 200)
        fprintf(stderr, "[irq] TAKE srr0=0x%05X depth=%d r13=%08X r15=%08X\n",
                ctx->srr0 & SPU_LS_MASK, ctx->host_depth,
                ctx->gpr[13]._u32[0], ctx->gpr[15]._u32[0]); } }
    ctx->pc = target & SPU_LS_MASK;
    { static int _n = 0;
      if (_n++ < 16) {
          /* WWS Load->Run diagnosis: the job-manager interrupt handler advances
           * g_WwsJob_loadJobState kReadCommands(1)->kExecuteCommands(2) only if
           * (a) the ch25 stall mask has tag 0 (kLoadJob_readCommands) AND
           * (b) loadJobState's HALFWORDS compare equal to 1 (ceqhi).
           * Log both at interrupt entry so we can see which test fails. */
          const uint8_t* js = ctx->ls + 0x12A0;   /* g_WwsJob_loadJobState quadword */
          fprintf(stderr, "[spu-int] TAKEN img=%d events=0x%X&0x%X vector->0x%05X "
                  "(srr0=0x%05X) stallstat=0x%X parked=0x%X loadJobState@12A0=%02X%02X%02X%02X "
                  "%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n", ctx->image_id,
                  ctx->event_status, ctx->event_mask, ctx->pc, ctx->srr0,
                  ctx->list_stall_stat, ctx->list_stall_mask,
                  js[0],js[1],js[2],js[3], js[4],js[5],js[6],js[7],
                  js[8],js[9],js[10],js[11], js[12],js[13],js[14],js[15]); } }
    return spu_indirect_branch;
}
