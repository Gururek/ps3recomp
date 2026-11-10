/*
 * ps3recomp - PC key prompts inside the game's DXT5 UI atlases
 * (see rsx_prompt_atlas.h)
 *
 * Rects come from the decoded US (NPUA80523) packs: art rect = bbox of the
 * glyph's connected alpha>0 component(s); clear rect = art + 2 px, clipped
 * to the sprite's UV cell (ELF table 0x27D340, normalised to 1024x512) and
 * to neighbouring art. Hashes are XXH64 seed 0 of the payload after the
 * 16-byte TJPACK entry header.
 *
 * Compiled only when PS3RECOMP_PROMPT_ART is defined.
 */

#ifdef PS3RECOMP_PROMPT_ART

#include "rsx_prompt_atlas.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* Colour of transparent margin texels: the key-cap plate (#101318), so
 * bilinear filtering at the cell edge darkens instead of fringing. */
#define TPA_CLEAR_R 0x10
#define TPA_CLEAR_G 0x13
#define TPA_CLEAR_B 0x18

#define K  PAD_PROMPT_STYLE_KEY
#define P  PAD_PROMPT_STYLE_PLATE
#define D  PAD_PROMPT_STYLE_DPAD
#define KT PAD_PROMPT_STYLE_KEY_TEXT
#define LU PAD_PROMPT_LIT_UP
#define LD PAD_PROMPT_LIT_DOWN
#define LL PAD_PROMPT_LIT_LEFT
#define LR PAD_PROMPT_LIT_RIGHT

/* lang/EN/effectTexPack_EN.pac[0]: in-game key guide / tutorial atlas,
 * 1024x512 DXT5. Drawn through func_000FDD3C (ELF table 0x27D340). Not
 * redrawn here: '+' idx26, arrow idx34, stick tilt frames idx37-48 and the
 * mission banners. */
