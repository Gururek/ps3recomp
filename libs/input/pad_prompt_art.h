/*
 * ps3recomp - PC key-cap art for PS3 button prompts
 *
 * Draws key caps that show the player's bound PC key where the game shows a
 * PS3 button glyph (Cross, L1, D-pad, ...). libs/video/rsx_prompt_atlas.c
 * uses it to rewrite the prompt cells of the game's DXT5 UI atlases.
 *
 * GDI only (Windows fonts, no downloads). Each call creates and frees its own
 * GDI objects, so it holds no global state and may run on any thread.
 *
 * The implementation compiles only when PS3RECOMP_PROMPT_ART is defined.
 * Every .c file in libs/input is globbed into ps3recomp_runtime, so without the define
 * this file adds nothing to a game build.
 */

#ifndef PS3RECOMP_PAD_PROMPT_ART_H
#define PS3RECOMP_PAD_PROMPT_ART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PS3 inputs, in the same order as the PAD_ACT_* enum in cellPad.c. */
typedef enum PadPromptButton {
    PAD_PROMPT_CROSS, PAD_PROMPT_CIRCLE, PAD_PROMPT_SQUARE, PAD_PROMPT_TRIANGLE,
    PAD_PROMPT_L1, PAD_PROMPT_R1, PAD_PROMPT_L2, PAD_PROMPT_R2,
    PAD_PROMPT_L3, PAD_PROMPT_R3, PAD_PROMPT_START, PAD_PROMPT_SELECT,
    PAD_PROMPT_UP, PAD_PROMPT_DOWN, PAD_PROMPT_LEFT, PAD_PROMPT_RIGHT,
    PAD_PROMPT_LS_UP, PAD_PROMPT_LS_DOWN, PAD_PROMPT_LS_LEFT, PAD_PROMPT_LS_RIGHT,
    PAD_PROMPT_RS_UP, PAD_PROMPT_RS_DOWN, PAD_PROMPT_RS_LEFT, PAD_PROMPT_RS_RIGHT,
    PAD_PROMPT_BUTTON_COUNT,

    /* Four-way groups, used with PAD_PROMPT_STYLE_DPAD. */
    PAD_PROMPT_GROUP_DPAD = 64,          /* UP, DOWN, LEFT, RIGHT */
    PAD_PROMPT_GROUP_LSTICK,             /* LS_UP .. LS_RIGHT */
    PAD_PROMPT_GROUP_RSTICK              /* RS_UP .. RS_RIGHT */
} PadPromptButton;

typedef enum PadPromptStyle {
    PAD_PROMPT_STYLE_KEY,                /* rounded square cap (face buttons) */
    PAD_PROMPT_STYLE_PLATE,              /* wide plate (L1/R1/L2/R2/SELECT/START) */
    PAD_PROMPT_STYLE_DPAD,               /* cross of four small caps with lit arms ("Num"
                                            hub + digits for numpad keys); a wide cell
                                            with nothing lit gets the inverted-T block */
    PAD_PROMPT_STYLE_KEY_TEXT            /* cap + caption ("Hold", "NEXT") */
} PadPromptStyle;

/* DPAD style: arms drawn highlighted (the PS3 art lights the direction to press). */
#define PAD_PROMPT_LIT_UP    0x1u
#define PAD_PROMPT_LIT_DOWN  0x2u
#define PAD_PROMPT_LIT_LEFT  0x4u
#define PAD_PROMPT_LIT_RIGHT 0x8u

#define PAD_PROMPT_LABEL_MAX 16

/* One label pair per PS3 input. UTF-8. The arrow keys use U+2190..U+2193,
 * which the renderer draws as vector arrows. Unbound inputs use U+2014. */
typedef struct PadPromptLabels {
    char full[PAD_PROMPT_BUTTON_COUNT][PAD_PROMPT_LABEL_MAX];     /* "Space", "Num8", "Bksp" */
    char compact[PAD_PROMPT_BUTTON_COUNT][PAD_PROMPT_LABEL_MAX];  /* small cells: "Spc", "N8" */
} PadPromptLabels;

/* Short names for a Windows virtual-key code, following the key names of
 * cellPad.c (s_pad_key_names). vk 0 means unbound. Mouse buttons are M1..M5. */
void pad_prompt_key_names(int vk, char* full, size_t full_cap,
                          char* compact, size_t compact_cap);

/* Labels from the primary key of each input (cellPad.c: the first nonzero
 * entry of s_pad_keys[action][]). */
void pad_prompt_labels_from_vks(PadPromptLabels* out,
                                const int vks[PAD_PROMPT_BUTTON_COUNT]);

typedef struct PadPromptArtSpec {
    PadPromptStyle style;
    int button;                          /* PadPromptButton, or a group for DPAD */
    unsigned lit;                        /* DPAD: PAD_PROMPT_LIT_* mask */
    const char* caption;                 /* KEY_TEXT: UTF-8 caption */
} PadPromptArtSpec;

/* Renders one prompt into rgba (w*h*4 bytes, R,G,B,A, straight alpha, rows
 * w*4 bytes apart). Fully transparent pixels carry the plate colour, so
 * bilinear filtering of the texture shows no light fringe. Returns 0 on
 * success, nonzero when GDI could not create its surfaces. */
int pad_prompt_art_render(const PadPromptArtSpec* spec, const PadPromptLabels* labels,
                          int w, int h, uint8_t* rgba);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_PAD_PROMPT_ART_H */
