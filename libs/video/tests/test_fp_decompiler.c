/*
 * Standalone unit test for the RSX fragment-program decompiler.
 *
 * Build (no D3D12 dependency):
 *   gcc -std=c11 -Wall -Wextra -I../../../include \
 *       test_fp_decompiler.c ../rsx_fp_decompiler.c ../rsx_vp_decompiler.c -o test_fp.exe
 *
 * Exercises the decode path (word swap, source swizzle/type, opcode → HLSL,
 * dest mask, inline constants) by hand-assembling tiny programs and checking
 * the generated HLSL contains the expected expressions.
 */

#include "../rsx_fp_decompiler.h"
#include "../rsx_fp_probe_inject.h"
#include "../rsx_vp_decompiler.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

/* Store host word H into guest bytes so that rsx_fp_read_word(p) == H.
 * rsx_fp_read_word does be32-load then 16-bit half-swap, so we invert: the
 * guest stores big-endian of swap16(H). */
static void put_word(u8* p, u32 h)
{
    u32 be = (h << 16) | (h >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16);
    p[2] = (u8)(be >> 8);  p[3] = (u8)(be);
}

static void put_float(u8* p, float f)
{
    u32 h; memcpy(&h, &f, 4);
    put_word(p, h);
}

static void check(const char* test, const char* hlsl, const char* needle)
{
    if (strstr(hlsl, needle)) {
        printf("[PASS] %s -- found \"%s\"\n", test, needle);
        g_pass++;
    } else {
        printf("[FAIL] %s -- missing \"%s\"\nHLSL:\n%s\n", test, needle, hlsl);
        g_fail++;
    }
}

static void check_bool(const char* test, int value)
{
    if (value) { printf("[PASS] %s\n", test); g_pass++; }
    else { printf("[FAIL] %s\n", test); g_fail++; }
}

static void copy_text(char* dst, size_t capacity, const char* src)
{
    size_t bytes = strlen(src) + 1;
    if (bytes <= capacity) memcpy(dst, src, bytes);
}

/* OPDEST/SRC bit helpers (mirror the decoder's macros). */
#define OPC(x)      ((u32)(x) << 24)
#define OUTMASK_ALL (0xFu << 9)
#define END         (1u << 0)
#define INSRC(x)    ((u32)(x) << 13)
#define TEXU(x)     ((u32)(x) << 17)
#define SWZ_IDENT   ((0u<<9)|(1u<<11)|(2u<<13)|(3u<<15))
#define SWZ_WXYZ    ((3u<<9)|(0u<<11)|(1u<<13)|(2u<<15))
#define T_TEMP      0u
#define T_INPUT     1u
#define T_CONST     2u
#define REG(i)      ((u32)(i) << 2)
#define EXEC_ALWAYS  (7u << 18)
#define BRANCH       (1u << 31)

