/* Headless GPU regression: packed depth bytes, stencil plane, FIFO snapshots,
 * partial clears, source preservation and non-multiple-of-eight dimensions. */
#ifdef _WIN32
#include "../rsx_d3d12_depth_sample.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 17u
#define H 9u
static const IID iid_device = {0x189819f1,0x1db6,0x4b57,{0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7}};
static const IID iid_queue = {0x0ec870a6,0x5d7e,0x4c22,{0x8c,0xfc,0x5b,0xaa,0xe0,0x76,0x16,0xed}};
static const IID iid_resource = {0x696442be,0xa72e,0x4059,{0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}};
static const IID iid_heap = {0x8efb471d,0x616c,0x4f49,{0x90,0xf7,0x12,0x7b,0xb7,0x63,0xfa,0x51}};
static const IID iid_allocator = {0x6102dee4,0xaf59,0x4b09,{0xb9,0x99,0xb4,0x4d,0x73,0xf0,0x9b,0x24}};
static const IID iid_list = {0x5b160d0f,0xac1b,0x4185,{0x8b,0xa8,0xb3,0xae,0x42,0xa5,0xa4,0x55}};
static const IID iid_fence = {0x0a753dcf,0xc4d8,0x4b91,{0xad,0xf6,0xbe,0x5a,0x60,0xd9,0x5a,0x76}};
static const IID iid_root = {0xc54a6b66,0x72df,0x4ee8,{0x8b,0xe5,0xa9,0x46,0xa1,0x42,0x92,0x14}};
static const IID iid_pso = {0x765a30f3,0xf624,0x4c6f,{0xa8,0x28,0xac,0xe9,0x48,0x62,0x24,0x45}};
static const unsigned depths[] = {0, 1, 0x010203, 0x123456, 0x7FFFFF, 0x800001, 0xFEFFFE, 0xFFFFFF};
#define REL(x) do { if (x) (x)->lpVtbl->Release(x); } while (0)
#define CHECK(x) do { HRESULT hr_=(x); if (FAILED(hr_)) { \
    fprintf(stderr,"FAIL %s 0x%08lX\n",#x,(unsigned long)hr_); goto done; } } while (0)

static void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res; b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->lpVtbl->ResourceBarrier(list, 1, &b);
}