static const TjPromptCell k_effect_en0_cells[] = {
    /* name                         art rect           clear rect          style button                lit      caption */
    {"idx9 dpad (down lit)",        2, 131,  61, 189,   0, 129,  63, 191,  D,  PAD_PROMPT_GROUP_DPAD,  LD,      NULL},
    {"idx7 dpad (left+right lit)",  2, 197,  61, 255,   0, 195,  63, 256,  D,  PAD_PROMPT_GROUP_DPAD,  LL | LR, NULL},
    {"idx10 dpad (up+down lit)",    2, 262,  61, 320,   0, 260,  63, 320,  D,  PAD_PROMPT_GROUP_DPAD,  LU | LD, NULL},
    {"idx11 triangle",             67, 134, 122, 189,  65, 132, 124, 191,  K,  PAD_PROMPT_TRIANGLE,    0,       NULL},
    {"idx12 circle",              131, 133, 186, 188, 129, 131, 188, 190,  K,  PAD_PROMPT_CIRCLE,      0,       NULL},
    {"idx14 square",               68, 198, 123, 253,  66, 196, 125, 255,  K,  PAD_PROMPT_SQUARE,      0,       NULL},
    {"idx13 cross",               131, 198, 186, 253, 129, 196, 188, 255,  K,  PAD_PROMPT_CROSS,       0,       NULL},
    {"idx17/31 r1",               214, 140, 293, 180, 212, 138, 295, 182,  P,  PAD_PROMPT_R1,          0,       NULL},
    {"idx15 l1",                  212, 208, 292, 245, 210, 206, 294, 247,  P,  PAD_PROMPT_L1,          0,       NULL},
    {"idx19 right stick",         320, 135, 400, 188, 318, 133, 402, 190,  D,  PAD_PROMPT_GROUP_RSTICK, 0,      NULL},
    {"idx32 right stick (alt)",   414, 140, 492, 188, 412, 138, 494, 190,  D,  PAD_PROMPT_GROUP_RSTICK, 0,      NULL},
    {"idx20 select",               84, 274, 165, 309,  82, 272, 167, 311,  P,  PAD_PROMPT_SELECT,      0,       NULL},
    {"idx21 start",               216, 268, 293, 314, 214, 266, 295, 316,  P,  PAD_PROMPT_START,       0,       NULL},
    {"idx33 cross NEXT",          388, 331, 488, 376, 386, 329, 490, 378,  KT, PAD_PROMPT_CROSS,       0,       "NEXT"},
    {"idx27 triangle",            260, 390, 315, 445, 258, 388, 317, 447,  K,  PAD_PROMPT_TRIANGLE,    0,       NULL},
    {"idx28 circle",              323, 390, 378, 445, 321, 388, 380, 447,  K,  PAD_PROMPT_CIRCLE,      0,       NULL},
    {"idx29 cross",               386, 389, 441, 444, 384, 387, 443, 446,  K,  PAD_PROMPT_CROSS,       0,       NULL},
    {"idx30 square",              449, 390, 504, 445, 447, 388, 506, 447,  K,  PAD_PROMPT_SQUARE,      0,       NULL},
    {"idx16 l2",                   23, 464, 101, 504,  21, 462, 103, 506,  P,  PAD_PROMPT_L2,          0,       NULL},
    /* D-pad with DOWN lit and no table entry; its top margin stays at y451
     * because the FAIL... glow reaches y450. */
    {"dpad (down lit, untabled)", 128, 451, 189, 509, 126, 451, 189, 511,  D,  PAD_PROMPT_GROUP_DPAD,  LD,      NULL},
    {"idx8 dpad (up lit)",        190, 452, 252, 509, 189, 451, 253, 511,  D,  PAD_PROMPT_GROUP_DPAD,  LU,      NULL},
    {"idx18 r2",                  276, 465, 354, 502, 274, 463, 356, 504,  P,  PAD_PROMPT_R2,          0,       NULL},
    {"idx35 triangle Hold",       447, 455, 557, 510, 445, 453, 559, 512,  KT, PAD_PROMPT_TRIANGLE,    0,       "Hold"},
    {"idx36 circle Hold",         574, 454, 683, 509, 572, 452, 685, 511,  KT, PAD_PROMPT_CIRCLE,      0,       "Hold"},
};

/* lang/EN/titleTexPack_EN.pac[3]: front-end glyph row, 1920x1920 DXT5
 * (stats L1/R1 tabs, OK/Back footers; matched by art, no UV source found). */
static const TjPromptCell k_title_en3_cells[] = {
    {"dpad",      406, 140,  468, 200,  404, 138,  470, 202,  D, PAD_PROMPT_GROUP_DPAD, 0, NULL},
    {"cross",     494, 141,  549, 196,  492, 139,  551, 198,  K, PAD_PROMPT_CROSS,      0, NULL},
    {"triangle",  580, 141,  634, 196,  578, 139,  636, 198,  K, PAD_PROMPT_TRIANGLE,   0, NULL},
    {"square",    667, 141,  722, 196,  665, 139,  724, 198,  K, PAD_PROMPT_SQUARE,     0, NULL},
    {"circle",    752, 141,  806, 196,  750, 139,  808, 198,  K, PAD_PROMPT_CIRCLE,     0, NULL},
    {"l1",        835, 150,  915, 188,  833, 148,  917, 190,  P, PAD_PROMPT_L1,         0, NULL},
    {"r1",        941, 149, 1020, 187,  939, 147, 1022, 189,  P, PAD_PROMPT_R1,         0, NULL},
    {"select",   1064, 151, 1145, 186, 1062, 149, 1147, 188,  P, PAD_PROMPT_SELECT,     0, NULL},
    {"start",    1216, 145, 1296, 191, 1214, 143, 1298, 193,  P, PAD_PROMPT_START,      0, NULL},
};

#undef K
#undef P
#undef D
#undef KT
#undef LU
#undef LD
#undef LL
#undef LR

