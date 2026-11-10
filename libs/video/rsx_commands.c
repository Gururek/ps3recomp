/*
 * ps3recomp - RSX Command Buffer Processor
 *
 * Parses NV47xx GPU methods from the command buffer and updates RSX state.
 * Dispatches to the registered graphics backend for actual rendering.
 *
 * Command buffer format (NV47xx FIFO):
 *   Each command is a 32-bit header followed by N data words.
 *   Header format: [31:29] type | [28:18] count | [17:13] subchannel | [12:2] method | [1:0] flags
 *
 *   Type 0 (increasing): method, method+4, method+8, ... for each data word
 *   Type 2 (non-increasing): same method repeated for each data word
 *   Type 1 (jump): jump to address in data
 *   Type 3 (call/return): call/return from subroutine
 */

#include "rsx_commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Global backend
 * -----------------------------------------------------------------------*/

static rsx_backend* s_backend = NULL;

/* ---------------------------------------------------------------------------
 * Immediate-mode draw assembly
 *
 * NV4097 can source a draw entirely from FIFO methods between BEGIN/END:
 * register vertex writes (ATTR0 completes a vertex), ARRAY_ELEMENT16/32, or
 * INLINE_ARRAY.  These are not constant-register updates only; games use them
 * for small UI/debug/overlay meshes.  Keep reusable host buffers here rather
 * than inflating rsx_state (several harnesses put that structure on the stack).
 * The backend consumes the published pointers synchronously at END.
 * -----------------------------------------------------------------------*/

#define RSX_IMMEDIATE_VERTEX_LIMIT 393216u
#define RSX_IMMEDIATE_INDEX_LIMIT  1048576u
#define RSX_INLINE_BYTE_LIMIT       (64u * 1024u * 1024u)

static rsx_state* s_immediate_owner;
static float* s_immediate_vertices; /* vertex-major [vertex][16][4] */
static u32 s_immediate_vertex_capacity;
static u32 s_immediate_vertex_count;
static u32 s_immediate_attr_count[RSX_MAX_VERTEX_ATTRIBS];
static u32* s_immediate_indices;
static u32 s_immediate_index_capacity;
static u32 s_immediate_index_count;
static u8* s_inline_bytes;
static u32 s_inline_byte_capacity;
static u32 s_inline_byte_count;
static int s_immediate_overflow;

static int rsx_grow_buffer(void** buffer, u32* capacity, u32 need,
                           u32 initial, u32 limit, size_t element_size)
{
    if (need <= *capacity) return 1;
    if (need > limit || element_size == 0) return 0;
    u32 next = *capacity ? *capacity : initial;
    while (next < need) {
        if (next > limit / 2u) { next = limit; break; }
        next *= 2u;
    }
    if (next < need || (size_t)next > (size_t)-1 / element_size) return 0;
    void* grown = realloc(*buffer, (size_t)next * element_size);
    if (!grown) return 0;
    *buffer = grown;
    *capacity = next;
    return 1;
}

static void rsx_immediate_unpublish(rsx_state* state)
{
    state->immediate_vertex_data = NULL;
    state->immediate_vertex_count = 0;
    state->immediate_vertex_mask = 0;
    state->immediate_index_data = NULL;
    state->immediate_index_count = 0;
}

static void rsx_immediate_begin(rsx_state* state)
{
    s_immediate_owner = state;
    s_immediate_vertex_count = 0;
    s_immediate_index_count = 0;
    s_inline_byte_count = 0;
    s_immediate_overflow = 0;
    memset(s_immediate_attr_count, 0, sizeof(s_immediate_attr_count));
    rsx_immediate_unpublish(state);
    state->immediate_draw_kind = 0;
}

static void rsx_immediate_attribute_complete(rsx_state* state, u32 attr)
{
    if (s_immediate_owner != state || !state->in_begin_end ||
        attr >= RSX_MAX_VERTEX_ATTRIBS ||
        !(state->vertex_attrib_input_mask & (1u << attr)))
        return;
    s_immediate_attr_count[attr]++;
    if (attr != 0 || s_immediate_overflow) return;
    if (!rsx_grow_buffer((void**)&s_immediate_vertices,
                         &s_immediate_vertex_capacity,
                         s_immediate_vertex_count + 1u, 256u,
                         RSX_IMMEDIATE_VERTEX_LIMIT,
                         sizeof(float) * RSX_MAX_VERTEX_ATTRIBS * 4u)) {
        s_immediate_overflow = 1;
        return;
    }
    memcpy(s_immediate_vertices +
               (size_t)s_immediate_vertex_count * RSX_MAX_VERTEX_ATTRIBS * 4u,
           state->vertex_data4f, sizeof(state->vertex_data4f));
    s_immediate_vertex_count++;
}

static void rsx_immediate_append_index(rsx_state* state, u32 index)
{
    if (s_immediate_owner != state || !state->in_begin_end ||
        s_immediate_overflow)
        return;
    if (!rsx_grow_buffer((void**)&s_immediate_indices,
                         &s_immediate_index_capacity,
                         s_immediate_index_count + 1u, 512u,
                         RSX_IMMEDIATE_INDEX_LIMIT, sizeof(u32))) {
        s_immediate_overflow = 1;
        return;
    }
    s_immediate_indices[s_immediate_index_count++] = index;
}

static void rsx_immediate_append_inline_word(rsx_state* state, u32 data)
{
    if (s_immediate_owner != state || !state->in_begin_end ||
        s_immediate_overflow)
        return;
    if (s_inline_byte_count > RSX_INLINE_BYTE_LIMIT - 4u ||
        !rsx_grow_buffer((void**)&s_inline_bytes, &s_inline_byte_capacity,
                         s_inline_byte_count + 4u, 4096u,
                         RSX_INLINE_BYTE_LIMIT, sizeof(u8))) {
        s_immediate_overflow = 1;
        return;
    }
    /* FIFO words have already been converted to host integers. Restore the
     * byte stream the RSX vertex fetcher sees (big-endian component order). */
    s_inline_bytes[s_inline_byte_count + 0] = (u8)(data >> 24);
    s_inline_bytes[s_inline_byte_count + 1] = (u8)(data >> 16);
    s_inline_bytes[s_inline_byte_count + 2] = (u8)(data >> 8);
    s_inline_bytes[s_inline_byte_count + 3] = (u8)data;
    s_inline_byte_count += 4u;
    state->immediate_draw_kind = 3;
}

static u16 rsx_inline_read16(const u8* p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static u32 rsx_inline_read32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | p[3];
}

