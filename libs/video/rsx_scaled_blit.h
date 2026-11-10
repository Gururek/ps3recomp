/* CPU implementation of the NV3089 scaled-image SRCCOPY path. */
#ifndef PS3RECOMP_RSX_SCALED_BLIT_H
#define PS3RECOMP_RSX_SCALED_BLIT_H

#include <stdint.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

enum rsx_scaled_blit_result {
    RSX_SCALED_BLIT_OK = 0,
    RSX_SCALED_BLIT_EMPTY,
    RSX_SCALED_BLIT_UNSUPPORTED_FORMAT,
    RSX_SCALED_BLIT_UNSUPPORTED_SURFACE,
    RSX_SCALED_BLIT_UNSUPPORTED_OPERATION
};

struct rsx_scaled_blit_desc {
    const uint8_t* src;
    uint8_t* dst;
    uint16_t src_pitch, dst_pitch;
    uint16_t in_width, in_height;
    uint16_t out_width, out_height;
    uint16_t clip_x, clip_y, clip_width, clip_height;
    uint16_t out_x, out_y;
    uint16_t in_x_4, in_y_4;
    int32_t ds_dx, dt_dy;
    uint8_t src_format, dst_format;
    uint8_t origin, interpolator;
};

enum rsx_scaled_blit_interpolator {
    RSX_SCALED_BLIT_INTERPOLATOR_ZOH = 0,
    RSX_SCALED_BLIT_INTERPOLATOR_FOH = 1
};

/* Exact 4-byte-pixel copy rectangle shared by the CPU and deferred GPU paths.
 * This deliberately excludes scaling and format conversion: the CPU blit
 * remains authoritative for every other NV3089 operation. */
struct rsx_scaled_blit_exact_region {
    uint16_t src_x, src_y;
    uint16_t dst_x, dst_y;
    uint16_t width, height;
};

/* Resolve a raw + byte-delta alias into a linear RGBA backing texture.  The
 * caller supplies the actual backing dimensions; aliases that would wrap a
 * row are rejected because CopyTextureRegion cannot represent that layout. */
static inline int rsx_scaled_blit_alias_origin(
    uint32_t base_raw, uint32_t raw, uint32_t pitch, uint32_t backing_w,
    uint32_t backing_h, uint32_t rect_w, uint32_t rect_h,
    uint32_t* out_x, uint32_t* out_y)
{
    if (!pitch || (pitch & 3u) || !backing_w || !backing_h || !rect_w || !rect_h ||
        raw < base_raw)
        return 0;
    uint64_t delta = (uint64_t)raw - base_raw;
    uint64_t backing_bytes = (uint64_t)pitch * backing_h;
    if ((delta & 3u) || delta >= backing_bytes) return 0;
    uint32_t x = (uint32_t)((delta % pitch) / 4u);
    uint32_t y = (uint32_t)(delta / pitch);
    if (x >= backing_w || y >= backing_h || rect_w > backing_w - x ||
        rect_h > backing_h - y)
        return 0;
    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    return 1;
}

static inline int rsx_scaled_blit_guest_span_valid(
    uint32_t ea, uint32_t pitch, uint32_t physical_w, uint32_t h,
    uint64_t vm_size, uint64_t* out_bytes)
{
    if (!pitch || (pitch & 3u) || !physical_w || !h ||
        physical_w > pitch / 4u)
        return 0;
    uint64_t bytes = (uint64_t)(h - 1u) * pitch + (uint64_t)physical_w * 4u;
    uint64_t cap = vm_size ? vm_size : (1ULL << 32);
    if (!bytes || (uint64_t)ea > cap || bytes > cap - ea)
        return 0;
    if (out_bytes) *out_bytes = bytes;
    return 1;
}

/* Generic linear guest surface validation for CPU NV3089 paths.  Keep this
 * separate from the RGBA-only deferred-record span helper above. */
