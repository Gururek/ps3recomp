/*
 * ps3recomp - RSX Fragment Program (NV40 ISA) → HLSL decompiler
 *
 * Translates an RSX fragment shader (NVIDIA NV40 "nvfx" fragment program
 * encoding) into an HLSL pixel shader source string suitable for D3DCompile.
 *
 * See docs/RSX_FRAGMENT_PROGRAM.md for the instruction encoding reference
 * (sourced from Mesa/nouveau nvfx_shader.h). This module is self-contained:
 * it has no D3D12 dependency and can be built/tested standalone.
 */

#ifndef PS3RECOMP_RSX_FP_DECOMPILER_H
#define PS3RECOMP_RSX_FP_DECOMPILER_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A 4 KiB RSX FP window holds at most 256 instruction words, or 128
 * instruction+inline-constant pairs.  Capturing host-order words at record
 * time makes a later replay independent of guest patching at the same EA. */
#define RSX_FP_MAX_PROGRAM_BYTES       4096u
#define RSX_FP_MAX_RAW_WORDS          (RSX_FP_MAX_PROGRAM_BYTES / 4u)
#define RSX_FP_MAX_INLINE_CONSTANTS   (RSX_FP_MAX_PROGRAM_BYTES / 32u)

/* Three bits per fragment texture unit describe the HLSL resource type.
 * Zero and values above CUBE are treated as 2D for backward compatibility. */
#define RSX_FP_TEXTURE_DIM_1D          1u
#define RSX_FP_TEXTURE_DIM_2D          2u
#define RSX_FP_TEXTURE_DIM_3D          3u
#define RSX_FP_TEXTURE_DIM_CUBE        4u
/* Bits 48..63 of the same u64 mark fragment units that use the RSX depth
 * comparison path.  The lower 48 bits remain the 16 packed dimensions. */
#define RSX_FP_TEXTURE_SHADOW_SHIFT    48u
#define RSX_FP_TEXTURE_SHADOW_BIT(unit) \
    (1ull << (RSX_FP_TEXTURE_SHADOW_SHIFT + (unit)))

typedef struct RSXFPStructuralProgram {
    u64 structural_hash;       /* FNV-1a over instruction words only */
    u32 size_bytes;            /* exact stream length, including payload */
    u32 raw_word_count;        /* exact host-order words copied from stream */
    u32 inline_count;          /* one slot per CONST-bearing instruction */
    u32 texture_read_mask;     /* union across all instructions, including branches */
    u32 inline_word_index[RSX_FP_MAX_INLINE_CONSTANTS];
    u32 raw_words[RSX_FP_MAX_RAW_WORDS];
} RSXFPStructuralProgram;

/* Read one fragment-program word from guest (PS3) memory: big-endian load
 * followed by a 16-bit half-word swap, yielding a host-order instruction
 * word. The decoder operates on host-order words. */
u32 rsx_fp_read_word(const u8* p);

/* Decompile an NV40 fragment program into an HLSL pixel shader.
 *
 *   ucode     : pointer to the fragment-program bytecode in guest memory
 *               (big-endian, half-word-swapped — i.e. exactly as the RSX
 *               reads it; this function applies rsx_fp_read_word internally).
 *   max_bytes : safety bound on how far to read (program stops at PROGRAM_END
 *               or when this many bytes are consumed).
 *   out       : caller buffer for the generated HLSL (NUL-terminated).
 *   out_size  : size of `out` in bytes.
 *
 * Returns the number of instructions decoded (>=0) on success, or -1 on
 * error (null args, output overflow). The emitted entry point is
 * `float4 main(PSInput input) : SV_TARGET`, matching the backend's
 * placeholder PSInput { float4 position : SV_POSITION; float4 col : COLOR; }.
 *
 * Texture sampling and full input plumbing are partial — see the doc's
 * "Integration TODO". Unhandled opcodes emit a comment and a safe default so
 * the shader still compiles. */
/* exports32: colour outputs come from r0/r2/r3/r4 (SET_SHADER_CONTROL bit
 * 0x40) instead of h0/h4/h6/h8 (half-precision programs). */
int rsx_fp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size,
                     int exports32);

/* Texture-state-aware variant. `texture_state` packs 16 three-bit
 * RSX_FP_TEXTURE_DIM_* values in bits 0..47 and an RSX depth-comparison mask
 * in bits 48..63, one bit for each t0..t15 texture unit. */
int rsx_fp_decompile_tex(const u8* ucode, u32 max_bytes, char* out,
                         u32 out_size, int exports32,
                         u64 texture_state);

/* Return the mnemonic for an NV40 fragment opcode (or "?" if unknown).
 * Useful for disassembly/logging. */
const char* rsx_fp_opcode_name(u32 opcode);

/* Total byte length of the fragment program at `ucode` (including inline
 * CONST slots), up to and including the PROGRAM_END instruction, bounded by
 * `max_bytes`. Returns 0 if no terminator is found within the bound. Shared
 * walk logic so callers (e.g. a shader cache hashing the bytecode) agree with
 * the decompiler on where a program ends. */
u32 rsx_fp_program_size(const u8* ucode, u32 max_bytes);

/* Strictly walk one exact FP stream and copy every decoded host-order word.
 * Inline CONST payload words are retained in raw_words for per-draw data, but
 * excluded from structural_hash so patching only those values reuses the same
 * template. Returns 0 on success, -1 for null output, truncated payload, no
 * END within the bound, or fixed capture capacity overflow. */
int rsx_fp_capture_structural(const u8* ucode, u32 max_bytes,
                              RSXFPStructuralProgram* out);

/* Exact shape comparison. Inline payload positions are structural, their
 * values are not; no raw-word memcmp may include those values. */
int rsx_fp_structural_equal(const RSXFPStructuralProgram* a,
                            const RSXFPStructuralProgram* b);

/* Emit an HLSL template from a strict captured program. Inline CONST sources
 * use `asfloat(rsx_inline[slot])` from b1; all CONST sources in one
 * instruction share one slot. `nrt` is the exact 1..4 PS output count, so it
 * changes emitted HLSL; callers cache that variant with their existing nrt
 * PSO key. The literal rsx_fp_decompile APIs keep their four-target output. */
int rsx_fp_decompile_template(const RSXFPStructuralProgram* program,
                              char* out, u32 out_size, int exports32,
                              int nrt);

int rsx_fp_decompile_template_tex(const RSXFPStructuralProgram* program,
                                  char* out, u32 out_size, int exports32,
                                  u64 texture_state, int nrt);

/* TJ_RENDER_SCALE: nonzero emits WPOS as SV_Position.xy * rsx_windowcontrol.w
 * (the backend's per-draw 1/raster scale) before the window bias/flip. Call
 * once before the first decompile; the default keeps the original 1x text. */
void rsx_fp_set_wpos_host_scale(int enabled);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_FP_DECOMPILER_H */