static float rsx_inline_half(u16 h)
{
    u32 sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    u32 bits;
    if (!exp) {
        if (!man) bits = sign << 31;
        else {
            int e = -14;
            while (!(man & 0x400u)) { man <<= 1; e--; }
            man &= 0x3FFu;
            bits = (sign << 31) | ((u32)(e + 127) << 23) | (man << 13);
        }
    } else if (exp == 31u) {
        bits = (sign << 31) | 0x7F800000u | (man << 13);
    } else {
        bits = (sign << 31) | ((exp - 15u + 127u) << 23) | (man << 13);
    }
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static u32 rsx_inline_attrib_bytes(const rsx_vertex_attrib* attr)
{
    u32 n = attr->size;
    if (!n || n > 4u) return 0;
    switch (attr->type) {
    case 1: case 3: case 5: return (n == 3u ? 4u : n) * 2u;
    case 2: return n * 4u;
    case 4: case 7: return n == 3u ? 4u : n;
    case 6: return 4u;
    default: return 0;
    }
}

static void rsx_inline_decode_attrib(const rsx_vertex_attrib* attr,
                                     const u8* src, float out[4])
{
    u32 n = attr->size > 4u ? 4u : attr->size;
    switch (attr->type) {
    case 1: /* signed normalized short */
        for (u32 i = 0; i < n; i++)
            out[i] = ((float)(s16)rsx_inline_read16(src + i * 2u) + 0.5f) /
                     32767.5f;
        break;
    case 2: /* float */
        for (u32 i = 0; i < n; i++) {
            u32 bits = rsx_inline_read32(src + i * 4u);
            memcpy(&out[i], &bits, sizeof(bits));
        }
        break;
    case 3: /* half */
        for (u32 i = 0; i < n; i++)
            out[i] = rsx_inline_half(rsx_inline_read16(src + i * 2u));
        break;
    case 4: /* unsigned byte normalized */
        for (u32 i = 0; i < n; i++) out[i] = (float)src[i] / 255.0f;
        break;
    case 5: /* signed short integer */
        for (u32 i = 0; i < n; i++)
            out[i] = (float)(s16)rsx_inline_read16(src + i * 2u);
        break;
    case 6: { /* signed normalized 11:11:10 */
        u32 packed = rsx_inline_read32(src);
        s16 x = (s16)((packed & 0x7FFu) << 5);
        s16 y = (s16)(((packed >> 11) & 0x7FFu) << 5);
        s16 z = (s16)(((packed >> 22) & 0x3FFu) << 6);
        out[0] = (float)x / 32767.0f;
        out[1] = (float)y / 32767.0f;
        out[2] = (float)z / 32767.0f;
        out[3] = 1.0f;
        break; }
    case 7: /* unsigned byte integer */
        for (u32 i = 0; i < n; i++) out[i] = (float)src[i];
        break;
    }
}

static int rsx_immediate_decode_inline(rsx_state* state)
{
    u32 offsets[RSX_MAX_VERTEX_ATTRIBS];
    u32 stride = 0, mask = 0;
    for (u32 i = 0; i < RSX_MAX_VERTEX_ATTRIBS; i++) {
        offsets[i] = stride;
        if (!state->vertex_attribs[i].enabled) continue;
        u32 bytes = rsx_inline_attrib_bytes(&state->vertex_attribs[i]);
        if (!bytes || stride > 0xFFFFFFFFu - bytes) return 0;
        stride += bytes;
        if (state->vertex_attrib_input_mask & (1u << i))
            mask |= 1u << i;
    }
    if (!stride || !s_inline_byte_count) return 0;
    u32 vertices = s_inline_byte_count / stride;
    if (!vertices ||
        !rsx_grow_buffer((void**)&s_immediate_vertices,
                         &s_immediate_vertex_capacity, vertices, 256u,
                         RSX_IMMEDIATE_VERTEX_LIMIT,
                         sizeof(float) * RSX_MAX_VERTEX_ATTRIBS * 4u))
        return 0;

    for (u32 v = 0; v < vertices; v++) {
        float* dst = s_immediate_vertices +
                     (size_t)v * RSX_MAX_VERTEX_ATTRIBS * 4u;
        memcpy(dst, state->vertex_data4f, sizeof(state->vertex_data4f));
        const u8* src = s_inline_bytes + (size_t)v * stride;
        for (u32 i = 0; i < RSX_MAX_VERTEX_ATTRIBS; i++) {
            if (!(mask & (1u << i))) continue;
            rsx_inline_decode_attrib(&state->vertex_attribs[i],
                                     src + offsets[i], dst + i * 4u);
        }
    }
    s_immediate_vertex_count = vertices;
    state->immediate_vertex_data = s_immediate_vertices;
    state->immediate_vertex_count = vertices;
    state->immediate_vertex_mask = mask;
    return 1;
}

static void rsx_immediate_publish_registers(rsx_state* state)
{
    if (!s_immediate_vertex_count) return;
    u32 mask = 0;
    for (u32 i = 0; i < RSX_MAX_VERTEX_ATTRIBS; i++)
        if (s_immediate_attr_count[i] > 1u &&
            (state->vertex_attrib_input_mask & (1u << i)))
            mask |= 1u << i;
    state->immediate_vertex_data = s_immediate_vertices;
    state->immediate_vertex_count = s_immediate_vertex_count;
    state->immediate_vertex_mask = mask;
}

static void rsx_immediate_finish(rsx_state* state)
{
    if (s_immediate_owner != state) return;
    if (s_immediate_overflow) {
        static u32 reports;
        if (reports++ < 8u)
            fprintf(stderr, "[RSX] immediate draw exceeded host staging limit; skipped\n");
    } else if (state->immediate_draw_kind == 3) {
        if (rsx_immediate_decode_inline(state) && s_backend && s_backend->draw_arrays)
            s_backend->draw_arrays(s_backend->userdata, state->primitive_type,
                                   0, state->immediate_vertex_count);
    } else if (state->immediate_draw_kind == 0 && s_immediate_index_count) {
        rsx_immediate_publish_registers(state);
        state->immediate_index_data = s_immediate_indices;
        state->immediate_index_count = s_immediate_index_count;
        if (s_backend && s_backend->draw_indexed)
            s_backend->draw_indexed(s_backend->userdata, state->primitive_type,
                                    0, s_immediate_index_count);
    } else if (state->immediate_draw_kind == 0 && s_immediate_vertex_count) {
        rsx_immediate_publish_registers(state);
        if (s_backend && s_backend->draw_arrays)
            s_backend->draw_arrays(s_backend->userdata, state->primitive_type,
                                   0, s_immediate_vertex_count);
    }
    rsx_immediate_unpublish(state);
    state->immediate_draw_kind = 0;
    s_immediate_owner = NULL;
}

/* Submit a deferred DRAW_INDEX_ARRAY span before the state it depends on can
 * change.  This deliberately calls the backend directly rather than routing
 * through method processing, so a flush cannot recurse. */
static void rsx_flush_pending_indexed(rsx_state* state)
{
    if (!state->pending_indexed_valid)
        return;
    if (s_backend && s_backend->draw_indexed) {
        s_backend->draw_indexed(s_backend->userdata,
                                state->pending_indexed_primitive,
                                state->pending_indexed_first,
                                state->pending_indexed_count);
    }
    state->pending_indexed_valid = 0;
}

void rsx_flush_pending_draws(rsx_state* state)
{
    if (state) rsx_flush_pending_indexed(state);
}

/* Optional rolling command trace.  Unlike YDKJ_RSXTRACE, this preserves only
 * the methods immediately preceding a draw, which is useful for identifying
 * inline/immediate vertex submission without flooding the log. */
typedef struct rsx_trace_entry {
    u32 method;
    u32 data;
} rsx_trace_entry;

#define RSX_DRAW_TRACE_CAP 128
static rsx_trace_entry s_draw_trace[RSX_DRAW_TRACE_CAP];
static u32 s_draw_trace_head;
static u32 s_draw_trace_count;

static int rsx_draw_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RSX_DRAWTRACE") ? 1 : 0;
    return enabled;
}

static void rsx_draw_trace_push(u32 method, u32 data)
{
    if (!rsx_draw_trace_enabled()) return;
    s_draw_trace[s_draw_trace_head].method = method;
    s_draw_trace[s_draw_trace_head].data = data;
    s_draw_trace_head = (s_draw_trace_head + 1) % RSX_DRAW_TRACE_CAP;
    if (s_draw_trace_count < RSX_DRAW_TRACE_CAP) s_draw_trace_count++;
}

static void rsx_draw_trace_dump(void)
{
    static u32 dumps;
    if (!rsx_draw_trace_enabled() || dumps++ >= 4) return;
    u32 start = (s_draw_trace_head + RSX_DRAW_TRACE_CAP - s_draw_trace_count)
              % RSX_DRAW_TRACE_CAP;
    fprintf(stderr, "[RSXDRAWTRACE] draw=%u prior_methods=%u\n",
            dumps - 1, s_draw_trace_count);
    for (u32 i = 0; i < s_draw_trace_count; i++) {
        const rsx_trace_entry* e =
            &s_draw_trace[(start + i) % RSX_DRAW_TRACE_CAP];
        fprintf(stderr, "[RSXDRAWTRACE] %03u method=0x%04X data=0x%08X\n",
                i, e->method, e->data);
    }
}

/* Last NV406E_SET_REFERENCE value seen in the FIFO. The title writes a
 * reference then spins on the GCM control register's `ref` field until the RSX
 * reports it as reached; cellGcmSys mirrors this into the guest control
 * register after each drain so those waits (cellGcmFinish / wait-label) unblock. */
u32 g_rsx_last_reference = 0;

void rsx_set_backend(rsx_backend* backend)
{
    s_backend = backend;
}

rsx_backend* rsx_get_backend(void)
{
    return s_backend;
}

/* ---------------------------------------------------------------------------
 * State initialization
 * -----------------------------------------------------------------------*/

