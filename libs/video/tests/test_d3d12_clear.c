/* Headless native regression test for rsx_d3d12_masked_clear. */
#ifdef _WIN32
#include "../rsx_d3d12_clear.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define W 8u
#define H 8u
static const IID iid_device = {0x189819f1,0x1db6,0x4b57,{0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7}};
static const IID iid_queue = {0x0ec870a6,0x5d7e,0x4c22,{0x8c,0xfc,0x5b,0xaa,0xe0,0x76,0x16,0xed}};
static const IID iid_resource = {0x696442be,0xa72e,0x4059,{0xbc,0x79,0x5b,0x5c,0x98,0x04,0x0f,0xad}};
static const IID iid_heap = {0x8efb471d,0x616c,0x4f49,{0x90,0xf7,0x12,0x7b,0xb7,0x63,0xfa,0x51}};
static const IID iid_allocator = {0x6102dee4,0xaf59,0x4b09,{0xb9,0x99,0xb4,0x4d,0x73,0xf0,0x9b,0x24}};
static const IID iid_list = {0x5b160d0f,0xac1b,0x4185,{0x8b,0xa8,0xb3,0xae,0x42,0xa5,0xa4,0x55}};
static const IID iid_fence = {0x0a753dcf,0xc4d8,0x4b91,{0xad,0xf6,0xbe,0x5a,0x60,0xd9,0x5a,0x76}};

#define REL(x) do { if (x) (x)->lpVtbl->Release(x); } while (0)
#define CHECK(x) do { HRESULT _hr=(x); if (FAILED(_hr)) { \
    fprintf(stderr, "FAIL %s 0x%08lX\n", #x, (unsigned long)_hr); goto done; } } while (0)

static void barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res; b.Transition.StateBefore = before;
    b.Transition.StateAfter = after; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->lpVtbl->ResourceBarrier(list, 1, &b);
}

int main(void)
{
    ID3D12Device *device = NULL; ID3D12CommandQueue *queue = NULL;
    ID3D12CommandAllocator *allocator = NULL; ID3D12GraphicsCommandList *list = NULL;
    ID3D12DescriptorHeap *rtv_heap = NULL; ID3D12Resource *texture = NULL, *readback = NULL;
    ID3D12Fence *fence = NULL; HANDLE event = NULL; D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_HEAP_PROPERTIES hp = {0}; D3D12_RESOURCE_DESC td = {0}, rb = {0};
    D3D12_COMMAND_QUEUE_DESC qd = {0}; D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout; UINT rows; UINT64 row_size, bytes;
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0}; D3D12_RANGE range = {0};
    D3D12_RECT center = {2,2,6,6}, red_blue = {0,0,2,2}, empty = {7,7,7,8};
    const float base[4] = {0.2f,0.4f,0.6f,0.8f};
    const float alpha[4] = {0.9f,0.1f,0.3f,0.25f};
    const float rb_color[4] = {0.9f,0.1f,0.7f,0.2f};
    unsigned char *pixels; unsigned x, y; int result = 1;

    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &iid_device, (void **)&device));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(device->lpVtbl->CreateCommandQueue(device, &qd, &iid_queue, (void **)&queue));
    CHECK(device->lpVtbl->CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   &iid_allocator, (void **)&allocator));
    CHECK(device->lpVtbl->CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocator, NULL, &iid_list, (void **)&list));
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = W; td.Height = H;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    CHECK(device->lpVtbl->CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &iid_resource, (void **)&texture));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    CHECK(device->lpVtbl->CreateDescriptorHeap(device, &hd, &iid_heap, (void **)&rtv_heap));
    rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv);
    device->lpVtbl->CreateRenderTargetView(device, texture, NULL, rtv);
    list->lpVtbl->ClearRenderTargetView(list, rtv, base, 0, NULL);
    if (rsx_d3d12_masked_clear(device, list, rtv, td.Format, W, H, &center, alpha,
                               D3D12_COLOR_WRITE_ENABLE_ALPHA) ||
        rsx_d3d12_masked_clear(device, list, rtv, td.Format, W, H, &red_blue, rb_color,
                               D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_BLUE) ||
        rsx_d3d12_masked_clear(device, list, rtv, td.Format, W, H, &empty, alpha,
                               D3D12_COLOR_WRITE_ENABLE_ALPHA) ||
        rsx_d3d12_masked_clear(device, list, rtv, td.Format, W, H, NULL, alpha, 0)) {
        fprintf(stderr, "FAIL masked-clear returned failure\n"); goto done;
    }
    device->lpVtbl->GetCopyableFootprints(device, &td, 0, 1, 0, &layout, &rows, &row_size, &bytes);
    hp.Type = D3D12_HEAP_TYPE_READBACK; rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width = bytes; rb.Height = 1; rb.DepthOrArraySize = 1; rb.MipLevels = 1;
    rb.SampleDesc.Count = 1; rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHECK(device->lpVtbl->CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rb,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &iid_resource, (void **)&readback));
    barrier(list, texture, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    src.pResource = texture; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;
    list->lpVtbl->CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(list->lpVtbl->Close(list));
    { ID3D12CommandList *lists[] = {(ID3D12CommandList *)list}; queue->lpVtbl->ExecuteCommandLists(queue, 1, lists); }
    CHECK(device->lpVtbl->CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &iid_fence, (void **)&fence));
    event = CreateEventW(NULL, FALSE, FALSE, NULL); if (!event) { fprintf(stderr, "FAIL CreateEvent\n"); goto done; }
    CHECK(queue->lpVtbl->Signal(queue, fence, 1));
    CHECK(fence->lpVtbl->SetEventOnCompletion(fence, 1, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) { fprintf(stderr, "FAIL GPU fence timeout\n"); goto done; }
    range.Begin = 0; range.End = (SIZE_T)bytes;
    CHECK(readback->lpVtbl->Map(readback, 0, &range, (void **)&pixels));
    for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) {
        unsigned char *p = pixels + (SIZE_T)y * layout.Footprint.RowPitch + x * 4;
        unsigned char expected[4] = {51, 102, 153, 204};
        if (x < 2 && y < 2) { expected[0] = 229; expected[2] = 178; }
        else if (x >= 2 && x < 6 && y >= 2 && y < 6) expected[3] = 64;
        if (memcmp(p, expected, 4)) {
            fprintf(stderr, "FAIL pixel %u,%u got %u,%u,%u,%u expected %u,%u,%u,%u\n", x,y,p[0],p[1],p[2],p[3],expected[0],expected[1],expected[2],expected[3]);
            readback->lpVtbl->Unmap(readback, 0, NULL); goto done;
        }
    }
    readback->lpVtbl->Unmap(readback, 0, NULL);
    printf("PASS d3d12 masked clear\n"); result = 0;
done:
    if (event) CloseHandle(event);
    rsx_d3d12_masked_clear_shutdown();
    REL(fence); REL(readback); REL(texture); REL(rtv_heap); REL(list); REL(allocator); REL(queue); REL(device);
    return result;
}
#else
int main(void) { return 0; }
#endif
