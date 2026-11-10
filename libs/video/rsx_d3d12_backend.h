/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU commands to Direct3D 12 rendering.
 * Implements the rsx_backend callback interface.
 *
 * Architecture:
 *   rsx_commands.c (state tracking) → rsx_backend callbacks → this file
 *                                                               ↓
 *                                                         D3D12 device
 *                                                         swap chain
 *                                                         command lists
 *                                                         pipeline states
 *
 * Current scope (Phase 1):
 *   - Device/swap chain creation
 *   - Clear to RSX clear color
 *   - Present with vsync
 *   - Basic triangle rendering (vertex position + color)
 *
 * Future phases:
 *   - Texture upload and sampling
 *   - RSX vertex/fragment program → HLSL translation
 *   - Framebuffer resolve and display buffer management
 *   - Depth/stencil buffer creation
 */

#ifndef PS3RECOMP_RSX_D3D12_BACKEND_H
#define PS3RECOMP_RSX_D3D12_BACKEND_H

#include "rsx_commands.h"

/* NV3089 exact-copy operation captured by cellGcmSys after its normal CPU
 * rsx_scaled_blit succeeds.  The D3D backend replays this at the matching FIFO
 * position so its persistent display/RT ownership follows the guest copy too.
 * `local` is nonzero for RSX local memory; raw offsets remain unmodified. */
typedef struct {
    u32 src_raw, dst_raw;
    /* Captured together at FIFO record time.  A later IO remap must never
     * replay this raw offset into a different MAIN backing. */
    u32 src_ea, dst_ea;
    u64 mapping_generation;
    u32 src_pitch, dst_pitch;
    u16 src_w, src_h, dst_w, dst_h;
    u16 src_x, src_y, dst_x, dst_y, copy_w, copy_h;
    u8  src_local, dst_local;
    u8  src_cpu_owned; /* LOCAL source proven never claimed by a GPU producer */
    /* Pre-CPU-copy guest bytes, captured by cellGcmSys.  The snapshot pointer
     * and byte count for GPU-owned LOCAL endpoints are NULL/zero. A proven
     * CPU-owned LOCAL source has an immutable snapshot, just like MAIN. Each
     * MAIN endpoint requires a populated pre-copy snapshot.
     * Ownership transfers to record_exact_blit on both success and rejection. */
    u8* src_snapshot;
    u8* dst_snapshot;
    u64 src_snapshot_bytes, dst_snapshot_bytes;
    /* Row-packed post-CPU MAIN destination dirty rectangle.  This is the guest
     * baseline that must still match before deferred GPU writeback commits.
     * LOCAL destinations use NULL/zero because they never enter that path. */
    u8* dst_post_snapshot;
    u64 dst_post_snapshot_bytes;
} RsxD3D12ExactBlit;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the D3D12 backend.
 * Creates a Win32 window, D3D12 device, swap chain, and registers
 * as the active rsx_backend.
 * Returns 0 on success, -1 on failure. */
int rsx_d3d12_backend_init(u32 width, u32 height, const char* title);

/* Shut down the D3D12 backend and release all resources. */
void rsx_d3d12_backend_shutdown(void);

/* Process Win32 messages. Returns 0 normally, -1 if window closed. */
int rsx_d3d12_backend_pump_messages(void);
/* Appends the one retained slow-pump record to the opt-in flip timeline. */
void rsx_d3d12_backend_dump_message_pump_trace(void);
/* Register/reset on the renderer window-owner thread. Called at WM_CLOSE
 * before DestroyWindow; observer must not issue GPU or input work. */
void rsx_d3d12_backend_set_close_observer(void (*observer)(void));

/* Force a present (useful for debugging). */
void rsx_d3d12_backend_present(void);
/* Present one already-consumed cellGcm flip tuple. */
void rsx_d3d12_backend_present_buffer(u64 generation, u32 buffer_id);
/* Reference-replay diagnostic only: finish the submitted frame before its
 * isolated VM image is rewritten.  This is never called by normal presentation. */
int rsx_d3d12_backend_finish_reference_frame(void);

/* Records only supported 1:1 RGBA NV3089 copies. Returns 1 when deferred for
 * GPU mirroring, 0 when the CPU guest-memory result remains the sole path. */
