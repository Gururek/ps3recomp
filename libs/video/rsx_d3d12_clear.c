/* Renderer-thread-only D3D12 draw clear with per-component write masking. */
#ifdef _WIN32

#include "rsx_d3d12_clear.h"

#include <d3dcompiler.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "d3dcompiler.lib")

/* Keep C builds independent of uuid.lib / __uuidof. */
static const IID rsx_iid_root_signature =
    {0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}};
static const IID rsx_iid_pipeline_state =
    {0x765a30f3, 0xf624, 0x4c6f, {0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45}};

/* DXGI formats currently fit below 256; this avoids renderer-thread heap work. */
#define RSX_CLEAR_FORMAT_SLOTS 256u
#define RSX_CLEAR_MASK_SLOTS 16u

typedef struct rsx_d3d12_clear_cache {
    ID3D12RootSignature *root_signature;
    ID3DBlob *vs_blob;
    ID3DBlob *ps_blob;
    ID3D12PipelineState *psos[RSX_CLEAR_FORMAT_SLOTS][RSX_CLEAR_MASK_SLOTS];
} rsx_d3d12_clear_cache;

static rsx_d3d12_clear_cache s_clear;

static void rsx_clear_log(const char *operation, HRESULT hr)
{
    fprintf(stderr, "[D3D12] masked clear %s failed (0x%08lX)\n",
            operation, (unsigned long)hr);
}

static int rsx_clear_prepare_shaders(ID3D12Device *device)
{
    static const char vs_source[] =
        "struct O { float4 p : SV_POSITION; };\n"
        "O main(uint id : SV_VertexID) {\n"
        "  float2 p = (id == 0) ? float2(-1,-1) :\n"
        "             (id == 1) ? float2(-1, 3) : float2(3,-1);\n"
        "  O o; o.p=float4(p,0,1); return o; }\n";
    static const char ps_source[] =
        "cbuffer C : register(b0) { float4 color; };\n"
        "float4 main() : SV_TARGET { return color; }\n";
    D3D12_ROOT_PARAMETER parameter = {0};
    D3D12_ROOT_SIGNATURE_DESC desc = {0};
    ID3DBlob *signature = NULL;
    ID3DBlob *error = NULL;
    HRESULT hr;

    if (s_clear.root_signature && s_clear.vs_blob && s_clear.ps_blob)
        return 0;
    /* A prior failed initialization must not leak or leave a partial cache. */
    if (s_clear.root_signature || s_clear.vs_blob || s_clear.ps_blob)
        rsx_d3d12_masked_clear_shutdown();

    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.ShaderRegister = 0;
    parameter.Constants.Num32BitValues = 4;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &signature, &error);
    if (FAILED(hr)) {
        rsx_clear_log("root signature serialization", hr);
        if (error) error->lpVtbl->Release(error);
        return -1;
    }
    hr = device->lpVtbl->CreateRootSignature(device, 0,
        signature->lpVtbl->GetBufferPointer(signature),
        signature->lpVtbl->GetBufferSize(signature), &rsx_iid_root_signature,
        (void **)&s_clear.root_signature);
    signature->lpVtbl->Release(signature);
    if (FAILED(hr)) {
        rsx_clear_log("root signature creation", hr);
        rsx_d3d12_masked_clear_shutdown();
        return -1;
    }
    if (error) { error->lpVtbl->Release(error); error = NULL; }

    hr = D3DCompile(vs_source, sizeof(vs_source) - 1, "rsx_clear_vs", NULL,
                    NULL, "main", "vs_5_0", 0, 0, &s_clear.vs_blob, &error);
    if (FAILED(hr)) {
        rsx_clear_log("vertex shader compilation", hr);
        if (error) error->lpVtbl->Release(error);
        rsx_d3d12_masked_clear_shutdown();
        return -1;
    }
    if (error) { error->lpVtbl->Release(error); error = NULL; }
    hr = D3DCompile(ps_source, sizeof(ps_source) - 1, "rsx_clear_ps", NULL,
                    NULL, "main", "ps_5_0", 0, 0, &s_clear.ps_blob, &error);
    if (FAILED(hr)) {
        rsx_clear_log("pixel shader compilation", hr);
        if (error) error->lpVtbl->Release(error);
        rsx_d3d12_masked_clear_shutdown();
        return -1;
    }
    if (error) error->lpVtbl->Release(error);
    return 0;
}