static inline int rsx_scaled_blit_guest_surface_valid(
    uint32_t ea, uint64_t pitch, uint32_t physical_w, uint32_t h,
    uint32_t bpp, uint64_t vm_size)
{
    if (!pitch || !physical_w || !h || !bpp ||
        (uint64_t)physical_w > UINT64_MAX / bpp)
        return 0;
    uint64_t row_bytes = (uint64_t)physical_w * bpp;
    if (pitch < row_bytes || (uint64_t)(h - 1u) >
        (UINT64_MAX - row_bytes) / pitch)
        return 0;
    uint64_t bytes = (uint64_t)(h - 1u) * pitch + row_bytes;
    uint64_t cap = vm_size ? vm_size : (UINT64_C(1) << 32);
    return (uint64_t)ea <= cap && bytes <= cap - ea;
}

/* Deferred D3D readback is R,G,B,A; guest A8R8G8B8 bytes are A,R,G,B.
 * Operate only on packed dirty rows, never D3D's padded row pitch. */
static inline void rsx_exact_blit_rgba_to_guest_argb(
    uint8_t* dst, const uint8_t* src, uint32_t pixels)
{
    uint32_t x = 0;
#if defined(__SSE2__) || defined(_M_X64)
    /* Runtime readback uses distinct allocations. Retain the original scalar
     * semantics for any overlapping caller rather than widening its reads. */
    uintptr_t d = (uintptr_t)dst, s = (uintptr_t)src;
    uint64_t bytes = (uint64_t)pixels * 4;
    if ((d > s ? d - s : s - d) >= bytes) {
        for (; pixels - x >= 4; x += 4) {
            __m128i rgba = _mm_loadu_si128((const __m128i*)(src + (uint64_t)x * 4));
            __m128i argb = _mm_or_si128(_mm_slli_epi32(rgba, 8),
                                         _mm_srli_epi32(rgba, 24));
            _mm_storeu_si128((__m128i*)(dst + (uint64_t)x * 4), argb);
        }
    }
#endif
    for (; x < pixels; ++x) {
        dst[x * 4 + 0] = src[x * 4 + 3];
        dst[x * 4 + 1] = src[x * 4 + 0];
        dst[x * 4 + 2] = src[x * 4 + 1];
        dst[x * 4 + 3] = src[x * 4 + 2];
    }
}

static inline int rsx_exact_blit_rects_overlap(
    uint32_t ax, uint32_t ay, uint32_t aw, uint32_t ah,
    uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh)
{
    uint64_t ar = (uint64_t)ax + aw, ab = (uint64_t)ay + ah;
    uint64_t br = (uint64_t)bx + bw, bb = (uint64_t)by + bh;
    return ax < br && bx < ar && ay < bb && by < ab;
}

/* A later terminal write may replace an earlier plan only when it covers the
 * entire older rectangle. Partial overlap would otherwise discard untouched
 * authoritative pixels, so backend preflight rejects it. */
static inline int rsx_exact_blit_rect_fully_contains(
    uint32_t outer_x, uint32_t outer_y, uint32_t outer_w, uint32_t outer_h,
    uint32_t inner_x, uint32_t inner_y, uint32_t inner_w, uint32_t inner_h)
{
    uint64_t outer_r = (uint64_t)outer_x + outer_w;
    uint64_t outer_b = (uint64_t)outer_y + outer_h;
    uint64_t inner_r = (uint64_t)inner_x + inner_w;
    uint64_t inner_b = (uint64_t)inner_y + inner_h;
    return outer_w && outer_h && inner_w && inner_h &&
           outer_x <= inner_x && outer_y <= inner_y &&
           outer_r >= inner_r && outer_b >= inner_b;
}

/* Sequential preflight snapshots source authority before it updates the
 * destination. The returned value is record-local: a future producer cannot
 * retroactively authorize an earlier source. */
static inline int rsx_exact_blit_authority_step(
    int source_authoritative, int* destination_authoritative)
{
    int cached = source_authoritative != 0;
    if (cached && destination_authoritative) *destination_authoritative = 1;
    return cached;
}

static inline int rsx_exact_blit_mapping_generation_stable(
    uint64_t before, uint64_t after)
{
    return !(before & 1u) && before == after;
}

