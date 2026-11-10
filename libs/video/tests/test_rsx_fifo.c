/*
 * ps3recomp - RSX FIFO parser test
 *
 * Verifies rsx_process_command_buffer: big-endian command-word decode and
 * method dispatch into the registered backend. This is the core of the
 * cellGcm -> RSX bridge (cellGcmSys's gcm_consume_fifo() just hands a
 * [get,put) slice of guest memory to this function).
 *
 * No D3D12: a tiny recording backend captures the dispatched calls.
 *
 * Build:
 *   gcc -std=c11 -O2 -I../../../include ../rsx_commands.c test_rsx_fifo.c -o t.exe
 */

#include "rsx_commands.h"
#include <stdio.h>
#include <string.h>

/* --- recording backend ------------------------------------------------- */
static int      rec_clear_calls;
static u32      rec_clear_color;
static u32      rec_clear_flags;
static float    rec_clear_depth;
static u8       rec_clear_stencil;
static int      rec_blend_calls;
static int      rec_blend_enable;
static int      rec_draw_calls;
static u32      rec_draw_prim, rec_draw_first, rec_draw_count;
#define REC_INDEXED_CAP 16
static int      rec_indexed_calls;
static u32      rec_indexed_prim[REC_INDEXED_CAP];
static u32      rec_indexed_first[REC_INDEXED_CAP];
static u32      rec_indexed_count[REC_INDEXED_CAP];
static int      rec_indexed_blend[REC_INDEXED_CAP];
static rsx_state* rec_live_state;

static void rb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    rec_clear_calls++;
    rec_clear_color = color;
    rec_clear_flags = flags;
    rec_clear_depth = depth;
    rec_clear_stencil = stencil;
}
static void rb_set_blend(void* ud, const rsx_state* s)
{ (void)ud; rec_blend_calls++; rec_blend_enable = s->blend_enable; }
static void rb_draw_arrays(void* ud, u32 prim, u32 first, u32 count)
{ (void)ud; rec_draw_calls++; rec_draw_prim = prim; rec_draw_first = first; rec_draw_count = count; }
static void rb_draw_indexed(void* ud, u32 prim, u32 first, u32 count)
{
    (void)ud;
    if (rec_indexed_calls < REC_INDEXED_CAP) {
        int i = rec_indexed_calls;
        rec_indexed_prim[i] = prim;
        rec_indexed_first[i] = first;
        rec_indexed_count[i] = count;
        rec_indexed_blend[i] = rec_live_state ? rec_live_state->blend_enable : -1;
    }
    rec_indexed_calls++;
}

static rsx_backend g_rec = {0};

/* The command processor links this backend-label write even though this
 * focused FIFO test never emits that method. */
void vm_write32(uint32_t address, uint32_t value)
{ (void)address; (void)value; }

/* --- big-endian FIFO writer -------------------------------------------- */
static u8  g_fifo[256];
static u32 g_fifo_len;
static u32 g_fifo_words[64];

static void put_be32(u32 v)
{
    g_fifo[g_fifo_len++] = (u8)(v >> 24);
    g_fifo[g_fifo_len++] = (u8)(v >> 16);
    g_fifo[g_fifo_len++] = (u8)(v >> 8);
    g_fifo[g_fifo_len++] = (u8)(v);
}

/* One increasing method + single data word (count = 1). */
static void emit_method(u32 method, u32 data)
{
    u32 header = (1u << 18) | (((method >> 2) & 0x7FF) << 2); /* type 0, count 1 */
    put_be32(header);
    put_be32(data);
}

static void emit_draw_indexed(u32 first, u32 count)
{
    emit_method(NV4097_DRAW_INDEX_ARRAY, ((count - 1) << 24) | first);
}

/* One type-2/non-increasing header followed by repeated data writes to the
 * same method, as emitted by the real FIFO walker. */
static void emit_nonincreasing(u32 method, const u32* data, u32 count)
{
    u32 header = (2u << 29) | (count << 18) | (((method >> 2) & 0x7FF) << 2);
    put_be32(header);
    for (u32 i = 0; i < count; ++i)
        put_be32(data[i]);
}

/* Type-0/increasing headers advance to method+4 for each payload. */
static void emit_increasing(u32 method, const u32* data, u32 count)
{
    u32 header = (count << 18) | (((method >> 2) & 0x7FF) << 2);
    put_be32(header);
    for (u32 i = 0; i < count; ++i)
        put_be32(data[i]);
}

static void reset_fifo(void)
{
    g_fifo_len = 0;
}

