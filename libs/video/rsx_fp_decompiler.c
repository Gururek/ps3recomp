/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * See rsx_fp_decompiler.h and docs/RSX_FRAGMENT_PROGRAM.md.
 *
 * The encoding (4 words / 16 bytes per instruction; CONST sources carry an
 * inline 16-byte constant in the following slot) is taken verbatim from
 * Mesa/nouveau nvfx_shader.h. Bit positions are spelled out as named macros
 * below so this file is self-documenting.
 */

#include "rsx_fp_decompiler.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Format one FP constant lane as an HLSL float literal. NaN/Inf constants
 * (a light-pass FP embeds them; %g prints the bare tokens "nan"/"inf" which
 * HLSL rejects -- X3004 undeclared identifier -> the whole PS fails to compile
 * and its pass renders black) are reproduced bit-exactly via asfloat(). */
static void fp_fmt_float(float v, char* buf, size_t n)
{
    if (isnan(v) || isinf(v)) {
        u32 bits; memcpy(&bits, &v, 4);
        snprintf(buf, n, "asfloat(0x%08Xu)", bits);
    } else {
        snprintf(buf, n, "%g", (double)v);
    }
}

/* ---- DWORD 0 (OPDEST) --------------------------------------------------- */
#define FP_END              (1u << 0)
#define FP_OUT_REG_SHIFT    1
#define FP_OUT_REG_MASK     (63u << 1)
#define FP_OUT_HALF         (1u << 7)
#define FP_COND_WRITE       (1u << 8)
#define FP_OUT_MASK_SHIFT   9          /* X@9 Y@10 Z@11 W@12 */
#define FP_INPUT_SRC_SHIFT  13
#define FP_INPUT_SRC_MASK   (15u << 13)
#define FP_TEX_UNIT_SHIFT   17
#define FP_TEX_UNIT_MASK    (15u << 17)
#define FP_OPCODE_SHIFT     24
#define FP_OPCODE_MASK      (0x3Fu << 24)
#define FP_OUT_NONE         (1u << 30)
#define FP_OUT_SAT          (1u << 31)

/* ---- DWORD 1/2/3 (SRC0/1/2) --------------------------------------------- */
#define FP_REG_TYPE_SHIFT   0
#define FP_REG_TYPE_MASK    (3u << 0)
#define   FP_REG_TYPE_TEMP  0
#define   FP_REG_TYPE_INPUT 1
#define   FP_REG_TYPE_CONST 2
#define FP_SRC_REG_SHIFT    2
#define FP_SRC_REG_MASK     (63u << 2)
#define FP_SRC_HALF         (1u << 8)
#define FP_SRC_SWZ_SHIFT    9          /* X@9 Y@11 Z@13 W@15, 2 bits each */
#define FP_SRC_NEGATE       (1u << 17)
#define FP_SRC0_ABS         (1u << 29) /* in DWORD1 */
#define FP_SRC1_ABS         (1u << 18) /* in DWORD2 */
#define FP_BRANCH           (1u << 31) /* DWORD2 bit31: instruction is a branch */

/* ---- Opcodes ------------------------------------------------------------ */
enum {
    OP_NOP=0x00, OP_MOV=0x01, OP_MUL=0x02, OP_ADD=0x03, OP_MAD=0x04,
    OP_DP3=0x05, OP_DP4=0x06, OP_DST=0x07, OP_MIN=0x08, OP_MAX=0x09,
    OP_SLT=0x0A, OP_SGE=0x0B, OP_SLE=0x0C, OP_SGT=0x0D, OP_SNE=0x0E,
    OP_SEQ=0x0F, OP_FRC=0x10, OP_FLR=0x11, OP_KIL=0x12, OP_PK4B=0x13,
    OP_UP4B=0x14, OP_DDX=0x15, OP_DDY=0x16, OP_TEX=0x17, OP_TXP=0x18,
    OP_TXD=0x19, OP_RCP=0x1A, OP_RSQ=0x1B, OP_EX2=0x1C, OP_LG2=0x1D,
    OP_LIT=0x1E, OP_LRP=0x1F, OP_STR=0x20, OP_SFL=0x21, OP_COS=0x22,
    OP_SIN=0x23, OP_PK2H=0x24, OP_UP2H=0x25, OP_POW=0x26, OP_PK4UB=0x27,
    OP_UP4UB=0x28, OP_PK2US=0x29, OP_UP2US=0x2A, OP_BEM=0x2B, OP_PKG=0x2C,
    OP_UPG=0x2D, OP_DP2A=0x2E, OP_TXL=0x2F, OP_TXB=0x31, OP_TEXBEM=0x33,
    OP_TXPBEM=0x34, OP_BEMLUM=0x35, OP_RFL=0x36, OP_TIMESWTEX=0x37,
    OP_DP2=0x38, OP_NRM=0x39, OP_DIV=0x3A,
    OP_DIVSQ=0x3B, OP_LIF=0x3C, OP_FENCT=0x3D, OP_FENCB=0x3E
};

const char* rsx_fp_opcode_name(u32 op)
{
    switch (op) {
    case OP_NOP: return "NOP"; case OP_MOV: return "MOV"; case OP_MUL: return "MUL";
    case OP_ADD: return "ADD"; case OP_MAD: return "MAD"; case OP_DP3: return "DP3";
    case OP_DP4: return "DP4"; case OP_DST: return "DST"; case OP_MIN: return "MIN";
    case OP_MAX: return "MAX"; case OP_SLT: return "SLT"; case OP_SGE: return "SGE";
    case OP_SLE: return "SLE"; case OP_SGT: return "SGT"; case OP_SNE: return "SNE";
    case OP_SEQ: return "SEQ"; case OP_FRC: return "FRC"; case OP_FLR: return "FLR";
    case OP_KIL: return "KIL"; case OP_PK4B: return "PK4B"; case OP_UP4B: return "UP4B";
    case OP_DDX: return "DDX"; case OP_DDY: return "DDY";
    case OP_TEX: return "TEX"; case OP_TXP: return "TXP"; case OP_TXD: return "TXD";
    case OP_RCP: return "RCP"; case OP_RSQ: return "RSQ"; case OP_EX2: return "EX2";
    case OP_LG2: return "LG2"; case OP_LIT: return "LIT"; case OP_LRP: return "LRP";
    case OP_STR: return "STR"; case OP_SFL: return "SFL"; case OP_COS: return "COS";
    case OP_SIN: return "SIN"; case OP_PK2H: return "PK2H"; case OP_UP2H: return "UP2H";
    case OP_POW: return "POW"; case OP_PK4UB: return "PK4UB"; case OP_UP4UB: return "UP4UB";
    case OP_PK2US: return "PK2US"; case OP_UP2US: return "UP2US"; case OP_BEM: return "BEM";
    case OP_PKG: return "PKG"; case OP_UPG: return "UPG"; case OP_DP2A: return "DP2A";
    case OP_TXL: return "TXL"; case OP_TXB: return "TXB"; case OP_TEXBEM: return "TEXBEM";
    case OP_TXPBEM: return "TXPBEM"; case OP_BEMLUM: return "BEMLUM";
    case OP_RFL: return "RFL"; case OP_TIMESWTEX: return "TIMESWTEX";
    case OP_DIV: return "DIV"; case OP_DP2: return "DP2"; case OP_NRM: return "NRM";
    case OP_DIVSQ: return "DIVSQ"; case OP_LIF: return "LIF";
    case OP_FENCT: return "FENCT"; case OP_FENCB: return "FENCB";
    default:     return "?";
    }
}

/* ------------------------------------------------------------------------- */

u32 rsx_fp_read_word(const u8* p)
{
    u32 be = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    return (be << 16) | (be >> 16);   /* 16-bit half-word swap */
}