static inline int rsx_exact_blit_record_mapping_matches(
    uint64_t record_generation, uint64_t before, uint64_t after,
    uint32_t record_src_ea, uint32_t resolved_src_ea,
    uint32_t record_dst_ea, uint32_t resolved_dst_ea)
{
    return rsx_exact_blit_mapping_generation_stable(before, after) &&
           record_generation == before &&
           record_src_ea == resolved_src_ea &&
           record_dst_ea == resolved_dst_ea;
}

static inline int rsx_exact_blit_canonical_spans_overlap(
    uint32_t ea_a, uint64_t span_a, uint32_t ea_b, uint64_t span_b)
{
    return span_a && span_b && (uint64_t)ea_a < (uint64_t)ea_b + span_b &&
           (uint64_t)ea_b < (uint64_t)ea_a + span_a;
}

/* A Texture2D SRV cannot crop or reinterpret a MAIN mirror.  Reuse is safe
 * only for the exact resolved layout; every other canonical overlap must be
 * rejected before an immutable guest snapshot can be created. */
static inline int rsx_exact_blit_main_source_layout_matches(
    uint32_t mirror_ea, uint32_t mirror_pitch, uint32_t mirror_w,
    uint32_t mirror_h, uint32_t source_ea, uint32_t source_pitch,
    uint32_t source_w, uint32_t source_h)
{
    return mirror_pitch == source_pitch && mirror_w == source_w &&
           mirror_h == source_h && mirror_ea == source_ea;
}

static inline int rsx_exact_blit_baseline_matches(
    const uint8_t* current, const uint8_t* baseline, uint64_t bytes)
{
    return current && baseline && bytes <= SIZE_MAX &&
           memcmp(current, baseline, (size_t)bytes) == 0;
}

/* The writeback path keeps the GPU result packed and checks every dirty guest
 * row immediately before its all-or-nothing commit.  Keep this pure so the
 * focused test can cover split rectangles without D3D/guest-thread state. */
static inline int rsx_exact_blit_dirty_rows_match(
    const uint8_t* guest, uint32_t guest_pitch, const uint8_t* baseline,
    uint32_t row_bytes, uint32_t rows)
{
    if (!guest || !baseline || !guest_pitch || !row_bytes || !rows ||
        row_bytes > guest_pitch)
        return 0;
    for (uint32_t y = 0; y < rows; ++y)
        if (memcmp(guest + (uint64_t)y * guest_pitch,
                   baseline + (uint64_t)y * row_bytes, row_bytes) != 0)
            return 0;
    return 1;
}

static inline void rsx_exact_blit_rgba_rows_to_guest_argb(
    uint8_t* guest, uint32_t guest_pitch, const uint8_t* rgba,
    uint32_t rgba_pitch, uint32_t pixels, uint32_t rows)
{
    for (uint32_t y = 0; y < rows; ++y)
        rsx_exact_blit_rgba_to_guest_argb(guest + (uint64_t)y * guest_pitch,
                                          rgba + (uint64_t)y * rgba_pitch,
                                          pixels);
}

static inline uint32_t rsx_scaled_src_bpp(uint8_t format)
{
    if (format == 3 || format == 4) return 4; /* A/X8R8G8B8 */
    if (format == 7) return 2;                /* R5G6B5 */
    return 0;
}

static inline uint32_t rsx_scaled_dst_bpp(uint8_t format)
{
    if (format == 10 || format == 11) return 4; /* A8R8G8B8 / Y32 */
    if (format == 4) return 2;                  /* R5G6B5 */
    return 0;
}

/* Morton/Z-order texel index for a power-of-two RSX swizzled surface.  Keep
 * the layout primitive here so CPU transfer paths use the same addressing as
 * the texture uploader. */
static inline uint32_t rsx_scaled_swizzled_offset(
    uint32_t x, uint32_t y, uint32_t log2w, uint32_t log2h)
{
    uint32_t off = 0, shift = 0;
    while (log2w && log2h) {
        off |= (x & 1u) << shift; x >>= 1; ++shift;
        off |= (y & 1u) << shift; y >>= 1; ++shift;
        --log2w; --log2h;
    }
    return off | ((x | y) << shift);
}

