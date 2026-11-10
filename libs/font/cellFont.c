/*
 * ps3recomp - cellFont HLE implementation
 *
 * Font system with optional stb_truetype.h backend for actual TTF rendering.
 * Without stb_truetype, returns valid (but empty) glyph data so games
 * don't crash.
 */

#include "cellFont.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <windows.h>
#endif

/* Try to include stb_truetype */
#if __has_include("stb_truetype.h")
  #define FONT_HAS_STB 1
  #ifndef STB_TRUETYPE_IMPLEMENTATION
    #define STB_TRUETYPE_IMPLEMENTATION
  #endif
  #include "stb_truetype.h"
#elif defined(PS3RECOMP_HAS_STB_TRUETYPE)
  #define FONT_HAS_STB 1
  #include "stb_truetype.h"
#else
  #define FONT_HAS_STB 0
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    int     in_use;
    u32     type;
    float   scale_x;
    float   scale_y;
    u32     resolution_dpi_h;
    u32     resolution_dpi_v;
    float   effect_weight;
    float   effect_slant;
    u32     renderer;       /* bound renderer index, or 0xFFFFFFFF */
    /* Font data */
    u8*     font_data;      /* TTF file data (owned if loaded from file) */
    u32     font_data_size;
    int     font_data_owned;
    u8*     glyph_bitmap;   /* returned alpha image, owned until next render/close */
#if FONT_HAS_STB
    stbtt_fontinfo stb_info;
    int     stb_valid;
#endif
} FontSlot;

typedef struct {
    int in_use;
} RendererSlot;

static int          s_font_initialized = 0;
static u32          s_open_mode = CELL_FONT_OPEN_MODE_DEFAULT;
static FontSlot     s_fonts[CELL_FONT_MAX_OPENED];
static RendererSlot s_renderers[CELL_FONT_MAX_RENDERERS];

/* ---------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

static FontSlot* font_alloc_slot(void)
{
    for (int i = 0; i < CELL_FONT_MAX_OPENED; i++) {
        if (!s_fonts[i].in_use) {
            memset(&s_fonts[i], 0, sizeof(FontSlot));
            s_fonts[i].in_use = 1;
            s_fonts[i].scale_x = 1.0f;
            s_fonts[i].scale_y = 1.0f;
            s_fonts[i].resolution_dpi_h = 72;
            s_fonts[i].resolution_dpi_v = 72;
            s_fonts[i].renderer = 0xFFFFFFFF;
            return &s_fonts[i];
        }
    }
    return NULL;
}

static FontSlot* font_get_slot(CellFont* font)
{
    if (!font || font->handle >= CELL_FONT_MAX_OPENED)
        return NULL;
    FontSlot* slot = &s_fonts[font->handle];
    if (!slot->in_use)
        return NULL;
    return slot;
}

#if FONT_HAS_STB
static float font_get_stb_scale_y(FontSlot* slot)
{
    if (!slot->stb_valid) return 0.0f;
    return stbtt_ScaleForPixelHeight(&slot->stb_info, slot->scale_y);
}

static float font_get_stb_scale_x(FontSlot* slot)
{
    float scale_y = font_get_stb_scale_y(slot);
    if (slot->scale_y <= 0.0f) return scale_y;
    return scale_y * slot->scale_x / slot->scale_y;
}

/* cellFont fontsets name PS3 system faces, which are not present in the game
 * VFS.  Use one known, local Windows face as the deterministic replacement so
 * the existing glyph/metrics path remains real rather than returning empty
 * images. */
static int font_open_system_fallback(FontSlot* slot)
{
    char path[MAX_PATH];
    const char* candidates[] = {
        "YuGothM.ttc", /* Japanese plus Latin: closest installed composite. */
        "msgothic.ttc",
        "segoeui.ttf"
    };
    FILE* f = NULL;
#if defined(_WIN32)
    UINT root_len = GetWindowsDirectoryA(path, MAX_PATH);
    if (root_len == 0 || root_len >= MAX_PATH - 32) return 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        int n = snprintf(path + root_len, MAX_PATH - root_len,
                         "\\Fonts\\%s", candidates[i]);
        if (n <= 0 || (size_t)n >= MAX_PATH - root_len) continue;
        f = fopen(path, "rb");
        if (f) break;
    }
