/* D3D12 helper for RSX color-mask clears. */
#ifndef RSX_D3D12_CLEAR_H
#define RSX_D3D12_CLEAR_H

#ifdef _WIN32
#include <d3d12.h>
#include <dxgiformat.h>

/* rgba_mask uses D3D12_COLOR_WRITE_ENABLE_{RED,GREEN,BLUE,ALPHA} bits. */
int rsx_d3d12_masked_clear(ID3D12Device *device,
                           ID3D12GraphicsCommandList *command_list,
                           D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                           DXGI_FORMAT format, UINT width, UINT height,
                           const D3D12_RECT *rect, const float color[4],
                           UINT8 rgba_mask);
void rsx_d3d12_masked_clear_shutdown(void);
#endif

#endif
