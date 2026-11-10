/*
 * ps3recomp - PC key prompts inside the game's DXT5 UI atlases
 *
 * Table of the Tokyo Jungle (NPUA80523) texture atlases that hold PS3 button
 * glyphs, keyed by the renderer's content hash (rsx_stream_hash64, XXH64
 * seed 0, of the whole single-mip BC3 payload), plus an in-place patcher that
 * redraws each glyph cell as a PC key cap (libs/input/pad_prompt_art.c).
 *
 * Patching rules
 *  - Each cell has an art rect (the original glyph's alpha bbox) and a clear
 *    rect (art + 2 px, clipped to the sprite's UV cell and to neighbours).
 *  - Every 4x4 block that the clear rect touches is decoded, the pixels inside
 *    the clear rect are REPLACED (art inside the art rect, transparent in the
 *    margin), pixels outside it keep their decoded value, and the block is
 *    re-encoded. Blocks no clear rect touches are left byte-identical.
 *  - The encoder always writes colour endpoints c0 > c1 (or c0 == c1 with all
 *    indices 0), so DXT1-style 3-colour decoders give the same result.
 *
 * This step covers effectTexPack_EN.pac[0] and titleTexPack_EN.pac[3] only.
 * Compiled only when PS3RECOMP_PROMPT_ART is defined (every .c file in libs/video is
 * globbed into ps3recomp_runtime; without the define this unit is empty).
 */

#ifndef PS3RECOMP_RSX_PROMPT_ATLAS_H
#define PS3RECOMP_RSX_PROMPT_ATLAS_H

#include <stddef.h>
#include <stdint.h>

#include "../input/pad_prompt_art.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TjPromptCell {
    const char* name;            /* e.g. "idx13 cross" (ELF table 0x27D340 index) */
    int16_t ax0, ay0, ax1, ay1;  /* art rect, end-exclusive texels */
    int16_t cx0, cy0, cx1, cy1;  /* clear rect (contains the art rect) */
    uint8_t style;               /* PadPromptStyle */
    uint8_t button;              /* PadPromptButton, or a PAD_PROMPT_GROUP_* */
    uint8_t lit;                 /* DPAD: PAD_PROMPT_LIT_* */
    const char* caption;         /* KEY_TEXT caption, else NULL */
} TjPromptCell;

typedef struct TjPromptAtlas {
    const char* name;            /* "effectTexPack_EN.pac[0]" */
    uint64_t xxh64;              /* rsx_stream_hash64 of the original payload */
    uint32_t bytes;              /* payload bytes (single mip, tight BC3) */
    uint32_t w, h;
    uint32_t rsx_fmt;            /* 0x88 = DXT45 (match with fmt & 0x9F) */
    const TjPromptCell* cells;
    uint32_t cell_count;
} TjPromptAtlas;

/* All known atlases. */
const TjPromptAtlas* tj_prompt_atlas_table(uint32_t* count);

/* Atlas with this content hash and shape, or NULL. */
const TjPromptAtlas* tj_prompt_atlas_find(uint64_t hash, size_t bytes, uint32_t w, uint32_t h);

/* Checks that every rect lies inside the texture, art rects lie inside their
 * clear rects and no two clear rects overlap. Returns 0 when valid; err gets
 * the first problem. */
int tj_prompt_atlas_validate(const TjPromptAtlas* atlas, char* err, size_t err_cap);

/* BC3 (DXT5) block codec. rgba = 16 texels row-major, R,G,B,A.
 * quality 0: PCA endpoints + least-squares refit; 1: adds an endpoint search. */
void tj_bc3_decode_block(const uint8_t src[16], uint8_t rgba[64]);
void tj_bc3_encode_block(const uint8_t rgba[64], uint8_t dst[16], int quality);

typedef struct TjPromptPatchInfo {
    uint32_t cells;              /* cells redrawn */
    uint32_t blocks;             /* 4x4 blocks re-encoded */
    double ms_art;               /* key-cap rendering */
    double ms_encode;            /* decode + merge + encode */
} TjPromptPatchInfo;

/* Rewrites the prompt cells of a BC3 payload in place.
 *   bc3/bytes/pitch: the copy to patch; pitch must be the tight row pitch
 *                    (w/4*16) and bytes == pitch * h/4, otherwise -1.
 *   intended:        optional w*h*4 RGBA; receives the merged pixels the
 *                    encoder was given, for every touched block.
 *   touched:         optional (w/4)*(h/4) bytes; set to 1 per touched block.
 * Returns 0 on success, -1 bad arguments, -2 art rendering failed, -3 no memory. */
int tj_prompt_atlas_patch(const TjPromptAtlas* atlas, uint8_t* bc3, size_t bytes,
                          uint32_t pitch, const PadPromptLabels* labels, int quality,
                          uint8_t* intended, uint8_t* touched, TjPromptPatchInfo* info);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_PROMPT_ATLAS_H */