static int fp_instruction_has_inline_const(u32 w1, u32 w2, u32 w3)
{
    if (w2 & FP_BRANCH) return 0;
    return ((w1 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
           ((w2 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST ||
           ((w3 & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT) == FP_REG_TYPE_CONST;
}

int rsx_fp_capture_structural(const u8* ucode, u32 max_bytes,
                              RSXFPStructuralProgram* out)
{
    u32 off = 0;
    u64 hash = 1469598103934665603ULL;
    /* Build in place: out is zeroed first, so unwritten slots match a
     * zero-initialized copy without a full-struct zero + copy per capture. */
    u32 raw_count = 0, inline_count = 0, texture_mask = 0;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!ucode) return -1;
    while (off + 16u <= max_bytes) {
        u32 words[4];
        if (off + 16u > RSX_FP_MAX_PROGRAM_BYTES ||
            raw_count > RSX_FP_MAX_RAW_WORDS - 4u)
            goto fail;
        for (u32 i = 0; i < 4u; ++i) {
            words[i] = rsx_fp_read_word(ucode + off + i * 4u);
            out->raw_words[raw_count++] = words[i];
            hash ^= words[i];
            hash *= 1099511628211ULL;
        }
        off += 16u;
        if (!(words[2] & FP_BRANCH)) {
            const u32 opcode = (words[0] & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
            switch (opcode) {
            case OP_TEX: case OP_TXP: case OP_TXD: case OP_TXL: case OP_TXB:
            case OP_TEXBEM: case OP_TXPBEM: case OP_BEMLUM: case OP_TIMESWTEX:
                texture_mask |=
                    1u << ((words[0] & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT);
                break;
            default:
                /* Unknown instructions never authorize dropping a binding. */
                if (rsx_fp_opcode_name(opcode)[0] == '?')
                    texture_mask = 0xFFFFu;
                break;
            }
        }
        if (fp_instruction_has_inline_const(words[1], words[2], words[3])) {
            if (off + 16u > max_bytes || off + 16u > RSX_FP_MAX_PROGRAM_BYTES ||
                inline_count >= RSX_FP_MAX_INLINE_CONSTANTS ||
                raw_count > RSX_FP_MAX_RAW_WORDS - 4u)
                goto fail;
            out->inline_word_index[inline_count++] = raw_count;
            for (u32 i = 0; i < 4u; ++i)
                out->raw_words[raw_count++] =
                    rsx_fp_read_word(ucode + off + i * 4u);
            off += 16u;
        }
        if (words[0] & FP_END) {
            out->structural_hash = hash;
            out->size_bytes = off;
            out->raw_word_count = raw_count;
            out->inline_count = inline_count;
            out->texture_read_mask = texture_mask;
            return 0;
        }
    }
fail:
    memset(out, 0, sizeof(*out));
    return -1; /* no END, truncation, or fixed capture overflow */
}

static int fp_structural_program_valid(const RSXFPStructuralProgram* program)
{
    if (!program || program->size_bytes < 16u ||
        (program->size_bytes & 15u) ||
        program->size_bytes > RSX_FP_MAX_PROGRAM_BYTES ||
        program->raw_word_count != program->size_bytes / 4u ||
        program->raw_word_count < 4u ||
        program->raw_word_count > RSX_FP_MAX_RAW_WORDS ||
        program->inline_count > RSX_FP_MAX_INLINE_CONSTANTS)
        return 0;
    u32 off = 0, slot = 0;
    while (off + 16u <= program->size_bytes) {
        u32 word = off / 4u;
        if (word > program->raw_word_count - 4u) return 0;
        u32 w0 = program->raw_words[word + 0u];
        u32 w1 = program->raw_words[word + 1u];
        u32 w2 = program->raw_words[word + 2u];
        u32 w3 = program->raw_words[word + 3u];
        off += 16u;
        if (fp_instruction_has_inline_const(w1, w2, w3)) {
            if (slot >= program->inline_count ||
                program->inline_word_index[slot] != off / 4u ||
                off + 16u > program->size_bytes)
                return 0;
            ++slot;
            off += 16u;
        }
        if (w0 & FP_END)
            return off == program->size_bytes && slot == program->inline_count;
    }
    return 0;
}

u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes)
{
    RSXFPStructuralProgram program;
    return rsx_fp_capture_structural(ucode, max_bytes, &program) == 0
        ? program.size_bytes : 0;
}

int rsx_fp_structural_equal(const RSXFPStructuralProgram* a,
                            const RSXFPStructuralProgram* b)
{
    if (!fp_structural_program_valid(a) || !b ||
        a->structural_hash != b->structural_hash ||
        a->size_bytes != b->size_bytes || a->raw_word_count != b->raw_word_count ||
        a->inline_count != b->inline_count) return 0;
    for (u32 slot = 0; slot < a->inline_count; ++slot)
        if (a->inline_word_index[slot] != b->inline_word_index[slot] ||
            a->inline_word_index[slot] > a->raw_word_count - 4u ||
            (slot && a->inline_word_index[slot] <= a->inline_word_index[slot - 1u] + 3u))
            return 0;
    for (u32 word = 0, slot = 0; word < a->raw_word_count; ++word) {
        if (slot < a->inline_count && word == a->inline_word_index[slot]) {
            word += 3u;
            ++slot;
            continue;
        }
        if (a->raw_words[word] != b->raw_words[word]) return 0;
    }
    return 1;
}

/* Bounded string appender. */
typedef struct { char* p; u32 cap; u32 len; int ok; } Out;

static void out_puts(Out* o, const char* s)
{
    if (!o->ok) return;
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->ok = 0; return; }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = '\0';
}

/* Process-wide WPOS form, fixed by the backend before any decompile. */
static int s_fp_wpos_host_scale;

void rsx_fp_set_wpos_host_scale(int enabled)
{
    s_fp_wpos_host_scale = enabled != 0;
}

/* Decoded view of one source word. */
typedef struct {
    u32 type;       /* FP_REG_TYPE_* */
    u32 index;
    int half;
    int negate;
    int abs;
    char swz[5];    /* e.g. "xyzw" */
} Src;

static void decode_src(u32 w, int abs_bit, Src* s)
{
    static const char comp[4] = {'x','y','z','w'};
    s->type   = (w & FP_REG_TYPE_MASK) >> FP_REG_TYPE_SHIFT;
    s->index  = (w & FP_SRC_REG_MASK)  >> FP_SRC_REG_SHIFT;
    s->half   = (w & FP_SRC_HALF) ? 1 : 0;
    s->negate = (w & FP_SRC_NEGATE) ? 1 : 0;
    s->abs    = abs_bit ? 1 : 0;
    for (int i = 0; i < 4; i++)
        s->swz[i] = comp[(w >> (FP_SRC_SWZ_SHIFT + 2 * i)) & 3];
    s->swz[4] = '\0';
}

/* HLSL expression for the interpolated input selected by OPDEST.INPUT_SRC.
 * The backend's passthrough VS forwards COL0/COL1/FOG/TEXCOORD0..9; WPOS maps
 * to SV_POSITION (screen-space — an approximation of the RSX window coord). */
static const char* input_expr(u32 input_src)
{
    switch (input_src) {
    case 0x0: return "_wpos"; /* SET_SHADER_WINDOW-adjusted WPOS */
    case 0x1: return "saturate(((_fpinput & 0x400u) != 0u && !input.front_face) ? input.bcol0 : input.col0)";
    case 0x2: return "saturate(((_fpinput & 0x400u) != 0u && !input.front_face) ? input.bcol1 : input.col1)";
    case 0x3: return "_fogc";          /* fixed fog equation output */
    case 0xE: return "float4(input.front_face ? 1.0 : -1.0, 0.0, 0.0, 1.0)"; /* SSA */
    default:
        return "float4(0,0,0,0)";      /* FACING/unrecognized not plumbed */
    }
}

/* Build the swizzled/negated/abs'd HLSL for one source into `buf`. */
static void emit_src(const Src* s, u32 input_src, const char* indexed_input,
                     int perspective_corr, const float* k, int has_k,
                     u32 inline_slot, int template_mode, char* buf, u32 bufsz)
{
    char base[192];
    if (s->type == FP_REG_TYPE_TEMP) {
        /* h and r are modelled as SEPARATE register files. On hardware the
         * h pairs overlay r bit-wise (h0 = r0.xy bits, h1 = r0.zw bits), so
         * cgc code like dbgfont's [r0.w = coverage; h0 = colour; r0.w *=
         * h0.w] works because the h0 write leaves r0.w's bits intact --
         * a lane-wise alias (h[N] = r[N>>1]) clobbers it (cellmark text
         * flooded white). Value-level cross-view reads are rare; keep the
         * files separate and select the export bank via SET_SHADER_CONTROL. */
        snprintf(base, sizeof(base), "%s[%u]", s->half ? "h" : "r", s->index);
    } else if (s->type == FP_REG_TYPE_INPUT) {
        if (!indexed_input && input_src >= 0x4 && input_src <= 0xD) {
            u32 tc = input_src - 0x4;
            /* SET_TEX_COORD_CONTROL is a runtime raster-state mask. A
             * selected 2D interpolator substitutes z=0 and WPOS.w for
             * ordinary input reads; perspective reads instead multiply xy by
             * WPOS.w and substitute w=1. Do this at the source, not once in
             * the _tc array, so normal and perspective reads can coexist. */
            if (perspective_corr)
                snprintf(base, sizeof(base),
                         "((_fpinput & (1u << %uu)) != 0u ? "
                         "float4(input.tc%u.xy * input.position.w, 0.0, 1.0) : "
                         "(input.tc%u * input.position.wwww))",
                         tc, tc, tc);
            else
                snprintf(base, sizeof(base),
                         "((_fpinput & (1u << %uu)) != 0u ? "
                         "float4(input.tc%u.xy, 0.0, 1.0 / input.position.w) : input.tc%u)",
                         tc, tc, tc);
        } else {
            const char* input = indexed_input ? indexed_input : input_expr(input_src);
            if (perspective_corr && indexed_input)
                snprintf(base, sizeof(base), "((%s) * input.position.wwww)", input);
            else
                snprintf(base, sizeof(base), "%s", input);
        }
    } else { /* CONST */
        if (template_mode && inline_slot < RSX_FP_MAX_INLINE_CONSTANTS) {
            snprintf(base, sizeof(base), "asfloat(rsx_inline[%u])", inline_slot);
        } else if (has_k) {
            char c0[24], c1[24], c2[24], c3[24];
            fp_fmt_float(k[0], c0, sizeof c0); fp_fmt_float(k[1], c1, sizeof c1);
            fp_fmt_float(k[2], c2, sizeof c2); fp_fmt_float(k[3], c3, sizeof c3);
            snprintf(base, sizeof(base), "float4(%s,%s,%s,%s)", c0, c1, c2, c3);
        } else
            snprintf(base, sizeof(base), "float4(0,0,0,0)");
    }

    char swz[160];
    snprintf(swz, sizeof(swz), "(%s).%s", base, s->swz);

    const char* pre = "";
    const char* post = "";
    if (s->abs)    { pre = "abs("; post = ")"; }
    if (s->negate) snprintf(buf, bufsz, "-(%s%s%s)", pre, swz, post);
    else           snprintf(buf, bufsz, "%s%s%s", pre, swz, post);
}

/* Dest write-mask letters from OPDEST bits 9..12 (X..W). */
static void dest_mask(u32 op0, char* m)
{
    int n = 0;
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 0))) m[n++] = 'x';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 1))) m[n++] = 'y';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 2))) m[n++] = 'z';
    if (op0 & (1u << (FP_OUT_MASK_SHIFT + 3))) m[n++] = 'w';
    if (n == 0) { m[0] = 'x'; m[1] = 'y'; m[2] = 'z'; m[3] = 'w'; n = 4; }
    m[n] = '\0';
}