static void copy_sample(ID3D12GraphicsCommandList *list, ID3D12Resource *rgba,
    ID3D12Resource *readback, D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout)
{
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    barrier(list, rgba, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    src.pResource = rgba; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;
    list->lpVtbl->CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    barrier(list, rgba, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

int main(void)
{
    ID3D12Device *device = NULL; ID3D12CommandQueue *queue = NULL;
    ID3D12CommandAllocator *allocator = NULL; ID3D12GraphicsCommandList *list = NULL;
    ID3D12DescriptorHeap *dsv_heap = NULL; ID3D12Resource *depth = NULL, *readback = NULL;
    ID3D12Fence *fence = NULL; HANDLE event = NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; D3D12_RESOURCE_DESC td = {0}, rb = {0}, rgba_desc;
    D3D12_COMMAND_QUEUE_DESC qd = {0}; D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    D3D12_DEPTH_STENCIL_VIEW_DESC vd = {0}; D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    D3D12_CLEAR_VALUE cv = {0}; D3D12_RANGE range = {0};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_layout;
    ID3D12Resource *color = NULL;
    ID3D12DescriptorHeap *rtv_heap = NULL;
    ID3D12RootSignature *root = NULL;
    ID3D12PipelineState *draw_pso[3] = {NULL, NULL, NULL};
    ID3DBlob *signature = NULL, *vs = NULL, *ps = NULL, *errors = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    const int legacy_clip = getenv("TJ_TEST_COLOR_CLIPPED_DEPTH") != NULL;
    UINT rows; UINT64 row_size, bytes, stride;
    D3D12_RECT depth_rect = {2,1,7,8}, stencil_rect = {9,0,17,4};
    rsx_d3d12_depth_sample sample = {0};
    unsigned char *pixels; unsigned x, y, stage; int result = 1, failures = 0;

    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &iid_device, (void **)&device));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(device->lpVtbl->CreateCommandQueue(device, &qd, &iid_queue, (void **)&queue));
    CHECK(device->lpVtbl->CreateCommandAllocator(device, qd.Type, &iid_allocator, (void **)&allocator));
    CHECK(device->lpVtbl->CreateCommandList(device, 0, qd.Type, allocator, NULL, &iid_list, (void **)&list));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = W; td.Height = H;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
    CHECK(device->lpVtbl->CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &iid_resource, (void **)&depth));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    CHECK(device->lpVtbl->CreateDescriptorHeap(device, &hd, &iid_heap, (void **)&dsv_heap));
    dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
    vd.Format = cv.Format; vd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->lpVtbl->CreateDepthStencilView(device, depth, &vd, dsv);
    if (rsx_d3d12_depth_sample_prepare(device, &sample, depth)) goto done;
    { ID3D12Resource *saved = sample.rgba;
      if (rsx_d3d12_depth_sample_prepare(device, &sample, depth) || sample.rgba != saved) {
          fprintf(stderr,"FAIL prepare did not reuse resource\n"); goto done;
      } }
    sample.rgba->lpVtbl->GetDesc(sample.rgba, &rgba_desc);
    device->lpVtbl->GetCopyableFootprints(device, &rgba_desc, 0, 1, 0, &layout, &rows, &row_size, &bytes);
    stride = (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1) &
        ~(UINT64)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1);
    hp.Type = D3D12_HEAP_TYPE_READBACK; rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width = stride * 7; rb.Height = 1; rb.DepthOrArraySize = 1; rb.MipLevels = 1;
    rb.SampleDesc.Count = 1; rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHECK(device->lpVtbl->CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rb,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &iid_resource, (void **)&readback));
    /* Reproduce the title's smaller, unused display colour attachment beside
     * a larger shadow depth surface. No window or game process is needed. */
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    td.Height = H / 2; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    CHECK(device->lpVtbl->CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &iid_resource, (void **)&color));
    device->lpVtbl->GetCopyableFootprints(device, &td, 0, 1, 0,
        &color_layout, &rows, &row_size, &bytes);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    CHECK(device->lpVtbl->CreateDescriptorHeap(device, &hd, &iid_heap, (void **)&rtv_heap));
    rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv);
    device->lpVtbl->CreateRenderTargetView(device, color, NULL, rtv);
    { const FLOAT red[4] = {1,0,0,1};
      list->lpVtbl->ClearRenderTargetView(list, rtv, red, 0, NULL); }
    {
        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
        static const char vertex[] =
            "float4 main(uint id:SV_VertexID):SV_Position {"
            "float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};"
            "return float4(p[id],0.25,1);}";
        static const char pixel[] = "float4 main():SV_Target0{return float4(0,1,0,1);}";
        CHECK(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors));
        CHECK(device->lpVtbl->CreateRootSignature(device, 0,
            signature->lpVtbl->GetBufferPointer(signature), signature->lpVtbl->GetBufferSize(signature),
            &iid_root, (void **)&root));
        CHECK(D3DCompile(vertex, sizeof(vertex)-1, NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &vs, NULL));
        CHECK(D3DCompile(pixel, sizeof(pixel)-1, NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &ps, NULL));
        pd.pRootSignature = root;
        pd.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs); pd.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
        pd.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps); pd.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.SampleMask = UINT_MAX; pd.SampleDesc.Count = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        for (unsigned mode = 0; mode < 3; ++mode) {
            pd.NumRenderTargets = mode ? 1 : 0;
            pd.RTVFormats[0] = mode ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = mode == 2 ? 15 : 0;
            CHECK(device->lpVtbl->CreateGraphicsPipelineState(device, &pd, &iid_pso, (void **)&draw_pso[mode]));
        }
    }
    for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
        D3D12_RECT r = {(LONG)x,(LONG)y,(LONG)x+1,(LONG)y+1};
        unsigned d = depths[(x+y)%8];
        list->lpVtbl->ClearDepthStencilView(list, dsv,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            (float)((double)d / 16777215.0), (UINT8)(x*17+y*31), 1, &r);
    }
    for (stage = 0; stage < 5; ++stage) {
        if (stage == 1) {
            list->lpVtbl->ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH,
                (float)(0x123456 / 16777215.0), 0, 1, &depth_rect);
            list->lpVtbl->ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_STENCIL,
                0, 0xA5, 1, &stencil_rect);
        }
        if (stage >= 3) {
            D3D12_VIEWPORT vp = {0,0,(FLOAT)W,(FLOAT)H,0,1};
            D3D12_RECT sc = {0,0,W,H};
            UINT mode = stage == 4 ? 2 : legacy_clip ? 1 : 0;
            if (stage == 3)
                list->lpVtbl->ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, NULL);
            if (mode) sc.bottom = H / 2;
            list->lpVtbl->OMSetRenderTargets(list, mode ? 1 : 0, mode ? &rtv : NULL, FALSE, &dsv);
            list->lpVtbl->SetGraphicsRootSignature(list, root);
            list->lpVtbl->SetPipelineState(list, draw_pso[mode]);
            list->lpVtbl->IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->lpVtbl->RSSetViewports(list, 1, &vp);
            list->lpVtbl->RSSetScissorRects(list, 1, &sc);
            list->lpVtbl->DrawInstanced(list, 3, 1, 0, 0);
            color_layout.Offset = stride * (stage + 2);
            barrier(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            copy_sample(list, color, readback, color_layout);
            barrier(list, color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (rsx_d3d12_depth_sample_update(list, &sample)) {
            fprintf(stderr,"FAIL update stage %u\n",stage); goto done;
        }
        layout.Offset = stride * stage;
        copy_sample(list, sample.rgba, readback, layout);
    }
    CHECK(list->lpVtbl->Close(list));
    { ID3D12CommandList *lists[] = {(ID3D12CommandList *)list};
      queue->lpVtbl->ExecuteCommandLists(queue, 1, lists); }
    CHECK(device->lpVtbl->CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &iid_fence, (void **)&fence));
    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event) { fprintf(stderr,"FAIL CreateEvent\n"); goto done; }
    CHECK(queue->lpVtbl->Signal(queue, fence, 1));
    CHECK(fence->lpVtbl->SetEventOnCompletion(fence, 1, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) { fprintf(stderr,"FAIL GPU timeout\n"); goto done; }
    range.End = (SIZE_T)rb.Width;
    CHECK(readback->lpVtbl->Map(readback, 0, &range, (void **)&pixels));
    for (stage = 0; stage < 5; ++stage) for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
        unsigned d = depths[(x+y)%8], s = (x*17+y*31)&255;
        unsigned char expected[4], *p = pixels + stride*stage + y*layout.Footprint.RowPitch + x*4;
        if (stage && x>=2 && x<7 && y>=1 && y<8) d = 0x123456;
        if (stage && x>=9 && y<4) s = 0xA5;
        if (stage >= 3) d = 0x400000;
        expected[0]=(unsigned char)(d>>8); expected[1]=(unsigned char)d;
        expected[2]=(unsigned char)s; expected[3]=(unsigned char)(d>>16);
        if (memcmp(p, expected, 4) && failures++ < 8)
            fprintf(stderr,"FAIL stage%u pixel%u,%u got%02X%02X%02X%02X expected%02X%02X%02X%02X\n",
                stage,x,y,p[0],p[1],p[2],p[3],expected[0],expected[1],expected[2],expected[3]);
    }
    for (stage = 0; stage < 2; ++stage) for (y = 0; y < H/2; ++y) for (x = 0; x < W; ++x) {
        const unsigned char expected[4] = {stage ? 0 : 255, stage ? 255 : 0, 0, 255};
        const unsigned char *p = pixels + stride*(stage+5) + y*color_layout.Footprint.RowPitch + x*4;
        if (memcmp(p, expected, 4) && failures++ < 8)
            fprintf(stderr,"FAIL colour stage%u pixel%u,%u\n",stage,x,y);
    }
    readback->lpVtbl->Unmap(readback, 0, NULL);
    printf("D3D12 depth sampling/binding: %u pixels, %d failures (legacy_clip=%d)\n",
        W*H*5 + W*(H/2)*2, failures, legacy_clip);
    result = failures ? 1 : 0;
done:
    if (event) CloseHandle(event);
    rsx_d3d12_depth_sample_release(&sample); rsx_d3d12_depth_sample_shutdown();
    for (stage = 0; stage < 3; ++stage) REL(draw_pso[stage]);
    REL(errors); REL(signature); REL(vs); REL(ps); REL(root); REL(color); REL(rtv_heap);
    REL(fence); REL(readback); REL(depth); REL(dsv_heap); REL(list); REL(allocator); REL(queue); REL(device);
    return result;
}
#else
int main(void) { return 0; }
#endif