void rsx_state_init(rsx_state* state)
{
    memset(state, 0, sizeof(rsx_state));
    /* Surface context-DMA reset/default: legacy handling was LOCAL. Keep the
     * actual handles so a later method can select MAIN without changing raw
     * offsets, while unprogrammed surfaces preserve that compatibility. */
    for (u32 i = 0; i < RSX_MAX_RENDER_TARGETS; ++i)
        state->surface_color_dma[i] = RSX_CONTEXT_DMA_LOCAL;
    state->surface_zeta_dma = RSX_CONTEXT_DMA_LOCAL;
    /* RSX reset value for constant/current vertex attributes is (0,0,0,1). */
    for (u32 i = 0; i < RSX_MAX_VERTEX_ATTRIBS; i++)
        state->vertex_data4f[i][3] = 1.0f;
    { const char* probe = getenv("TJ_TCONST_PROBE_C4_7");
      state->tconst_probe_enabled = probe && strcmp(probe, "1") == 0; }

    /* Exact NV4097 reset viewport transform. D3D handles x/y through its
     * viewport, but the generated VP epilogue consumes z scale/offset to map
     * RSX's [-w,+w] clip depth into D3D's [0,+w] interval. Leaving these
     * zeroed made every draw before the guest's first viewport upload use an
     * identity z transform instead of the hardware's 0.5/0.5 mapping. */
    state->viewport_w = 1280;
    state->viewport_h = 720;
    state->viewport_offset[0] = 2048.0f;
    state->viewport_offset[1] = 2048.0f;
    state->viewport_offset[2] = 0.5f;
    state->viewport_scale[0] = 2048.0f;
    state->viewport_scale[1] = 2048.0f;
    state->viewport_scale[2] = 0.5f;
    state->clip_min = 0.0f;
    state->clip_max = 1.0f;

    /* Default scissor */
    state->scissor_w = 4096;
    state->scissor_h = 4096;

    state->fog_mode = 0x0800u; /* EXP */
    state->fog_param[0] = 0.0f;
    state->fog_param[1] = 0.0f;

    /* Default depth */
    state->depth_func = 0x0201; /* LESS */
    state->depth_mask = 1;
    state->depth_bounds_test_enable = 0;
    state->depth_bounds_min = 0.0f;
    state->depth_bounds_max = 1.0f;
    state->zmin_max_control = 1; /* depth clipping enabled */

    /* Exact NV4097 reset blend state. */
    state->blend_sfactor = 0x00010001; /* ONE, ONE */
    state->blend_dfactor = 0x00000000; /* ZERO, ZERO */
    state->blend_equation = 0x80068006; /* ADD, ADD */
    state->logic_op = 0x1503; /* COPY */
    state->restart_index = 0xFFFFFFFFu;

    state->clip_min = 0.0f;
    state->clip_max = 1.0f;

    /* Default cull */
    state->cull_face = 0x0405; /* BACK */
    state->front_face = 0x0901; /* CCW */
    state->front_polygon_mode = 0x1B02; /* FILL */
    state->back_polygon_mode = 0x1B02;  /* FILL */
    state->shade_mode = 0x1D01;         /* SMOOTH */

    /* Default color mask: all channels writable (A|R|G|B) */
    state->color_mask = 0x01010101;
    state->color_mask_mrt = 0;

    /* Default stencil: sensible initial values */
    state->stencil_write_mask = 0xFF;
    state->stencil_func = 0x0207; /* ALWAYS */
    state->stencil_ref = 0;
    state->stencil_mask = 0xFF;
    state->stencil_op_fail = 0x1E00;  /* KEEP */
    state->stencil_op_zfail = 0x1E00; /* KEEP */
    state->stencil_op_zpass = 0x1E00; /* KEEP */
    state->back_stencil_write_mask = 0xFF;
    state->back_stencil_func = 0x0207; /* ALWAYS */
    state->back_stencil_ref = 0;
    state->back_stencil_mask = 0xFF;
    state->back_stencil_op_fail = 0x1E00;  /* KEEP */
    state->back_stencil_op_zfail = 0x1E00; /* KEEP */
    state->back_stencil_op_zpass = 0x1E00; /* KEEP */

    /* NV4097 reset values. Unprogrammed texture fields still participate in
     * sampling once CONTROL0 enables a unit, so zero-filled wrap/filter state
     * is not a valid hardware default. */
    for (u32 i = 0; i < RSX_MAX_TEXTURES; i++) {
        state->textures[i].format = 0x00018429;
        state->textures[i].address = 0x00030101;
        state->textures[i].control0 = 0x00060000;
        state->textures[i].control1 = 0x0000AAE4;
        state->textures[i].filter = 0x02052000;
        state->textures[i].image_rect = 0x00080008;
        state->textures[i].pitch = 8;
        state->textures[i].depth = 1;
    }
    for (u32 i = 0; i < RSX_MAX_VERTEX_TEXTURES; i++) {
        /* NV4097 reset: one linear X32F mip, 2D, LOCAL, 8x8, pitch 8. */
        state->vertex_textures[i].format = 0x0001BC21;
        state->vertex_textures[i].address = 0x00000101;
        state->vertex_textures[i].control0 = 0x00060000;
        state->vertex_textures[i].control1 = 0x0000AAE4;
        state->vertex_textures[i].filter = 0;
        state->vertex_textures[i].image_rect = 0x00080008;
        state->vertex_textures[i].pitch = 8;
        state->vertex_textures[i].depth = 1;
    }

    /* Default alpha test */
    state->alpha_func = 0x0207; /* ALWAYS */
    state->alpha_ref = 0;

    /* Default shader control: 32-bit colour exports (r0..) -- matches every
     * title that never programs the register. */
    state->shader_control = CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    state->shader_packer = 0;
    state->shader_window = 0x1000u;
    state->vertex_attrib_input_mask = 0xFFFFu;
    state->vertex_attrib_output_mask = 0x003FFFFFu;
    state->user_clip_plane_control = 0;

    /* Mark everything dirty */
    state->surface_dirty = 1;
    state->viewport_dirty = 1;
    state->blend_dirty = 1;
    state->depth_dirty = 1;
    state->stencil_dirty = 1;
    state->texture_dirty = 1;
    state->vertex_texture_dirty = 1;
    state->vertex_dirty = 1;
    state->color_mask_dirty = 1;
    state->alpha_dirty = 1;
    state->shader_dirty = 1;
}

/* Reset the supported NV4097 context registers at the HLE flip boundary.
 * Reference: RPCS3 9b5cc7cda7821b3819a1f6d1b8fedb974bebf5fa,
 * Emu/RSX/rsx_methods.cpp, rsx_state::reset. This is NOT state initialization:
 * shader addresses/control, program RAM/load cursors, constants/current vertex
 * values, index bindings and texture storage/remap registers survive.
 * Flush first so the final deferred draw retains its original state; backend
 * draw records own their render-state snapshots before these defaults change. */
void rsx_state_reset(rsx_state* state)
{
    if (!state) return;
    rsx_flush_pending_draws(state);

    state->surface_format = 0x121;
    state->color_target = 1;
    for (u32 i = 0; i < RSX_MAX_RENDER_TARGETS; ++i) {
        state->surface_color_offset[i] = 0;
        state->surface_color_pitch[i] = 0x40;
        state->surface_color_dma[i] = RSX_CONTEXT_DMA_LOCAL;
    }
    state->surface_zeta_offset = 0;
    state->surface_zeta_pitch = 0x40;
    state->surface_zeta_dma = RSX_CONTEXT_DMA_LOCAL;
    state->surface_clip_x = state->surface_clip_y = 0;
    state->surface_clip_w = state->surface_clip_h = 0;
    state->window_offset_x = state->window_offset_y = 0;
    state->viewport_x = state->viewport_y = 0;
    state->viewport_w = state->viewport_h = 4096;
    state->viewport_offset[0] = state->viewport_offset[1] = 2048.0f;
    state->viewport_scale[0] = state->viewport_scale[1] = 2048.0f;
    state->viewport_offset[2] = state->viewport_scale[2] = 0.5f;
    state->viewport_offset[3] = state->viewport_scale[3] = 0.0f;
    state->clip_min = 0.0f;
    state->clip_max = 1.0f;
    state->scissor_x = state->scissor_y = 0;
    state->scissor_w = state->scissor_h = 4096;
    state->fog_mode = 0x800;
    state->fog_param[0] = state->fog_param[1] = 0.0f;
    state->color_clear_value = 0;
    state->zstencil_clear_value = 0xFFFFFF00u;

    state->depth_test_enable = 0;
    state->depth_func = 0x201;
    state->depth_mask = 1;
    state->depth_bounds_test_enable = 0;
    state->depth_bounds_min = 0.0f;
    state->depth_bounds_max = 1.0f;
    state->poly_offset_point_enable = 0;
    state->poly_offset_line_enable = 0;
    state->poly_offset_fill_enable = 0;
    state->polygon_offset_scale_factor = state->polygon_offset_bias = 0.0f;
    state->zmin_max_control = 1;

    state->blend_enable = 0;
    state->blend_enable_mrt = 0;
    state->blend_sfactor = 0x00010001;
    state->blend_dfactor = 0;
    state->blend_equation = 0x80068006;
    state->blend_color = state->blend_color2 = 0;
    state->logic_op_enable = 0;
    state->logic_op = 0x1503;
    state->stencil_test_enable = 0;
    state->two_sided_stencil_test_enable = 0;
    state->stencil_write_mask = state->back_stencil_write_mask = 0xFF;
    state->stencil_func = state->back_stencil_func = 0x207;
    state->stencil_ref = state->back_stencil_ref = 0;
    state->stencil_mask = state->back_stencil_mask = 0xFF;
    state->stencil_op_fail = state->back_stencil_op_fail = 0x1E00;
    state->stencil_op_zfail = state->back_stencil_op_zfail = 0x1E00;
    state->stencil_op_zpass = state->back_stencil_op_zpass = 0x1E00;
    state->cull_face_enable = 0;
    state->cull_face = 0x405;
    state->front_face = 0x901;
    state->front_polygon_mode = state->back_polygon_mode = 0x1B02;
    state->shade_mode = 0x1D01;
    state->color_mask = 0x01010101;
    state->color_mask_mrt = 0;
    state->alpha_test_enable = 0;
    state->alpha_func = 0x207;
    state->alpha_ref = 0;

    for (u32 i = 0; i < RSX_MAX_TEXTURES; ++i) {
        state->textures[i].address = 0x30101;
        state->textures[i].control0 = 0x60000;
        state->textures[i].filter = 0x2052000;
        state->textures[i].border_color = 0;
        state->textures[i].dirty = 1;
    }
    for (u32 i = 0; i < RSX_MAX_VERTEX_TEXTURES; ++i) {
        state->vertex_textures[i].address = 0x101;
        state->vertex_textures[i].control0 = 0x60000;
        state->vertex_textures[i].filter = 0;
        state->vertex_textures[i].border_color = 0;
        state->vertex_textures[i].dirty = 1;
    }
    for (u32 i = 0; i < RSX_MAX_VERTEX_ATTRIBS; ++i) {
        rsx_vertex_attrib* attr = &state->vertex_attribs[i];
        attr->format = attr->type = 2;
        attr->offset = attr->stride = attr->size = attr->frequency = 0;
        attr->enabled = 0;
    }
    state->vertex_data_base_offset = state->vertex_data_base_index = 0;
    state->frequency_divider_op = 0;
    state->restart_index_enable = 0;
    state->restart_index = 0xFFFFFFFFu;
    state->shader_packer = 0;
    state->shader_window = 0x1000;
    state->texcoord_control_mask = 0;
    state->two_side_light_enable = 0;
    state->vertex_attrib_input_mask = 0xFFFF;
    state->vertex_attrib_output_mask = 0x3FFFFF;
    state->user_clip_plane_control = 0;
    state->transform_branch_bits = 0;

    state->surface_dirty = state->viewport_dirty = state->blend_dirty = 1;
    state->depth_dirty = state->stencil_dirty = state->texture_dirty = 1;
    state->vertex_texture_dirty = state->vertex_dirty = 1;
    state->color_mask_dirty = state->alpha_dirty = state->shader_dirty = 1;
}