#endif
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long size = ftell(f);
    if (size <= 0 || (unsigned long)size > UINT32_MAX ||
        fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    u8* data = (u8*)malloc((size_t)size);
    if (!data) { fclose(f); return 0; }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return 0;
    }
    fclose(f);
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&slot->stb_info, data, offset)) {
        free(data);
        return 0;
    }
    slot->font_data = data;
    slot->font_data_size = (u32)size;
    slot->font_data_owned = 1;
    slot->stb_valid = 1;
    return 1;
}
#endif

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellFontInit(const CellFontConfig* config)
{
    (void)config;
    printf("[cellFont] Init()\n");

    if (s_font_initialized)
        return CELL_FONT_ERROR_ALREADY_INITIALIZED;

    memset(s_fonts, 0, sizeof(s_fonts));
    memset(s_renderers, 0, sizeof(s_renderers));
    s_font_initialized = 1;
    return CELL_OK;
}

s32 cellFontEnd(void)
{
    printf("[cellFont] End()\n");

    if (!s_font_initialized)
        return CELL_FONT_ERROR_UNINITIALIZED;

    for (int i = 0; i < CELL_FONT_MAX_OPENED; i++) {
        if (s_fonts[i].in_use) {
            CellFont font = {0};
            font.handle = (u32)i;
            cellFontCloseFont(&font);
        }
    }

    s_font_initialized = 0;
    return CELL_OK;
}

s32 cellFontOpenFontFile(CellFont* font, const char* fontPath, u32 subNum, s32 uniqueId)
{
    (void)subNum;
    (void)uniqueId;
    printf("[cellFont] OpenFontFile(%s)\n", fontPath ? fontPath : "(null)");

    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!font || !fontPath)  return CELL_FONT_ERROR_INVALID_PARAMETER;

    FontSlot* slot = font_alloc_slot();
    if (!slot) return CELL_FONT_ERROR_ALLOCATION_FAILED;

    /* Load TTF file */
    FILE* f = fopen(fontPath, "rb");
    if (!f) {
        slot->in_use = 0;
        printf("[cellFont] Cannot open font file: %s\n", fontPath);
        return CELL_FONT_ERROR_OPEN_FAILED;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    slot->font_data = (u8*)malloc((size_t)size);
    if (!slot->font_data) {
        fclose(f);
        slot->in_use = 0;
        return CELL_FONT_ERROR_ALLOCATION_FAILED;
    }

    slot->font_data_size = (u32)fread(slot->font_data, 1, (size_t)size, f);
    slot->font_data_owned = 1;
    fclose(f);

#if FONT_HAS_STB
    if (stbtt_InitFont(&slot->stb_info, slot->font_data,
                       stbtt_GetFontOffsetForIndex(slot->font_data, 0))) {
        slot->stb_valid = 1;
    } else {
        printf("[cellFont] stbtt_InitFont failed for: %s\n", fontPath);
        slot->stb_valid = 0;
    }
#endif

    font->handle = (u32)(slot - s_fonts);
    font->type = 0;
    font->scale_x = 1.0f;
    font->scale_y = 1.0f;

    return CELL_OK;
}

s32 cellFontOpenFontMemory(CellFont* font, const void* data, u32 dataSize, u32 subNum, s32 uniqueId)
{
    (void)subNum;
    (void)uniqueId;
    printf("[cellFont] OpenFontMemory(size=%u)\n", dataSize);

    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!font || !data || dataSize == 0) return CELL_FONT_ERROR_INVALID_PARAMETER;

    FontSlot* slot = font_alloc_slot();
    if (!slot) return CELL_FONT_ERROR_ALLOCATION_FAILED;

    slot->font_data = (u8*)data; /* borrowed pointer */
    slot->font_data_size = dataSize;
    slot->font_data_owned = 0;

#if FONT_HAS_STB
    if (stbtt_InitFont(&slot->stb_info, slot->font_data,
                       stbtt_GetFontOffsetForIndex(slot->font_data, 0))) {
        slot->stb_valid = 1;
    } else {
        slot->stb_valid = 0;
    }
#endif

    font->handle = (u32)(slot - s_fonts);
    font->type = 0;
    font->scale_x = 1.0f;
    font->scale_y = 1.0f;

    return CELL_OK;
}