static inline void rsx_scaled_convert_pixel(
    uint8_t* dst, const uint8_t* src, uint32_t src_bpp, uint32_t dst_bpp)
{
    if (src_bpp == dst_bpp) {
        memcpy(dst, src, src_bpp);
    } else if (src_bpp == 2) {
        uint16_t p = (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
        dst[0] = 0xFF;
        dst[1] = (uint8_t)((((p >> 11) & 31) * 255 + 15) / 31);
        dst[2] = (uint8_t)((((p >> 5) & 63) * 255 + 31) / 63);
        dst[3] = (uint8_t)(((p & 31) * 255 + 15) / 31);
    } else {
        uint16_t p = (uint16_t)(((uint16_t)(src[1] >> 3) << 11) |
                                ((uint16_t)(src[2] >> 2) << 5) |
                                (src[3] >> 3));
        dst[0] = (uint8_t)(p >> 8);
        dst[1] = (uint8_t)p;
    }
}

static inline void rsx_scaled_read_rgba(
    uint8_t out[4], const uint8_t* src, uint32_t src_bpp)
{
    if (src_bpp == 4)
        memcpy(out, src, 4);
    else
        rsx_scaled_convert_pixel(out, src, src_bpp, 4);
}

static inline int rsx_scaled_blit_exact_rgba_region(
    const struct rsx_scaled_blit_desc* d,
    struct rsx_scaled_blit_exact_region* out)
{
    uint32_t src_bpp = rsx_scaled_src_bpp(d->src_format);
    uint32_t dst_bpp = rsx_scaled_dst_bpp(d->dst_format);
    if (src_bpp != 4 || dst_bpp != 4 || !d->in_width || !d->in_height ||
        !d->out_width || !d->out_height || !d->clip_width || !d->clip_height ||
        !d->ds_dx || !d->dt_dy)
        return 0;
    uint32_t step_x = (uint32_t)d->ds_dx;
    uint32_t step_y = (uint32_t)d->dt_dy;
    if (step_x != 0x100000u || step_y != 0x100000u ||
        d->clip_x >= d->out_width || d->clip_y >= d->out_height)
        return 0;
    uint32_t width = d->clip_width, height = d->clip_height;
    if (width > d->out_width - d->clip_x) width = d->out_width - d->clip_x;
    if (height > d->out_height - d->clip_y) height = d->out_height - d->clip_y;
    int32_t base_x = (int32_t)d->in_x_4 - (d->origin == 1 ? 8 : 0);
    int32_t base_y = (int32_t)d->in_y_4 - (d->origin == 1 ? 8 : 0);
    base_x = base_x > 0 ? base_x / 16 : 0;
    base_y = base_y > 0 ? base_y / 16 : 0;
    uint32_t src_x = (uint32_t)base_x + d->clip_x;
    uint32_t src_y = (uint32_t)base_y + d->clip_y;
    if (src_x >= d->in_width || src_y >= d->in_height) return 0;
    if (width > d->in_width - src_x) width = d->in_width - src_x;
    if (height > d->in_height - src_y) height = d->in_height - src_y;
    if (!width || !height || d->out_x > 0xFFFFu - d->clip_x ||
        d->out_y > 0xFFFFu - d->clip_y)
        return 0;
    if (out) {
        out->src_x = (uint16_t)src_x; out->src_y = (uint16_t)src_y;
        out->dst_x = (uint16_t)(d->out_x + d->clip_x);
        out->dst_y = (uint16_t)(d->out_y + d->clip_y);
        out->width = (uint16_t)width; out->height = (uint16_t)height;
    }
    return 1;
}

/* Negative NV3089 scale mirrors around the remaining source extent.  Clamp
 * the absolute-scale sample before reversing it so oversized coordinates keep
 * sampling the nearest edge instead of repeating the wrong edge. */
static inline uint32_t rsx_scaled_blit_axis_coordinate(
    uint32_t sample, int32_t step, uint32_t source_extent)
{
    uint64_t magnitude = step < 0 ? (uint64_t)-(int64_t)step : (uint64_t)step;
    uint64_t coordinate = ((uint64_t)sample * magnitude) >> 20;
    if (coordinate >= source_extent)
        coordinate = source_extent - 1u;
    return step < 0 ? source_extent - 1u - (uint32_t)coordinate :
                      (uint32_t)coordinate;
}

/* FOH blends toward the next texel in the signed scale direction.  Keep the
 * fixed-point product in 64 bits before clamping extreme 11.20 scales. */
static inline void rsx_scaled_blit_axis_foh(
    uint32_t sample, int32_t step, uint32_t source_extent,
    uint32_t* first, uint32_t* second, uint32_t* fraction)
{
    const uint64_t magnitude = step < 0 ? (uint64_t)-(int64_t)step :
                                           (uint64_t)step;
    const uint64_t fixed = (uint64_t)sample * magnitude;
    uint64_t coordinate = fixed >> 20;
    uint32_t frac = (uint32_t)(fixed & 0xFFFFFu);
    if (coordinate >= source_extent - 1u) {
        coordinate = source_extent - 1u;
        frac = 0;
    }
    if (step < 0) {
        *first = source_extent - 1u - (uint32_t)coordinate;
        *second = *first ? *first - 1u : *first;
    } else {
        *first = (uint32_t)coordinate;
        *second = *first + 1u < source_extent ? *first + 1u : *first;
    }
    *fraction = frac;
}

static inline uint8_t rsx_scaled_blit_lerp_u8(
    uint8_t first, uint8_t second, uint32_t fraction)
{
    const uint32_t one = 0x100000u;
    return (uint8_t)(((uint64_t)first * (one - fraction) +
                      (uint64_t)second * fraction + (one >> 1)) >> 20);
}

static inline enum rsx_scaled_blit_result rsx_scaled_blit(
    const struct rsx_scaled_blit_desc* d)
{
    uint32_t src_bpp = rsx_scaled_src_bpp(d->src_format);
    uint32_t dst_bpp = rsx_scaled_dst_bpp(d->dst_format);
    if (!src_bpp || !dst_bpp)
        return RSX_SCALED_BLIT_UNSUPPORTED_FORMAT;
    if (!d->in_width || !d->in_height || !d->out_width || !d->out_height ||
        !d->clip_width || !d->clip_height)
        return RSX_SCALED_BLIT_EMPTY;
    if (!d->ds_dx || !d->dt_dy)
        return RSX_SCALED_BLIT_UNSUPPORTED_OPERATION;

    uint32_t src_pitch = d->src_pitch ? d->src_pitch : d->in_width * src_bpp;
    uint32_t dst_pitch = d->dst_pitch ? d->dst_pitch : d->out_width * dst_bpp;
    if (d->clip_x >= d->out_width || d->clip_y >= d->out_height)
        return RSX_SCALED_BLIT_EMPTY;
    uint32_t copy_w = d->clip_width;
    uint32_t copy_h = d->clip_height;
    if (copy_w > d->out_width - d->clip_x)
        copy_w = d->out_width - d->clip_x;
    if (copy_h > d->out_height - d->clip_y)
        copy_h = d->out_height - d->clip_y;
    int32_t base_x = (int32_t)d->in_x_4 - (d->origin == 1 ? 8 : 0);
    int32_t base_y = (int32_t)d->in_y_4 - (d->origin == 1 ? 8 : 0);
    base_x = base_x > 0 ? base_x / 16 : 0;
    base_y = base_y > 0 ? base_y / 16 : 0;
    if ((uint32_t)base_x >= d->in_width || (uint32_t)base_y >= d->in_height)
        return RSX_SCALED_BLIT_EMPTY;
    /* rel_x/rel_y are added to base before forming src.  Clamp against the
     * remaining source extent, not the whole source, so a nonzero IMAGE_IN
     * point cannot form a pointer past the source allocation. */
    uint32_t source_width = d->in_width - (uint32_t)base_x;
    uint32_t source_height = d->in_height - (uint32_t)base_y;
    int32_t step_x = d->ds_dx;
    int32_t step_y = d->dt_dy;

    if (src_bpp == dst_bpp && step_x == 0x100000 && step_y == 0x100000) {
        uint32_t sx = base_x + d->clip_x;
        uint32_t sy = base_y + d->clip_y;
        if (sx >= d->in_width || sy >= d->in_height)
            return RSX_SCALED_BLIT_EMPTY;
        if (copy_w > d->in_width - sx) copy_w = d->in_width - sx;
        if (copy_h > d->in_height - sy) copy_h = d->in_height - sy;
        for (uint32_t y = 0; y < copy_h; y++)
            memcpy(d->dst + (size_t)(d->out_y + d->clip_y + y) * dst_pitch +
                              (size_t)(d->out_x + d->clip_x) * dst_bpp,
                   d->src + (size_t)(sy + y) * src_pitch +
                              (size_t)sx * src_bpp,
                   (size_t)copy_w * src_bpp);
        return RSX_SCALED_BLIT_OK;
    }

    for (uint32_t y = 0; y < copy_h; y++) {
        uint32_t rel_y = rsx_scaled_blit_axis_coordinate(
            d->clip_y + y, step_y, source_height);
        for (uint32_t x = 0; x < copy_w; x++) {
            uint32_t rel_x = rsx_scaled_blit_axis_coordinate(
                d->clip_x + x, step_x, source_width);
            uint8_t* dst = d->dst +
                (size_t)(d->out_y + d->clip_y + y) * dst_pitch +
                (size_t)(d->out_x + d->clip_x + x) * dst_bpp;
            if (d->interpolator != RSX_SCALED_BLIT_INTERPOLATOR_FOH) {
                const uint8_t* src = d->src +
                    (size_t)(base_y + rel_y) * src_pitch +
                    (size_t)(base_x + rel_x) * src_bpp;
                rsx_scaled_convert_pixel(dst, src, src_bpp, dst_bpp);
            } else {
                uint32_t rel_x1, rel_y1, frac_x, frac_y;
                uint8_t p00[4], p10[4], p01[4], p11[4];
                uint8_t top[4], bottom[4], blended[4];
                rsx_scaled_blit_axis_foh(d->clip_x + x, step_x, source_width,
                                          &rel_x, &rel_x1, &frac_x);
                rsx_scaled_blit_axis_foh(d->clip_y + y, step_y, source_height,
                                          &rel_y, &rel_y1, &frac_y);
                const uint8_t* s00 = d->src +
                    (size_t)(base_y + rel_y) * src_pitch +
                    (size_t)(base_x + rel_x) * src_bpp;
                const uint8_t* s10 = d->src +
                    (size_t)(base_y + rel_y) * src_pitch +
                    (size_t)(base_x + rel_x1) * src_bpp;
                const uint8_t* s01 = d->src +
                    (size_t)(base_y + rel_y1) * src_pitch +
                    (size_t)(base_x + rel_x) * src_bpp;
                const uint8_t* s11 = d->src +
                    (size_t)(base_y + rel_y1) * src_pitch +
                    (size_t)(base_x + rel_x1) * src_bpp;
                rsx_scaled_read_rgba(p00, s00, src_bpp);
                rsx_scaled_read_rgba(p10, s10, src_bpp);
                rsx_scaled_read_rgba(p01, s01, src_bpp);
                rsx_scaled_read_rgba(p11, s11, src_bpp);
                for (uint32_t c = 0; c < 4u; ++c) {
                    top[c] = rsx_scaled_blit_lerp_u8(p00[c], p10[c], frac_x);
                    bottom[c] = rsx_scaled_blit_lerp_u8(p01[c], p11[c], frac_x);
                    blended[c] = rsx_scaled_blit_lerp_u8(top[c], bottom[c], frac_y);
                }
                rsx_scaled_convert_pixel(dst, blended, 4, dst_bpp);
            }
        }
    }
    return RSX_SCALED_BLIT_OK;
}

#endif