/* ---------------------------------------------------------------------------
 * Method processing
 * -----------------------------------------------------------------------*/

static int process_surface_method(rsx_state* state, u32 method, u32 data)
{
    switch (method) {
    case NV4097_SET_CONTEXT_DMA_COLOR_A:
        state->surface_color_dma[0] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_CONTEXT_DMA_COLOR_B:
        state->surface_color_dma[1] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_CONTEXT_DMA_COLOR_C:
        state->surface_color_dma[2] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_CONTEXT_DMA_COLOR_D:
        state->surface_color_dma[3] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_CONTEXT_DMA_ZETA:
        state->surface_zeta_dma = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_FORMAT:
        state->surface_format = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_CLIP_HORIZONTAL:
        state->surface_clip_x = data & 0xFFFF;
        state->surface_clip_w = (data >> 16) & 0xFFFF;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_CLIP_VERTICAL:
        state->surface_clip_y = data & 0xFFFF;
        state->surface_clip_h = (data >> 16) & 0xFFFF;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_AOFFSET:
        state->surface_color_offset[0] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_BOFFSET:
        state->surface_color_offset[1] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_COFFSET:
        state->surface_color_offset[2] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_DOFFSET:
        state->surface_color_offset[3] = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_WINDOW_OFFSET:
        state->window_offset_x = data & 0xFFFF;
        state->window_offset_y = (data >> 16) & 0xFFFF;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_ZETA_OFFSET:
        state->surface_zeta_offset = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_COLOR_TARGET:
        { static int _ct=0; if (_ct++ < 24 && getenv("RTT_DUMP"))
            fprintf(stderr, "[RSXCT] color_target=0x%X offA=0x%X offB=0x%X offC=0x%X offD=0x%X%s",
                    data, state->surface_color_offset[0], state->surface_color_offset[1],
                    state->surface_color_offset[2], state->surface_color_offset[3], "\n"); }
        state->color_target = data;
        state->surface_dirty = 1;
        return 0;
    case NV4097_SET_SURFACE_PITCH_A:
        state->surface_color_pitch[0] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_B:
        state->surface_color_pitch[1] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_C:
        state->surface_color_pitch[2] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_D:
        state->surface_color_pitch[3] = data;
        return 0;
    case NV4097_SET_SURFACE_PITCH_Z:
        state->surface_zeta_pitch = data;
        return 0;
    default:
        return -1;
    }
}

/* ---------------------------------------------------------------------------
 * Texture method processing
 *
 * Texture registers are laid out in 16 units, each spanning 0x20 bytes:
 *   Unit 0: 0x1A00..0x1A1C
 *   Unit 1: 0x1A20..0x1A3C
 *   ...
 *   Unit N: 0x1A00 + N*0x20 .. 0x1A1C + N*0x20
 * -----------------------------------------------------------------------*/

static int process_texture_method(rsx_state* state, u32 method, u32 data)
{
    /* Compute texture unit index and register offset within the unit */
    u32 base = method - NV4097_SET_TEXTURE_OFFSET; /* 0x1A00 */
    u32 unit = base / 0x20;
    u32 reg  = base % 0x20;

    if (unit >= RSX_MAX_TEXTURES)
        return -1;

    rsx_texture_state* tex = &state->textures[unit];

    switch (reg) {
    case 0x00: /* TEXTURE_OFFSET */
        /* Bit 31 is not part of the fragment-texture byte offset. Keep the
         * canonical 31-bit identity just like the vertex-texture bank. */
        tex->offset = data & 0x7FFFFFFFu;
        break;
    case 0x04: /* TEXTURE_FORMAT */
        tex->format = data;
        break;
    case 0x08: /* TEXTURE_ADDRESS (wrap S/T/R) */
        tex->address = data;
        break;
    case 0x0C: /* TEXTURE_CONTROL0 (enable, min/max LOD, max aniso) */
        tex->control0 = data;
        break;
    case 0x10: /* TEXTURE_CONTROL1 (remap) */
        tex->control1 = data;
        break;
    case 0x14: /* TEXTURE_FILTER (bias, min/mag filter) */
        tex->filter = data;
        break;
    case 0x18: /* TEXTURE_IMAGE_RECT (width << 16 | height) */
        tex->image_rect = data;
        break;
    case 0x1C: /* TEXTURE_BORDER_COLOR */
        tex->border_color = data;
        break;
    default:
        return -1;
    }

    tex->dirty = 1;
    state->texture_dirty = 1;
    return 0;
}

static int process_texture_size1_method(rsx_state* state, u32 method, u32 data)
{
    u32 unit = (method - NV4097_SET_TEXTURE_SIZE1) / 4;
    if (unit >= RSX_MAX_TEXTURES)
        return -1;

    rsx_texture_state* tex = &state->textures[unit];
    tex->pitch = data & 0xFFFFu;
    tex->depth = (data >> 20) & 0xFFFu;
    tex->dirty = 1;
    state->texture_dirty = 1;
    return 0;
}