static void reset_indexed_recording(rsx_state* state)
{
    rec_indexed_calls = 0;
    memset(rec_indexed_prim, 0, sizeof(rec_indexed_prim));
    memset(rec_indexed_first, 0, sizeof(rec_indexed_first));
    memset(rec_indexed_count, 0, sizeof(rec_indexed_count));
    memset(rec_indexed_blend, 0, sizeof(rec_indexed_blend));
    rec_live_state = state;
}

/* rsx_process_command_buffer consumes host-order decoded command words; the
 * FIFO reader performs this conversion before handing it the guest bytes. */
static const u32* fifo_host_words(void)
{
    for (u32 i = 0; i < g_fifo_len / 4; ++i) {
        const u8* p = g_fifo + i * 4;
        g_fifo_words[i] = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                          ((u32)p[2] << 8) | p[3];
    }
    return g_fifo_words;
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("[PASS] %s\n", msg); } \
    else { printf("[FAIL] %s\n", msg); failures++; } } while (0)

int main(void)
{
    int failures = 0;

    g_rec.clear        = rb_clear;
    g_rec.set_blend    = rb_set_blend;
    g_rec.draw_arrays  = rb_draw_arrays;
    g_rec.draw_indexed = rb_draw_indexed;
    rsx_set_backend(&g_rec);

    rsx_state st;
    rsx_state_init(&st);

    int fp_local = -1;
    CHECK(rsx_shader_program_location_is_local(0x07607881u, &fp_local) && fp_local,
          "FP program location 1 decodes LOCAL");
    CHECK(rsx_shader_program_location_is_local(0x07607882u, &fp_local) && !fp_local,
          "FP program location 2 decodes MAIN");
    CHECK(!rsx_shader_program_location_is_local(0x07607880u, &fp_local) &&
          !rsx_shader_program_location_is_local(0x07607883u, &fp_local),
          "FP program reserved locations 0 and 3 reject");

    CHECK(st.surface_color_dma[0] == RSX_CONTEXT_DMA_LOCAL &&
          st.surface_color_dma[1] == RSX_CONTEXT_DMA_LOCAL &&
          st.surface_zeta_dma == RSX_CONTEXT_DMA_LOCAL,
          "surface DMA reset preserves LOCAL compatibility");

    reset_fifo();
    emit_method(NV4097_SET_CONTEXT_DMA_COLOR_B, RSX_CONTEXT_DMA_MAIN);
    emit_method(NV4097_SET_CONTEXT_DMA_COLOR_A, RSX_CONTEXT_DMA_LOCAL);
    emit_method(NV4097_SET_CONTEXT_DMA_ZETA,    RSX_CONTEXT_DMA_MAIN);
    emit_method(NV4097_SET_CONTEXT_DMA_COLOR_C, 0x12345678u);
    emit_method(NV4097_SET_CONTEXT_DMA_COLOR_D, RSX_CONTEXT_DMA_MAIN);
    int dma_n = rsx_process_command_buffer(&st, fifo_host_words(), g_fifo_len);
    CHECK(dma_n == 5 && st.surface_color_dma[0] == RSX_CONTEXT_DMA_LOCAL &&
          st.surface_color_dma[1] == RSX_CONTEXT_DMA_MAIN &&
          st.surface_color_dma[2] == 0x12345678u &&
          st.surface_color_dma[3] == RSX_CONTEXT_DMA_MAIN &&
          st.surface_zeta_dma == RSX_CONTEXT_DMA_MAIN,
          "surface context DMA methods decode A/B/C/D and zeta");
    CHECK(rsx_surface_dma_is_local(st.surface_color_dma[2]),
          "unknown surface DMA retains LOCAL compatibility");
    reset_fifo();

    /* Build a small frame: set clear color, clear, enable blend, draw a tri. */
    emit_method(NV4097_SET_COLOR_CLEAR_VALUE, 0xAABBCCDD);
    emit_method(NV4097_CLEAR_SURFACE,         0xF);
    emit_method(NV4097_SET_BLEND_ENABLE,      1);
    emit_method(NV4097_SET_BEGIN_END,         RSX_PRIMITIVE_TRIANGLES);
    emit_method(NV4097_DRAW_ARRAYS,           (2u << 24) | 0u); /* count 3, first 0 */
    emit_method(NV4097_SET_BEGIN_END,         0);

    int n = rsx_process_command_buffer(&st, fifo_host_words(), g_fifo_len);

    printf("methods processed: %d\n", n);
    CHECK(n == 6, "all 6 methods dispatched");
    CHECK(rec_clear_calls == 1 && rec_clear_color == 0xAABBCCDD,
          "clear dispatched with correct color (BE decode)");
    CHECK(rec_blend_calls == 1 && rec_blend_enable == 1,
          "set_blend flushed on BEGIN_END with enable=1");
    CHECK(rec_draw_calls == 1 && rec_draw_prim == RSX_PRIMITIVE_TRIANGLES &&
          rec_draw_first == 0 && rec_draw_count == 3,
          "draw_arrays dispatched (prim=5, first=0, count=3)");

    /* RSX stencil clears use the front write-mask gate, not the comparison
     * mask, back-face mask, stencil-test enable, or depth-write enable. */
    rsx_state clear_state;
    rsx_state_init(&clear_state);
    reset_fifo();
    emit_method(NV4097_SET_STENCIL_MASK, 0);
    emit_method(NV4097_SET_DEPTH_MASK, 0);
    emit_method(NV4097_SET_COLOR_CLEAR_VALUE, 0x11223344u);
    emit_method(NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF5Au);
    emit_method(NV4097_CLEAR_SURFACE, RSX_CLEAR_STENCIL_BIT);
    int clear_before = rec_clear_calls;
    CHECK(rsx_process_command_buffer(&clear_state, fifo_host_words(), g_fifo_len) == 5,
          "masked stencil-clear commands decoded");
    CHECK(rec_clear_calls == clear_before,
          "disabled stencil writes suppress stencil-only clear");

    clear_before = rec_clear_calls;
    rsx_process_method(&clear_state, NV4097_CLEAR_SURFACE, 0xF3u);
    CHECK(rec_clear_calls == clear_before + 1 && rec_clear_flags == 0xF1u &&
          rec_clear_color == 0x11223344u && rec_clear_depth == 1.0f &&
          rec_clear_stencil == 0x5Au,
          "masked stencil clear preserves requested depth/color and values");
    rsx_process_method(&clear_state, NV4097_CLEAR_SURFACE, RSX_CLEAR_DEPTH_BIT);
    CHECK(rec_clear_flags == RSX_CLEAR_DEPTH_BIT && rec_clear_depth == 1.0f,
          "depth clear ignores disabled depth writes");

    rsx_process_method(&clear_state, NV4097_SET_STENCIL_MASK, 0x100u);
    clear_before = rec_clear_calls;
    rsx_process_method(&clear_state, NV4097_CLEAR_SURFACE, RSX_CLEAR_STENCIL_BIT);
    CHECK(rec_clear_calls == clear_before,
          "only low eight stencil write-mask bits enable clear");

    clear_before = rec_clear_calls;
    rsx_process_method(&clear_state, NV4097_SET_STENCIL_MASK, 0xFFu);
    rsx_process_method(&clear_state, NV4097_SET_STENCIL_FUNC_MASK, 0);
    rsx_process_method(&clear_state, NV4097_SET_BACK_STENCIL_MASK, 0);
    rsx_process_method(&clear_state, NV4097_CLEAR_SURFACE, RSX_CLEAR_STENCIL_BIT);
    CHECK(rec_clear_calls == clear_before + 1 && rec_clear_flags == RSX_CLEAR_STENCIL_BIT,
          "comparison/back-face masks do not suppress front-enabled stencil clear");
    rsx_process_method(&clear_state, NV4097_SET_STENCIL_MASK, 0x0Fu);
    rsx_process_method(&clear_state, NV4097_SET_STENCIL_TEST_ENABLE, 0);
    rsx_process_method(&clear_state, NV4097_CLEAR_SURFACE, RSX_CLEAR_STENCIL_BIT);
    CHECK(rec_clear_calls == clear_before + 2 && rec_clear_flags == RSX_CLEAR_STENCIL_BIT,
          "nonzero front write mask permits clear while stencil test is disabled");

    clear_before = rec_clear_calls;
    rsx_process_method(&clear_state, NV4097_CLEAR_SURFACE, 0);
    rsx_process_method(&clear_state, NV4097_CLEAR_SURFACE, 0xCu);
    CHECK(rec_clear_calls == clear_before, "empty/reserved-only clear flags are a no-op");

    /* Consecutive indexed packets inside one triangle BEGIN stream must be
     * submitted as one contiguous range so the backend sees the real list. */
    rsx_state indexed;
    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    emit_draw_indexed(0, 40);
    emit_draw_indexed(40, 98);
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 1 && rec_indexed_prim[0] == RSX_PRIMITIVE_TRIANGLES &&
          rec_indexed_first[0] == 0 && rec_indexed_count[0] == 138,
          "indexed 40+98 coalesces to one 138-index backend call");

    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    {
        const u32 spans[] = { ((40 - 1) << 24) | 0, ((98 - 1) << 24) | 40 };
        emit_nonincreasing(NV4097_DRAW_INDEX_ARRAY, spans, 2);
    }
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 1 && rec_indexed_first[0] == 0 &&
          rec_indexed_count[0] == 138,
          "type-2 DRAW_INDEX_ARRAY packet coalesces repeated contiguous writes");

    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    {
        const u32 payload[] = { ((40 - 1) << 24) | 0, 0xDEADBEEFu };
        emit_increasing(NV4097_DRAW_INDEX_ARRAY, payload, 2);
    }
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 1 && rec_indexed_first[0] == 0 &&
          rec_indexed_count[0] == 40,
          "type-0 packet advances past DRAW_INDEX_ARRAY after its first payload");

    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    emit_draw_indexed(0, 56);
    emit_draw_indexed(56, 256);
    emit_draw_indexed(312, 256);
    emit_draw_indexed(568, 224);
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 1 && rec_indexed_first[0] == 0 &&
          rec_indexed_count[0] == 792,
          "indexed 56+256+256+224 coalesces to 792");

    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    emit_draw_indexed(0, 40);
    emit_draw_indexed(41, 98);
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 2 && rec_indexed_first[0] == 0 &&
          rec_indexed_count[0] == 40 && rec_indexed_first[1] == 41 &&
          rec_indexed_count[1] == 98,
          "noncontiguous indexed packet flushes then starts an exact new call");

    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    emit_draw_indexed(0, 40);
    emit_method(NV4097_SET_BLEND_ENABLE, 1);
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 1 && rec_indexed_count[0] == 40 &&
          rec_indexed_blend[0] == 0 && indexed.blend_enable == 1,
          "state method flushes indexed span before mutating blend state");

    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    emit_draw_indexed(0, 4);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLE_STRIP);
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 1 && rec_indexed_prim[0] == RSX_PRIMITIVE_TRIANGLES &&
          rec_indexed_count[0] == 4,
          "new BEGIN flushes old indexed span before changing primitive");

    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    emit_draw_indexed(0, 4);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x21);
    emit_draw_indexed(4, 4);
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123500);
    emit_draw_indexed(8, 4);
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 3 && rec_indexed_first[0] == 0 &&
          rec_indexed_first[1] == 4 && rec_indexed_first[2] == 8 &&
          rec_indexed_count[0] == 4 && rec_indexed_count[1] == 4 &&
          rec_indexed_count[2] == 4,
          "DMA and address changes prevent indexed-span merging");

    /* FIFO drains may end between a packet and END.  The pending span belongs
     * to the state object, so END in the next command-buffer segment flushes
     * it exactly once. */
    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    emit_draw_indexed(0, 40);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 0 && indexed.pending_indexed_valid,
          "cross-buffer pending indexed span remains live before END");
    reset_fifo();
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 1 && rec_indexed_first[0] == 0 &&
          rec_indexed_count[0] == 40 && !indexed.pending_indexed_valid,
          "cross-buffer END flushes the pending indexed span exactly once");

    /* A continuation packet in the next parser call must still merge with the
     * retained span, then END submits the complete logical triangle list. */
    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    reset_fifo();
    emit_method(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    emit_method(NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    emit_method(NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    emit_draw_indexed(0, 40);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 0 && indexed.pending_indexed_valid,
          "cross-buffer contiguous stream defers its first indexed packet");
    reset_fifo();
    emit_draw_indexed(40, 98);
    emit_method(NV4097_SET_BEGIN_END, 0);
    rsx_process_command_buffer(&indexed, fifo_host_words(), g_fifo_len);
    CHECK(rec_indexed_calls == 1 && rec_indexed_first[0] == 0 &&
          rec_indexed_count[0] == 138 && !indexed.pending_indexed_valid,
          "cross-buffer contiguous stream merges 40+98 before END");

    /* Keep one logical primitive stream beyond the upload-buffer capacity.
     * The backend's primitive-aware upload helpers clamp to available space;
     * the parser must not split at 65536 (not triangle-aligned). */
    rsx_state_init(&indexed);
    reset_indexed_recording(&indexed);
    rsx_process_method(&indexed, NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00123400);
    rsx_process_method(&indexed, NV4097_SET_INDEX_ARRAY_DMA, 0x11);
    rsx_process_method(&indexed, NV4097_SET_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    for (u32 i = 0; i < 256; ++i)
        rsx_process_method(&indexed, NV4097_DRAW_INDEX_ARRAY, (255u << 24) | (i * 256));
    rsx_process_method(&indexed, NV4097_DRAW_INDEX_ARRAY, (2u << 24) | 65536u);
    rsx_process_method(&indexed, NV4097_SET_BEGIN_END, 0);
    CHECK(rec_indexed_calls == 1 && rec_indexed_first[0] == 0 &&
          rec_indexed_count[0] == 65539,
          "oversized contiguous stream remains one backend call for topology-aware upload");

    /* TJ_TCONST_PROBE_C4_7 provenance: a real increasing FIFO method block
     * writes c4.xyzw then c5.xyzw.  The test sets the cached state gate
     * directly so it is deterministic regardless of the process environment. */
    rsx_state probe;
    rsx_state_init(&probe);
    probe.tconst_probe_enabled = 1;
    CHECK(probe.tconst_probe_method_sequence == 0 &&
          probe.tconst_probe_last_sequence[0][0] == 0,
          "tconst probe starts never-written with seq=0");
    const u32 values[8] = {
        0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u,
        0x40A00000u, 0x40C00000u, 0x40E00000u, 0x41000000u
    };
    const u32 probe_fifo[] = {
        (1u << 18) | (((NV4097_SET_TRANSFORM_CONSTANT_LOAD >> 2) & 0x7FF) << 2), 4,
        (8u << 18) | (((NV4097_SET_TRANSFORM_CONSTANT >> 2) & 0x7FF) << 2),
        values[0], values[1], values[2], values[3],
        values[4], values[5], values[6], values[7]
    };
    int probe_n = rsx_process_command_buffer(&probe, probe_fifo, sizeof(probe_fifo));
    CHECK(probe_n == 9 && probe.tconst_probe_method_sequence == 9 &&
          probe.tconst_probe_last_sequence[0][0] == 2 &&
          probe.tconst_probe_last_sequence[0][3] == 5 &&
          probe.tconst_probe_last_sequence[1][0] == 6 &&
          probe.tconst_probe_last_sequence[1][3] == 9,
          "tconst FIFO block records increasing per-method write sequences");
    CHECK(probe.vertex_constants[4][0] == 1.0f && probe.vertex_constants[5][3] == 8.0f &&
          probe.tconst_probe_last_raw[0][2] == values[2] &&
          probe.tconst_probe_last_load[1][1] == 4 &&
          probe.tconst_probe_last_slot[1][1] == 5 && probe.tconst_probe_last_lane[1][1] == 1 &&
          probe.tconst_probe_last_sequence[2][0] == 0 && probe.tconst_probe_last_sequence[3][3] == 0,
          "tconst records raw/load/resolved slot-lane and leaves c6-c7 never-written");
    rsx_process_method(&probe, NV4097_SET_TRANSFORM_CONSTANT_LOAD, 4);
    rsx_process_method(&probe, NV4097_SET_TRANSFORM_CONSTANT + 4, 0xBF800000u);
    CHECK(probe.tconst_probe_method_sequence == 11 && probe.vertex_constants[4][1] == -1.0f &&
          probe.tconst_probe_last_sequence[0][1] == 11 &&
          probe.tconst_probe_last_raw[0][1] == 0xBF800000u &&
          probe.tconst_probe_last_load[0][1] == 4 &&
          probe.tconst_probe_last_slot[0][1] == 4 && probe.tconst_probe_last_lane[0][1] == 1,
          "tconst overwrite keeps latest sequence and exact raw bits");

    rsx_state disabled;
    rsx_state_init(&disabled);
    disabled.tconst_probe_enabled = 0;
    rsx_process_method(&disabled, NV4097_SET_TRANSFORM_CONSTANT_LOAD, 4);
    rsx_process_method(&disabled, NV4097_SET_TRANSFORM_CONSTANT, 0x3F800000u);
    CHECK(disabled.tconst_probe_method_sequence == 0 &&
          disabled.tconst_probe_last_sequence[0][0] == 0 &&
          disabled.vertex_constants[4][0] == 1.0f,
          "tconst probe disabled leaves normal constant writes uninstrumented");

    printf("\n===========================================\n");
    printf("Results: %s (%d failure(s))\n", failures ? "FAIL" : "OK", failures);
    return failures ? 1 : 0;
}