static const TjPromptAtlas k_atlases[] = {
    {"effectTexPack_EN.pac[0]", 0x8C81BB63DB97BBB4ull, 1024u * 512u, 1024u, 512u, 0x88u,
     k_effect_en0_cells, (uint32_t)(sizeof(k_effect_en0_cells) / sizeof(k_effect_en0_cells[0]))},
    {"titleTexPack_EN.pac[3]", 0xE13FEE561726C941ull, 1920u * 1920u, 1920u, 1920u, 0x88u,
     k_title_en3_cells, (uint32_t)(sizeof(k_title_en3_cells) / sizeof(k_title_en3_cells[0]))},
};

const TjPromptAtlas* tj_prompt_atlas_table(uint32_t* count)
{
    if (count) *count = (uint32_t)(sizeof(k_atlases) / sizeof(k_atlases[0]));
    return k_atlases;
}

const TjPromptAtlas* tj_prompt_atlas_find(uint64_t hash, size_t bytes, uint32_t w, uint32_t h)
{
    for (size_t i = 0; i < sizeof(k_atlases) / sizeof(k_atlases[0]); i++) {
        const TjPromptAtlas* a = &k_atlases[i];
        if (a->xxh64 == hash && a->bytes == bytes && a->w == w && a->h == h) return a;
    }
    return NULL;
}

int tj_prompt_atlas_validate(const TjPromptAtlas* atlas, char* err, size_t err_cap)
{
    for (uint32_t i = 0; i < atlas->cell_count; i++) {
        const TjPromptCell* c = &atlas->cells[i];
        if (c->cx0 < 0 || c->cy0 < 0 || c->cx1 > (int)atlas->w || c->cy1 > (int)atlas->h ||
            c->cx0 >= c->cx1 || c->cy0 >= c->cy1) {
            snprintf(err, err_cap, "%s: clear rect outside the texture", c->name);
            return -1;
        }
        if (c->ax0 < c->cx0 || c->ay0 < c->cy0 || c->ax1 > c->cx1 || c->ay1 > c->cy1 ||
            c->ax0 >= c->ax1 || c->ay0 >= c->ay1) {
            snprintf(err, err_cap, "%s: art rect not inside clear rect", c->name);
            return -1;
        }
        for (uint32_t j = i + 1; j < atlas->cell_count; j++) {
            const TjPromptCell* d = &atlas->cells[j];
            if (c->cx0 < d->cx1 && d->cx0 < c->cx1 && c->cy0 < d->cy1 && d->cy0 < c->cy1) {
                snprintf(err, err_cap, "%s overlaps %s", c->name, d->name);
                return -1;
            }
        }
    }
    if (err && err_cap) err[0] = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * BC3 codec
 * -----------------------------------------------------------------------*/
static void tpa_565_expand(uint16_t c, int out[3])
{
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = (r << 3) | (r >> 2);
    out[1] = (g << 2) | (g >> 4);
    out[2] = (b << 3) | (b >> 2);
}

static void tpa_color_palette(uint16_t c0, uint16_t c1, int pal[4][3])
{
    tpa_565_expand(c0, pal[0]);
    tpa_565_expand(c1, pal[1]);
    for (int k = 0; k < 3; k++) {          /* BC3 colour is always 4-colour */
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
}

static void tpa_alpha_palette(int a0, int a1, int pal[8])
{
    pal[0] = a0; pal[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = 0; pal[7] = 255;
    }
}

void tj_bc3_decode_block(const uint8_t src[16], uint8_t rgba[64])
{
    int ap[8];
    tpa_alpha_palette(src[0], src[1], ap);
    uint64_t abits = 0;
    for (int i = 0; i < 6; i++) abits |= (uint64_t)src[2 + i] << (8 * i);
    int cp[4][3];
    uint16_t c0 = (uint16_t)(src[8] | (src[9] << 8)), c1 = (uint16_t)(src[10] | (src[11] << 8));
    tpa_color_palette(c0, c1, cp);
    uint32_t cbits = (uint32_t)src[12] | ((uint32_t)src[13] << 8) |
                     ((uint32_t)src[14] << 16) | ((uint32_t)src[15] << 24);
    for (int i = 0; i < 16; i++) {
        const int* c = cp[(cbits >> (2 * i)) & 3u];
        rgba[4 * i + 0] = (uint8_t)c[0];
        rgba[4 * i + 1] = (uint8_t)c[1];
        rgba[4 * i + 2] = (uint8_t)c[2];
        rgba[4 * i + 3] = (uint8_t)ap[(abits >> (3 * i)) & 7u];
    }
}

/* -- alpha -- */
static int tpa_alpha_eval(const uint8_t a[16], int a0, int a1, uint64_t* bits)
{
    int pal[8], err = 0;
    uint64_t b = 0;
    tpa_alpha_palette(a0, a1, pal);
    for (int i = 0; i < 16; i++) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; k++) {
            int d = (int)a[i] - pal[k];
            d *= d;
            if (d < bd) { bd = d; best = k; }
        }
        err += bd;
        b |= (uint64_t)best << (3 * i);
    }
    *bits = b;
    return err;
}