static int process_vertex_texture_method(rsx_state* state, u32 method, u32 data)
{
    u32 base = method - NV4097_SET_VERTEX_TEXTURE_OFFSET;
    u32 unit = base / 0x20u;
    u32 reg = base % 0x20u;
    if (unit >= RSX_MAX_VERTEX_TEXTURES) return -1;

    rsx_texture_state* tex = &state->vertex_textures[unit];
    switch (reg) {
    case 0x00: tex->offset = data & 0x7FFFFFFFu; break;
    case 0x04: tex->format = data; break;
    case 0x08: tex->address = data; break;
    case 0x0C: tex->control0 = data; break;
    case 0x10: /* CONTROL3: pitch[19:0], depth[31:20]. */
        tex->pitch = data & 0xFFFFFu;
        tex->depth = (data >> 20) & 0xFFFu;
        if (!tex->depth) tex->depth = 1;
        break;
    case 0x14: tex->filter = data; break;
    case 0x18: tex->image_rect = data; break;
    case 0x1C: tex->border_color = data; break;
    default: return -1;
    }
    tex->dirty = 1;
    state->vertex_texture_dirty = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Vertex attribute method processing
 *
 * FORMAT registers: 0x1740 + attrib*4  (16 attributes)
 * OFFSET registers: 0x1680 + attrib*4  (16 attributes)
 * -----------------------------------------------------------------------*/

static int process_vertex_attrib_method(rsx_state* state, u32 method, u32 data)
{
    if (method >= NV4097_SET_VERTEX_DATA_ARRAY_FORMAT &&
        method < NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + RSX_MAX_VERTEX_ATTRIBS * 4) {
        u32 index = (method - NV4097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4;
        rsx_vertex_attrib* attr = &state->vertex_attribs[index];

        /*
         * Format register layout:
         *   [3:0]   type   (float, half, ubyte, short, etc.)
         *   [7:4]   size   (number of components: 1-4)
         *   [15:8]  stride (bytes between consecutive elements)
         *   [31:16] frequency divisor
         */
        attr->type      = data & 0xF;
        attr->size      = (data >> 4) & 0xF;
        attr->stride    = (data >> 8) & 0xFF;
        attr->frequency = (data >> 16) & 0xFFFF;   /* instancing divisor */
        /* Component count enables the array. PSGL can leave a valid type in an
         * unused slot while writing size 0; type-based enablement then feeds a
         * degenerate array instead of the constant/current attribute. */
        attr->enabled   = (attr->size != 0);
        attr->format    = data;
        state->vertex_dirty = 1;
        return 0;
    }

    if (method >= NV4097_SET_VERTEX_DATA_ARRAY_OFFSET &&
        method < NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + RSX_MAX_VERTEX_ATTRIBS * 4) {
        u32 index = (method - NV4097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4;
        state->vertex_attribs[index].offset = data;
        state->vertex_dirty = 1;
        return 0;
    }

    return -1;
}

static void current_vertex_reset(rsx_state* state, u32 attr)
{
    state->vertex_data4f[attr][0] = 0.0f;
    state->vertex_data4f[attr][1] = 0.0f;
    state->vertex_data4f[attr][2] = 0.0f;
    state->vertex_data4f[attr][3] = 1.0f;
}

static float current_vertex_snorm16(u16 raw)
{
    s16 value = (s16)raw;
    return ((float)value + 0.5f) / 32767.5f;
}

/* Register vertex methods define both the disabled-array constant and, inside
 * BEGIN/END, an immediate push-buffer stream.  Return the completed attribute
 * in completed_attr; multiword formats complete only on their last data lane.
 * ATTR0 completion is the hardware's vertex boundary. */
static int process_current_vertex_method(rsx_state* state, u32 method, u32 data,
                                         u32* completed_attr)
{
    u32 word, attr, lane;
    int complete = 0;
    *completed_attr = ~0u;
    if (method >= NV4097_SET_VERTEX_DATA4UB_M &&
        method < NV4097_SET_VERTEX_DATA4UB_M + RSX_MAX_VERTEX_ATTRIBS * 4u) {
        attr = (method - NV4097_SET_VERTEX_DATA4UB_M) / 4u;
        state->vertex_data4f[attr][0] = (float)(data & 0xFFu) / 255.0f;
        state->vertex_data4f[attr][1] = (float)((data >> 8) & 0xFFu) / 255.0f;
        state->vertex_data4f[attr][2] = (float)((data >> 16) & 0xFFu) / 255.0f;
        state->vertex_data4f[attr][3] = (float)(data >> 24) / 255.0f;
        complete = 1;
    } else if (method >= NV4097_SET_VERTEX_DATA1F_M &&
               method < NV4097_SET_VERTEX_DATA1F_M + RSX_MAX_VERTEX_ATTRIBS * 4u) {
        attr = (method - NV4097_SET_VERTEX_DATA1F_M) / 4u;
        current_vertex_reset(state, attr);
        memcpy(&state->vertex_data4f[attr][0], &data, sizeof(data));
        complete = 1;
    } else if (method >= NV4097_SET_VERTEX_DATA2F_M &&
               method < NV4097_SET_VERTEX_DATA2F_M + RSX_MAX_VERTEX_ATTRIBS * 8u) {
        word = (method - NV4097_SET_VERTEX_DATA2F_M) / 4u;
        attr = word / 2u; lane = word & 1u;
        if (!lane) current_vertex_reset(state, attr);
        memcpy(&state->vertex_data4f[attr][lane], &data, sizeof(data));
        complete = lane == 1u;
    } else if (method >= NV4097_SET_VERTEX_DATA3F_M &&
               method < NV4097_SET_VERTEX_DATA3F_M + RSX_MAX_VERTEX_ATTRIBS * 16u) {
        word = (method - NV4097_SET_VERTEX_DATA3F_M) / 4u;
        attr = word / 4u; lane = word & 3u;
        if (lane >= 3u) return 0; /* fourth register is alignment padding */
        if (!lane) current_vertex_reset(state, attr);
        memcpy(&state->vertex_data4f[attr][lane], &data, sizeof(data));
        complete = lane == 2u;
    } else if (method >= NV4097_SET_VERTEX_DATA4F_M &&
               method < NV4097_SET_VERTEX_DATA4F_M + RSX_MAX_VERTEX_ATTRIBS * 16u) {
        word = (method - NV4097_SET_VERTEX_DATA4F_M) / 4u;
        attr = word / 4u; lane = word & 3u;
        if (!lane) current_vertex_reset(state, attr);
        memcpy(&state->vertex_data4f[attr][lane], &data, sizeof(data));
        complete = lane == 3u;
    } else if (method >= NV4097_SET_VERTEX_DATA2S_M &&
               method < NV4097_SET_VERTEX_DATA2S_M + RSX_MAX_VERTEX_ATTRIBS * 4u) {
        attr = (method - NV4097_SET_VERTEX_DATA2S_M) / 4u;
        current_vertex_reset(state, attr);
        state->vertex_data4f[attr][0] = (float)(s16)(data & 0xFFFFu);
        state->vertex_data4f[attr][1] = (float)(s16)(data >> 16);
        complete = 1;
    } else if (method >= NV4097_SET_VERTEX_DATA4S_M &&
               method < NV4097_SET_VERTEX_DATA4S_M + RSX_MAX_VERTEX_ATTRIBS * 8u) {
        word = (method - NV4097_SET_VERTEX_DATA4S_M) / 4u;
        attr = word / 2u; lane = (word & 1u) * 2u;
        if (!lane) current_vertex_reset(state, attr);
        state->vertex_data4f[attr][lane] = (float)(s16)(data & 0xFFFFu);
        state->vertex_data4f[attr][lane + 1u] = (float)(s16)(data >> 16);
        complete = lane == 2u;
    } else if (method >= NV4097_SET_VERTEX_DATA_SCALED4S_M &&
               method < NV4097_SET_VERTEX_DATA_SCALED4S_M + RSX_MAX_VERTEX_ATTRIBS * 8u) {
        word = (method - NV4097_SET_VERTEX_DATA_SCALED4S_M) / 4u;
        attr = word / 2u; lane = (word & 1u) * 2u;
        if (!lane) current_vertex_reset(state, attr);
        state->vertex_data4f[attr][lane] = current_vertex_snorm16((u16)data);
        state->vertex_data4f[attr][lane + 1u] = current_vertex_snorm16((u16)(data >> 16));
        complete = lane == 2u;
    } else {
        return -1;
    }
    state->vertex_dirty = 1;
    if (complete) *completed_attr = attr;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Main method dispatch
 * -----------------------------------------------------------------------*/

int rsx_process_method(rsx_state* state, u32 method, u32 data)
{
    if (state->tconst_probe_enabled)
        state->tconst_probe_method_sequence++;
    rsx_draw_trace_push(method, data);
    /* A deferred indexed span is valid only while all subsequent FIFO methods
     * leave its draw state intact.  Flush before every non-indexed method;
     * this covers END, a new BEGIN, state writes, and index binding changes. */
    if (method != NV4097_DRAW_INDEX_ARRAY)
        rsx_flush_pending_indexed(state);
    { static int _rt=-1; if(_rt<0) _rt=getenv("YDKJ_RSXTRACE")?1:0;
      if(_rt){ static int _m=0; if(_m++<250) fprintf(stderr,"[rsxm] method=0x%04X data=0x%08X\n", method, data); } }
    /* Hot path: TRANSFORM_CONSTANT words match none of the checks below
     * except these four registers inside its 64-word window. */
    if ((u32)(method - NV4097_SET_TRANSFORM_CONSTANT) < 64u * 4u &&
        method != 0x00001FC0u &&
        method != NV4097_SET_TRANSFORM_BRANCH_BITS &&
        method != NV4097_SET_VERTEX_ATTRIB_INPUT_MASK &&
        method != NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK)
        goto transform_constant;
    /* Back-end write label / semaphore (cellGcmSetWriteBackEndLabel): the RSX
     * writes a value to a report/label the CPU polls for CPU<->RSX sync (double
     * buffering). Real hardware DOES this; without it the game's frame-fence
     * loop (func_0006E6E0 polling label 0x41 @ 0x03000410) spins forever and
     * never reaches render. NV4097_SET_SEMAPHORE_OFFSET(0x1D6C)=offset (index*0x10
     * into the GCM label window @0x03000000); NV4097_BACK_END_WRITE_SEMAPHORE_
     * RELEASE(0x1D70)=byte-swapped value; TEXTURE_READ_SEMAPHORE_RELEASE
     * (0x1D74)=unmodified value. Both follow ordered FIFO-prefix completion.
     * NV406E frontend offset/acquire/release (0x64/68/6C) are handled by the
     * FIFO walker, which can park an unsatisfied acquire. */
    /* NV4097_SET_FREQUENCY_DIVIDER_OPERATION: per-attribute modulo/divide mask
     * for the frequency divisor (instancing). Captured for read_vp_vertex. */
    if (method == 0x00001FC0) { state->frequency_divider_op = data; return 0; }
    if (method == NV4097_SET_VERTEX_DATA_BASE_OFFSET) {
        state->vertex_data_base_offset = data;
        state->vertex_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_VERTEX_DATA_BASE_INDEX) {
        state->vertex_data_base_index = data;
        state->vertex_dirty = 1;
        return 0;
    }
    { u32 completed_attr;
      if (process_current_vertex_method(state, method, data, &completed_attr) == 0) {
        if (completed_attr != ~0u)
            rsx_immediate_attribute_complete(state, completed_attr);
        return 0;
      }
    }

    { static u32 s_sem_off = 0;
      if (method == 0x1D6C) { s_sem_off = data; return 0; }
      if (method == 0x1D70 || method == 0x1D74) {
        extern void vm_write32(uint32_t a, uint32_t v);
        /* The RSX back-end semaphore write swaps bytes 0<->2 of the value (hw
         * quirk); libgcm's cellGcmSetWriteBackEndLabel PRE-swaps to compensate
         * (SDK gcm_implementation_sub.h: "// swap byte 0 and 2"). Writing the
         * FIFO value verbatim left the label byte-swapped, so gcmutil's
         * cellGcmUtilFinish poll (label 255 == sLabelVal, starts at 1) compared
         * 0x00010000 != 1 and spun forever (gcm/cube sample hang after init).
         * Apply the same swap the hardware does so the pre-swap cancels out.
         * TEXTURE_READ has no such swap: geometry jobs publish allocation-end
         * addresses verbatim to retire their output rings. */
        u32 val = method == 0x1D70
            ? (data & 0xff00ff00u) | ((data >> 16) & 0xffu) | ((data & 0xffu) << 16)
            : data;
        if (method == 0x1D74 && s_backend && s_backend->defer_texture_label &&
            s_backend->defer_texture_label(s_backend->userdata,
                                           s_sem_off & 0x00FFFFFFu, val)) return 0;
        vm_write32(0x03000000u + (s_sem_off & 0x00FFFFFFu), val);
        { static int _l=0; if(_l++<8) fprintf(stderr,"[RSX] label write @0x%08X = 0x%08X (sync fence, raw 0x%08X)\n", 0x03000000u+(s_sem_off&0xFFFFFF), val, data); }
        return 0;
      } }
    if (method == NV4097_SET_CONTEXT_DMA_COLOR_B ||
        method == NV4097_SET_CONTEXT_DMA_COLOR_A ||
        method == NV4097_SET_CONTEXT_DMA_ZETA ||
        method == NV4097_SET_CONTEXT_DMA_COLOR_C ||
        method == NV4097_SET_CONTEXT_DMA_COLOR_D ||
        (method >= 0x200 && method <= 0x23C) ||
        (method >= 0x280 && method <= 0x28C) ||
        method == NV4097_SET_WINDOW_OFFSET)
        return process_surface_method(state, method, data);

    if (method >= NV4097_SET_VERTEX_TEXTURE_OFFSET &&
        method < NV4097_SET_VERTEX_TEXTURE_OFFSET + RSX_MAX_VERTEX_TEXTURES * 0x20u)
        return process_vertex_texture_method(state, method, data);

    /* NV40TCL_TEX_SIZE1: one pitch/depth register per unit at 0x1840 + unit*4.
     * Linear textures are not necessarily tightly packed; ignoring this made
     * packed post-process views read the wrong row after row zero. */
    if (method >= NV4097_SET_TEXTURE_SIZE1 &&
        method < NV4097_SET_TEXTURE_SIZE1 + RSX_MAX_TEXTURES * 4)
        return process_texture_size1_method(state, method, data);

    /* Texture methods: 0x1A00..0x1A00 + 16*0x20 - 1 */
    if (method >= 0x1A00 && method < 0x1A00 + RSX_MAX_TEXTURES * 0x20)
        return process_texture_method(state, method, data);

    /* Vertex attribute FORMAT: 0x1740..0x177C */
    if (method >= 0x1740 && method < 0x1740 + RSX_MAX_VERTEX_ATTRIBS * 4)
        return process_vertex_attrib_method(state, method, data);

    /* Vertex attribute OFFSET: 0x1680..0x16BC */
    if (method >= 0x1680 && method < 0x1680 + RSX_MAX_VERTEX_ATTRIBS * 4)
        return process_vertex_attrib_method(state, method, data);

    /* Viewport */
    /* Viewport transform: window = ndc*scale + offset. The z lane is the
     * GL->[0,1] depth remap (offset.z=0.5/scale.z=0.5); without honoring it,
     * GL-convention projections (SDK gcm samples) get their near-camera
     * geometry clipped by D3D's 0<=z<=w rule (gcm/cube: missing polygons). */
    if (method >= 0x0A20 && method < 0x0A30) {
        u32 f = data; float v; memcpy(&v, &f, 4);
        state->viewport_offset[(method - 0x0A20) >> 2] = v;
        return 0;
    }
    if (method >= 0x0A30 && method < 0x0A40) {
        u32 f = data; float v; memcpy(&v, &f, 4);
        state->viewport_scale[(method - 0x0A30) >> 2] = v;
        return 0;
    }

    if (method == NV4097_SET_VIEWPORT_HORIZONTAL) {
        state->viewport_x = data & 0xFFFF;
        state->viewport_w = (data >> 16) & 0xFFFF;
        state->viewport_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_VIEWPORT_VERTICAL) {
        state->viewport_y = data & 0xFFFF;
        state->viewport_h = (data >> 16) & 0xFFFF;
        state->viewport_dirty = 1;
        return 0;
    }

    if (method == NV4097_SET_FOG_MODE) {
        state->fog_mode = data;
        state->shader_dirty = 1;
        return 0;
    }
    if (method >= NV4097_SET_FOG_PARAMS &&
        method < NV4097_SET_FOG_PARAMS + 2u * 4u) {
        u32 index = (method - NV4097_SET_FOG_PARAMS) / 4u;
        memcpy(&state->fog_param[index], &data, sizeof(data));
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_CLIP_MIN) {
        memcpy(&state->clip_min, &data, sizeof(data));
        state->viewport_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_CLIP_MAX) {
        memcpy(&state->clip_max, &data, sizeof(data));
        state->viewport_dirty = 1;
        return 0;
    }

    /* Color mask */
    if (method == NV4097_SET_COLOR_MASK) {
        state->color_mask = data;
        state->color_mask_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_COLOR_MASK_MRT) {
        state->color_mask_mrt = data;
        state->color_mask_dirty = 1;
        return 0;
    }

    /* Alpha test */
    if (method == NV4097_SET_ALPHA_TEST_ENABLE) {
        state->alpha_test_enable = data ? 1 : 0;
        state->alpha_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_ALPHA_FUNC) {
        state->alpha_func = data;
        state->alpha_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_ALPHA_REF) {
        state->alpha_ref = data;
        state->alpha_dirty = 1;
        return 0;
    }

    /* Clear */
    if (method == NV4097_SET_COLOR_CLEAR_VALUE) {
        state->color_clear_value = data;
        return 0;
    }
    if (method == NV4097_SET_ZSTENCIL_CLEAR_VALUE) {
        state->zstencil_clear_value = data;
        return 0;
    }
    if (method == NV4097_CLEAR_SURFACE) {
        { static int _c=0; if (_c++ < 12) fprintf(stdout, "[RSX] CLEAR_SURFACE mask=0x%X color=0x%08X\n", data, state->color_clear_value); }
        /* A disabled front stencil write mask suppresses stencil clearing.
         * The comparison mask and depth-write enable do not gate clears. */
        u32 flags = data;
        if (!(state->stencil_write_mask & 0xFFu))
            flags &= ~RSX_CLEAR_STENCIL_BIT;
        if (!(flags & (RSX_CLEAR_COLOR_MASK | RSX_CLEAR_DEPTH_STENCIL_MASK)))
            return 0;
        if (s_backend && s_backend->clear) {
            float depth = (float)(state->zstencil_clear_value >> 8) / (float)0xFFFFFF;
            u8 stencil = state->zstencil_clear_value & 0xFF;
            s_backend->clear(s_backend->userdata, flags,
                           state->color_clear_value, depth, stencil);
        }
        return 0;
    }

    /* Blend */
    if (method == NV4097_SET_BLEND_ENABLE) {
        state->blend_enable = data ? 1 : 0;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_ENABLE_MRT) {
        state->blend_enable_mrt = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_SHADE_MODE) {
        state->shade_mode = data;
        return 0;
    }
    if (method == NV4097_SET_BLEND_FUNC_SFACTOR) {
        state->blend_sfactor = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_FUNC_DFACTOR) {
        state->blend_dfactor = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_EQUATION) {
        state->blend_equation = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_COLOR) {
        state->blend_color = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BLEND_COLOR2) {
        state->blend_color2 = data;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_LOGIC_OP_ENABLE) {
        state->logic_op_enable = data ? 1 : 0;
        state->blend_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_LOGIC_OP) {
        state->logic_op = data;
        state->blend_dirty = 1;
        return 0;
    }

    /* Depth */
    if (method == NV4097_SET_DEPTH_TEST_ENABLE) {
        state->depth_test_enable = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_FUNC) {
        state->depth_func = data;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_MASK) {
        state->depth_mask = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE) {
        state->depth_bounds_test_enable = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_BOUNDS_MIN) {
        memcpy(&state->depth_bounds_min, &data, sizeof(data));
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_DEPTH_BOUNDS_MAX) {
        memcpy(&state->depth_bounds_max, &data, sizeof(data));
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_POLY_OFFSET_POINT_ENABLE) {
        state->poly_offset_point_enable = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_POLY_OFFSET_LINE_ENABLE) {
        state->poly_offset_line_enable = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_POLY_OFFSET_FILL_ENABLE) {
        state->poly_offset_fill_enable = data ? 1 : 0;
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR) {
        memcpy(&state->polygon_offset_scale_factor, &data, sizeof(data));
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_POLYGON_OFFSET_BIAS) {
        memcpy(&state->polygon_offset_bias, &data, sizeof(data));
        state->depth_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_ZMIN_MAX_CONTROL) {
        state->zmin_max_control = data;
        state->depth_dirty = 1;
        return 0;
    }

    /* Stencil */
    if (method == NV4097_SET_STENCIL_TEST_ENABLE) {
        state->stencil_test_enable = data ? 1 : 0;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_MASK) {
        state->stencil_write_mask = data & 0xFF;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_FUNC) {
        state->stencil_func = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_FUNC_REF) {
        state->stencil_ref = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_FUNC_MASK) {
        state->stencil_mask = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_OP_FAIL) {
        state->stencil_op_fail = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_OP_ZFAIL) {
        state->stencil_op_zfail = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_STENCIL_OP_ZPASS) {
        state->stencil_op_zpass = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE) {
        state->two_sided_stencil_test_enable = data ? 1 : 0;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BACK_STENCIL_MASK) {
        state->back_stencil_write_mask = data & 0xFFu;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BACK_STENCIL_FUNC) {
        state->back_stencil_func = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BACK_STENCIL_FUNC_REF) {
        state->back_stencil_ref = data & 0xFFu;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BACK_STENCIL_FUNC_MASK) {
        state->back_stencil_mask = data & 0xFFu;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BACK_STENCIL_OP_FAIL) {
        state->back_stencil_op_fail = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BACK_STENCIL_OP_ZFAIL) {
        state->back_stencil_op_zfail = data;
        state->stencil_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_BACK_STENCIL_OP_ZPASS) {
        state->back_stencil_op_zpass = data;
        state->stencil_dirty = 1;
        return 0;
    }

    /* Culling */
    if (method == NV4097_SET_CULL_FACE_ENABLE) {
        state->cull_face_enable = data ? 1 : 0;
        return 0;
    }
    if (method == NV4097_SET_CULL_FACE) {
        state->cull_face = data;
        return 0;
    }
    if (method == NV4097_SET_FRONT_FACE) {
        state->front_face = data;
        return 0;
    }
    if (method == NV4097_SET_FRONT_POLYGON_MODE) {
        state->front_polygon_mode = data;
        return 0;
    }
    if (method == NV4097_SET_BACK_POLYGON_MODE) {
        state->back_polygon_mode = data;
        return 0;
    }

    /* Shader programs */
    if (method == NV4097_SET_SHADER_PROGRAM) {
        /*
         * Fragment program address register:
         *   [1:0]  location + 1 (1 = local, 2 = main)
         *   [31:2] offset (4-byte aligned)
         */
        state->shader_program = data;
        state->fragment_program_addr = data & ~0x3u;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_TRANSFORM_PROGRAM_LOAD) {
        /* Vertex program load slot — instruction index (each = 16 bytes).
         * Following NV4097_SET_TRANSFORM_PROGRAM words fill from here. */
        state->transform_program_load = data;
        state->vp_ucode_write = data * 16;
        state->shader_dirty = 1;
        return 0;
    }

    if (method == NV4097_SET_TRANSFORM_PROGRAM_START) {
        if (state->transform_program_start != data) state->vp_dirty = 1;
        state->transform_program_start = data;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_TRANSFORM_BRANCH_BITS) {
        state->transform_branch_bits = data;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_VERTEX_ATTRIB_INPUT_MASK) {
        state->vertex_attrib_input_mask = data & 0xFFFFu;
        state->vertex_dirty = 1;
        return 0;
    }

    /* NV4097_SET_TRANSFORM_PROGRAM[0..31] — a run of 32-bit vertex-program
     * microcode words appended at the current write cursor. Capture them so the
     * backend can decompile the real VP (4 words = one NV40 instruction). */
    if (method >= NV4097_SET_TRANSFORM_PROGRAM &&
        method <  NV4097_SET_TRANSFORM_PROGRAM + 32 * 4) {
        u32 w = state->vp_ucode_write;
        if (w + 4 <= sizeof(state->vp_ucode)) {
            /* data is host-endian already; store as little-endian bytes. */
            state->vp_ucode[w+0] = (u8)(data);
            state->vp_ucode[w+1] = (u8)(data >> 8);
            state->vp_ucode[w+2] = (u8)(data >> 16);
            state->vp_ucode[w+3] = (u8)(data >> 24);
            state->vp_ucode_write = w + 4;
            if (w + 4 > state->vp_ucode_bytes) state->vp_ucode_bytes = w + 4;
            state->vp_dirty = 1;
        }
        return 0;
    }
    if (method == NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK) {
        state->vertex_attrib_output_mask = data;
        return 0;
    }
    if (method == NV4097_SET_USER_CLIP_PLANE_CONTROL) {
        state->user_clip_plane_control = data;
        return 0;
    }
    if (method == NV4097_SET_TRANSFORM_CONSTANT_LOAD) {
        /* Launch-only diagnostics: avoid an environment scan per FIFO packet. */
        static _Thread_local int load_dbg_enabled = -1;
        if (load_dbg_enabled < 0) load_dbg_enabled = getenv("LOAD_DBG") != NULL;
        { static int _n=0; if (load_dbg_enabled && _n++ < 200)
            fprintf(stderr, "[LOAD] transform_constant_load = %u\n", data); }
        state->transform_constant_load = data;
        return 0;
    }

    /* NV4097_SET_TRANSFORM_CONSTANT[0..63] — up to 64 dwords (16 vec4s) per
     * command. Each register slot writes to a lane of one vertex constant
     * vec4: vec_index = LOAD + (reg_offset/4), lane = reg_offset%4.
     * The data arrives as a host-endian u32; reinterpret the bits as float
     * because the game's intent is "these 32 bits are a float". The hardware
     * does NOT auto-advance LOAD between commands — games re-issue
     * SET_TRANSFORM_CONSTANT_LOAD before each block. */
transform_constant:
    if (method >= NV4097_SET_TRANSFORM_CONSTANT &&
        method <  NV4097_SET_TRANSFORM_CONSTANT + 64 * 4) {
        u32 reg_offset = (method - NV4097_SET_TRANSFORM_CONSTANT) / 4;
        u32 slot = state->transform_constant_load + (reg_offset >> 2);
        u32 lane = reg_offset & 3;
        if (slot < RSX_MAX_VERTEX_CONSTANTS) {
            float f;
            memcpy(&f, &data, 4);
            { static int _en=-1; if(_en<0){const char*e=getenv("TCONST_DBG");_en=e?1:0;}
              static int _n=0;
              int _hit = _en && (getenv("TCONST_ALL") ? (_n<64) : (slot>=12 && slot<=30 && _n<400));
              if(_hit){ _n++; fprintf(stderr,"[TCONST] load=%u slot=%u lane=%u = %.4f\n", state->transform_constant_load, slot, lane, f); } }
            state->vertex_constants[slot][lane] = f;
            if (state->tconst_probe_enabled && slot >= 4 && slot <= 7) {
                u32 probe_slot = slot - 4;
                state->tconst_probe_last_sequence[probe_slot][lane] =
                    state->tconst_probe_method_sequence;
                state->tconst_probe_last_raw[probe_slot][lane] = data;
                state->tconst_probe_last_load[probe_slot][lane] =
                    state->transform_constant_load;
                state->tconst_probe_last_slot[probe_slot][lane] = slot;
                state->tconst_probe_last_lane[probe_slot][lane] = lane;
            }
            /* Only c256.x can emit this diagnostic. Avoid a CRT environment
             * lock and full environment scan for every other constant word. */
            { static int _sq=0; if (slot==256 && lane==0 && getenv("SEQ_DBG") && _sq++ < 500)
                fprintf(stderr, "[SEQ] upload c256.x=%.4f (load=%u)\n", f, state->transform_constant_load); }
            if (!state->vertex_constants_dirty) {
                state->vertex_constants_lo = slot;
                state->vertex_constants_hi = slot;
                state->vertex_constants_dirty = 1;
            } else {
                if (slot < state->vertex_constants_lo) state->vertex_constants_lo = slot;
                if (slot > state->vertex_constants_hi) state->vertex_constants_hi = slot;
            }
        }
        return 0;
    }

    /* Draw */
    if (method == NV4097_SET_BEGIN_END) {
        if (data != 0) {
            state->primitive_type = data;
            state->in_begin_end = 1;
            rsx_immediate_begin(state);

            /* Flush dirty state to backend before drawing */
            if (s_backend) {
                if (state->surface_dirty && s_backend->set_render_target)
                    s_backend->set_render_target(s_backend->userdata, state);
                if (state->viewport_dirty && s_backend->set_viewport)
                    s_backend->set_viewport(s_backend->userdata, state);
                if (state->blend_dirty && s_backend->set_blend)
                    s_backend->set_blend(s_backend->userdata, state);
                if ((state->depth_dirty || state->stencil_dirty) && s_backend->set_depth_stencil)
                    s_backend->set_depth_stencil(s_backend->userdata, state);
                if (state->color_mask_dirty && s_backend->set_color_mask)
                    s_backend->set_color_mask(s_backend->userdata, state);
                if (state->alpha_dirty && s_backend->set_alpha_test)
                    s_backend->set_alpha_test(s_backend->userdata, state);
                if (state->shader_dirty && s_backend->set_shader)
                    s_backend->set_shader(s_backend->userdata, state);
                if (state->vertex_dirty && s_backend->set_vertex_attribs)
                    s_backend->set_vertex_attribs(s_backend->userdata, state);

                /* Bind dirty textures */
                if (state->texture_dirty && s_backend->bind_texture) {
                    for (u32 i = 0; i < RSX_MAX_TEXTURES; i++) {
                        if (state->textures[i].dirty) {
                            s_backend->bind_texture(s_backend->userdata, i, &state->textures[i]);
                            state->textures[i].dirty = 0;
                        }
                    }
                }
                if (state->vertex_texture_dirty && s_backend->bind_vertex_texture) {
                    for (u32 i = 0; i < RSX_MAX_VERTEX_TEXTURES; i++) {
                        if (state->vertex_textures[i].dirty) {
                            s_backend->bind_vertex_texture(
                                s_backend->userdata, i, &state->vertex_textures[i]);
                            state->vertex_textures[i].dirty = 0;
                        }
                    }
                }

                state->surface_dirty = 0;
                state->viewport_dirty = 0;
                state->blend_dirty = 0;
                state->depth_dirty = 0;
                state->stencil_dirty = 0;
                state->color_mask_dirty = 0;
                state->alpha_dirty = 0;
                state->shader_dirty = 0;
                state->vertex_dirty = 0;
                state->texture_dirty = 0;
                state->vertex_texture_dirty = 0;
            }
        } else {
            /* Register vertices, immediate elements, and INLINE_ARRAY have no
             * separate DRAW_* packet. END compiles and submits that stream. */
            rsx_immediate_finish(state);
            state->in_begin_end = 0;
        }
        return 0;
    }

    if (method == NV4097_ARRAY_ELEMENT16) {
        rsx_immediate_append_index(state, data & 0xFFFFu);
        rsx_immediate_append_index(state, data >> 16);
        return 0;
    }

    if (method == NV4097_ARRAY_ELEMENT32) {
        rsx_immediate_append_index(state, data);
        return 0;
    }

    if (method == NV4097_INLINE_ARRAY) {
        rsx_immediate_append_inline_word(state, data);
        return 0;
    }

    if (method == NV4097_DRAW_ARRAYS) {
        state->immediate_draw_kind = 1;
        rsx_draw_trace_dump();
        u32 first = data & 0xFFFFFF;
        u32 count = ((data >> 24) & 0xFF) + 1;
        { static int _d=0; if (_d++ < 32) fprintf(stdout, "[RSX] DRAW_ARRAYS prim=%u first=%u count=%u\n", state->primitive_type, first, count); }
        { static int _sq=0; static _Thread_local int seq_dbg = -1;
          if (seq_dbg < 0) seq_dbg = getenv("SEQ_DBG") != NULL;
          if (seq_dbg && _sq++ < 500)
            fprintf(stderr, "[SEQ] DRAW surf0=0x%X c256.x=%.4f c257.y=%.4f\n",
                    state->surface_color_offset[0], state->vertex_constants[256][0],
                    state->vertex_constants[257][1]); }
        /* MVPDBG: dump every non-zero vertex-constant slot the FIRST time a
         * G-buffer draw (surf0=0xCC0000) is dispatched -- the true MVP the GPU
         * will use, read live (no snapshot/parity indirection). */
        static _Thread_local int mvp_dbg_enabled = -1;
        if (mvp_dbg_enabled < 0) mvp_dbg_enabled = getenv("MVPDBG") != NULL;
        if (mvp_dbg_enabled && state->surface_color_offset[0] == 0xCC0000) {
            static int _once = 0;
            if (!_once) { _once = 1;
                for (int s = 0; s < RSX_MAX_VERTEX_CONSTANTS; s++) {
                    float* v = state->vertex_constants[s];
                    if (v[0]||v[1]||v[2]||v[3])
                        fprintf(stderr, "[MVP] c[%3d] = %10.4f %10.4f %10.4f %10.4f\n",
                                s, v[0], v[1], v[2], v[3]);
                }
            }
        }
        if (s_backend && s_backend->draw_arrays)
            s_backend->draw_arrays(s_backend->userdata, state->primitive_type, first, count);
        return 0;
    }

    if (method == NV4097_SET_SHADER_CONTROL) {
        { static int _sc=0; if (_sc++ < 12 && getenv("RTT_DUMP"))
            fprintf(stderr, "[RSXSC] shader_control=0x%X\n", data); }
        state->shader_control = data;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_SHADER_PACKER) {
        state->shader_packer = data;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_SHADER_WINDOW) {
        state->shader_window = data;
        state->shader_dirty = 1;
        return 0;
    }
    if (method >= NV4097_SET_TEX_COORD_CONTROL &&
        method < NV4097_SET_TEX_COORD_CONTROL + 10u * 4u) {
        u32 unit = (method - NV4097_SET_TEX_COORD_CONTROL) / 4u;
        u32 bit = 1u << unit;
        state->texcoord_control_mask = data & 1u
            ? state->texcoord_control_mask | bit
            : state->texcoord_control_mask & ~bit;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_TWO_SIDE_LIGHT_EN) {
        state->two_side_light_enable = data ? 1 : 0;
        state->shader_dirty = 1;
        return 0;
    }
    if (method == NV4097_SET_INDEX_ARRAY_ADDRESS) {
        state->index_array_offset = data;
        return 0;
    }
    if (method == NV4097_SET_INDEX_ARRAY_DMA) {
        state->index_array_dma = data;
        return 0;
    }
    if (method == NV4097_SET_RESTART_INDEX_ENABLE) {
        state->restart_index_enable = data ? 1 : 0;
        return 0;
    }
    if (method == NV4097_SET_RESTART_INDEX) {
        state->restart_index = data;
        return 0;
    }
    if (method == NV4097_DRAW_INDEX_ARRAY) {
        state->immediate_draw_kind = 2;
        /* [23:0] first index, [31:24] count-1 (same packing as DRAW_ARRAYS). */
        u32 first = data & 0xFFFFFF;
        u32 count = ((data >> 24) & 0xFF) + 1;
        { static int _d=0; if (_d++ < 8) fprintf(stderr, "[RSX] DRAW_INDEX_ARRAY prim=%u first=%u count=%u idxoff=0x%X dma=0x%X\n", state->primitive_type, first, count, state->index_array_offset, state->index_array_dma); }
        if (!state->in_begin_end) {
            /* Preserve the legacy immediate behavior for malformed/outside-
             * BEGIN streams rather than allowing a span to cross an epoch. */
            if (s_backend && s_backend->draw_indexed)
                s_backend->draw_indexed(s_backend->userdata,
                                        state->primitive_type, first, count);
        } else if (state->pending_indexed_valid &&
                   state->pending_indexed_primitive == state->primitive_type &&
                   state->pending_indexed_offset == state->index_array_offset &&
                   state->pending_indexed_dma == state->index_array_dma &&
                   first >= state->pending_indexed_first &&
                   first - state->pending_indexed_first == state->pending_indexed_count) {
            state->pending_indexed_count += count;
        } else {
            rsx_flush_pending_indexed(state);
            state->pending_indexed_valid = 1;
            state->pending_indexed_primitive = state->primitive_type;
            state->pending_indexed_first = first;
            state->pending_indexed_count = count;
            state->pending_indexed_offset = state->index_array_offset;
            state->pending_indexed_dma = state->index_array_dma;
        }
        return 0;
    }

    /* Scissor */
    if (method == NV4097_SET_SCISSOR_HORIZONTAL) {
        state->scissor_x = data & 0xFFFF;
        state->scissor_w = (data >> 16) & 0xFFFF;
        return 0;
    }
    if (method == NV4097_SET_SCISSOR_VERTICAL) {
        state->scissor_y = data & 0xFFFF;
        state->scissor_h = (data >> 16) & 0xFFFF;
        return 0;
    }

    /* NV406E_SET_REFERENCE (0x0050): the title writes a reference value it then
     * spins on (cellGcmFinish / cellGcmSetWaitLabel). Record the latest so the
     * GCM control register can report completion back to the guest and unblock
     * the spin. Class 0 (software) method, no subchannel state needed here. */
    if (method == 0x0050) {
        g_rsx_last_reference = data;
        return 0;
    }

    /* Unrecognized method — log in debug builds */
#ifndef NDEBUG
    static int s_unknown_count = 0;
    if (s_unknown_count < 50) {
        printf("[RSX] unknown method 0x%04X = 0x%08X\n", method, data);
        s_unknown_count++;
    }
#endif

    return -1;
}

/* ---------------------------------------------------------------------------
 * Command buffer parsing
 * -----------------------------------------------------------------------*/

int rsx_process_command_buffer(rsx_state* state, const u32* buf, u32 size)
{
    int methods_processed = 0;
    u32 pos = 0;
    u32 count = size / 4; /* size in dwords */

    { static int _c=0; if (count && _c++ < 16) fprintf(stderr, "[RSX] process_cmd_buffer words=%u first_hdr=0x%08X\n", count, buf[0]); }
    while (pos < count) {
        u32 header = buf[pos++];
        u32 type = (header >> 29) & 0x7;

        if (type == 0 || type == 2) {
            /* Increasing or non-increasing method */
            u32 method = (header >> 2) & 0x7FF;
            method <<= 2; /* method addresses are dword-aligned */
            u32 num_data = (header >> 18) & 0x7FF;
            int increasing = (type == 0);

            for (u32 i = 0; i < num_data && pos < count; i++) {
                u32 data = buf[pos++];
                u32 m = increasing ? (method + i * 4) : method;
                rsx_process_method(state, m, data);
                methods_processed++;
            }
        } else if (type == 1) {
            /* Jump — change command buffer read position */
            /* In recomp context, this is handled by the caller */
            break;
        } else {
            /* Unknown type, skip */
            break;
        }
    }

    return methods_processed;
}