s32 cellFontOpenFontset(CellFontLibrary lib, CellFontType* fontType, CellFont* font)
{
    (void)lib;
    printf("[cellFont] OpenFontset(type=0x%X)\n", fontType ? fontType->type : 0);

    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!font || !fontType) return CELL_FONT_ERROR_INVALID_PARAMETER;

    /* The title opens the PS3 system-font set (type 0x201).  Map all fontset
     * requests to the same deterministic local fallback; type remains the
     * guest-requested value for font-id queries. */
    FontSlot* slot = font_alloc_slot();
    if (!slot) return CELL_FONT_ERROR_ALLOCATION_FAILED;

    slot->type = fontType->type;

#if FONT_HAS_STB
    if (!font_open_system_fallback(slot)) {
        slot->in_use = 0;
        printf("[cellFont] Cannot open system fallback font\n");
        return CELL_FONT_ERROR_OPEN_FAILED;
    }
#else
    slot->in_use = 0;
    return CELL_FONT_ERROR_NO_SUPPORT_FUNCTION;
#endif

    font->handle = (u32)(slot - s_fonts);
    font->type = slot->type;
    font->scale_x = 1.0f;
    font->scale_y = 1.0f;

    return CELL_OK;
}

s32 cellFontOpenFontInstance(CellFont* source, CellFont* font)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    FontSlot* parent = font_get_slot(source);
    if (!parent || !font) return CELL_FONT_ERROR_INVALID_PARAMETER;
    FontSlot* instance = font_alloc_slot();
    if (!instance) return CELL_FONT_ERROR_ALLOCATION_FAILED;
    *instance = *parent;
    instance->glyph_bitmap = NULL;
    *font = *source;
    font->handle = (u32)(instance - s_fonts);
    return CELL_OK;
}

s32 cellFontCloseFont(CellFont* font)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    /* Instances share immutable font bytes, but own scale/render/glyph state.
     * Keep the face alive until the final instance closes. */
    slot->in_use = 0;
    if (slot->font_data_owned && slot->font_data) {
        int shared = 0;
        for (int i = 0; i < CELL_FONT_MAX_OPENED; ++i)
            if (s_fonts[i].in_use && s_fonts[i].font_data == slot->font_data)
                shared = 1;
        if (!shared) free(slot->font_data);
    }
    free(slot->glyph_bitmap);
    slot->glyph_bitmap = NULL;
    return CELL_OK;
}

s32 cellFontSetFontOpenMode(u32 openMode)
{
    s_open_mode = openMode;
    return CELL_OK;
}

s32 cellFontCreateRenderer(CellFontLibrary lib, CellFontRenderer* renderer)
{
    (void)lib;
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!renderer) return CELL_FONT_ERROR_INVALID_PARAMETER;

    for (u32 i = 0; i < CELL_FONT_MAX_RENDERERS; i++) {
        if (!s_renderers[i].in_use) {
            s_renderers[i].in_use = 1;
            *renderer = i;
            return CELL_OK;
        }
    }

    return CELL_FONT_ERROR_ALLOCATION_FAILED;
}

s32 cellFontDestroyRenderer(CellFontRenderer renderer)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (renderer >= CELL_FONT_MAX_RENDERERS) return CELL_FONT_ERROR_INVALID_PARAMETER;

    s_renderers[renderer].in_use = 0;
    return CELL_OK;
}

s32 cellFontBindRenderer(CellFont* font, CellFontRenderer renderer)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;
    if (renderer >= CELL_FONT_MAX_RENDERERS || !s_renderers[renderer].in_use)
        return CELL_FONT_ERROR_INVALID_PARAMETER;

    if (slot->renderer != 0xFFFFFFFF)
        return CELL_FONT_ERROR_RENDERER_ALREADY_BIND;

    slot->renderer = renderer;
    return CELL_OK;
}

s32 cellFontUnbindRenderer(CellFont* font)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    slot->renderer = 0xFFFFFFFF;
    return CELL_OK;
}

s32 cellFontSetScalePixel(CellFont* font, float w, float h)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    slot->scale_x = w;
    slot->scale_y = h;
    font->scale_x = w;
    font->scale_y = h;

    return CELL_OK;
}