int rsx_d3d12_backend_record_exact_blit(const RsxD3D12ExactBlit* blit);
/* Optional CPU-only storage reuse for exact-blit snapshots. Buffers remain
 * ordinary malloc pointers; callers return only released buffers with the
 * exact allocation size. */
u8* rsx_d3d12_blit_snapshot_acquire(u64 bytes);
void rsx_d3d12_blit_snapshot_release(u8* snapshot, u64 bytes);
void rsx_d3d12_blit_snapshot_pool_init(void);
void rsx_d3d12_blit_snapshot_pool_shutdown(void);
int rsx_d3d12_backend_local_source_is_cpu(u32 raw, u32 pitch, u32 width, u32 height);
int rsx_d3d12_backend_exact_blit_capture_enabled(void);
/* Latches the backend off after a CPU-successful exact-copy could not be
 * mirrored.  Continuing would sample GPU state known to disagree with RAM. */
void rsx_d3d12_backend_note_exact_blit_fault(void);

/* FIFO completion edge.  Called only by the present/FIFO owner after it has
 * consumed SET_REFERENCE or SCE_DRIVER_INTERRUPT.  Replays the pending prefix,
 * fences it, and commits any authoritative exact MAIN copies without Present. */
int rsx_d3d12_backend_complete_fifo_prefix(void);
/* Install an optional same-thread wait-service callback for pending checked
 * fence waits. The callback may service presenter work and returns the
 * requested next OS wait slice in milliseconds; fence readiness is still
 * decided only by the checked fence value. Passing NULL clears the service. */
void rsx_d3d12_backend_set_fence_wait_service(
    u32 (*service)(void* context, u32 remaining_ms), void* context);
/* Optional deadline service at complete FIFO packet boundaries. Clearing or
 * replacing the wait service disables pulses. Neither pulse readiness nor
 * its returned wait slice is evidence of GPU completion. */
void rsx_d3d12_backend_set_fence_wait_fifo_pulse(int enabled);
void rsx_d3d12_backend_pulse_fence_wait_service(void);
/* Named completion and preparation boundaries. complete_fifo_prefix_at always
 * completes; the optional overlap path may defer only preparation flushes. */
typedef enum RsxD3D12CompletionReason {
    RSX_COMPLETION_OTHER,
    RSX_COMPLETION_TEXTURE_RELEASE,
    RSX_COMPLETION_BACKEND_RELEASE,
    RSX_COMPLETION_ACQUIRE,
    RSX_COMPLETION_LOCAL_MAIN_COPY,
    RSX_COMPLETION_INPUT,
    RSX_COMPLETION_CAPACITY,
    RSX_COMPLETION_CLEAR,
    RSX_COMPLETION_LABEL_LIMIT,
    RSX_COMPLETION_DRAIN,
    RSX_COMPLETION_CAPTURE,
    RSX_COMPLETION_RESOURCE_RELEASE,
    RSX_COMPLETION_REASON_COUNT
} RsxD3D12CompletionReason;
int rsx_d3d12_backend_complete_fifo_prefix_at(RsxD3D12CompletionReason reason);
/* Diagnostic-only prefix trigger attribution for [TJ-PRESENT-WORK]. Values are
 * completion reasons plus these FIFO-frontend edges; never changes behaviour.
 * Note before a plain complete_fifo_prefix(), restore RSX_COMPLETION_OTHER after. */
enum {
    RSX_PREFIX_TRIGGER_REFERENCE = RSX_COMPLETION_REASON_COUNT,
    RSX_PREFIX_TRIGGER_INTERRUPT,
    RSX_PREFIX_TRIGGER_NON3D,
    RSX_PREFIX_TRIGGER_COUNT
};
void rsx_d3d12_backend_note_prefix_trigger(u32 trigger);
/* Opt-in (TJ_TEXTURE_LABEL_EARLY=3): nonzero when an NV406E acquire may skip
 * the pre-compare label flush because no texture label is pending. */
int rsx_d3d12_backend_acquire_gpu_free(void);
/* Opt-in bounded batching. Prepare either completes normally or arms exactly
 * the immediately following texture-label callback. Flush at every FIFO drain
 * end and before observations/other engines; publication still requires a fence. */
int rsx_d3d12_backend_prepare_texture_label(void);
int rsx_d3d12_backend_flush_texture_labels(void);
/* Checked flush. Only INPUT/CAPACITY/CLEAR preparation boundaries may use the
 * opt-in owned overlap route. All guest observers and FIFO drains still wait. */