static ID3D12PipelineState *rsx_clear_get_pso(ID3D12Device *device,
                                                DXGI_FORMAT format, UINT8 mask)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {0};
    ID3D12PipelineState **slot;
    HRESULT hr;

    if ((unsigned)format >= RSX_CLEAR_FORMAT_SLOTS) {
        fprintf(stderr, "[D3D12] masked clear unsupported DXGI format %u\n",
                (unsigned)format);
        return NULL;
    }
    slot = &s_clear.psos[(unsigned)format][mask];
    if (*slot) return *slot;
    desc.pRootSignature = s_clear.root_signature;
    desc.VS.pShaderBytecode = s_clear.vs_blob->lpVtbl->GetBufferPointer(s_clear.vs_blob);
    desc.VS.BytecodeLength = s_clear.vs_blob->lpVtbl->GetBufferSize(s_clear.vs_blob);
    desc.PS.pShaderBytecode = s_clear.ps_blob->lpVtbl->GetBufferPointer(s_clear.ps_blob);
    desc.PS.BytecodeLength = s_clear.ps_blob->lpVtbl->GetBufferSize(s_clear.ps_blob);
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = mask;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
    desc.SampleDesc.Count = 1;
    hr = device->lpVtbl->CreateGraphicsPipelineState(device, &desc,
        &rsx_iid_pipeline_state, (void **)slot);
    if (FAILED(hr)) { rsx_clear_log("pipeline creation", hr); return NULL; }
    return *slot;
}

int rsx_d3d12_masked_clear(ID3D12Device *device,
                           ID3D12GraphicsCommandList *command_list,
                           D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                           DXGI_FORMAT format, UINT width, UINT height,
                           const D3D12_RECT *rect, const float color[4],
                           UINT8 rgba_mask)
{
    D3D12_VIEWPORT viewport;
    D3D12_RECT full_rect;
    ID3D12PipelineState *pso;

    if (!device || !command_list || !color || !width || !height ||
        width > LONG_MAX || height > LONG_MAX ||
        (rgba_mask & ~D3D12_COLOR_WRITE_ENABLE_ALL)) {
        fprintf(stderr, "[D3D12] masked clear invalid arguments\n");
        return -1;
    }
    if (!rgba_mask) return 0;
    if (rsx_clear_prepare_shaders(device) != 0) return -1;
    pso = rsx_clear_get_pso(device, format, rgba_mask);
    if (!pso) return -1;
    full_rect.left = 0; full_rect.top = 0;
    full_rect.right = (LONG)width; full_rect.bottom = (LONG)height;
    viewport.TopLeftX = 0.0f; viewport.TopLeftY = 0.0f;
    viewport.Width = (float)width; viewport.Height = (float)height;
    viewport.MinDepth = 0.0f; viewport.MaxDepth = 1.0f;
    command_list->lpVtbl->OMSetRenderTargets(command_list, 1, &rtv, FALSE, NULL);
    command_list->lpVtbl->SetPipelineState(command_list, pso);
    command_list->lpVtbl->SetGraphicsRootSignature(command_list, s_clear.root_signature);
    command_list->lpVtbl->SetGraphicsRoot32BitConstants(command_list, 0, 4, color, 0);
    command_list->lpVtbl->RSSetViewports(command_list, 1, &viewport);
    command_list->lpVtbl->RSSetScissorRects(command_list, 1, rect ? rect : &full_rect);
    command_list->lpVtbl->IASetPrimitiveTopology(command_list,
                                                  D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->lpVtbl->DrawInstanced(command_list, 3, 1, 0, 0);
    return 0;
}

void rsx_d3d12_masked_clear_shutdown(void)
{
    unsigned format, mask;
    for (format = 0; format < RSX_CLEAR_FORMAT_SLOTS; ++format)
        for (mask = 0; mask < RSX_CLEAR_MASK_SLOTS; ++mask)
            if (s_clear.psos[format][mask])
                s_clear.psos[format][mask]->lpVtbl->Release(s_clear.psos[format][mask]);
    if (s_clear.ps_blob) s_clear.ps_blob->lpVtbl->Release(s_clear.ps_blob);
    if (s_clear.vs_blob) s_clear.vs_blob->lpVtbl->Release(s_clear.vs_blob);
    if (s_clear.root_signature) s_clear.root_signature->lpVtbl->Release(s_clear.root_signature);
    memset(&s_clear, 0, sizeof(s_clear));
}

#endif