s32 cellFontSetResolutionDpi(CellFont* font, u32 hDpi, u32 vDpi)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;
    slot->resolution_dpi_h = hDpi ? hDpi : 72;
    slot->resolution_dpi_v = vDpi ? vDpi : 72;
    return CELL_OK;
}

s32 cellFontSetScalePoint(CellFont* font, float w, float h)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;
    return cellFontSetScalePixel(font,
                                 w * (float)slot->resolution_dpi_h / 72.0f,
                                 h * (float)slot->resolution_dpi_v / 72.0f);
}

s32 cellFontSetEffectWeight(CellFont* font, float weight)
{
    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;
    slot->effect_weight = weight;
    return CELL_OK;
}

s32 cellFontSetEffectSlant(CellFont* font, float slant)
{
    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;
    slot->effect_slant = slant;
    return CELL_OK;
}

s32 cellFontGetHorizontalLayout(CellFont* font, CellFontHorizontalLayout* layout)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!layout) return CELL_FONT_ERROR_INVALID_PARAMETER;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

#if FONT_HAS_STB
    if (slot->stb_valid) {
        float scale_y = font_get_stb_scale_y(slot);
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&slot->stb_info, &ascent, &descent, &lineGap);
        layout->baseLineY    = (float)ascent * scale_y;
        layout->lineHeight   = (float)(ascent - descent + lineGap) * scale_y;
        layout->effectHeight = (float)(ascent - descent) * scale_y;
        return CELL_OK;
    }
#endif

    /* Fallback: reasonable defaults based on scale */
    layout->baseLineY    = slot->scale_y * 0.8f;
    layout->lineHeight   = slot->scale_y * 1.2f;
    layout->effectHeight = slot->scale_y;
    return CELL_OK;
}

s32 cellFontGetRenderCharGlyphMetrics(CellFont* font, u32 code,
                                       CellFontGlyphMetrics* metrics)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!metrics) return CELL_FONT_ERROR_INVALID_PARAMETER;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    memset(metrics, 0, sizeof(CellFontGlyphMetrics));

#if FONT_HAS_STB
    if (slot->stb_valid) {
        float scale_x = font_get_stb_scale_x(slot);
        float scale_y = font_get_stb_scale_y(slot);
        int glyph = stbtt_FindGlyphIndex(&slot->stb_info, (int)code);
        if (glyph == 0) {
            /* Glyph not found -- return space-like metrics */
            metrics->h_advance = slot->scale_x * 0.5f;
            return CELL_OK;
        }

        int advanceWidth, leftSideBearing;
        stbtt_GetGlyphHMetrics(&slot->stb_info, glyph, &advanceWidth, &leftSideBearing);

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&slot->stb_info, glyph, scale_x, scale_y,
                               &x0, &y0, &x1, &y1);

        metrics->width      = (float)(x1 - x0);
        metrics->height     = (float)(y1 - y0);
        metrics->h_bearing_x = (float)leftSideBearing * scale_x;
        metrics->h_bearing_y = (float)(-y0);
        metrics->h_advance  = (float)advanceWidth * scale_x;
        metrics->v_advance  = metrics->height;

        return CELL_OK;
    }
#endif

    /* Fallback: monospace-like metrics */
    metrics->width       = slot->scale_x * 0.6f;
    metrics->height      = slot->scale_y * 0.8f;
    metrics->h_bearing_x = 0.0f;
    metrics->h_bearing_y = slot->scale_y * 0.7f;
    metrics->h_advance   = slot->scale_x * 0.6f;
    metrics->v_advance   = slot->scale_y;

    return CELL_OK;
}

s32 cellFontGetCharGlyphMetrics(CellFont* font, u32 code,
                                CellFontGlyphMetrics* metrics)
{
    /* The non-rendering query uses the font's current scale and glyph just as
     * the rendering metrics query does. Keeping one implementation also keeps
     * fallback and stb_truetype behavior identical. */
    return cellFontGetRenderCharGlyphMetrics(font, code, metrics);
}

s32 cellFontGetFontIdCode(CellFont* font, u32 code,
                          u32* fontId, u32* fontCode)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;
    if (!fontId || !fontCode) return CELL_FONT_ERROR_INVALID_PARAMETER;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;

    /* A real composite fontset can redirect a character to another backing
     * font. This runtime has one backing slot per opened font, so the selected
     * font type and the unchanged Unicode code point are the exact mapping. */
    *fontId = slot->type;
    *fontCode = code;
    return CELL_OK;
}