int main(void)
{
    char hlsl[64 * 1024];

    /* Texture use is instruction-derived, not inherited binding state or
     * inline payload bits. Include every predicate/branch possibility. */
    {
        const u32 texture_ops[] = {0x17,0x18,0x19,0x2F,0x31,0x33,0x34,0x35,0x37};
        int all_units = 1;
        for (u32 op = 0; op < sizeof(texture_ops)/sizeof(texture_ops[0]); ++op)
            for (u32 unit = 0; unit < 16; ++unit) {
                u8 prog[16] = {0}; RSXFPStructuralProgram captured;
                put_word(prog, OPC(texture_ops[op]) | TEXU(unit) | END);
                all_units &= rsx_fp_capture_structural(prog, sizeof(prog), &captured) == 0 &&
                    captured.texture_read_mask == (1u << unit);
            }
        check_bool("fp_texture_mask_all_sampling_ops_and_units", all_units);
        u8 prog[64] = {0}; RSXFPStructuralProgram captured;
        put_word(prog, OPC(0x01)); put_word(prog + 4, T_CONST);
        put_word(prog + 16, OPC(0x17) | TEXU(15) | END); /* data, not an instruction */
        put_word(prog + 32, OPC(0x17) | TEXU(2));
        put_word(prog + 48, OPC(0x18) | TEXU(9) | END);
        check_bool("fp_texture_mask_skips_inline_and_unions_predicates",
            rsx_fp_capture_structural(prog, sizeof(prog), &captured) == 0 &&
            captured.texture_read_mask == ((1u << 2) | (1u << 9)));
        memset(prog, 0, sizeof(prog));
        put_word(prog, OPC(0x17) | TEXU(15)); put_word(prog + 8, BRANCH);
        put_word(prog + 16, OPC(0x17) | TEXU(4) | END);
        check_bool("fp_texture_mask_flow_words_are_not_samples",
            rsx_fp_capture_structural(prog, 32, &captured) == 0 &&
            captured.texture_read_mask == (1u << 4));
        memset(prog, 0, sizeof(prog)); put_word(prog, OPC(0x3F) | END);
        check_bool("fp_texture_mask_unknown_keeps_all_bindings",
            rsx_fp_capture_structural(prog, 16, &captured) == 0 &&
            captured.texture_read_mask == 0xFFFFu);
    }

    /* Test 1: MOV r0, COL0  (write all, END). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END); /* MOV, COL0 */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);         /* src0 = INPUT */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        int n = rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        printf("-- test1 MOV: %d instr\n", n);
        check("mov_dest",  hlsl, "r[0].xyzw =");
        check("mov_input", hlsl, "input.col");
        check("mov_return", hlsl, "return _po;");
        {
            const char* tc9 = strstr(hlsl, "TEXCOORD9");
            const char* front_face = strstr(hlsl, "SV_IsFrontFace");
            check_bool("system_input_after_interpolants",
                       tc9 && front_face && front_face > tc9);
        }
    }

    /* Test 2: MUL r1.xy, r0, r0  (partial write mask). */
    {
        u8 prog[16];
        u32 mask_xy = (1u<<9)|(1u<<10);
        put_word(prog + 0, OPC(0x02) | mask_xy | (1u<<1) | END); /* MUL, dest r1 */
        put_word(prog + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_TEMP | REG(0) | SWZ_IDENT);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("mul_mask", hlsl, "r[1].xy =");
        check("mul_expr", hlsl, "(r[0]).xyzw) * ((r[0]).xyzw)");
    }

    /* Test 3: MAD r0, r0, CONST, r0 with inline constant + saturate. */
    {
        u8 prog[32];
        put_word(prog + 0, OPC(0x04) | OUTMASK_ALL | (1u<<31) | END); /* MAD + SAT */
        put_word(prog + 4, T_TEMP  | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_CONST | SWZ_IDENT);                       /* src1 CONST */
        put_word(prog + 12, T_TEMP | REG(0) | SWZ_IDENT);
        put_float(prog + 16, 0.5f);  /* inline constant float4 */
        put_float(prog + 20, 0.25f);
        put_float(prog + 24, 0.0f);
        put_float(prog + 28, 1.0f);
        int n = rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        printf("-- test3 MAD: %d instr\n", n);
        check("mad_sat",   hlsl, "saturate(");
        check("mad_const", hlsl, "float4(0.5,0.25,0,1)");
    }

    /* Test 4: TEX r0, TC0 from texture unit 3. */
    /* Structural template capture: changing only an inline payload preserves
     * exact instruction shape, while the generated template reads b1 slots. */
    {
        u8 a_bytes[32], b_bytes[32];
        RSXFPStructuralProgram a, b;
        put_word(a_bytes + 0, OPC(0x04) | OUTMASK_ALL | END);
        put_word(a_bytes + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(a_bytes + 8, T_CONST | SWZ_IDENT);
        put_word(a_bytes + 12, T_TEMP | REG(0) | SWZ_IDENT);
        put_float(a_bytes + 16, 0.5f); put_float(a_bytes + 20, 0.25f);
        put_float(a_bytes + 24, 0.0f); put_float(a_bytes + 28, 1.0f);
        memcpy(b_bytes, a_bytes, sizeof(a_bytes));
        put_float(b_bytes + 16, 7.0f);
        check_bool("fp_capture_a", rsx_fp_capture_structural(a_bytes, sizeof(a_bytes), &a) == 0);
        check_bool("fp_capture_b", rsx_fp_capture_structural(b_bytes, sizeof(b_bytes), &b) == 0);
        check_bool("fp_capture_inline_count", a.inline_count == 1 && a.size_bytes == 32u);
        check_bool("fp_shape_ignores_payload", a.structural_hash == b.structural_hash &&
                   rsx_fp_structural_equal(&a, &b));
        check_bool("fp_capture_retains_payload", a.raw_words[a.inline_word_index[0]] !=
                   b.raw_words[b.inline_word_index[0]]);
        check_bool("fp_template_decompile", rsx_fp_decompile_template(&a, hlsl, sizeof(hlsl), 0, 4) > 0);
        check("fp_template_inline_bank", hlsl, "uint4 rsx_inline[128]");
        check("fp_template_inline_asfloat", hlsl, "asfloat(rsx_inline[0])");
    }

    /* One inline payload is shared by every CONST source in its instruction. */
    {
        u8 prog[32]; RSXFPStructuralProgram captured;
        put_word(prog + 0, OPC(0x04) | OUTMASK_ALL | END);
        put_word(prog + 4, T_CONST | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_CONST | SWZ_IDENT);
        put_word(prog + 12, T_TEMP | REG(0) | SWZ_IDENT);
        put_float(prog + 16, 1.0f); put_float(prog + 20, 2.0f);
        put_float(prog + 24, 3.0f); put_float(prog + 28, 4.0f);
        check_bool("fp_repeat_const_capture", rsx_fp_capture_structural(prog, sizeof(prog), &captured) == 0 &&
                   captured.inline_count == 1);
        check_bool("fp_repeat_const_template", rsx_fp_decompile_template(&captured, hlsl, sizeof(hlsl), 0, 4) > 0 &&
                   strstr(hlsl, "asfloat(rsx_inline[0])") != NULL &&
                   strstr(hlsl, "rsx_inline[1]") == NULL);
    }

    /* Structural templates must emit the exact PSO render-target count. */
    {
        u8 prog[16]; RSXFPStructuralProgram captured;
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(1) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0); put_word(prog + 12, 0);
        check_bool("fp_nrt_capture", rsx_fp_capture_structural(prog, sizeof(prog), &captured) == 0);
        check_bool("fp_nrt_one_target", rsx_fp_decompile_template(&captured, hlsl, sizeof(hlsl), 0, 1) > 0 &&
                   strstr(hlsl, "PSOut _po; _po.c0 = ") != NULL &&
                   strstr(hlsl, "SV_Target0") != NULL &&
                   strstr(hlsl, "SV_Target1") == NULL &&
                   strstr(hlsl, "SV_Target2") == NULL &&
                   strstr(hlsl, "SV_Target3") == NULL &&
                   strstr(hlsl, "_po.c1") == NULL &&
                   strstr(hlsl, "_po.c2") == NULL &&
                   strstr(hlsl, "_po.c3") == NULL);
        check_bool("fp_nrt_four_targets", rsx_fp_decompile_template(&captured, hlsl, sizeof(hlsl), 0, 4) > 0 &&
                   strstr(hlsl, "SV_Target0") != NULL &&
                   strstr(hlsl, "SV_Target1") != NULL &&
                   strstr(hlsl, "SV_Target2") != NULL &&
                   strstr(hlsl, "SV_Target3") != NULL &&
                   strstr(hlsl, "_po.c1") != NULL &&
                   strstr(hlsl, "_po.c2") != NULL &&
                   strstr(hlsl, "_po.c3") != NULL);
    }

    /* Strict capture rejects malformed streams and leaves no partial output. */
    {
        u8 truncated[16], no_end[16], too_large[RSX_FP_MAX_PROGRAM_BYTES + 16u] = {0};
        RSXFPStructuralProgram captured, zero = {0};
        put_word(truncated + 0, OPC(0x01) | OUTMASK_ALL | END);
        put_word(truncated + 4, T_CONST | SWZ_IDENT | EXEC_ALWAYS);
        put_word(truncated + 8, 0); put_word(truncated + 12, 0);
        memset(&captured, 0xA5, sizeof(captured));
        check_bool("fp_capture_truncated_inline_rejected",
                   rsx_fp_capture_structural(truncated, sizeof(truncated), &captured) != 0 &&
                   rsx_fp_program_size(truncated, sizeof(truncated)) == 0 &&
                   memcmp(&captured, &zero, sizeof(captured)) == 0 &&
                   rsx_fp_decompile(truncated, sizeof(truncated), hlsl, sizeof(hlsl), 0) < 0);
        put_word(no_end + 0, OPC(0x01) | OUTMASK_ALL);
        put_word(no_end + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(no_end + 8, 0); put_word(no_end + 12, 0);
        check_bool("fp_capture_no_end_rejected",
                   rsx_fp_capture_structural(no_end, sizeof(no_end), &captured) != 0);
        for (u32 off = 0; off < RSX_FP_MAX_PROGRAM_BYTES; off += 16u) {
            put_word(too_large + off + 0, OPC(0x01) | OUTMASK_ALL);
            put_word(too_large + off + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
            put_word(too_large + off + 8, 0); put_word(too_large + off + 12, 0);
        }
        put_word(too_large + RSX_FP_MAX_PROGRAM_BYTES, OPC(0x01) | OUTMASK_ALL | END);
        put_word(too_large + RSX_FP_MAX_PROGRAM_BYTES + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        check_bool("fp_capture_overflow_rejected",
                   rsx_fp_capture_structural(too_large, sizeof(too_large), &captured) != 0);
    }

    /* Host-order raw words survive capture exactly, including values that are
     * unsafe to compare as floats. */
    {
        static const u32 bits[] = {0x80000000u, 0x7FC12345u, 0x7F800000u,
                                   0xFF800000u, 0x00000001u};
        u8 prog[64]; RSXFPStructuralProgram captured;
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL);
        put_word(prog + 4, T_CONST | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0); put_word(prog + 12, 0);
        for (u32 i = 0; i < 4u; ++i) put_word(prog + 16u + i * 4u, bits[i]);
        put_word(prog + 32, OPC(0x01) | OUTMASK_ALL | END);
        put_word(prog + 36, T_CONST | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 40, 0); put_word(prog + 44, 0);
        put_word(prog + 48, bits[4]);
        put_word(prog + 52, 0); put_word(prog + 56, 0); put_word(prog + 60, 0);
        int exact = rsx_fp_capture_structural(prog, sizeof(prog), &captured) == 0 &&
                    captured.inline_count == 2 && captured.raw_word_count == 16u;
        for (u32 i = 0; exact && i < 4u; ++i)
            exact = captured.raw_words[captured.inline_word_index[0] + i] == bits[i] &&
                    rsx_fp_read_word(prog + 16u + i * 4u) == bits[i];
        exact = exact && captured.raw_words[captured.inline_word_index[1]] == bits[4] &&
                rsx_fp_read_word(prog + 48) == bits[4];
        check_bool("fp_raw_bit_roundtrip_negzero_nan_inf_subnormal", exact);
    }

    /* Branch words can look like CONST sources but have no inline payload. */
    {
        u8 prog[16]; RSXFPStructuralProgram captured;
        put_word(prog + 0, OPC(0x02) | OUTMASK_ALL | END);
        put_word(prog + 4, T_CONST | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, BRANCH | T_CONST);
        put_word(prog + 12, T_CONST | SWZ_IDENT);
        check_bool("fp_branch_const_words_no_payload",
                   rsx_fp_capture_structural(prog, sizeof(prog), &captured) == 0 &&
                   captured.inline_count == 0 && captured.size_bytes == 16u);
    }

    /* Fixed capture limit: 128 instruction+payload pairs fit exactly in 4 KiB;
     * 129 must fail before reading or retaining an oversized program. */
    {
        u8 exact[RSX_FP_MAX_PROGRAM_BYTES] = {0};
        u8 overflow[RSX_FP_MAX_PROGRAM_BYTES + 32u] = {0};
        RSXFPStructuralProgram captured;
        for (u32 i = 0; i < 128u; ++i) {
            u8* p = exact + i * 32u;
            put_word(p + 0, OPC(0x01) | OUTMASK_ALL | (i == 127u ? END : 0));
            put_word(p + 4, T_CONST | SWZ_IDENT | EXEC_ALWAYS);
            put_word(p + 8, 0); put_word(p + 12, 0);
            put_word(p + 16, i); put_word(p + 20, 0);
            put_word(p + 24, 0); put_word(p + 28, 0);
        }
        for (u32 i = 0; i < 129u; ++i) {
            u8* p = overflow + i * 32u;
            put_word(p + 0, OPC(0x01) | OUTMASK_ALL | (i == 128u ? END : 0));
            put_word(p + 4, T_CONST | SWZ_IDENT | EXEC_ALWAYS);
            put_word(p + 8, 0); put_word(p + 12, 0);
            put_word(p + 16, i); put_word(p + 20, 0);
            put_word(p + 24, 0); put_word(p + 28, 0);
        }
        check_bool("fp_capture_exactly_128_inline_slots",
                   rsx_fp_capture_structural(exact, sizeof(exact), &captured) == 0 &&
                   captured.inline_count == RSX_FP_MAX_INLINE_CONSTANTS &&
                   captured.size_bytes == RSX_FP_MAX_PROGRAM_BYTES);
        check_bool("fp_capture_129_inline_slots_rejected",
                   rsx_fp_capture_structural(overflow, sizeof(overflow), &captured) != 0);
    }

    /* Test 4: TEX r0, TC0 from texture unit 3. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | TEXU(3) | END); /* TEX */
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("tex_sample", hlsl, "rsx_tex3.Sample(rsx_samp3");
    }

    /* TXL/TXB take their explicit LOD/bias from source0.w, not source1.x. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x2F) | OUTMASK_ALL | TEXU(3) | END);
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_TEMP | REG(2) | SWZ_IDENT);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("txl_lod_source0_w", hlsl, "SampleLevel(rsx_samp3");
        check("txl_lod_source0_w_exact", hlsl, "((r[1]).xyzw).w");
        check_bool("txl_never_source1_x", strstr(hlsl, "((r[2]).xyzw).x") == NULL);
    }

    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x31) | OUTMASK_ALL | TEXU(3) | END);
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_TEMP | REG(2) | SWZ_IDENT);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("txb_bias_source0_w", hlsl, "SampleBias(rsx_samp3");
        check("txb_bias_source0_w_exact", hlsl, "((r[1]).xyzw).w");
        check_bool("txb_never_source1_x", strstr(hlsl, "((r[2]).xyzw).x") == NULL);
    }

    /* Cube TEX still applies the unnormalized texel-to-unit scale to xyz. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x17) | OUTMASK_ALL | TEXU(3) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0); put_word(prog + 12, 0);
        rsx_fp_decompile_tex(prog, sizeof(prog), hlsl, sizeof(hlsl), 0,
                             (u64)RSX_FP_TEXTURE_DIM_CUBE << (3u * 3u));
        check("cube_tex_type", hlsl, "TextureCube rsx_tex3");
        check_bool("cube_direction_not_scaled",
                   strstr(hlsl, "rsx_texscale[3].xyz") == NULL);
    }

    /* Test 5: negate + abs on a source (MOV r0, -|r1|). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | END);
        put_word(prog + 4, T_TEMP | REG(1) | SWZ_IDENT | EXEC_ALWAYS | (1u<<17) | (1u<<29)); /* neg+abs */
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("neg_abs", hlsl, "-(abs((r[1]).xyzw))");
    }

    /* Test 6: MOV r0, TC9 (input source 0xD). */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(0xD) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("tc9_input", hlsl, "input.tc9");
    }

    /* SET_TEX_COORD_CONTROL is runtime state. Static TC sources retain the
     * unselected branch, while a selected 2D source gets RSX's z/w rewrite. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(0x4) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0); put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("tc_control_runtime_selected_nonperspective", hlsl,
              "((_fpinput & (1u << 0u)) != 0u ? float4(input.tc0.xy, 0.0, 1.0 / input.position.w) : input.tc0)");
    }

    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(0x4) | END);
        put_word(prog + 4, T_INPUT | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, 0); put_word(prog + 12, 1u << 31);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("tc_control_runtime_selected_perspective", hlsl,
              "((_fpinput & (1u << 0u)) != 0u ? float4(input.tc0.xy * input.position.w, 0.0, 1.0) : (input.tc0 * input.position.wwww))");
    }

    /* Source swizzle applies after the 2D-coordinate substitution. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x01) | OUTMASK_ALL | INSRC(0x4) | END);
        put_word(prog + 4, T_INPUT | SWZ_WXYZ | EXEC_ALWAYS);
        put_word(prog + 8, 0); put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("tc_control_transform_before_swizzle", hlsl,
              "1.0 / input.position.w) : input.tc0)).wxyz");
    }

    /* Test 7: VP output wiring carries RSX o15/o6 to TC8/TC9 and keeps
     * GL_NV_fragment_program FOGC scalar in x only. */
    {
        u8 prog[16] = {0};
        char vp_hlsl[8192];
        prog[12] = 1; /* D3.end in little-endian instruction storage. */
        rsx_vp_decompile(prog, sizeof(prog), vp_hlsl, sizeof(vp_hlsl));
        check("vp_tc8_decl", vp_hlsl, "float4 t8:TEXCOORD8");
        check("vp_tc9_decl", vp_hlsl, "float4 t9:TEXCOORD9");
        check("vp_tc8_route", vp_hlsl, "Out.t8=o[15]");
        check("vp_tc9_route", vp_hlsl, "Out.t9=o[6]");
        check("vp_fogc_x_only", vp_hlsl, "Out.fog = float4(o[5].x,0,0,0)");
    }

    /* Test 8: strict target parser + post-instruction StageGrid injection. */
    {
        static const char source[] =
            "struct PSInput { float4 position : SV_POSITION; float4 col0 : COLOR0; "
            "float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; };\n"
            "Texture2D rsx_tex0 : register(t0); Texture2D rsx_tex1 : register(t1);\n"
            "cbuffer FPTex { float4 rsx_texscale[4]; };\n"
            "SamplerState rsx_samp0 : register(s0); SamplerState rsx_samp1 : register(s1);\n"
            "PSOut main(PSInput input) {\n"
            "    float4 r[48]; float4 h[48];\n"
            "    r[0] = input.col0;\n"
            "    PSOut _po; _po.c0 = r[0];\n"
            "    return _po;\n}\n";
        uint32_t parsed = 0;
        check_bool("stagegrid_parse_valid", rsx_fp_probe_parse_u32_hex("07600741", &parsed) && parsed == 0x07600741u);
        check_bool("stagegrid_parse_rejects_invalid", !rsx_fp_probe_parse_u32_hex(" 7600741", &parsed) &&
                   !rsx_fp_probe_parse_u32_hex("+7600741", &parsed) &&
                   !rsx_fp_probe_parse_u32_hex("100000000", &parsed));
        check_bool("stagegrid_target_exact", rsx_fp_probe_target_matches("07600741", 0x07600741u) &&
                   !rsx_fp_probe_target_matches("07600742", 0x07600741u));

        char wrong[4096]; copy_text(wrong, sizeof(wrong), source);
        if (rsx_fp_probe_target_matches("07600742", 0x07600741u))
            rsx_fp_probe_stagegrid_inject(wrong, sizeof(wrong));
        check_bool("stagegrid_wrong_target_no_mutation", strcmp(wrong, source) == 0);

        char missing[4096] = "float4 position : SV_POSITION;";
        char missing_before[4096]; copy_text(missing_before, sizeof(missing_before), missing);
        check_bool("stagegrid_missing_marker_no_mutation",
                   !rsx_fp_probe_stagegrid_inject(missing, sizeof(missing)) &&
                   strcmp(missing, missing_before) == 0);

        char capacity[4096]; copy_text(capacity, sizeof(capacity), source);
        char capacity_before[4096]; copy_text(capacity_before, sizeof(capacity_before), source);
        check_bool("stagegrid_capacity_no_mutation",
                   !rsx_fp_probe_stagegrid_inject(capacity, strlen(capacity) + 1) &&
                   strcmp(capacity, capacity_before) == 0);

        char staged[4096]; copy_text(staged, sizeof(staged), source);
        int inserted = rsx_fp_probe_stagegrid_inject(staged, sizeof(staged));
        const char* stage = strstr(staged, "_tj_fp_stage_band");
        const char* export_marker = strstr(staged, "PSOut _po;");
        check_bool("stagegrid_insert_order", inserted && stage && export_marker && stage < export_marker);
        check("stagegrid_band_count", staged, "input.position.x / (1280.0 / 7.0)");
        check("stagegrid_uv_band", staged, "frac(input.tc1.xy / input.tc1.w)");
        check("stagegrid_raw_t0_band", staged,
              "rsx_tex0.Sample(rsx_samp0, (input.tc1.xy / input.tc1.w) * rsx_texscale[0].xy)");
        check("stagegrid_t1_col_band", staged, "rsx_tex1.Sample(rsx_samp1");
        check("stagegrid_zero_init", staged, "PSOut _tj_fp_stage_out = (PSOut)0;");
        check_bool("stagegrid_no_absent_targets",
                   strstr(staged, "_tj_fp_stage_out.c1") == NULL &&
                   strstr(staged, "_tj_fp_stage_out.c2") == NULL &&
                   strstr(staged, "_tj_fp_stage_out.c3") == NULL);
        check("stagegrid_export", staged, "return _tj_fp_stage_out;");
    }

    /* Test 9: target-only LateGrid isolates the final fog/multiply path. */
    {
        static const char source[] =
            "struct PSInput { float4 position : SV_POSITION; float4 col0 : COLOR0; float4 fog : FOG; float4 tc0:TEXCOORD0; };\n"
            "Texture2D rsx_tex1 : register(t1);\n"
            "cbuffer FPTex { float4 rsx_texscale[4]; };\n"
            "SamplerState rsx_samp1 : register(s1);\n"
            "PSOut main(PSInput input) {\n"
            "    float4 r[48]; float4 h[48];\n"
            "    PSOut _po; _po.c0 = h[0];\n"
            "    return _po;\n}\n";
        char untouched[4096]; copy_text(untouched, sizeof(untouched), source);
        if (rsx_fp_probe_target_matches("07600742", 0x07600741u))
            rsx_fp_probe_lategrid_inject(untouched, sizeof(untouched));
        check_bool("lategrid_wrong_target_no_mutation", strcmp(untouched, source) == 0);

        char missing[4096] = "float4 position : SV_POSITION;";
        char missing_before[4096]; copy_text(missing_before, sizeof(missing_before), missing);
        check_bool("lategrid_marker_failure_no_mutation",
                   !rsx_fp_probe_lategrid_inject(missing, sizeof(missing)) &&
                   strcmp(missing, missing_before) == 0);

        char capacity[4096]; copy_text(capacity, sizeof(capacity), source);
        char capacity_before[4096]; copy_text(capacity_before, sizeof(capacity_before), capacity);
        check_bool("lategrid_capacity_no_mutation",
                   !rsx_fp_probe_lategrid_inject(capacity, strlen(capacity) + 1) &&
                   strcmp(capacity, capacity_before) == 0);

        char patched[4096]; copy_text(patched, sizeof(patched), source);
        int applied = rsx_fp_probe_target_matches("07600741", 0x07600741u) &&
                      rsx_fp_probe_lategrid_inject(patched, sizeof(patched));
        const char* late = strstr(patched, "_tj_fp_late_band");
        const char* export_marker = strstr(patched, "PSOut _po;");
        check_bool("lategrid_exact_target_insert_order", applied && late && export_marker && late < export_marker);
        check("lategrid_band_count", patched, "input.position.x / (1280.0 / 7.0)");
        check("lategrid_t1_col", patched, "rsx_tex1.Sample(rsx_samp1, input.tc0.xy * rsx_texscale[1].xy).rgb * input.col0.rgb");
        check("lategrid_fog", patched, "input.fog.yyy");
        check("lategrid_h2w", patched, "h[2].www");
        check("lategrid_r1w", patched, "r[1].www");
        check("lategrid_final_h0", patched, "else _tj_fp_late_rgb = h[0].rgb;");
        check("lategrid_zero_init", patched, "PSOut _tj_fp_late_out = (PSOut)0;");
        check_bool("lategrid_no_absent_targets",
                   strstr(patched, "_tj_fp_late_out.c1") == NULL &&
                   strstr(patched, "_tj_fp_late_out.c2") == NULL &&
                   strstr(patched, "_tj_fp_late_out.c3") == NULL);
        check("lategrid_export", patched, "return _tj_fp_late_out;");
    }

    /* Test 9: DIVSQ preserves a zero numerator lane instead of emitting
     * raw 0/sqrt(0), which would become NaN in HLSL. */
    {
        u8 prog[16];
        put_word(prog + 0, OPC(0x3B) | OUTMASK_ALL | END); /* DIVSQ r0, r0, r1 */
        put_word(prog + 4, T_TEMP | REG(0) | SWZ_IDENT | EXEC_ALWAYS);
        put_word(prog + 8, T_TEMP | REG(1) | SWZ_IDENT);
        put_word(prog + 12, 0);
        rsx_fp_decompile(prog, sizeof(prog), hlsl, sizeof(hlsl), 0);
        check("divsq_helper", hlsl, "float4 _builtin_divsq(float4 a, float b)");
        check("divsq_call", hlsl, "_builtin_divsq(");
        check("divsq_zero_x", hlsl, "abs(a.x) > 0 ? tmp.x : a.x");
        check("divsq_zero_y", hlsl, "abs(a.y) > 0 ? tmp.y : a.y");
        check("divsq_zero_z", hlsl, "abs(a.z) > 0 ? tmp.z : a.z");
        check("divsq_zero_w", hlsl, "abs(a.w) > 0 ? tmp.w : a.w");
    }

    /* Test 10: exact target-only t0 TXP zero-q substitution. */
    {
        static const char source[] =
            "Texture2D rsx_tex0 : register(t0);\n"
            "SamplerState rsx_samp0 : register(s0);\n"
            "cbuffer FPTex { float4 rsx_texscale[4]; };\n"
            "PSOut main(PSInput input) {\n"
            "    r[0] = rsx_tex0.Sample(rsx_samp0, ((input.tc1).xyzw).xy / ((input.tc1).xyzw).w * rsx_texscale[0].xy);\n"
            "    PSOut _po; _po.c0 = r[0];\n"
            "    return _po;\n}\n";
        char untouched[4096]; copy_text(untouched, sizeof(untouched), source);
        if (rsx_fp_probe_target_matches(NULL, 0x07600741u))
            rsx_fp_probe_txp_zeroq_inject(untouched, sizeof(untouched));
        check_bool("txp_zeroq_env_off_no_mutation", strcmp(untouched, source) == 0);
        if (rsx_fp_probe_target_matches("07600741", 0x07600742u))
            rsx_fp_probe_txp_zeroq_inject(untouched, sizeof(untouched));
        check_bool("txp_zeroq_wrong_target_no_mutation", strcmp(untouched, source) == 0);

        char missing[4096] = "PSOut main(PSInput input) {\n";
        char missing_before[4096]; copy_text(missing_before, sizeof(missing_before), missing);
        check_bool("txp_zeroq_marker_failure_no_mutation",
                   !rsx_fp_probe_txp_zeroq_inject(missing, sizeof(missing)) &&
                   strcmp(missing, missing_before) == 0);

        char patched[4096]; copy_text(patched, sizeof(patched), source);
        int applied = rsx_fp_probe_target_matches("07600741", 0x07600741u) &&
                      rsx_fp_probe_txp_zeroq_inject(patched, sizeof(patched));
        check_bool("txp_zeroq_exact_target_applied", applied);
        check("txp_zeroq_exact_replace", patched,
              "_tj_txp_zeroq(((input.tc1).xyzw).xy, ((input.tc1).xyzw).w, rsx_texscale[0].xy)");
        check("txp_zeroq_nonzero_preserved", patched,
              "q == 0.0 ? float2(0,0) : (xy / q) * scale");
        check("txp_zeroq_only_before_main", patched,
              "float2 _tj_txp_zeroq(float2 xy, float q, float2 scale)");
    }

    /* Test 11: target-only screen-space t0 resource scan. */
    {
        static const char source[] =
            "struct PSInput { float4 position : SV_POSITION; };\n"
            "Texture2D rsx_tex0 : register(t0);\n"
            "cbuffer FPTex { float4 rsx_texscale[4]; };\n"
            "SamplerState rsx_samp0 : register(s0);\n"
            "PSOut main(PSInput input) {\n"
            "    PSOut _po; _po.c0 = r[0];\n"
            "    return _po;\n}\n";
        char untouched[4096]; copy_text(untouched, sizeof(untouched), source);
        if (rsx_fp_probe_target_matches(NULL, 0x07600741u))
            rsx_fp_probe_t0_screen_inject(untouched, sizeof(untouched));
        check_bool("t0_probe_env_off_no_mutation", strcmp(untouched, source) == 0);
        if (rsx_fp_probe_target_matches("07600741", 0x07600742u))
            rsx_fp_probe_t0_screen_inject(untouched, sizeof(untouched));
        check_bool("t0_probe_wrong_target_no_mutation", strcmp(untouched, source) == 0);

        char missing[4096] = "struct PSInput { float4 position : SV_POSITION; };";
        char missing_before[4096]; copy_text(missing_before, sizeof(missing_before), missing);
        check_bool("t0_probe_marker_failure_no_mutation",
                   !rsx_fp_probe_t0_screen_inject(missing, sizeof(missing)) &&
                   strcmp(missing, missing_before) == 0);

        char patched[4096]; copy_text(patched, sizeof(patched), source);
        int applied = rsx_fp_probe_target_matches("07600741", 0x07600741u) &&
                      rsx_fp_probe_t0_screen_inject(patched, sizeof(patched));
        check_bool("t0_probe_exact_target_applied", applied);
        check("t0_probe_screen_uv", patched,
              "saturate(input.position.xy * float2(1.0/1280.0,1.0/720.0)) * rsx_texscale[0].xy");
        check("t0_probe_alpha_visible", patched,
              "float4(_tj_fp_probe_t0_s.r, _tj_fp_probe_t0_s.g, max(_tj_fp_probe_t0_s.b, _tj_fp_probe_t0_s.a), 1)");
        check("t0_probe_zero_init", patched, "PSOut _tj_fp_probe_t0_out = (PSOut)0;");
        check_bool("t0_probe_no_absent_targets",
                   strstr(patched, "_tj_fp_probe_t0_out.c1") == NULL &&
                   strstr(patched, "_tj_fp_probe_t0_out.c2") == NULL &&
                   strstr(patched, "_tj_fp_probe_t0_out.c3") == NULL);
        check("t0_probe_early_return", patched, "return _tj_fp_probe_t0_out;");
    }

    printf("\n===========================================\n");
    printf("Results: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