static void tpa_encode_alpha(const uint8_t a[16], uint8_t dst[8], int quality)
{
    int mn = 255, mx = 0, lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) {
        if (a[i] < mn) mn = a[i];
        if (a[i] > mx) mx = a[i];
        if (a[i] != 0 && a[i] != 255) {
            if (a[i] < lo) lo = a[i];
            if (a[i] > hi) hi = a[i];
        }
    }
    int best_a0 = mn, best_a1 = mn, best_err = 1 << 30;
    uint64_t best_bits = 0, bits;
    if (mn == mx) {                          /* a0 == a1: index 0 everywhere */
        best_err = 0; best_bits = 0;
    } else {
        /* 8-level, a0 > a1 */
        int r = quality > 0 ? 3 : 0;
        for (int d0 = 0; d0 <= r; d0++)
            for (int d1 = 0; d1 <= r; d1++) {
                int a0 = mx - d0, a1 = mn + d1;
                if (a0 <= a1) continue;
                int e = tpa_alpha_eval(a, a0, a1, &bits);
                if (e < best_err) { best_err = e; best_a0 = a0; best_a1 = a1; best_bits = bits; }
            }
        /* 6-level with explicit 0 and 255, a0 <= a1 */
        if (lo > hi) { lo = 0; hi = 0; }
        int e = tpa_alpha_eval(a, lo, hi, &bits);
        if (e < best_err) { best_err = e; best_a0 = lo; best_a1 = hi; best_bits = bits; }
    }
    dst[0] = (uint8_t)best_a0;
    dst[1] = (uint8_t)best_a1;
    for (int i = 0; i < 6; i++) dst[2 + i] = (uint8_t)(best_bits >> (8 * i));
}

/* -- colour -- */
typedef struct TpaColorIn {
    float p[16][3];
    float w[16];
} TpaColorIn;

static uint16_t tpa_pack565(const float c[3])
{
    int r = (int)lroundf(c[0] * 31.f / 255.f);
    int g = (int)lroundf(c[1] * 63.f / 255.f);
    int b = (int)lroundf(c[2] * 31.f / 255.f);
    r = r < 0 ? 0 : (r > 31 ? 31 : r);
    g = g < 0 ? 0 : (g > 63 ? 63 : g);
    b = b < 0 ? 0 : (b > 31 ? 31 : b);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static float tpa_color_eval(const TpaColorIn* in, uint16_t c0, uint16_t c1, uint32_t* idx)
{
    int pal[4][3];
    tpa_color_palette(c0, c1, pal);
    float err = 0.f;
    uint32_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        float bd = 1e30f;
        for (int k = 0; k < 4; k++) {
            float dr = in->p[i][0] - (float)pal[k][0];
            float dg = in->p[i][1] - (float)pal[k][1];
            float db = in->p[i][2] - (float)pal[k][2];
            float d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = k; }
        }
        err += in->w[i] * bd;
        bits |= (uint32_t)best << (2 * i);
    }
    *idx = bits;
    return err;
}