int rsx_d3d12_backend_flush_texture_labels_at(RsxD3D12CompletionReason reason);
/* Optional metadata coverage at a texture-read label; does not complete or skip work. */
void rsx_d3d12_backend_trace_texture_read_prefix(void);
/* Immediately completes a newly-recorded LOCAL->MAIN exact-copy prefix.  The
 * backend owns the structural endpoint check and the later authority decision. */
int rsx_d3d12_backend_complete_local_main_prefix(void);
/* Sticky for this backend lifetime; FIFO must not cross a failed exact copy. */
int rsx_d3d12_backend_fifo_faulted(void);
/* TJ_SNAPSHOT_LATE_PROOF: resolve deferred texture-snapshot proofs before a
 * FIFO-frontend guest-visible store that no backend call precedes (walk-end
 * GET, flip request). One load when off or nothing is pending. */
void rsx_d3d12_backend_snapshot_late_resolve(void);

/* Opt-in display-locked VBlank (TJ_VBLANK_SOURCE=display; the presenter
 * decides). The backend owns DXGI, so it owns the output and a waiter thread
 * that blocks in IDXGIOutput::WaitForVBlank on the output containing the
 * window and publishes one emulated tick per `divisor` real vblanks
 * (divisor = round(refresh / 60), refresh within 2% of that multiple).
 * start/stop run on the window-owner thread after init; start returns 1 when
 * the waiter runs, else 0 (keep the QPC cadence) with the reason in `note`.
 * ticks() is the monotonic published count plus the QPC stamp of the newest
 * tick (0 = none). The auto-reset event is set per publication (NULL when
 * not running). describe() appends key=value counters for the opt-in trace;
 * window != 0 reports per-call deltas and also covers DXGI present cadence
 * (TJ_VBLANK_SOURCE_TRACE=1 samples frame statistics after each Present). */
int   rsx_d3d12_backend_display_vblank_start(char* note, size_t note_size);
void  rsx_d3d12_backend_display_vblank_stop(void);
u64   rsx_d3d12_backend_display_vblank_ticks(u64* last_tick_qpc);
void* rsx_d3d12_backend_display_vblank_event(void);
void  rsx_d3d12_backend_display_vblank_describe(char* out, size_t size, int window);
/* Real vblanks per published tick (round(refresh / 60)); 0 = waiter off. */
u32   rsx_d3d12_backend_display_vblank_divisor(void);

/* Opt-in present phase (TJ_PRESENT_PHASE=1 or hold; the presenter decides;
 * window-owner thread only). interval_arm(n >= 2) before present_buffer
 * makes that call's final Present use SyncInterval n (max 4; paced_count);
 * the caller disarms with 0 after the call. Hold mode: arm(1) before
 * present_buffer makes that call's final frame replay, composite and submit
 * as usual but keep its Present(1,0); the caller disarms after the call.
 * held() reports such a frame; release() presents it (1 = presented). The
 * backend presents a held frame itself before the next replay touches the
 * swap chain (forced_count; forced_take returns the window's count and QPC
 * offsets after the tick set by phase_tick). count() is every successful
 * Present. Both modes select 3 swap buffers (swap_buffer_count;
 * TJ_PRESENT_PHASE_BUFFERS=2 keeps 2). */
void  rsx_d3d12_backend_present_hold_arm(int arm);
void  rsx_d3d12_backend_present_interval_arm(u32 interval);
void  rsx_d3d12_backend_present_phase_tick(u64 tick_qpc);
int   rsx_d3d12_backend_present_held(void);
int   rsx_d3d12_backend_present_release(void);
u64   rsx_d3d12_backend_present_count(void);
u64   rsx_d3d12_backend_present_paced_count(void);
u64   rsx_d3d12_backend_present_forced_count(void);
u64   rsx_d3d12_backend_present_forced_take(u64* stamped, u64* sum, u64* min, u64* max);
u32   rsx_d3d12_backend_swap_buffer_count(void);

/* Arm Tokyo Jungle's persistent Survival boundary profile and bounded
 * diagnostic render manifest. */
void rsx_d3d12_arm_survival_trace(void);

#ifdef __cplusplus
}
#endif
#endif