static u32 fp_texture_dimension(u64 dimensions, u32 unit)
{
    u32 dimension = (u32)((dimensions >> (unit * 3u)) & 7u);
    if (dimension < RSX_FP_TEXTURE_DIM_1D ||
        dimension > RSX_FP_TEXTURE_DIM_CUBE)
        dimension = RSX_FP_TEXTURE_DIM_2D;
    return dimension;
}

static int fp_texture_shadow(u64 texture_state, u32 unit)
{
    return (texture_state & RSX_FP_TEXTURE_SHADOW_BIT(unit)) != 0;
}

static const char* fp_texture_type(u32 dimension)
{
    switch (dimension) {
    case RSX_FP_TEXTURE_DIM_1D:   return "Texture1D";
    case RSX_FP_TEXTURE_DIM_3D:   return "Texture3D";
    case RSX_FP_TEXTURE_DIM_CUBE: return "TextureCube";
    default:                      return "Texture2D";
    }
}

static const char* fp_texture_swizzle(u32 dimension)
{
    return dimension == RSX_FP_TEXTURE_DIM_1D ? "x" :
           dimension == RSX_FP_TEXTURE_DIM_2D ? "xy" : "xyz";
}

static void fp_texture_coordinate(char* out, size_t out_size, u32 dimension,
                                  u32 unit, const char* source, int project)
{
    const char* swizzle = fp_texture_swizzle(dimension);
    if (dimension == RSX_FP_TEXTURE_DIM_CUBE) {
        /* Cube coordinates are a direction vector, not texel coordinates.
         * Per-axis 1/width and 1/height scaling changes that direction and
         * therefore selects the wrong face/position. */
        if (project)
            snprintf(out, out_size,
                     "(%s).%s / (%s).w",
                     source, swizzle, source);
        else
            snprintf(out, out_size, "(%s).%s", source, swizzle);
    } else if (project) {
        snprintf(out, out_size,
                 "(%s).%s / (%s).w * rsx_texscale[%u].%s",
                 source, swizzle, source, unit, swizzle);
    } else {
        snprintf(out, out_size, "(%s).%s * rsx_texscale[%u].%s",
                 source, swizzle, unit, swizzle);
    }
}

/* RSX shadow lookup operands use the next source lane as the reference:
 * 1D/2D use z, while cube uses w.  D3D comparison sampling performs the
 * filtering after comparison, matching the RSX percentage-closer result.
 * Texture3D comparison is deliberately not selected by the D3D12 backend:
 * HLSL SampleCmp does not support Texture3D. */
static void fp_texture_shadow_operands(char* coordinate, size_t coordinate_size,
                                       char* reference, size_t reference_size,
                                       u32 dimension, u32 unit,
                                       const char* source, int project)
{
    if (dimension == RSX_FP_TEXTURE_DIM_1D) {
        if (project) {
            snprintf(coordinate, coordinate_size,
                     "(%s).x / (%s).w * rsx_texscale[%u].x",
                     source, source, unit);
            snprintf(reference, reference_size,
                     "min((%s).z / (%s).w, rsx_texscale[%u].w)",
                     source, source, unit);
        } else {
            snprintf(coordinate, coordinate_size,
                     "(%s).x * rsx_texscale[%u].x", source, unit);
            snprintf(reference, reference_size,
                     "min((%s).z, rsx_texscale[%u].w)", source, unit);
        }
    } else if (dimension == RSX_FP_TEXTURE_DIM_2D) {
        if (project) {
            snprintf(coordinate, coordinate_size,
                     "(%s).xy / (%s).w * rsx_texscale[%u].xy",
                     source, source, unit);
            snprintf(reference, reference_size,
                     "min((%s).z / (%s).w, rsx_texscale[%u].w)",
                     source, source, unit);
        } else {
            snprintf(coordinate, coordinate_size,
                     "(%s).xy * rsx_texscale[%u].xy", source, unit);
            snprintf(reference, reference_size,
                     "min((%s).z, rsx_texscale[%u].w)", source, unit);
        }
    } else {
        /* Cube shadow lookup: xyz direction plus w comparison reference.
         * Projected cube/3D shadow operations have no independent divisor. */
        fp_texture_coordinate(coordinate, coordinate_size, dimension, unit,
                              source, 0);
        snprintf(reference, reference_size,
                 "min((%s).w, rsx_texscale[%u].w)", source, unit);
    }
}

static void fp_texture_shadow_sample(char* out, size_t out_size,
                                     u32 dimension, u32 unit,
                                     const char* source, int project)
{
    char coordinate[768], reference[768];
    fp_texture_shadow_operands(coordinate, sizeof(coordinate),
                               reference, sizeof(reference),
                               dimension, unit, source, project);
    snprintf(out, out_size,
             "_builtin_shadow(rsx_tex%u.SampleCmp(rsx_samp%u, %s, %s))",
             unit, unit, coordinate, reference);
}