/* Least-squares endpoints for fixed indices. Returns 0 when singular. */
static int tpa_color_refit(const TpaColorIn* in, uint32_t idx, float e0[3], float e1[3])
{
    static const float wa[4] = {1.f, 0.f, 2.f / 3.f, 1.f / 3.f};
    float aa = 0, ab = 0, bb = 0, ap[3] = {0, 0, 0}, bp[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        float a = wa[(idx >> (2 * i)) & 3u], b = 1.f - a, w = in->w[i];
        aa += w * a * a; ab += w * a * b; bb += w * b * b;
        for (int k = 0; k < 3; k++) { ap[k] += w * a * in->p[i][k]; bp[k] += w * b * in->p[i][k]; }
    }
    float det = aa * bb - ab * ab;
    if (fabsf(det) < 1e-6f) return 0;
    float inv = 1.f / det;
    for (int k = 0; k < 3; k++) {
        e0[k] = (ap[k] * bb - bp[k] * ab) * inv;
        e1[k] = (bp[k] * aa - ap[k] * ab) * inv;
        e0[k] = e0[k] < 0.f ? 0.f : (e0[k] > 255.f ? 255.f : e0[k]);
        e1[k] = e1[k] < 0.f ? 0.f : (e1[k] > 255.f ? 255.f : e1[k]);
    }
    return 1;
}

static uint16_t tpa_565_add(uint16_t c, int ch, int d)
{
    int v[3] = {(c >> 11) & 31, (c >> 5) & 63, c & 31};
    const int lim[3] = {31, 63, 31};
    v[ch] += d;
    if (v[ch] < 0 || v[ch] > lim[ch]) return c;
    return (uint16_t)((v[0] << 11) | (v[1] << 5) | v[2]);
}

