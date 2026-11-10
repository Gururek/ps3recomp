/*
 * ps3recomp - cellResc HLE implementation
 *
 * Resolution scaling bridge. The host present path performs the final scale,
 * but RESC still owns source registration, flip submission and flip state.
 */

#include "cellResc.h"
#include "cellGcmSys.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_initialized = 0;
static CellRescInitConfig s_config;
static u32 s_display_mode = 0;
static CellRescSrc s_src[8]; /* up to 8 color buffers */
static u8 s_src_set[8];
static CellRescDsts s_dsts[4]; /* one per display mode */
static uintptr_t s_color_buffer_ea = 0;
static uintptr_t s_vertex_buffer_ea = 0;
static uintptr_t s_fragment_buffer_ea = 0;
static float s_aspect_h = 1.0f;
static float s_aspect_v = 1.0f;
static float s_pal_ratio = 0.5f;

static int resc_valid_display_mode(u32 mode)
{
    return mode == CELL_RESC_720x480 || mode == CELL_RESC_720x576 ||
           mode == CELL_RESC_1280x720 || mode == CELL_RESC_1920x1080;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellRescInit(const CellRescInitConfig* initConfig)
{
    printf("[cellResc] Init()\n");

    if (s_initialized)
        return (s32)CELL_RESC_ERROR_REINITIALIZED;

    if (!initConfig || initConfig->size > sizeof(CellRescInitConfig))
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    s_config = *initConfig;
    memset(s_src, 0, sizeof(s_src));
    memset(s_src_set, 0, sizeof(s_src_set));
    memset(s_dsts, 0, sizeof(s_dsts));
    s_display_mode = 0;
    s_color_buffer_ea = 0;
    s_vertex_buffer_ea = 0;
    s_fragment_buffer_ea = 0;
    s_aspect_h = 1.0f;
    s_aspect_v = 1.0f;
    s_initialized = 1;
    return CELL_OK;
}

void cellRescExit(void)
{
    printf("[cellResc] Exit()\n");
    cellGcmSetFlipHandler(NULL);
    cellGcmSetVBlankHandler(NULL);
    s_initialized = 0;
}

s32 cellRescSetDisplayMode(u32 displayMode)
{
    printf("[cellResc] SetDisplayMode(0x%x)\n", displayMode);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!resc_valid_display_mode(displayMode) ||
        !(s_config.supportModes & displayMode))
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    if (displayMode == CELL_RESC_720x576) {
        const u32 temporal = s_config.palTemporalMode;
        if (((temporal >= CELL_RESC_PAL_60_DROP &&
              temporal <= CELL_RESC_PAL_60_INTERPOLATE_DROP_FLEXIBLE) &&
             s_config.flipMode == CELL_RESC_DISPLAY_HSYNC) ||
            (temporal == CELL_RESC_PAL_60_FOR_HSYNC &&
             s_config.flipMode == CELL_RESC_DISPLAY_VSYNC))
            return (s32)CELL_RESC_ERROR_BAD_COMBINATION;
    }

    s_display_mode = displayMode;
    return CELL_OK;
}

s32 cellRescGetNumColorBuffers(u32 displayMode, u32 palTemporalMode, s32 reserved)
{
    if (reserved != 0)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    if (displayMode == CELL_RESC_720x576) {
        if (palTemporalMode >= CELL_RESC_PAL_60_INTERPOLATE &&
            palTemporalMode <= CELL_RESC_PAL_60_INTERPOLATE_DROP_FLEXIBLE)
            return 6;
        if (palTemporalMode == CELL_RESC_PAL_60_DROP)
            return 3;
    }
    return 2;
}

s32 cellRescGetBufferSize(u32* colorBufSize, u32* vertexBufSize, u32* fragmentBufSize)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    /* Provide reasonable buffer sizes for state tracking */
    if (colorBufSize)
        *colorBufSize = 1920 * 1080 * 4; /* RGBA 1080p */
    if (vertexBufSize)
        *vertexBufSize = 64 * 1024; /* 64KB vertex buffer */
    if (fragmentBufSize)
        *fragmentBufSize = 256 * 1024; /* 256KB fragment shader */

    return CELL_OK;
}

s32 cellRescSetBufferAddress(void* colorBuf, void* vertexBuf, void* fragmentBuf)
{
    printf("[cellResc] SetBufferAddress()\n");

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!colorBuf || !vertexBuf || !fragmentBuf)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    if (((uintptr_t)colorBuf & 127u) || ((uintptr_t)vertexBuf & 3u) ||
        ((uintptr_t)fragmentBuf & 63u))
        return (s32)CELL_RESC_ERROR_BAD_ALIGNMENT;

    s_color_buffer_ea = (uintptr_t)colorBuf;
    s_vertex_buffer_ea = (uintptr_t)vertexBuf;
    s_fragment_buffer_ea = (uintptr_t)fragmentBuf;
    return CELL_OK;
}