s32 cellFontGetRenderCharGlyphMetricsVertical(CellFont* font, u32 code,
                                               CellFontGlyphMetrics* metrics)
{
    /* Vertical metrics: use horizontal metrics as base, swap axes */
    s32 rc = cellFontGetRenderCharGlyphMetrics(font, code, metrics);
    if (rc != (s32)CELL_OK) return rc;

    /* Swap horizontal/vertical bearings for vertical layout */
    float tmp;
    tmp = metrics->h_bearing_x; metrics->h_bearing_x = metrics->v_bearing_x; metrics->v_bearing_x = tmp;
    tmp = metrics->h_bearing_y; metrics->h_bearing_y = metrics->v_bearing_y; metrics->v_bearing_y = tmp;
    tmp = metrics->h_advance;   metrics->h_advance = metrics->v_advance;     metrics->v_advance = tmp;

    return CELL_OK;
}

s32 cellFontRenderCharGlyphImage(CellFont* font, u32 code,
                                  CellFontRenderSurface* surface,
                                  float x, float y,
                                  CellFontGlyphMetrics* metrics,
                                  CellFontGlyphImage* image)
{
    if (!s_font_initialized) return CELL_FONT_ERROR_UNINITIALIZED;

    FontSlot* slot = font_get_slot(font);
    if (!slot) return CELL_FONT_ERROR_INVALID_PARAMETER;
    if (image) memset(image, 0, sizeof(*image));
    free(slot->glyph_bitmap);
    slot->glyph_bitmap = NULL;

    /* Get metrics if requested */
    CellFontGlyphMetrics local_metrics;
    if (!metrics) metrics = &local_metrics;
    cellFontGetRenderCharGlyphMetrics(font, code, metrics);

#if FONT_HAS_STB
    if (slot->stb_valid && surface && surface->buffer) {
        float scale_x = font_get_stb_scale_x(slot);
        float scale_y = font_get_stb_scale_y(slot);
        int glyph = stbtt_FindGlyphIndex(&slot->stb_info, (int)code);
        if (glyph == 0) goto done;

        int bw, bh, bx, by;
        u8* bitmap = stbtt_GetGlyphBitmap(&slot->stb_info, scale_x, scale_y,
                                           glyph, &bw, &bh, &bx, &by);
        if (!bitmap) goto done;

        /* CellFont coordinates are relative to the line's top, not its
         * baseline. Use the same ascent as GetHorizontalLayout. */
        int ascent, descent, line_gap;
        stbtt_GetFontVMetrics(&slot->stb_info, &ascent, &descent, &line_gap);
        int dx = (int)x + bx;
        int dy = (int)y + (int)(ascent * scale_y) + by;

        /* Packed color surfaces consume ImageTransInfo for caller-owned
         * blending. Do not overwrite their first color channel with alpha. */
        for (int row = 0; row < bh && (surface->pixelSizeByte == 1 || !image); row++) {
            int sy = dy + row;
            if (sy < 0 || sy >= surface->height) continue;
            for (int col = 0; col < bw; col++) {
                int sx = dx + col;
                if (sx < 0 || sx >= surface->width) continue;
                u8 alpha = bitmap[row * bw + col];
                if (alpha > 0) {
                    int offset = sy * surface->widthByte + sx * surface->pixelSizeByte;
                    /* Alpha-blend (simple overwrite for alpha-only surface) */
                    surface->buffer[offset] = alpha;
                }
            }
        }

        /* Fill image output if requested */
        if (image) {
            slot->glyph_bitmap = bitmap;
            image->buffer = bitmap;
            image->width = bw;
            image->height = bh;
            image->widthByte = bw;
            image->pixelSizeByte = 1;
            image->x = dx;
            image->y = dy;
        } else {
            stbtt_FreeBitmap(bitmap, NULL);
        }

        return CELL_OK;
    }
#endif

done:
    /* Space/missing glyph: empty transfer, with the metrics' advance intact. */

    return CELL_OK;
}

s32 cellFontSetupRenderScalePixel(CellFont* font, float w, float h)
{
    return cellFontSetScalePixel(font, w, h);
}