static void tpa_encode_color(const TpaColorIn* in, uint8_t dst[8], int quality)
{
    float wsum = 0.f, mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        wsum += in->w[i];
        for (int k = 0; k < 3; k++) mean[k] += in->w[i] * in->p[i][k];
    }
    for (int k = 0; k < 3; k++) mean[k] /= wsum;
    float cov[6] = {0, 0, 0, 0, 0, 0};    /* rr rg rb gg gb bb */
    for (int i = 0; i < 16; i++) {
        float d[3] = {in->p[i][0] - mean[0], in->p[i][1] - mean[1], in->p[i][2] - mean[2]};
        float w = in->w[i];
        cov[0] += w * d[0] * d[0]; cov[1] += w * d[0] * d[1]; cov[2] += w * d[0] * d[2];
        cov[3] += w * d[1] * d[1]; cov[4] += w * d[1] * d[2]; cov[5] += w * d[2] * d[2];
    }
    float v[3] = {1.f, 1.f, 1.f};
    for (int it = 0; it < 8; it++) {
        float n[3] = {cov[0] * v[0] + cov[1] * v[1] + cov[2] * v[2],
                      cov[1] * v[0] + cov[3] * v[1] + cov[4] * v[2],
                      cov[2] * v[0] + cov[4] * v[1] + cov[5] * v[2]};
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-6f) break;
        v[0] = n[0] / len; v[1] = n[1] / len; v[2] = n[2] / len;
    }
    /* Extent along the axis over the texels that will be visible. */
    float tmin = 1e30f, tmax = -1e30f;
    for (int pass = 0; pass < 2 && tmin > tmax; pass++)
        for (int i = 0; i < 16; i++) {
            if (pass == 0 && in->w[i] < 0.25f) continue;
            float t = (in->p[i][0] - mean[0]) * v[0] + (in->p[i][1] - mean[1]) * v[1] +
                      (in->p[i][2] - mean[2]) * v[2];
            if (t < tmin) tmin = t;
            if (t > tmax) tmax = t;
        }
    float inset = (tmax - tmin) / 16.f;
    float e0[3], e1[3];
    for (int k = 0; k < 3; k++) {
        e0[k] = mean[k] + (tmax - inset) * v[k];
        e1[k] = mean[k] + (tmin + inset) * v[k];
        e0[k] = e0[k] < 0.f ? 0.f : (e0[k] > 255.f ? 255.f : e0[k]);
        e1[k] = e1[k] < 0.f ? 0.f : (e1[k] > 255.f ? 255.f : e1[k]);
    }
    uint16_t c0 = tpa_pack565(e0), c1 = tpa_pack565(e1);
    uint32_t idx, bidx;
    float best = tpa_color_eval(in, c0, c1, &bidx);
    for (int it = 0; it < 2; it++) {
        if (!tpa_color_refit(in, bidx, e0, e1)) break;
        uint16_t n0 = tpa_pack565(e0), n1 = tpa_pack565(e1);
        float e = tpa_color_eval(in, n0, n1, &idx);
        if (e < best) { best = e; c0 = n0; c1 = n1; bidx = idx; } else break;
    }
    if (quality > 0) {
        for (int pass = 0; pass < 8; pass++) {
            int improved = 0;
            for (int ch = 0; ch < 6; ch++)
                for (int d = -1; d <= 1; d += 2) {
                    uint16_t n0 = ch < 3 ? tpa_565_add(c0, ch, d) : c0;
                    uint16_t n1 = ch < 3 ? c1 : tpa_565_add(c1, ch - 3, d);
                    float e = tpa_color_eval(in, n0, n1, &idx);
                    if (e < best) { best = e; c0 = n0; c1 = n1; bidx = idx; improved = 1; }
                }
            if (!improved) break;
        }
    }
    /* Canonical order: c0 > c1 (4-colour under every decoder) or c0 == c1. */
    if (c0 < c1) {
        uint16_t t = c0; c0 = c1; c1 = t;
        bidx ^= 0x55555555u;                 /* 0<->1, 2<->3 */
    } else if (c0 == c1) {
        bidx = 0;
    }
    dst[0] = (uint8_t)c0; dst[1] = (uint8_t)(c0 >> 8);
    dst[2] = (uint8_t)c1; dst[3] = (uint8_t)(c1 >> 8);
    dst[4] = (uint8_t)bidx; dst[5] = (uint8_t)(bidx >> 8);
    dst[6] = (uint8_t)(bidx >> 16); dst[7] = (uint8_t)(bidx >> 24);
}

void tj_bc3_encode_block(const uint8_t rgba[64], uint8_t dst[16], int quality)
{
    uint8_t a[16];
    TpaColorIn in;
    for (int i = 0; i < 16; i++) {
        a[i] = rgba[4 * i + 3];
        for (int k = 0; k < 3; k++) in.p[i][k] = (float)rgba[4 * i + k];
        /* Hidden texels still count a little: bilinear filtering samples them. */
        in.w[i] = 0.06f + 0.94f * (float)a[i] / 255.f;
    }
    tpa_encode_alpha(a, dst, quality);
    tpa_encode_color(&in, dst + 8, quality);
}

/* ---------------------------------------------------------------------------
 * Patcher
 * -----------------------------------------------------------------------*/
static double tpa_now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    return 0.0;
#endif
}