s32 cellRescSetSrc(s32 index, const CellRescSrc* src)
{
    printf("[cellResc] SetSrc(index=%d)\n", index);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!src || index < 0 || index >= 8 || !src->width || !src->height ||
        src->height > 4096 || !src->pitch)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    s_src[index] = *src;
    s_src_set[index] = 1;
    return CELL_OK;
}

s32 cellRescSetDsts(u32 displayMode, const CellRescDsts* dsts)
{
    printf("[cellResc] SetDsts(mode=0x%x)\n", displayMode);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (!dsts || !resc_valid_display_mode(displayMode))
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    /* Map display mode to index */
    int idx = 0;
    if (displayMode & CELL_RESC_720x576) idx = 1;
    else if (displayMode & CELL_RESC_1280x720) idx = 2;
    else if (displayMode & CELL_RESC_1920x1080) idx = 3;

    s_dsts[idx] = *dsts;
    return CELL_OK;
}

s32 cellRescSetConvertAndFlip(void* context, s32 index)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    if (index < 0 || index >= 8 || !s_src_set[index])
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    const CellRescSrc* src = &s_src[index];
    s32 rc = cellGcmSetDisplayBuffer((u32)index, src->offset, src->pitch,
                                     src->width, src->height);
    if (rc != CELL_OK)
        return (s32)CELL_RESC_ERROR_GCM_FLIP_QUE_FULL;

    (void)context; /* HLE submits immediately; no guest command words needed. */
    rc = cellGcmSetFlipCommand((u32)index);
    return rc == CELL_OK ? CELL_OK : (s32)CELL_RESC_ERROR_GCM_FLIP_QUE_FULL;
}

s32 cellRescSetWaitFlip(void* context)
{
    (void)context;
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    cellGcmSetWaitFlip();
    return CELL_OK;
}

void cellRescSetFlipHandler(void (*handler)(u32))
{
    printf("[cellResc] SetFlipHandler(%p)\n", (void*)(uintptr_t)handler);
    cellGcmSetFlipHandler((CellGcmFlipHandler)handler);
}

void cellRescSetVBlankHandler(void (*handler)(u32))
{
    printf("[cellResc] SetVBlankHandler(%p)\n", (void*)(uintptr_t)handler);
    cellGcmSetVBlankHandler((CellGcmVBlankHandler)handler);
}

s32 cellRescGetDisplayMode(u32* displayMode)
{
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    if (!displayMode)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    *displayMode = s_display_mode;
    return CELL_OK;
}

u64 cellRescGetLastFlipTime(void)
{
    return cellGcmGetLastFlipTime();
}

void cellRescResetFlipStatus(void)
{
    cellGcmResetFlipStatus();
}

s32 cellRescGetFlipStatus(void)
{
    return (s32)cellGcmGetFlipStatus();
}

s32 cellRescSetPalInterpolateDropFlexRatio(float ratio)
{
    printf("[cellResc] SetPalInterpolateDropFlexRatio(%.2f)\n", ratio);
    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;
    if (ratio < 0.0f || ratio > 1.0f)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    s_pal_ratio = ratio;
    return CELL_OK;
}

s32 cellRescCreateInterlaceTable(void* buf, float ea, u32 tableLen, s32 depth)
{
    (void)ea;
    (void)depth;

    printf("[cellResc] CreateInterlaceTable(len=%u, depth=%d)\n", tableLen, depth);

    if (!buf || ea <= 0.0f || tableLen == 0 || depth < 0 || depth > 1)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;
    if (!s_display_mode)
        return (s32)CELL_RESC_ERROR_BAD_COMBINATION;

    /* Fill with simple linear interpolation weights */
    float* table = (float*)buf;
    for (u32 i = 0; i < tableLen; i++)
        table[i] = (float)i / (float)(tableLen > 1 ? tableLen - 1 : 1);

    return CELL_OK;
}

s32 cellRescAdjustAspectRatio(float horizontal, float vertical)
{
    printf("[cellResc] AdjustAspectRatio(h=%.2f, v=%.2f)\n", horizontal, vertical);

    if (!s_initialized)
        return (s32)CELL_RESC_ERROR_NOT_INITIALIZED;

    if (horizontal < 0.5f || horizontal > 2.0f ||
        vertical < 0.5f || vertical > 2.0f)
        return (s32)CELL_RESC_ERROR_BAD_ARGUMENT;

    s_aspect_h = horizontal;
    s_aspect_v = vertical;
    return CELL_OK;
}
