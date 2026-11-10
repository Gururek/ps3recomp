/*
 * ps3recomp - rsx_d3d12_backend_* entry points for non-D3D12 hosts
 *
 * cellGcmSys.c drives the RSX through the backend-neutral rsx_state plus the
 * D3D12 backend's completion/boundary API: FIFO prefixes are completed at
 * explicit boundaries, texture labels are prepared and flushed around guest
 * observations, and the exact-copy capture records GPU-side blits. On Windows
 * that API is rsx_d3d12_backend.c. The Vulkan backend (the Linux/macOS path)
 * grew the same surface for the pieces that matter to it -- prefix completion,
 * texture-label prepare/flush, prefix-trigger attribution, the GPU-free probe
 * and the FIFO fault latch -- and the rest are D3D12 implementation details.
 *
 * This file provides the remaining entry points, mapping onto the Vulkan
 * backend where it has an equivalent and returning the neutral answer where it
 * has none, so a Vulkan build links the same GCM front end as a D3D12 one.
 */

#include "rsx_d3d12_backend.h"
#include "rsx_null_backend.h"

#if !defined(_WIN32)

#if defined(PS3RECOMP_RSX_VULKAN)
#include "rsx_vulkan_backend.h"

int rsx_d3d12_backend_complete_fifo_prefix(void)
{
    return rsx_vulkan_backend_complete_fifo_prefix_at(RSX_COMPLETION_DRAIN);
}

int rsx_d3d12_backend_complete_fifo_prefix_at(RsxD3D12CompletionReason reason)
{
    return rsx_vulkan_backend_complete_fifo_prefix_at(reason);
}

int rsx_d3d12_backend_complete_local_main_prefix(void)
{
    return rsx_vulkan_backend_complete_local_main_prefix_at(RSX_COMPLETION_DRAIN);
}

int rsx_d3d12_backend_prepare_texture_label(void)
{
    return rsx_vulkan_backend_prepare_texture_label();
}

int rsx_d3d12_backend_flush_texture_labels(void)
{
    return rsx_vulkan_backend_flush_texture_labels();
}

int rsx_d3d12_backend_flush_texture_labels_at(RsxD3D12CompletionReason reason)
{
    return rsx_vulkan_backend_flush_texture_labels_at(reason);
}

int rsx_d3d12_backend_acquire_gpu_free(void)
{
    return rsx_vulkan_backend_acquire_gpu_free();
}

void rsx_d3d12_backend_note_prefix_trigger(u32 trigger)
{
    rsx_vulkan_backend_note_prefix_trigger((RsxD3D12PrefixTrigger)trigger);
}

void rsx_d3d12_backend_trace_texture_read_prefix(void)
{
    rsx_vulkan_backend_trace_texture_read_prefix();
}

int rsx_d3d12_backend_fifo_faulted(void)
{
    return rsx_vulkan_backend_fifo_faulted();
}

#else /* !PS3RECOMP_RSX_VULKAN: the null backend needs no boundaries at all. */

int  rsx_d3d12_backend_complete_fifo_prefix(void) { return 0; }
int  rsx_d3d12_backend_complete_fifo_prefix_at(RsxD3D12CompletionReason reason)
{ (void)reason; return 0; }
int  rsx_d3d12_backend_complete_local_main_prefix(void) { return 0; }
int  rsx_d3d12_backend_prepare_texture_label(void) { return 0; }
int  rsx_d3d12_backend_flush_texture_labels(void) { return 0; }
int  rsx_d3d12_backend_flush_texture_labels_at(RsxD3D12CompletionReason reason)
{ (void)reason; return 0; }
int  rsx_d3d12_backend_acquire_gpu_free(void) { return 1; }
void rsx_d3d12_backend_note_prefix_trigger(u32 trigger) { (void)trigger; }
void rsx_d3d12_backend_trace_texture_read_prefix(void) {}
int  rsx_d3d12_backend_fifo_faulted(void) { return 0; }

#endif /* PS3RECOMP_RSX_VULKAN */

/* The exact-copy capture: cellGcmSys does the CPU blit into guest VRAM and, on
 * D3D12, records the same copy so the backend's display/rendertarget ownership
 * follows it. The Vulkan backend reads guest VRAM directly, so a no-op capture
 * (disabled) is the truthful answer; `local_source_is_cpu` says yes because a
 * CPU-written LOCAL tile is exactly what the caller has just produced. */
int rsx_d3d12_backend_exact_blit_capture_enabled(void) { return 0; }

int rsx_d3d12_backend_record_exact_blit(const RsxD3D12ExactBlit* blit)
{
    (void)blit;
    return 1;
}

int rsx_d3d12_backend_local_source_is_cpu(u32 raw, u32 pitch, u32 width, u32 height)
{
    (void)raw; (void)pitch; (void)width; (void)height;
    return 1;
}

void rsx_d3d12_backend_note_exact_blit_fault(void) {}

/* Fence-wait service + late framebuffer resolve: D3D12 queue bookkeeping. */
void rsx_d3d12_backend_pulse_fence_wait_service(void) {}
void rsx_d3d12_backend_snapshot_late_resolve(void) {}


/* Backend lifecycle + frame loop: the title binds these as "the" RSX backend,
 * so on hosts without D3D12 they drive the null backend's window/frame state. */
int  rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    return rsx_null_backend_init(width, height, title);
}
void rsx_d3d12_backend_shutdown(void) { rsx_null_backend_shutdown(); }
int  rsx_d3d12_backend_pump_messages(void) { return rsx_null_backend_pump_messages(); }
void rsx_d3d12_backend_present(void) { rsx_null_backend_present(); }

/* Explicit-generation flip: the title names the framebuffer a completed GPU
 * generation produced. Headless there is one surface, so it is a plain commit. */
void rsx_d3d12_backend_present_buffer(u64 generation, u32 buffer_id)
{
    (void)generation; (void)buffer_id;
    rsx_null_backend_present();
}

/* Survival-mode geometry trace arming (the D3D12 backend's own diagnostics). */
void rsx_d3d12_arm_survival_trace(void) {}
volatile long g_tj_geometry_trace_armed = 0;

#endif /* !(_WIN32) */

/* cellSpurs.c's RSX-survival trace switch: the D3D12 backend arms it from its
 * own diagnostics; on hosts without D3D12 it stays off. */
volatile long g_rsx_survival_trace_armed = 0;