int tj_prompt_atlas_patch(const TjPromptAtlas* atlas, uint8_t* bc3, size_t bytes,
                          uint32_t pitch, const PadPromptLabels* labels, int quality,
                          uint8_t* intended, uint8_t* touched, TjPromptPatchInfo* info)
{
    if (info) memset(info, 0, sizeof(*info));
    if (!atlas || !bc3 || !labels || (atlas->w & 3u) || (atlas->h & 3u)) return -1;
    const uint32_t bw = atlas->w / 4u, bh = atlas->h / 4u;
    if (pitch != bw * 16u || bytes != (size_t)pitch * bh || bytes != atlas->bytes) return -1;
    char err[128];
    if (tj_prompt_atlas_validate(atlas, err, sizeof(err)) != 0) return -1;

    int rc = 0;
    double t0 = tpa_now_ms();
    uint8_t** art = (uint8_t**)calloc(atlas->cell_count, sizeof(uint8_t*));
    uint8_t* tb = touched ? touched : (uint8_t*)malloc((size_t)bw * bh);
    if (!art || !tb) { rc = -3; goto done; }
    memset(tb, 0, (size_t)bw * bh);

    for (uint32_t i = 0; i < atlas->cell_count; i++) {
        const TjPromptCell* c = &atlas->cells[i];
        int aw = c->ax1 - c->ax0, ah = c->ay1 - c->ay0;
        art[i] = (uint8_t*)malloc((size_t)aw * (size_t)ah * 4u);
        if (!art[i]) { rc = -3; goto done; }
        PadPromptArtSpec spec;
        spec.style = (PadPromptStyle)c->style;
        spec.button = c->button;
        spec.lit = c->lit;
        spec.caption = c->caption;
        if (pad_prompt_art_render(&spec, labels, aw, ah, art[i]) != 0) { rc = -2; goto done; }
        for (int by = c->cy0 / 4; by <= (c->cy1 - 1) / 4; by++)
            for (int bx = c->cx0 / 4; bx <= (c->cx1 - 1) / 4; bx++)
                tb[(size_t)by * bw + (size_t)bx] = 1;
    }
    double t1 = tpa_now_ms();

    uint32_t blocks = 0;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            if (!tb[(size_t)by * bw + bx]) continue;
            uint8_t* blk = bc3 + (size_t)by * pitch + (size_t)bx * 16u;
            uint8_t px[64];
            tj_bc3_decode_block(blk, px);
            for (int t = 0; t < 16; t++) {
                int X = (int)bx * 4 + (t & 3), Y = (int)by * 4 + (t >> 2);
                for (uint32_t i = 0; i < atlas->cell_count; i++) {
                    const TjPromptCell* c = &atlas->cells[i];
                    if (X < c->cx0 || X >= c->cx1 || Y < c->cy0 || Y >= c->cy1) continue;
                    uint8_t* d = px + 4 * t;
                    if (X >= c->ax0 && X < c->ax1 && Y >= c->ay0 && Y < c->ay1) {
                        const uint8_t* s = art[i] + 4u * ((size_t)(Y - c->ay0) * (size_t)(c->ax1 - c->ax0) +
                                                          (size_t)(X - c->ax0));
                        memcpy(d, s, 4);
                    } else {
                        d[0] = TPA_CLEAR_R; d[1] = TPA_CLEAR_G; d[2] = TPA_CLEAR_B; d[3] = 0;
                    }
                    break;
                }
            }
            if (intended)
                for (int r = 0; r < 4; r++)
                    memcpy(intended + 4u * ((size_t)(by * 4u + (uint32_t)r) * atlas->w + bx * 4u),
                           px + 16 * r, 16);
            tj_bc3_encode_block(px, blk, quality);
            blocks++;
        }
    double t2 = tpa_now_ms();
    if (info) {
        info->cells = atlas->cell_count;
        info->blocks = blocks;
        info->ms_art = t1 - t0;
        info->ms_encode = t2 - t1;
    }

done:
    if (art)
        for (uint32_t i = 0; i < atlas->cell_count; i++) free(art[i]);
    free(art);
    if (tb && tb != touched) free(tb);
    return rc;
}

#else  /* !PS3RECOMP_PROMPT_ART */
typedef int rsx_prompt_atlas_disabled;   /* ISO C: a unit needs one declaration */
#endif