static int rsx_fp_decompile_internal(const u8* ucode, u32 max_bytes,
                                     const RSXFPStructuralProgram* program,
                                     char* out, u32 out_size, int exports32,
                                     u64 texture_dimensions, int nrt)
{
    if ((!ucode && !program) || !out || out_size == 0) return -1;
    if (program && !fp_structural_program_valid(program)) return -1;
    if (nrt < 1 || nrt > 4) return -1;

    Out o = { out, out_size, 0, 1 };

    /* Preamble: PSInput matches the backend's placeholder layout; temp/half
     * register files; texture+sampler banks for TEX. */
    out_puts(&o,
        "struct PSInput {\n"
        "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
        "    float4 bcol0 : COLOR2; float4 bcol1 : COLOR3;\n"
        "    float4 fog : FOG;\n"
        "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
        "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
        "    float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n"
        /* D3D requires SV_IsFrontFace after ordinary interpolants. */
        "    bool front_face : SV_IsFrontFace;\n"
        "};\n");
    for (u32 unit = 0; unit < 16u; ++unit) {
        char declaration[96];
        snprintf(declaration, sizeof(declaration),
                 "%s rsx_tex%u : register(t%u);\n",
                 fp_texture_type(fp_texture_dimension(texture_dimensions, unit)),
                 unit, unit);
        out_puts(&o, declaration);
    }
    for (u32 unit = 0; unit < 16u; ++unit) {
        char declaration[96];
        snprintf(declaration, sizeof(declaration),
                 "%s rsx_samp%u : register(s%u);\n",
                 fp_texture_shadow(texture_dimensions, unit)
                    ? "SamplerComparisonState" : "SamplerState",
                 unit, unit);
        out_puts(&o, declaration);
    }
    out_puts(&o,
        /* Per-draw texcoord scale (b1): RSX textures with the UNnormalized
         * flag are sampled in texel space; the backend supplies 1/size for
         * those units and 1.0 for normalized ones. */
        "cbuffer FPTex : register(b1) { float4 rsx_texscale[16]; "
        "uint4 rsx_texcontrol[4]; float4 rsx_alphatest; "
        "float4 rsx_rastercontrol; float4 rsx_windowcontrol; "
        "float4 rsx_fogcontrol; uint4 rsx_inline[128]; };\n"
        "struct PSOut { "
    );
    for (int target = 0; target < nrt; ++target) {
        char field[48];
        snprintf(field, sizeof(field), "float4 c%d:SV_Target%d; ", target, target);
        out_puts(&o, field);
    }
    out_puts(&o,
        "};\n"
        "float4 _builtin_shadow(float v) { return v.xxxx; }\n"
        /* RSX DIVSQ is lane-sensitive: a zero numerator stays zero even if
         * b is zero, whereas raw HLSL 0/sqrt(0) yields NaN. */
        "float4 _builtin_divsq(float4 a, float b) {\n"
        "    float4 tmp = a / sqrt(b);\n"
        "    return float4(abs(a.x) > 0 ? tmp.x : a.x, abs(a.y) > 0 ? tmp.y : a.y,\n"
        "                  abs(a.z) > 0 ? tmp.z : a.z, abs(a.w) > 0 ? tmp.w : a.w);\n"
        "}\n"
        /* RSX lighting helpers. LIF is the final hardware stage used by the
         * compiler's expanded LIT sequence; keep it distinct from full LIT. */
        "float4 _builtin_lit(float4 v) {\n"
        "    float2 t = max(v.xy, float2(0.0, 1.0e-10));\n"
        "    return float4(1.0, t.x, t.x > 0.0 ? pow(t.y, v.w) : 0.0, 1.0);\n"
        "}\n"
        "float4 _builtin_lif(float4 v) {\n"
        "    return float4(1.0, v.y, v.y > 0.0 ? exp2(v.w) : 0.0, 1.0);\n"
        "}\n"
        /* NV_fragment_program pack/unpack operations store the packed word in
         * the bit pattern of a float and broadcast it to all result lanes. */
        "uint _pack_snorm8(float v) {\n"
        "    return ((uint)((int)round(clamp(v, -1.0, 1.0) * 127.0))) & 0xffu;\n"
        "}\n"
        "uint _pack_unorm8(float v) { return (uint)round(saturate(v) * 255.0); }\n"
        "uint _pack_unorm16(float v) { return (uint)round(saturate(v) * 65535.0); }\n"
        "float _unpack_snorm8(uint v) {\n"
        "    int s = (int)(v << 24u) >> 24;\n"
        "    return max((float)s / 127.0, -1.0);\n"
        "}\n"
        "float4 _builtin_pk2h(float4 v) {\n"
        "    uint p = (f32tof16(v.x) & 0xffffu) | ((f32tof16(v.y) & 0xffffu) << 16u);\n"
        "    return (float4)asfloat(p);\n"
        "}\n"
        "float4 _builtin_up2h(float4 v) {\n"
        "    uint p = asuint(v.x);\n"
        "    float2 q = float2(f16tof32(p & 0xffffu), f16tof32(p >> 16u));\n"
        "    return q.xyxy;\n"
        "}\n"
        "float4 _builtin_pk4b(float4 v) {\n"
        "    uint p = _pack_snorm8(v.x) | (_pack_snorm8(v.y) << 8u) |\n"
        "             (_pack_snorm8(v.z) << 16u) | (_pack_snorm8(v.w) << 24u);\n"
        "    return (float4)asfloat(p);\n"
        "}\n"
        "float4 _builtin_up4b(float4 v) {\n"
        "    uint p = asuint(v.x);\n"
        "    return float4(_unpack_snorm8(p), _unpack_snorm8(p >> 8u),\n"
        "                  _unpack_snorm8(p >> 16u), _unpack_snorm8(p >> 24u));\n"
        "}\n"
        "float4 _builtin_pk4ub(float4 v) {\n"
        "    uint p = _pack_unorm8(v.x) | (_pack_unorm8(v.y) << 8u) |\n"
        "             (_pack_unorm8(v.z) << 16u) | (_pack_unorm8(v.w) << 24u);\n"
        "    return (float4)asfloat(p);\n"
        "}\n"
        "float4 _builtin_up4ub(float4 v) {\n"
        "    uint p = asuint(v.x);\n"
        "    return float4((p & 0xffu), ((p >> 8u) & 0xffu),\n"
        "                  ((p >> 16u) & 0xffu), ((p >> 24u) & 0xffu)) / 255.0;\n"
        "}\n"
        "float4 _builtin_pk2us(float4 v) {\n"
        "    uint p = _pack_unorm16(v.x) | (_pack_unorm16(v.y) << 16u);\n"
        "    return (float4)asfloat(p);\n"
        "}\n"
        "float4 _builtin_up2us(float4 v) {\n"
        "    uint p = asuint(v.x);\n"
        "    float2 q = float2((p & 0xffffu), (p >> 16u)) / 65535.0;\n"
        "    return q.xyxy;\n"
        "}\n"
        "float4 _linear_to_srgb(float4 v) {\n"
        "    float4 lo = v * 12.92;\n"
        "    float4 hi = 1.055 * pow(max(v, (float4)0), (float4)(1.0 / 2.4)) - 0.055;\n"
        "    return saturate(float4(v.x < 0.0031308 ? lo.x : hi.x,\n"
        "                           v.y < 0.0031308 ? lo.y : hi.y,\n"
        "                           v.z < 0.0031308 ? lo.z : hi.z,\n"
        "                           v.w < 0.0031308 ? lo.w : hi.w));\n"
        "}\n"
        "float4 _srgb_to_linear(float4 v) {\n"
        "    float4 lo = v / 12.92;\n"
        "    float4 hi = pow(max((v + 0.055) / 1.055, (float4)0), (float4)2.4);\n"
        "    return float4(v.x > 0.04045 ? hi.x : lo.x,\n"
        "                  v.y > 0.04045 ? hi.y : lo.y,\n"
        "                  v.z > 0.04045 ? hi.z : lo.z,\n"
        "                  v.w > 0.04045 ? hi.w : lo.w);\n"
        "}\n"
        /* control: alphaKill | gammaRGBA<<1 | signedRGBA<<5 |
         * biasedRGBA<<9 | channels16<<13. The SRV has already performed the
         * guest component crossbar, so every conversion mask names final
         * HLSL RGBA lanes. Conversion precedence is signed > gamma > BX2. */
        "float4 _builtin_texel(float4 v, uint unit) {\n"
        "    uint control = rsx_texcontrol[unit >> 2u][unit & 3u];\n"
        "    if ((control & 1u) && v.a < 0.000001) discard;\n"
        "    uint sm = (control >> 5u) & 15u;\n"
        "    if (sm) {\n"
        "        float scale = (control & (1u << 13u)) ? 65535.0 : 255.0;\n"
        "        float divisor = (control & (1u << 13u)) ? 32767.0 : 127.0;\n"
        "        uint shift = (control & (1u << 13u)) ? 16u : 24u;\n"
        "        uint4 raw = (uint4)floor(saturate(v) * scale + 0.5);\n"
        "        int4 si = (int4)(raw << shift) >> shift;\n"
        "        float4 cv = max((float4)si / divisor, (float4)-1.0);\n"
        "        if (sm & 1u) v.x = cv.x; if (sm & 2u) v.y = cv.y;\n"
        "        if (sm & 4u) v.z = cv.z; if (sm & 8u) v.w = cv.w;\n"
        "    }\n"
        "    uint gm = (control >> 1u) & 15u;\n"
        "    if (gm) {\n"
        "        float4 cv = _srgb_to_linear(v);\n"
        "        if (gm & 1u) v.x = cv.x; if (gm & 2u) v.y = cv.y;\n"
        "        if (gm & 4u) v.z = cv.z; if (gm & 8u) v.w = cv.w;\n"
        "    }\n"
        "    uint bm = (control >> 9u) & 15u;\n"
        "    if (bm) {\n"
        "        float scale = (control & (1u << 13u)) ? 65535.0 : 255.0;\n"
        "        float divisor = (control & (1u << 13u)) ? 32767.0 : 127.0;\n"
        "        float midpoint = (control & (1u << 13u)) ? 32768.0 : 128.0;\n"
        "        float4 cv = (floor(saturate(v) * scale + 0.5) - midpoint) / divisor;\n"
        "        if (bm & 1u) v.x = cv.x; if (bm & 2u) v.y = cv.y;\n"
        "        if (bm & 4u) v.z = cv.z; if (bm & 8u) v.w = cv.w;\n"
        "    }\n"
        "    return v;\n"
        "}\n"
        "float4 _builtin_pkg(float4 v) { return _builtin_pk4ub(_linear_to_srgb(v)); }\n"
        "float4 _builtin_upg(float4 v) { return _srgb_to_linear(_builtin_up4ub(v)); }\n"
        /* NV40 BEM/X2D coordinate transform and its fused texture forms. */
        "float4 _builtin_bem(float4 a, float4 b, float4 c) {\n"
        "    return a.xyxy + b.xxxx * c.xzxz + b.yyyy * c.ywyw;\n"
        "}\n"
        "float2 _builtin_bem_uv(float4 a, float4 b, float4 c, float2 scale) {\n"
        "    return _builtin_bem(a, b, c).xy * scale;\n"
        "}\n"
        "float2 _builtin_bem_proj_uv(float4 a, float4 b, float4 c, float2 scale) {\n"
        "    float4 q = _builtin_bem(a, b, c);\n"
        "    return (q.xy / q.w) * scale;\n"
        "}\n"
        "PSOut main(PSInput input) {\n"
        /* D3D12's legacy graphics PSO has no depth-bounds enable. Emulate the
         * independent RSX test from post-viewport SV_Position.z. */
        "    if (rsx_rastercontrol.x != 0.0 && "
        "(input.position.z < rsx_rastercontrol.y || input.position.z > rsx_rastercontrol.z)) discard;\n"
        "    float4 r[48]; float4 h[48];\n"
        "    float4 _tc[10];\n"
        "    _tc[0]=input.tc0; _tc[1]=input.tc1; _tc[2]=input.tc2; _tc[3]=input.tc3; _tc[4]=input.tc4;\n"
        "    _tc[5]=input.tc5; _tc[6]=input.tc6; _tc[7]=input.tc7; _tc[8]=input.tc8; _tc[9]=input.tc9;\n"
        /* SET_TEX_COORD_CONTROL is applied when a static input source is
         * emitted, because ordinary and perspective reads need different W.
         * _tc remains the unmodified indexed-input backing array. */
        "    uint _fpinput = (uint)rsx_rastercontrol.w;\n");
    /* TJ_RENDER_SCALE: host pixels return to guest WPOS through the per-draw
     * 1/raster-scale lane before the window bias/flip. Emitted only when the
     * backend enabled it, so 1x HLSL text and shader-cache keys are unchanged. */
    if (s_fp_wpos_host_scale)
        out_puts(&o,
            "    float4 _wpos = input.position;\n"
            "    _wpos.xy *= rsx_windowcontrol.w;\n"
            "    _wpos.x += rsx_windowcontrol.y;\n"
            "    _wpos.y = _wpos.y * rsx_windowcontrol.x + rsx_windowcontrol.z;\n");
    else
        out_puts(&o,
            "    float4 _wpos = input.position;\n"
            "    _wpos.x += rsx_windowcontrol.y;\n"
            "    _wpos.y = input.position.y * rsx_windowcontrol.x + rsx_windowcontrol.z;\n");
    out_puts(&o,
        "    float4 _fogc = float4(input.fog.x, 0.0, 0.0, 0.0);\n"
        "    float _fogx = input.fog.x; int _fogmode = (int)rsx_fogcontrol.z;\n"
        "    if (_fogmode == 1) _fogc.y = rsx_fogcontrol.y * _fogx + (rsx_fogcontrol.x - 1.0);\n"
        "    else if (_fogmode == 2) _fogc.y = exp(11.084 * (rsx_fogcontrol.y * _fogx + rsx_fogcontrol.x - 1.5));\n"
        "    else if (_fogmode == 3) _fogc.y = exp(-pow(4.709 * (rsx_fogcontrol.y * _fogx + rsx_fogcontrol.x - 1.5), 2.0));\n"
        "    else if (_fogmode == 4) _fogc.y = exp(11.084 * (rsx_fogcontrol.y * abs(_fogx) + rsx_fogcontrol.x - 1.5));\n"
        "    else if (_fogmode == 5) _fogc.y = exp(-pow(4.709 * (rsx_fogcontrol.y * abs(_fogx) + rsx_fogcontrol.x - 1.5), 2.0));\n"
        "    else if (_fogmode == 6) _fogc.y = rsx_fogcontrol.y * abs(_fogx) + (rsx_fogcontrol.x - 1.0);\n"
        "    if (_fogmode != 0) _fogc.y = saturate(_fogc.y);\n"
        /* Fully initialise both register files: RSX programs routinely read a
         * register lane before writing it (the hardware reads undefined), but
         * HLSL rejects use-before-init (error X4000). Zero everything. */
        "    [unroll] for (int _i = 0; _i < 48; _i++) { r[_i] = (float4)0; h[_i] = (float4)0; }\n"
        /* RET outside a subroutine terminates fragment-program execution but
         * still exports the registers written so far. Keep that state in a
         * flag so RET also escapes correctly from nested LOOP/REP blocks. */
        "    bool _fp_terminated = false;\n"
        /* NV40 FP condition registers: set_cond instructions latch their
         * result; later instructions execute per-lane where the (swizzled)
         * cc value passes the exec_if_lt/eq/gr test. */
        "    float4 cc0 = (float4)0, cc1 = (float4)0;\n");

    int wrote_r0 = 0, wrote_h0 = 0;
    int count = 0;
    u32 off = 0;
    /* Structured NV40 control flow. Targets are byte offsets in the captured
     * program. A unified stack preserves ordering when an inner end and an
     * outer else land at the same offset. */
    typedef struct {
        u32 end_off;
        u32 else_off;
        u8 kind;          /* 1 = IFE, 2 = LOOP, 3 = REP */
        u8 has_else;
        u8 in_else;
        u8 close_count;
        u32 loop_id;
    } FPFlowBlock;
    FPFlowBlock flow[32];
    int n_flow = 0;
    int loop_depth = 0;

    u32 inline_slot_next = 0;
    while (off + 16 <= max_bytes) {
        u32 word = off / 4u;
        if (program && word > program->raw_word_count - 4u) return -1;
        u32 w0 = program ? program->raw_words[word + 0] : rsx_fp_read_word(ucode + off + 0);
        u32 w1 = program ? program->raw_words[word + 1] : rsx_fp_read_word(ucode + off + 4);
        u32 w2 = program ? program->raw_words[word + 2] : rsx_fp_read_word(ucode + off + 8);
        u32 w3 = program ? program->raw_words[word + 3] : rsx_fp_read_word(ucode + off + 12);

        /* Resolve the innermost boundary first. This matters when a nested
         * ENDIF/ENDLOOP shares an offset with its parent's ELSE. */
        for (;;) {
            if (n_flow <= 0) break;
            FPFlowBlock* f = &flow[n_flow - 1];
            if (f->kind == 1 && f->has_else && !f->in_else &&
                f->else_off == off) {
                out_puts(&o, "    } else {\n");
                f->in_else = 1;
                break;
            }
            if (f->end_off != off) break;
            for (u32 i = 0; i < f->close_count; ++i)
                out_puts(&o, "    }");
            out_puts(&o, "\n");
            if (f->kind >= 2) loop_depth--;
            n_flow--;
        }

        off += 16;
        count++;

        u32 opcode    = (w0 & FP_OPCODE_MASK) >> FP_OPCODE_SHIFT;
        u32 input_src = (w0 & FP_INPUT_SRC_MASK) >> FP_INPUT_SRC_SHIFT;
        u32 tex_unit  = (w0 & FP_TEX_UNIT_MASK) >> FP_TEX_UNIT_SHIFT;
        int is_branch = (w2 & FP_BRANCH) ? 1 : 0;

        Src s0, s1, s2;
        decode_src(w1, w1 & FP_SRC0_ABS, &s0);
        decode_src(w2, w2 & FP_SRC1_ABS, &s1);
        decode_src(w3, 0,                &s2);

        /* Per-instruction predication (SRC0 word): exec_if_lt/eq/gr @18-20,
         * cond swizzle @21-28, cond_mod reg @30 (set_cond target), cond reg
         * @31 (gate source). exec=7 -> unconditional; exec=0 -> never runs. */
        u32 exec_cond = (w1 >> 18) & 7u;
        int cond_reg  = (int)((w1 >> 31) & 1u);
        int cond_mod  = (int)((w1 >> 30) & 1u);
        int set_cond  = (w0 & FP_COND_WRITE) ? 1 : 0;
        char cswz[5];
        {
            static const char comp2[4] = {'x','y','z','w'};
            for (int i = 0; i < 4; i++) cswz[i] = comp2[(w1 >> (21 + 2*i)) & 3];
            cswz[4] = 0;
        }
        /* Result scale modifier (SRC1 word bits 28-30): the value is scaled
         * before saturation/write. demosaic's parity math is floor(coord)/2
         * then frac() -- without the /2 the parity network is constant 0. */
        u32 scale_bits = (w2 >> 28) & 7u;
        const char* scale = NULL;
        switch (scale_bits) {
        case 1: scale = " _v = _v * 2.0;";   break;
        case 2: scale = " _v = _v * 4.0;";   break;
        case 3: scale = " _v = _v * 8.0;";   break;
        case 5: scale = " _v = _v * 0.5;";   break;
        case 6: scale = " _v = _v * 0.25;";  break;
        case 7: scale = " _v = _v * 0.125;"; break;
        default: break;
        }

        const char* cmp = NULL;   /* NULL = unconditional */
        switch (exec_cond) {
        case 1: cmp = "< 0";  break;
        case 2: cmp = "== 0"; break;
        case 3: cmp = "<= 0"; break;
        case 4: cmp = "> 0";  break;
        case 5: cmp = "!= 0"; break;
        case 6: cmp = ">= 0"; break;
        default: break;       /* 7 unconditional, 0 never */
        }

        /* One inline payload belongs to the instruction, even when multiple
         * source operands select CONST. Templates refer to that one b1 slot. */
        float k[4] = {0,0,0,0};
        int has_k = 0;
        u32 inline_slot = (u32)~0u;
        if (!is_branch &&
            (s0.type == FP_REG_TYPE_CONST || s1.type == FP_REG_TYPE_CONST ||
             s2.type == FP_REG_TYPE_CONST)) {
            if (program) {
                if (inline_slot_next >= program->inline_count ||
                    off + 16u > max_bytes ||
                    program->inline_word_index[inline_slot_next] != off / 4u)
                    return -1;
                inline_slot = inline_slot_next++;
                off += 16u;
                has_k = 1;
            } else if (off + 16 <= max_bytes) {
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_read_word(ucode + off + i * 4);
                    memcpy(&k[i], &cw, 4);
                }
                off += 16;
                has_k = 1;
            } else return -1; /* literal API must not invent a zero CONST */
        }

        /* SRC2 bits 30/31 apply indexed texcoord addressing and perspective
         * correction to an input source. LOOP owns A0.x; REP does not. */
        char indexed_input[96];
        const char* indexed = NULL;
        int perspective_corr = !is_branch && ((w3 >> 31) & 1u);
        if (!is_branch && ((w3 >> 30) & 1u)) {
            u32 addr = (w3 >> 19) & 0x7FFu;
            int active_loop = -1;
            for (int i = n_flow - 1; i >= 0; --i) {
                if (flow[i].kind == 2) {
                    active_loop = (int)flow[i].loop_id;
                    break;
                }
            }
            if (active_loop >= 0)
                snprintf(indexed_input, sizeof(indexed_input),
                         "_tc[_a0_%d + %u]", active_loop, addr);
            else if (addr < 10u)
                snprintf(indexed_input, sizeof(indexed_input), "_tc[%u]", addr);
            else
                snprintf(indexed_input, sizeof(indexed_input),
                         "float4(0.0, 0.0, 0.0, 1.0)");
            indexed = indexed_input;
        }

        char a[320], b[320], c[320];
        emit_src(&s0, input_src, indexed, perspective_corr, k, has_k,
                 inline_slot, program != NULL, a, sizeof(a));
        emit_src(&s1, input_src, indexed, perspective_corr, k, has_k,
                 inline_slot, program != NULL, b, sizeof(b));
        emit_src(&s2, input_src, indexed, perspective_corr, k, has_k,
                 inline_slot, program != NULL, c, sizeof(c));

        if (is_branch) {
            /* High opcode table: BRK/CAL/IFE/LOOP/REP/RET are 0x40..0x45.
             * The low six bits remain in OPDEST and opcode_hi is SRC1 bit31. */
            switch (opcode) {
            case 0x00: { /* BRK */
                if (loop_depth <= 0) {
                    out_puts(&o, "    /* Invalid FP BRK outside LOOP/REP. */\n");
                } else if (exec_cond != 0) {
                    char bl[256];
                    if (cmp)
                        snprintf(bl, sizeof(bl),
                                 "    { float4 _cs = cc%d.%s; if (!_fp_terminated && any(_cs %s)) break; }\n",
                                 cond_reg, cswz, cmp);
                    else
                        snprintf(bl, sizeof(bl),
                                 "    if (!_fp_terminated) break;\n");
                    out_puts(&o, bl);
                }
                break; }
            case 0x01: /* CAL -- absent from Cg-generated RSX programs. */
                out_puts(&o, "    /* Unsupported FP CAL (not emitted by the RSX compiler). */\n");
                break;
            case 0x02: { /* IFE */
                /* Encoded targets are in four-byte units. off already points
                 * to the first instruction in the controlled block. */
                u32 else_b = (w2 & 0x7FFFFFFFu) << 2;
                u32 end_b  = w3 << 2;
                if (end_b < off || else_b > end_b || n_flow >= 32) return -1;
                if (end_b == off) break; /* Empty IF. */

                int invert = (else_b == off && else_b != end_b);
                char bl[320];
                if (cmp) {
                    snprintf(bl, sizeof(bl),
                             invert
                                ? "    { float4 _cs = cc%d.%s; if (!_fp_terminated && !any(_cs %s)) {\n"
                                : "    { float4 _cs = cc%d.%s; if (!_fp_terminated && any(_cs %s)) {\n",
                             cond_reg, cswz, cmp);
                } else if (exec_cond == 0) {
                    snprintf(bl, sizeof(bl), invert
                             ? "    { if (!_fp_terminated) {\n"
                             : "    { if (false) {\n");
                } else {
                    snprintf(bl, sizeof(bl), invert
                             ? "    { if (false) {\n"
                             : "    { if (!_fp_terminated) {\n");
                }
                out_puts(&o, bl);
                flow[n_flow].end_off = end_b;
                flow[n_flow].else_off = else_b;
                flow[n_flow].kind = 1;
                flow[n_flow].has_else = (!invert && else_b > off && else_b != end_b);
                flow[n_flow].in_else = 0;
                flow[n_flow].close_count = 2;
                flow[n_flow].loop_id = 0;
                n_flow++;
                break; }
            case 0x03: /* LOOP */
            case 0x04: { /* REP */
                u32 end_b = w3 << 2;
                u32 reps = (w2 >> 2) & 0xFFu;
                u32 init = (w2 >> 10) & 0xFFu;
                int incr = (int)((w2 >> 19) & 0xFFu);
                if (incr & 0x80) incr -= 0x100;
                if (end_b < off || n_flow >= 32) return -1;
                if (end_b == off) break; /* Empty LOOP/REP. */

                char bl[512];
                if (cmp) {
                    snprintf(bl, sizeof(bl),
                             "    { float4 _cs = cc%d.%s; if (!_fp_terminated && any(_cs %s)) { for (int _lc%d = 0, _a0_%d = %u; _lc%d < %u && !_fp_terminated; ++_lc%d, _a0_%d += %d) {\n",
                             cond_reg, cswz, cmp, count, count, init,
                             count, reps, count, count, incr);
                } else if (exec_cond == 0) {
                    snprintf(bl, sizeof(bl),
                             "    { if (false) { for (int _lc%d = 0, _a0_%d = %u; _lc%d < %u; ++_lc%d, _a0_%d += %d) {\n",
                             count, count, init, count, reps, count, count, incr);
                } else {
                    snprintf(bl, sizeof(bl),
                             "    { if (!_fp_terminated) { for (int _lc%d = 0, _a0_%d = %u; _lc%d < %u && !_fp_terminated; ++_lc%d, _a0_%d += %d) {\n",
                             count, count, init, count, reps, count, count, incr);
                }
                out_puts(&o, bl);
                flow[n_flow].end_off = end_b;
                flow[n_flow].else_off = 0;
                flow[n_flow].kind = opcode == 0x03 ? 2 : 3;
                flow[n_flow].has_else = 0;
                flow[n_flow].in_else = 0;
                flow[n_flow].close_count = 3;
                flow[n_flow].loop_id = (u32)count;
                n_flow++;
                loop_depth++;
                break; }
            case 0x05: { /* RET: with an empty call stack, stop main. */
                if (exec_cond != 0) {
                    char bl[256];
                    if (cmp)
                        snprintf(bl, sizeof(bl),
                                 "    { float4 _cs = cc%d.%s; if (!_fp_terminated && any(_cs %s)) _fp_terminated = true; }\n",
                                 cond_reg, cswz, cmp);
                    else
                        snprintf(bl, sizeof(bl),
                                 "    _fp_terminated = true;\n");
                    out_puts(&o, bl);
                }
                break; }
            default:
                out_puts(&o, "    /* Invalid FP high-table opcode. */\n");
                break;
            }
            if (w0 & FP_END) break;
            continue;
        }

        /* Build the RHS expression for this opcode. */
        char rhs[2048];
        int handled = 1;
        switch (opcode) {
        case OP_NOP: rhs[0] = '\0'; handled = 0; break;
        case OP_MOV: snprintf(rhs, sizeof(rhs), "%s", a); break;
        case OP_MUL: snprintf(rhs, sizeof(rhs), "(%s) * (%s)", a, b); break;
        case OP_ADD: snprintf(rhs, sizeof(rhs), "(%s) + (%s)", a, b); break;
        case OP_MAD: snprintf(rhs, sizeof(rhs), "(%s) * (%s) + (%s)", a, b, c); break;
        case OP_DP3: snprintf(rhs, sizeof(rhs), "dot((%s).xyz, (%s).xyz)", a, b); break;
        case OP_DP4: snprintf(rhs, sizeof(rhs), "dot((%s), (%s))", a, b); break;
        case OP_DST:
            snprintf(rhs, sizeof(rhs), "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)",
                     a, b, a, b);
            break;
        case OP_MIN: snprintf(rhs, sizeof(rhs), "min((%s), (%s))", a, b); break;
        case OP_MAX: snprintf(rhs, sizeof(rhs), "max((%s), (%s))", a, b); break;
        case OP_FRC: snprintf(rhs, sizeof(rhs), "frac(%s)", a); break;
        case OP_FLR: snprintf(rhs, sizeof(rhs), "floor(%s)", a); break;
        case OP_PK4B: snprintf(rhs, sizeof(rhs), "_builtin_pk4b(%s)", a); break;
        case OP_UP4B: snprintf(rhs, sizeof(rhs), "_builtin_up4b(%s)", a); break;
        case OP_DDX: snprintf(rhs, sizeof(rhs), "ddx(%s)", a); break;
        case OP_DDY: snprintf(rhs, sizeof(rhs), "ddy(%s)", a); break;
        case OP_RCP: snprintf(rhs, sizeof(rhs), "(1.0 / (%s).x)", a); break;
        case OP_RSQ: snprintf(rhs, sizeof(rhs), "rsqrt((%s).x)", a); break;
        case OP_EX2: snprintf(rhs, sizeof(rhs), "exp2((%s).x)", a); break;
        case OP_LG2: snprintf(rhs, sizeof(rhs), "log2((%s).x)", a); break;
        case OP_LIT: snprintf(rhs, sizeof(rhs), "_builtin_lit(%s)", a); break;
        case OP_COS: snprintf(rhs, sizeof(rhs), "cos((%s).x)", a); break;
        case OP_SIN: snprintf(rhs, sizeof(rhs), "sin((%s).x)", a); break;
        case OP_PK2H: snprintf(rhs, sizeof(rhs), "_builtin_pk2h(%s)", a); break;
        case OP_UP2H: snprintf(rhs, sizeof(rhs), "_builtin_up2h(%s)", a); break;
        case OP_POW: snprintf(rhs, sizeof(rhs), "pow((%s).x, (%s).x)", a, b); break;
        case OP_PK4UB: snprintf(rhs, sizeof(rhs), "_builtin_pk4ub(%s)", a); break;
        case OP_UP4UB: snprintf(rhs, sizeof(rhs), "_builtin_up4ub(%s)", a); break;
        case OP_PK2US: snprintf(rhs, sizeof(rhs), "_builtin_pk2us(%s)", a); break;
        case OP_UP2US: snprintf(rhs, sizeof(rhs), "_builtin_up2us(%s)", a); break;
        case OP_BEM: snprintf(rhs, sizeof(rhs), "_builtin_bem((%s), (%s), (%s))", a, b, c); break;
        case OP_PKG: snprintf(rhs, sizeof(rhs), "_builtin_pkg(%s)", a); break;
        case OP_UPG: snprintf(rhs, sizeof(rhs), "_builtin_upg(%s)", a); break;
        case OP_DP2A:
            snprintf(rhs, sizeof(rhs), "dot((%s).xy, (%s).xy) + (%s).x", a, b, c);
            break;
        case OP_DIV: snprintf(rhs, sizeof(rhs), "(%s) / (%s).x", a, b); break;
        case OP_DIVSQ: snprintf(rhs, sizeof(rhs), "_builtin_divsq((%s), (%s).x)", a, b); break;
        case OP_DP2: snprintf(rhs, sizeof(rhs), "dot((%s).xy, (%s).xy)", a, b); break;
        case OP_NRM: snprintf(rhs, sizeof(rhs), "(normalize((%s).xyz)).xyzz", a); break;
        case OP_RFL: snprintf(rhs, sizeof(rhs), "reflect((%s), (%s))", a, b); break;
        case OP_LIF: snprintf(rhs, sizeof(rhs), "_builtin_lif(%s)", a); break;
        case OP_LRP: snprintf(rhs, sizeof(rhs), "lerp((%s), (%s), (%s))", c, b, a); break;
        case OP_STR: snprintf(rhs, sizeof(rhs), "float4(1.0, 1.0, 1.0, 1.0)"); break;
        case OP_SFL: snprintf(rhs, sizeof(rhs), "float4(0.0, 0.0, 0.0, 0.0)"); break;
        /* Texture/branch fences: ordering hints with no result -- no-op. */
        case OP_FENCT: case OP_FENCB: rhs[0] = '\0'; handled = 0; break;
        case OP_SLT: snprintf(rhs, sizeof(rhs), "(float4)((%s) <  (%s))", a, b); break;
        case OP_SGE: snprintf(rhs, sizeof(rhs), "(float4)((%s) >= (%s))", a, b); break;
        case OP_SLE: snprintf(rhs, sizeof(rhs), "(float4)((%s) <= (%s))", a, b); break;
        case OP_SGT: snprintf(rhs, sizeof(rhs), "(float4)((%s) >  (%s))", a, b); break;
        case OP_SNE: snprintf(rhs, sizeof(rhs), "(float4)((%s) != (%s))", a, b); break;
        case OP_SEQ: snprintf(rhs, sizeof(rhs), "(float4)((%s) == (%s))", a, b); break;
        case OP_TEX: {
            char coordinate[768];
            u32 dimension = fp_texture_dimension(texture_dimensions, tex_unit);
            if (fp_texture_shadow(texture_dimensions, tex_unit)) {
                fp_texture_shadow_sample(rhs, sizeof(rhs), dimension,
                                         tex_unit, a, 0);
            } else {
                fp_texture_coordinate(coordinate, sizeof(coordinate), dimension,
                                      tex_unit, a, 0);
                snprintf(rhs, sizeof(rhs),
                         "_builtin_texel(rsx_tex%u.Sample(rsx_samp%u, %s), %uu)",
                         tex_unit, tex_unit, coordinate, tex_unit);
            }
            break; }
        case OP_TXP: {
            char coordinate[768];
            u32 dimension = fp_texture_dimension(texture_dimensions, tex_unit);
            if (fp_texture_shadow(texture_dimensions, tex_unit)) {
                fp_texture_shadow_sample(rhs, sizeof(rhs), dimension,
                                         tex_unit, a, 1);
            } else {
                fp_texture_coordinate(coordinate, sizeof(coordinate), dimension,
                                      tex_unit, a, 1);
                snprintf(rhs, sizeof(rhs),
                         "_builtin_texel(rsx_tex%u.Sample(rsx_samp%u, %s), %uu)",
                         tex_unit, tex_unit, coordinate, tex_unit);
            }
            break; }
        case OP_TXD: {
            char coordinate[768], gradient_x[768], gradient_y[768];
            u32 dimension = fp_texture_dimension(texture_dimensions, tex_unit);
            if (fp_texture_shadow(texture_dimensions, tex_unit)) {
                /* RSX shadow variants collapse explicit gradient/LOD/bias
                 * texture opcodes to the comparison sampler path. */
                fp_texture_shadow_sample(rhs, sizeof(rhs), dimension,
                                         tex_unit, a, 0);
            } else {
                fp_texture_coordinate(coordinate, sizeof(coordinate), dimension,
                                      tex_unit, a, 0);
                fp_texture_coordinate(gradient_x, sizeof(gradient_x), dimension,
                                      tex_unit, b, 0);
                fp_texture_coordinate(gradient_y, sizeof(gradient_y), dimension,
                                      tex_unit, c, 0);
                snprintf(rhs, sizeof(rhs),
                         "_builtin_texel(rsx_tex%u.SampleGrad(rsx_samp%u, %s, %s, %s), %uu)",
                         tex_unit, tex_unit, coordinate, gradient_x, gradient_y,
                         tex_unit);
            }
            break; }
        case OP_TXL: {
            char coordinate[768];
            u32 dimension = fp_texture_dimension(texture_dimensions, tex_unit);
            if (fp_texture_shadow(texture_dimensions, tex_unit)) {
                fp_texture_shadow_sample(rhs, sizeof(rhs), dimension,
                                         tex_unit, a, 0);
            } else {
                fp_texture_coordinate(coordinate, sizeof(coordinate), dimension,
                                      tex_unit, a, 0);
                snprintf(rhs, sizeof(rhs),
                         "_builtin_texel(rsx_tex%u.SampleLevel(rsx_samp%u, %s, (%s).w), %uu)",
                         tex_unit, tex_unit, coordinate, a, tex_unit);
            }
            break; }
        case OP_TXB: {
            char coordinate[768];
            u32 dimension = fp_texture_dimension(texture_dimensions, tex_unit);
            if (fp_texture_shadow(texture_dimensions, tex_unit)) {
                fp_texture_shadow_sample(rhs, sizeof(rhs), dimension,
                                         tex_unit, a, 0);
            } else {
                fp_texture_coordinate(coordinate, sizeof(coordinate), dimension,
                                      tex_unit, a, 0);
                snprintf(rhs, sizeof(rhs),
                         "_builtin_texel(rsx_tex%u.SampleBias(rsx_samp%u, %s, (%s).w), %uu)",
                         tex_unit, tex_unit, coordinate, a, tex_unit);
            }
            break; }
        case OP_TEXBEM: {
            char bem[768], coordinate[1536];
            snprintf(bem, sizeof(bem), "_builtin_bem((%s), (%s), (%s))", a, b, c);
            u32 dimension = fp_texture_dimension(texture_dimensions, tex_unit);
            if (fp_texture_shadow(texture_dimensions, tex_unit)) {
                fp_texture_shadow_sample(rhs, sizeof(rhs), dimension,
                                         tex_unit, bem, 0);
            } else {
                fp_texture_coordinate(coordinate, sizeof(coordinate), dimension,
                                      tex_unit, bem, 0);
                snprintf(rhs, sizeof(rhs),
                         "_builtin_texel(rsx_tex%u.Sample(rsx_samp%u, %s), %uu)",
                         tex_unit, tex_unit, coordinate, tex_unit);
            }
            break; }
        case OP_TXPBEM: {
            char bem[768], coordinate[1536];
            snprintf(bem, sizeof(bem), "_builtin_bem((%s), (%s), (%s))", a, b, c);
            u32 dimension = fp_texture_dimension(texture_dimensions, tex_unit);
            if (fp_texture_shadow(texture_dimensions, tex_unit)) {
                fp_texture_shadow_sample(rhs, sizeof(rhs), dimension,
                                         tex_unit, bem, 1);
            } else {
                fp_texture_coordinate(coordinate, sizeof(coordinate), dimension,
                                      tex_unit, bem, 1);
                snprintf(rhs, sizeof(rhs),
                         "_builtin_texel(rsx_tex%u.Sample(rsx_samp%u, %s), %uu)",
                         tex_unit, tex_unit, coordinate, tex_unit);
            }
            break; }
        case OP_KIL: {
            if (exec_cond != 0) {
                char kl[280];
                if (cmp)
                    snprintf(kl, sizeof(kl),
                             "    { float4 _cs = cc%d.%s; if (!_fp_terminated && any(_cs %s)) discard; }\n",
                             cond_reg, cswz, cmp);
                else
                    snprintf(kl, sizeof(kl),
                             "    if (!_fp_terminated) discard;\n");
                out_puts(&o, kl);
            }
            handled = 0;
            break; }
        default:
            out_puts(&o, "    /* TODO: unhandled FP opcode ");
            out_puts(&o, rsx_fp_opcode_name(opcode));
            out_puts(&o, " */\n");
            handled = 0;
            break;
        }

        if (handled && exec_cond != 0 && (!(w0 & FP_OUT_NONE) || set_cond)) {
            u32 dst_idx = (w0 & FP_OUT_REG_MASK) >> FP_OUT_REG_SHIFT;
            int dst_half = (w0 & FP_OUT_HALF) ? 1 : 0;
            const char* rf = dst_half ? "h" : "r";
            char m[5];
            dest_mask(w0, m);

            /* Broadcast the result to float4 first so the write-mask picks
             * the CORRESPONDING components: `dst.w = rhs` would HLSL-truncate
             * a vector rhs to its .x, but NV40 writes rhs.w to dst.w. Scalar
             * results (DPx/RCP/...) replicate, which is also the hardware
             * behavior. */
            char line[2048];
            int pos = 0;
            const char* sat = (w0 & FP_OUT_SAT) ? " _v = saturate(_v);" : "";
            pos += snprintf(line + pos, sizeof(line) - pos,
                            "    if (!_fp_terminated) { float4 _v = (float4)(%s);%s%s",
                            rhs, scale ? scale : "", sat);
            if (cmp)
                pos += snprintf(line + pos, sizeof(line) - pos,
                                " float4 _cs = cc%d.%s;", cond_reg, cswz);
            if (!(w0 & FP_OUT_NONE)) {
                if (cmp) {
                    /* Per-lane predicated write: lane L updates only where
                     * the swizzled cc lane passes the test. */
                    for (const char* L = m; *L; L++)
                        pos += snprintf(line + pos, sizeof(line) - pos,
                                        " %s[%u].%c = (_cs.%c %s) ? _v.%c : %s[%u].%c;",
                                        rf, dst_idx, *L, *L, cmp, *L, rf, dst_idx, *L);
                } else {
                    pos += snprintf(line + pos, sizeof(line) - pos,
                                    " %s[%u].%s = _v.%s;", rf, dst_idx, m, m);
                }
            }
            if (set_cond) {
                if (cmp) {
                    for (const char* L = m; *L; L++)
                        pos += snprintf(line + pos, sizeof(line) - pos,
                                        " cc%d.%c = (_cs.%c %s) ? _v.%c : cc%d.%c;",
                                        cond_mod, *L, *L, cmp, *L, cond_mod, *L);
                } else {
                    pos += snprintf(line + pos, sizeof(line) - pos,
                                    " cc%d.%s = _v.%s;", cond_mod, m, m);
                }
            }
            snprintf(line + pos, sizeof(line) - pos, " }\n");
            out_puts(&o, line);

            if (!(w0 & FP_OUT_NONE) && dst_idx == 0) {
                if (dst_half) wrote_h0 = 1; else wrote_r0 = 1;
            }
        }

        if (w0 & FP_END) break;
    }

    /* Close blocks when END terminates the program before their encoded merge
     * target. This is syntax repair only; END already stops code emission. */
    while (n_flow > 0) {
        FPFlowBlock* f = &flow[--n_flow];
        for (u32 i = 0; i < f->close_count; ++i)
            out_puts(&o, "    }");
        out_puts(&o, "\n");
    }

    /* Fragment colour outputs, selected by SET_SHADER_CONTROL: 32-bit
     * programs export r0/r2/r3/r4, half programs h0/h4/h6/h8 (wave's water
     * height FP writes its result to h0 and uses r0 lanes as scratch --
     * heuristics picked r0 and the pond stayed flat forever). Unbound MRT
     * writes are discarded. */
    /* Colour exports: SET_SHADER_CONTROL bit 0x40 selects the 32-bit bank
     * (r0/r2/r3/r4) vs half (h0/h4/h6/h8); fall back to whichever file the
     * program actually wrote for c0 (PSL1GHT runs half-export control over
     * r0-writing programs). Secondaries use the zero-init sum trick. */
    if (exports32 ? wrote_r0 : !wrote_h0)
        out_puts(&o, "    PSOut _po; _po.c0 = r[0];\n");
    else
        out_puts(&o, "    PSOut _po; _po.c0 = h[0];\n");
    for (int target = 1; target < nrt; ++target) {
        static const char* const secondary_exports[] = {
            "r[2] + h[4]", "r[3] + h[6]", "r[4] + h[8]"
        };
        char assignment[64];
        snprintf(assignment, sizeof(assignment), "    _po.c%d = %s;\n",
                 target, secondary_exports[target - 1]);
        out_puts(&o, assignment);
    }
    out_puts(&o,
        /* Guest alpha test (D3D12 has none fixed-function): func codes are
         * RSX 0x200+f with f: 0=NEVER 1=LESS 2=EQUAL 3=LEQUAL 4=GREATER
         * 5=NOTEQUAL 6=GEQUAL 7=ALWAYS. */
        "    if (rsx_alphatest.x != 0.0) {\n"
        "        float _a = _po.c0.a, _r = rsx_alphatest.y;\n"
        "        int _f = (int)rsx_alphatest.z;\n"
        "        bool _p = (_f == 7) || (_f == 1 && _a < _r) || (_f == 2 && _a == _r) ||\n"
        "                  (_f == 3 && _a <= _r) || (_f == 4 && _a > _r) ||\n"
        "                  (_f == 5 && _a != _r) || (_f == 6 && _a >= _r);\n"
        "        if (!_p) discard;\n"
        "    }\n"
        /* SET_SHADER_PACKER converts RGB only; alpha remains linear. The
         * backend enables this solely for half exports to integer surfaces. */
        "    if (rsx_alphatest.w != 0.0) {\n");
    for (int target = 0; target < nrt; ++target) {
        char conversion[80];
        snprintf(conversion, sizeof(conversion),
                 "        _po.c%d.rgb = _linear_to_srgb(_po.c%d).rgb;\n",
                 target, target);
        out_puts(&o, conversion);
    }
    out_puts(&o, "    }\n    return _po;\n}\n");

    if (program && inline_slot_next != program->inline_count) return -1;
    if (!o.ok) return -1;
    return count;
}

int rsx_fp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size,
                     int exports32)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, NULL, out, out_size,
                                     exports32, 0, 4);
}

int rsx_fp_decompile_tex(const u8* ucode, u32 max_bytes, char* out,
                         u32 out_size, int exports32,
                         u64 texture_dimensions)
{
    return rsx_fp_decompile_internal(ucode, max_bytes, NULL, out, out_size,
                                     exports32, texture_dimensions, 4);
}

int rsx_fp_decompile_template(const RSXFPStructuralProgram* program,
                              char* out, u32 out_size, int exports32,
                              int nrt)
{
    return rsx_fp_decompile_internal(NULL, program ? program->size_bytes : 0,
                                     program, out, out_size, exports32, 0, nrt);
}

int rsx_fp_decompile_template_tex(const RSXFPStructuralProgram* program,
                                  char* out, u32 out_size, int exports32,
                                  u64 texture_dimensions, int nrt)
{
    return rsx_fp_decompile_internal(NULL, program ? program->size_bytes : 0,
                                     program, out, out_size, exports32,
                                     texture_dimensions, nrt);
}
