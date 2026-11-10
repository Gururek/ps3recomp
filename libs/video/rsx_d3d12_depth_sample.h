#pragma once
#ifdef _WIN32
#include <d3d12.h>

/* Renderer-thread-owned, ordered D24S8 -> RSX A8R8G8B8 texture snapshot.
 * Zero-initialize. Prepare/release only after prior GPU work has completed.
 * The caller owns depth and keeps it alive until release. */
typedef struct rsx_d3d12_depth_sample {
    ID3D12Resource *depth, *rgba;
    ID3D12DescriptorHeap *heap;
    D3D12_RESOURCE_STATES rgba_state;
    UINT width, height;
} rsx_d3d12_depth_sample;

int rsx_d3d12_depth_sample_prepare(ID3D12Device *device,
    rsx_d3d12_depth_sample *sample, ID3D12Resource *depth);
/* Depth enters and leaves DEPTH_WRITE; rgba leaves PIXEL_SHADER_RESOURCE.
 * Binds a compute PSO/root and descriptor heap. Restore graphics bindings. */
int rsx_d3d12_depth_sample_update(ID3D12GraphicsCommandList *list,
    rsx_d3d12_depth_sample *sample);
void rsx_d3d12_depth_sample_release(rsx_d3d12_depth_sample *sample);
void rsx_d3d12_depth_sample_shutdown(void);
#endif
