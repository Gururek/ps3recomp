/* GPU-owned zeta memory sampled through the guest's packed colour format. */
#ifdef _WIN32
#include "rsx_d3d12_depth_sample.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

static const IID iid_root = {0xc54a6b66,0x72df,0x4ee8,{0x8b,0xe5,0xa9,0x46,0xa1,0x42,0x92,0x14}};
static const IID iid_pso = {0x765a30f3,0xf624,0x4c6f,{0xa8,0x28,0xac,0xe9,0x48,0x62,0x24,0x45}};
static const IID iid_resource = {0x696442be,0xa72e,0x4059,{0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}};
static const IID iid_heap = {0x8efb471d,0x616c,0x4f49,{0x90,0xf7,0x12,0x7b,0xb7,0x63,0xfa,0x51}};
static ID3D12RootSignature *s_root;
static ID3D12PipelineState *s_pso;

static int prepare_pipeline(ID3D12Device *device)
{
    /* RSX stores BE (depth24 << 8) | stencil8. The colour uploader exposes
     * those A,R,G,B bytes as R,G,B,A. Pack before texture filtering, not after:
     * filtering a normalized depth value does not reproduce filtering bytes.
     * D3D's X24_TYPELESS_G8_UINT SRV exposes stencil in component G. */
    static const char shader[] =
        "Texture2D<float> z : register(t0);\n"
        "Texture2D<uint2> s : register(t1);\n"
        "RWTexture2D<float4> rgba : register(u0);\n"
        "[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {\n"
        " uint w,h; rgba.GetDimensions(w,h); if(id.x>=w || id.y>=h) return;\n"
        " uint d=(uint)round(saturate(z.Load(int3(id.xy,0)))*16777215.0);\n"
        " uint st=s.Load(int3(id.xy,0)).y;\n"
        " rgba[id.xy]=float4((d>>8)&255,d&255,st&255,(d>>16)&255)/255.0;\n"
        "}\n";
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{0}};
    D3D12_ROOT_PARAMETER param = {0};
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
    ID3DBlob *signature = NULL, *code = NULL, *error = NULL;
    HRESULT hr;
    if (s_root && s_pso) return 0;
    rsx_d3d12_depth_sample_shutdown();
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges = ranges;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rd.NumParameters = 1; rd.pParameters = &param;
    hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) goto fail;
    if (error) { error->lpVtbl->Release(error); error = NULL; }
    hr = device->lpVtbl->CreateRootSignature(device, 0,
        signature->lpVtbl->GetBufferPointer(signature),
        signature->lpVtbl->GetBufferSize(signature), &iid_root, (void **)&s_root);
    if (FAILED(hr)) goto fail;
    hr = D3DCompile(shader, sizeof(shader)-1, "rsx_depth_sample", NULL, NULL,
        "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &error);
    if (FAILED(hr)) goto fail;
    pd.pRootSignature = s_root;
    pd.CS.pShaderBytecode = code->lpVtbl->GetBufferPointer(code);
    pd.CS.BytecodeLength = code->lpVtbl->GetBufferSize(code);
    hr = device->lpVtbl->CreateComputePipelineState(device, &pd, &iid_pso, (void **)&s_pso);
    if (FAILED(hr)) goto fail;
    if (error) error->lpVtbl->Release(error);
    code->lpVtbl->Release(code); signature->lpVtbl->Release(signature);
    return 0;
fail:
    fprintf(stderr, "[D3D12] depth sample pipeline failed 0x%08lX: %s\n",
        (unsigned long)hr, error ? (const char *)error->lpVtbl->GetBufferPointer(error) : "");
    if (error) error->lpVtbl->Release(error);
    if (code) code->lpVtbl->Release(code);
    if (signature) signature->lpVtbl->Release(signature);
    rsx_d3d12_depth_sample_shutdown();
    return -1;
}

void rsx_d3d12_depth_sample_release(rsx_d3d12_depth_sample *sample)
{
    if (!sample) return;
    if (sample->rgba) sample->rgba->lpVtbl->Release(sample->rgba);
    if (sample->heap) sample->heap->lpVtbl->Release(sample->heap);
    memset(sample, 0, sizeof(*sample));
}

void rsx_d3d12_depth_sample_shutdown(void)
{
    if (s_pso) s_pso->lpVtbl->Release(s_pso);
    if (s_root) s_root->lpVtbl->Release(s_root);
    s_pso = NULL; s_root = NULL;
}

int rsx_d3d12_depth_sample_prepare(ID3D12Device *device,
    rsx_d3d12_depth_sample *sample, ID3D12Resource *depth)
{
    D3D12_RESOURCE_DESC desc;
    D3D12_HEAP_PROPERTIES hp = {0};
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {0};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {0};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    UINT inc;
    HRESULT hr;
    if (!device || !sample || !depth) return -1;
    if (sample->depth == depth && sample->rgba && sample->heap) return 0;
    depth->lpVtbl->GetDesc(depth, &desc);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Format != DXGI_FORMAT_R24G8_TYPELESS || desc.SampleDesc.Count != 1 ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1) return -1;
    if (prepare_pipeline(device)) return -1;
    rsx_d3d12_depth_sample_release(sample);
    sample->width = (UINT)desc.Width; sample->height = desc.Height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    sample->rgba_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    hr = device->lpVtbl->CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE,
        &desc, sample->rgba_state, NULL, &iid_resource, (void **)&sample->rgba);
    if (FAILED(hr)) goto fail;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 3; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->lpVtbl->CreateDescriptorHeap(device, &hd, &iid_heap, (void **)&sample->heap);
    if (FAILED(hr)) goto fail;
    sample->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(sample->heap, &cpu);
    inc = device->lpVtbl->GetDescriptorHandleIncrementSize(device, hd.Type);
    srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->lpVtbl->CreateShaderResourceView(device, depth, &srv, cpu);
    cpu.ptr += inc;
    srv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    srv.Texture2D.PlaneSlice = 1;
    device->lpVtbl->CreateShaderResourceView(device, depth, &srv, cpu);
    cpu.ptr += inc;
    uav.Format = desc.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->lpVtbl->CreateUnorderedAccessView(device, sample->rgba, NULL, &uav, cpu);
    sample->depth = depth;
    return 0;
fail:
    fprintf(stderr, "[D3D12] depth sample resource failed 0x%08lX\n", (unsigned long)hr);
    rsx_d3d12_depth_sample_release(sample);
    return -1;
}

static void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->lpVtbl->ResourceBarrier(list, 1, &b);
}

int rsx_d3d12_depth_sample_update(ID3D12GraphicsCommandList *list,
    rsx_d3d12_depth_sample *sample)
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    if (!list || !sample || !sample->depth || !sample->rgba || !sample->heap ||
        !s_root || !s_pso) return -1;
    transition(list, sample->depth, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list, sample->rgba, sample->rgba_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->lpVtbl->SetDescriptorHeaps(list, 1, &sample->heap);
    list->lpVtbl->SetComputeRootSignature(list, s_root);
    list->lpVtbl->SetPipelineState(list, s_pso);
    sample->heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(sample->heap, &gpu);
    list->lpVtbl->SetComputeRootDescriptorTable(list, 0, gpu);
    list->lpVtbl->Dispatch(list, (sample->width+7)/8, (sample->height+7)/8, 1);
    transition(list, sample->rgba, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, sample->depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    sample->rgba_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return 0;
}
#endif
