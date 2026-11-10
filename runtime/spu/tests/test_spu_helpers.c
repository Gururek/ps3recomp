/*
 * Unit tests for SPU instruction-semantics helpers (runtime/spu/spu_helpers.h).
 *
 * Each test fixes a known input and asserts the bit-exact output. Vectors
 * are taken from the IBM Cell BE Architecture / SPU ISA reference; tricky
 * cases (selector specials in shufb, boundary shifts at 0 and >=width,
 * negative-direction rotates in rotmi/rotmai, insertion-mask generation)
 * have multiple vectors.
 *
 * Build:
 *   gcc -std=c11 -I../.. test_spu_helpers.c -o test_spu_helpers
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints summary.
 */

#include "../spu_helpers.h"
#include "../spu_workload.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;
static const char* g_current = "(none)";

static u128 make128(uint64_t hi, uint64_t lo) {
    u128 r; r._u64[0] = hi; r._u64[1] = lo; return r;
}

/* Construct from 16 explicit bytes — endianness-neutral (writes _u8[i]=b[i]).
 * Use this for any test that cares about specific byte layout (shufb, cwd,
 * fsmbi, byte permutations). make128 above interprets values according to
 * host endianness, which is fine only for lane-symmetric vectors. */
static u128 from_bytes(const uint8_t b[16]) {
    /* Adapted to THIS runtime's u128 convention. Our u128 is host-native
     * little-endian (_u32[i] = SPU word i as a value), so SPU byte i lives at
     * _u8[SPU_W(i)], not _u8[i]. Upstream's u128 is a big-endian byte array
     * (_u8[i] = SPU byte i), which is what these ISA vectors were written for.
     * Routing the byte vector through SPU_W is the only change needed to make
     * the byte-layout tests (shufb, cwd/cbd/chd/cdd, byte shifts) evaluate in
     * our representation; every byte-position op already maps through SPU_W. */
    u128 r;
    for (int i = 0; i < 16; i++) r._u8[SPU_W(i)] = b[i];
    return r;
}

static int u128_eq(u128 a, u128 b) {
    return a._u64[0] == b._u64[0] && a._u64[1] == b._u64[1];
}

static void print_u128(const char* label, u128 v) {
    fprintf(stderr, "  %-8s %016llX %016llX\n", label,
            (unsigned long long)v._u64[0],
            (unsigned long long)v._u64[1]);
}

#define EXPECT_EQ(actual, expected) do {                          \
    u128 _a = (actual), _e = (expected);                          \
    if (u128_eq(_a, _e)) { g_pass++; }                            \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n", g_current);                 \
        print_u128("actual",   _a);                               \
        print_u128("expected", _e);                               \
    }                                                             \
} while (0)

#define TEST(name) g_current = name

/* ===========================================================================
 * Immediate loaders: broadcast semantics
 * ===========================================================================*/
static void test_immediate_loaders(void) {
    TEST("il splats sign-extended 16-bit to 4 words");
    EXPECT_EQ(spu_il(5),    make128(0x0000000500000005ULL, 0x0000000500000005ULL));
    EXPECT_EQ(spu_il(-1),   make128(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL));
    EXPECT_EQ(spu_il(-2),   make128(0xFFFFFFFEFFFFFFFEULL, 0xFFFFFFFEFFFFFFFEULL));

    TEST("ilh splats 16-bit to 8 halfwords");
    EXPECT_EQ(spu_ilh(0xABCD), make128(0xABCDABCDABCDABCDULL, 0xABCDABCDABCDABCDULL));

    TEST("ilhu shifts immediate left 16 then splats to words");
    EXPECT_EQ(spu_ilhu(0x1234), make128(0x1234000012340000ULL, 0x1234000012340000ULL));

    TEST("iohl OR's halfword lower into each word");
    EXPECT_EQ(spu_iohl(spu_ilhu(0x1234), 0x5678),
              make128(0x1234567812345678ULL, 0x1234567812345678ULL));
}

/* ===========================================================================
 * Per-lane SIMD arithmetic
 * ===========================================================================*/
static void test_simd_arith(void) {
    TEST("a (add word) per-lane");
    u128 a4 = make128(0x0000000100000002ULL, 0x0000000300000004ULL);
    u128 b4 = make128(0x0000001000000020ULL, 0x0000003000000040ULL);
    EXPECT_EQ(spu_a(a4, b4),
              make128(0x0000001100000022ULL, 0x0000003300000044ULL));

    TEST("sf (subtract from word) does rb-ra, not ra-rb");
    EXPECT_EQ(spu_sf(spu_il(3), spu_il(7)), spu_il(4));   /* 7-3 = 4 */

    TEST("ah (add halfword) per-lane");
    EXPECT_EQ(spu_ah(spu_ilh(10), spu_ilh(20)), spu_ilh(30));

    TEST("ai (add immediate, sign-extended)");
    EXPECT_EQ(spu_ai(spu_il(100), -5), spu_il(95));

    TEST("cg (carry generate, per word) — sets 1 when overflow");
    u128 cg_in_a = spu_il((int32_t)0xFFFFFFFF);
    u128 cg_in_b = spu_il(1);
    EXPECT_EQ(spu_cg(cg_in_a, cg_in_b), spu_il(1));

    TEST("cg — no overflow returns 0");
    EXPECT_EQ(spu_cg(spu_il(1), spu_il(2)), spu_il(0));

    TEST("addx (add extended w/ carry-in from rt LSB)");
    u128 carry_one = spu_il(1);   /* rt._u32[i] & 1 = 1 */
    EXPECT_EQ(spu_addx(spu_il(10), spu_il(20), carry_one), spu_il(31));
    u128 carry_zero = spu_il(2);  /* LSB = 0 */
    EXPECT_EQ(spu_addx(spu_il(10), spu_il(20), carry_zero), spu_il(30));
}

/* ===========================================================================
 * Multiply family — these mix lanes; common to get wrong
 * ===========================================================================*/
static void test_multiply(void) {
    /* mpy: low halfword of each word × low halfword → 32-bit word. */
    TEST("mpy multiplies low halfwords of each word");
    /* word i = 0x00030007 -> low half = 7, high half = 3.
       For mpy: low * low = 7*5 = 35 per word. */
    u128 a = make128(0x0003000700030007ULL, 0x0003000700030007ULL);
    u128 b = make128(0x0002000500020005ULL, 0x0002000500020005ULL);
    EXPECT_EQ(spu_mpy(a, b), spu_il(35));

    TEST("mpyh multiplies high half of ra by low half of rb, then <<16");
    /* per word: (high_a * low_b) << 16 = (3 * 5) << 16 = 15 << 16 = 0xF0000 */
    EXPECT_EQ(spu_mpyh(a, b), spu_il(0xF0000));

    TEST("mpyhh multiplies high halves of both");
    /* high_a * high_b = 3 * 2 = 6 per word */
    EXPECT_EQ(spu_mpyhh(a, b), spu_il(6));

    TEST("mpyu unsigned matches mpy for positive small values");
    EXPECT_EQ(spu_mpyu(a, b), spu_il(35));
}

/* ===========================================================================
 * Compares — produce all-ones or all-zeros per lane
 * ===========================================================================*/
static void test_compares(void) {
    TEST("ceq word equal");
    u128 x = make128(0x0000000A0000000BULL, 0x0000000C0000000BULL);
    u128 y = spu_il(0xB);
    EXPECT_EQ(spu_ceq(x, y),
              make128(0x00000000FFFFFFFFULL, 0x00000000FFFFFFFFULL));

    TEST("cgt signed greater (negative is less)");
    EXPECT_EQ(spu_cgt(spu_il(1), spu_il(-1)), spu_il(-1));   /* 1 > -1 */
    EXPECT_EQ(spu_cgt(spu_il(-1), spu_il(1)), spu_il(0));    /* -1 not > 1 */

    TEST("clgt unsigned greater");
    /* 0xFFFFFFFF > 1 unsigned */
    EXPECT_EQ(spu_clgt(spu_il(-1), spu_il(1)), spu_il(-1));

    TEST("ceqb byte-wise equal produces 0xFF per matching byte");
    {
        uint8_t zb_bytes[16] = {1,2,3,4,5,6,7,8, 1,2,3,4,5,6,7,8};
        uint8_t wb_bytes[16] = {1,0,3,0,4,6,5,8, 1,0,3,0,4,6,5,8};
        /* per byte: ==FF / !=00; expected mask: FF,00,FF,00,00,FF,00,FF (x2) */
        uint8_t exp_bytes[16] = {0xFF,0,0xFF,0,0,0xFF,0,0xFF,
                                 0xFF,0,0xFF,0,0,0xFF,0,0xFF};
        EXPECT_EQ(spu_ceqb(from_bytes(zb_bytes), from_bytes(wb_bytes)),
                  from_bytes(exp_bytes));
    }
}

/* ===========================================================================
 * Shift / rotate: boundary cases at 0 and >= width
 * ===========================================================================*/
static void test_shift_rotate_boundaries(void) {
    TEST("shli with sh=0 is identity (no UB)");
    EXPECT_EQ(spu_shli(spu_il(0xAB), 0), spu_il(0xAB));

    TEST("shli with sh=31 keeps only LSB");
    EXPECT_EQ(spu_shli(spu_il(1), 31), spu_il((int32_t)0x80000000));

    TEST("shli with sh>=32 is zero");
    EXPECT_EQ(spu_shli(spu_il(0x1234), 32), spu_il(0));

    TEST("shlhi halfword sh=0 is identity");
    EXPECT_EQ(spu_shlhi(spu_ilh(0xABCD), 0), spu_ilh(0xABCD));

    TEST("shlhi sh>=16 is zero");
    EXPECT_EQ(spu_shlhi(spu_ilh(0xABCD), 16), spu_ilh(0));

    TEST("roti at sh=0 is identity (was UB pre-fix)");
    EXPECT_EQ(spu_roti(spu_il((int32_t)0xDEADBEEF), 0), spu_il((int32_t)0xDEADBEEF));

    TEST("roti with sh=8 rotates left 8 bits per word");
    EXPECT_EQ(spu_roti(spu_il((int32_t)0x12345678), 8), spu_il((int32_t)0x34567812));

    TEST("rotmi: sh effective = (-i7) & 0x3F; i7=1 -> shift right by 63 -> 0");
    EXPECT_EQ(spu_rotmi(spu_il((int32_t)0x80000000), 1), spu_il(0));

    TEST("rotmi: i7=-4 -> shift right by 4");
    EXPECT_EQ(spu_rotmi(spu_il((int32_t)0xFFFF0000), -4), spu_il(0x0FFFF000));

    TEST("rotmai: signed right shift, sign-extends");
    EXPECT_EQ(spu_rotmai(spu_il((int32_t)0x80000000), -4),
              spu_il((int32_t)0xF8000000));

    TEST("shlqbyi shifts whole quadword left by 4 bytes");
    {
        uint8_t v[16]   = {1,2,3,4,5,6,7,8, 9,10,11,12,13,14,15,16};
        uint8_t exp[16] = {5,6,7,8,9,10,11,12, 13,14,15,16,0,0,0,0};
        EXPECT_EQ(spu_shlqbyi(from_bytes(v), 4), from_bytes(exp));
    }

    TEST("rotqbyi rotates whole quadword by 4 bytes (no zero fill)");
    {
        uint8_t v[16]   = {1,2,3,4,5,6,7,8, 9,10,11,12,13,14,15,16};
        uint8_t exp[16] = {5,6,7,8,9,10,11,12, 13,14,15,16,1,2,3,4};
        EXPECT_EQ(spu_rotqbyi(from_bytes(v), 4), from_bytes(exp));
    }
}

/* ===========================================================================
 * shufb: special selector values and byte permutation
 * ===========================================================================*/
static void test_shufb(void) {
    uint8_t a_bytes[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                           0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
    uint8_t b_bytes[16] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                           0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F};
    u128 a = from_bytes(a_bytes);
    u128 b = from_bytes(b_bytes);

    TEST("shufb selectors 0x00..0x0F pick a in order");
    EXPECT_EQ(spu_shufb(a, b, from_bytes(a_bytes)), a);

    TEST("shufb selectors 0x10..0x1F pick b in order");
    EXPECT_EQ(spu_shufb(a, b, from_bytes(b_bytes)), b);

    TEST("shufb selector banks map 0..15 to a and 16..31 to b");
    EXPECT_EQ(spu_shufb(spu_splat_u8(0x11), spu_splat_u8(0x22), spu_splat_u8(0x00)),
              spu_splat_u8(0x11));
    EXPECT_EQ(spu_shufb(spu_splat_u8(0x11), spu_splat_u8(0x22), spu_splat_u8(0x10)),
              spu_splat_u8(0x22));

    TEST("shufb special: sel & 0xE0 == 0xE0 -> 0x80");
    EXPECT_EQ(spu_shufb(a, b, spu_splat_u8(0xE0)), spu_splat_u8(0x80));
    EXPECT_EQ(spu_shufb(a, b, spu_splat_u8(0xE3)), spu_splat_u8(0x80));

    TEST("shufb special: sel & 0xC0 == 0xC0 (and not 0xE0) -> 0xFF");
    EXPECT_EQ(spu_shufb(a, b, spu_splat_u8(0xC0)), spu_splat_u8(0xFF));

    TEST("shufb special: sel & 0xC0 == 0x80 -> 0x00");
    EXPECT_EQ(spu_shufb(a, b, spu_splat_u8(0x80)), spu_splat_u8(0x00));

    TEST("shufb mixed: reverse first 4 bytes of a, rest from b");
    {
        uint8_t sel[16] = {0x03,0x02,0x01,0x00,
                           0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,
                           0x1C,0x1D,0x1E,0x1F};
        uint8_t expected[16] = {0x03,0x02,0x01,0x00,
                                0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,
                                0x1C,0x1D,0x1E,0x1F};
        EXPECT_EQ(spu_shufb(a, b, from_bytes(sel)), from_bytes(expected));
    }

    TEST("Tokyo SMC cntb plus ila(0x00010203) shufb packs byte counts per word");
    {
        uint8_t bits[16] = {0xFF,0x0F,0x03,0x01, 0,0,0,0, 0,0,0,0, 0,0,0,0};
        uint8_t expected[16] = {8,4,2,1, 8,4,2,1, 8,4,2,1, 8,4,2,1};
        u128 counts = spu_cntb(from_bytes(bits));
        EXPECT_EQ(spu_shufb(counts, counts, spu_ila(0x00010203)), from_bytes(expected));
    }

    TEST("cntb returns exact per-byte counts for mixed, zero, and full bytes");
    {
        uint8_t bits[16] = {
            0x00,0x01,0x02,0x03, 0x07,0x0F,0x1F,0x3F,
            0x7F,0xFF,0x55,0xAA, 0x81,0x42,0x24,0x18
        };
        uint8_t expected[16] = {
            0,1,1,2, 3,4,5,6, 7,8,4,4, 2,2,2,2
        };
        EXPECT_EQ(spu_cntb(from_bytes(bits)), from_bytes(expected));
    }

    TEST("cntb is exact for all 256 byte values");
    for (unsigned value = 0; value <= 0xFFu; ++value) {
        uint8_t bits[16];
        uint8_t expected[16];
        unsigned count = 0;
        for (unsigned scan = value; scan; scan >>= 1) count += scan & 1u;
        for (int lane = 0; lane < 16; ++lane) {
            bits[lane] = (uint8_t)value;
            expected[lane] = (uint8_t)count;
        }
        EXPECT_EQ(spu_cntb(from_bytes(bits)), from_bytes(expected));
    }

    TEST("shufb all 256 selectors are exact in every output lane");
    {
        int ok = 1;
        for (int lane = 0; lane < 16 && ok; ++lane) {
            for (int selector = 0; selector < 256; ++selector) {
                uint8_t control[16], expected[16];
                for (int i = 0; i < 16; ++i) control[i] = (uint8_t)(i * 17);
                control[lane] = (uint8_t)selector;
                for (int i = 0; i < 16; ++i) {
                    uint8_t s = control[i];
                    if ((s & 0xE0) == 0xE0) expected[i] = 0x80;
                    else if ((s & 0xC0) == 0xC0) expected[i] = 0xFF;
                    else if ((s & 0xC0) == 0x80) expected[i] = 0x00;
                    else expected[i] = (s & 0x10)
                        ? b_bytes[s & 0x0F] : a_bytes[s & 0x0F];
                }
                u128 actual = spu_shufb(a, b, from_bytes(control));
                if (!u128_eq(actual, from_bytes(expected))) {
                    fprintf(stderr, "FAIL: %s lane=%d selector=0x%02X\n",
                            g_current, lane, selector);
                    print_u128("actual", actual);
                    print_u128("expected", from_bytes(expected));
                    ok = 0;
                    break;
                }
            }
        }
        if (ok) g_pass++; else g_fail++;
    }
}

/* ===========================================================================
 * selb: bitwise select (c-bit ? b : a)
 * ===========================================================================*/
static void test_selb(void) {
    TEST("selb with c=all-zeros returns a");
    u128 a = make128(0xAAAAAAAAAAAAAAAAULL, 0xAAAAAAAAAAAAAAAAULL);
    u128 b = make128(0xBBBBBBBBBBBBBBBBULL, 0xBBBBBBBBBBBBBBBBULL);
    EXPECT_EQ(spu_selb(a, b, spu_zero()), a);

    TEST("selb with c=all-ones returns b");
    EXPECT_EQ(spu_selb(a, b, spu_splat_u8(0xFF)), b);

    TEST("selb per-bit mix");
    /* c = 0x0F repeating -> low nibble picks b, high nibble picks a */
    u128 c = spu_splat_u8(0x0F);
    u128 expected = spu_splat_u8(0xAB);   /* high nibble A from a, low nibble B from b */
    EXPECT_EQ(spu_selb(a, b, c), expected);
}

/* ===========================================================================
 * Constant generators: insertion mask shape
 * ===========================================================================*/
static void test_constant_generators(void) {
    /* Default selector pattern = identity-from-B: 0x10, 0x11, ..., 0x1F. */
    uint8_t identity_b[16] = {0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
                              0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F};

    TEST("cwd_pos at byte 0 inserts word at slot 0");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[0]=0x00; exp[1]=0x01; exp[2]=0x02; exp[3]=0x03;
        EXPECT_EQ(spu_cwd_pos(0), from_bytes(exp));
    }
    TEST("cwd_pos at byte 4 inserts word at slot 1");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[4]=0x00; exp[5]=0x01; exp[6]=0x02; exp[7]=0x03;
        EXPECT_EQ(spu_cwd_pos(4), from_bytes(exp));
    }
    TEST("cwd_pos at byte 12 inserts word at slot 3");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[12]=0x00; exp[13]=0x01; exp[14]=0x02; exp[15]=0x03;
        EXPECT_EQ(spu_cwd_pos(12), from_bytes(exp));
    }
    TEST("cwd_pos byte 5 aligns down to slot 1 (offset 4)");
    EXPECT_EQ(spu_cwd_pos(5), spu_cwd_pos(4));

    TEST("cbd_pos at byte 7 inserts selector 0x03 at offset 7");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[7]=0x03;
        EXPECT_EQ(spu_cbd_pos(7), from_bytes(exp));
    }
    TEST("chd_pos at byte 6 inserts selectors [02,03] at offset 6 (aligned)");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        exp[6]=0x02; exp[7]=0x03;
        EXPECT_EQ(spu_chd_pos(6), from_bytes(exp));
    }
    TEST("cdd_pos at byte 0 inserts dword selectors [00..07] at slot 0");
    {
        uint8_t exp[16]; memcpy(exp, identity_b, 16);
        for (int j = 0; j < 8; j++) exp[j] = (uint8_t)j;
        EXPECT_EQ(spu_cdd_pos(0), from_bytes(exp));
    }

    uint8_t a_bytes[16] = {0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
                           0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF};
    uint8_t b_bytes[16] = {0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,
                           0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF};
    u128 a = from_bytes(a_bytes), b = from_bytes(b_bytes);

    TEST("cbd control inserts a preferred byte into b");
    {
        uint8_t exp[16]; memcpy(exp, b_bytes, 16); exp[7] = a_bytes[3];
        EXPECT_EQ(spu_shufb(a, b, spu_cbd_pos(7)), from_bytes(exp));
    }
    TEST("chd control inserts a preferred halfword into b");
    {
        uint8_t exp[16]; memcpy(exp, b_bytes, 16); exp[6] = a_bytes[2]; exp[7] = a_bytes[3];
        EXPECT_EQ(spu_shufb(a, b, spu_chd_pos(6)), from_bytes(exp));
    }
    TEST("cwd control inserts a preferred word into b");
    {
        uint8_t exp[16]; memcpy(exp, b_bytes, 16); for (int j = 0; j < 4; j++) exp[4+j] = a_bytes[j];
        EXPECT_EQ(spu_shufb(a, b, spu_cwd_pos(4)), from_bytes(exp));
    }
    TEST("cdd control inserts a preferred doubleword into b");
    {
        uint8_t exp[16]; memcpy(exp, b_bytes, 16); for (int j = 0; j < 8; j++) exp[8+j] = a_bytes[j];
        EXPECT_EQ(spu_shufb(a, b, spu_cdd_pos(8)), from_bytes(exp));
    }
}

/* ===========================================================================
 * fsmbi: bit-mask to byte-mask
 * ===========================================================================*/
static void test_fsmbi(void) {
    TEST("fsmbi 0x0000 -> all zeros");
    EXPECT_EQ(spu_fsmbi(0x0000), spu_zero());

    TEST("fsmbi 0xFFFF -> all 0xFF");
    EXPECT_EQ(spu_fsmbi(0xFFFF), spu_splat_u8(0xFF));

    TEST("fsmbi 0x8000 -> byte 0 = 0xFF (MSB bit -> first byte)");
    {
        uint8_t exp[16] = {0xFF,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};
        EXPECT_EQ(spu_fsmbi(0x8000), from_bytes(exp));
    }
    TEST("fsmbi 0x0001 -> byte 15 = 0xFF (LSB bit -> last byte)");
    {
        uint8_t exp[16] = {0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0xFF};
        EXPECT_EQ(spu_fsmbi(0x0001), from_bytes(exp));
    }

    TEST("fsmb dynamic path matches exact immediate masks");
    {
        u128 input = spu_zero();
        input._u32[0] = 0xA5C3u;
        EXPECT_EQ(spu_fsmb(input), spu_fsmbi(0xA5C3));
        input._u32[0] = 0;
        EXPECT_EQ(spu_fsmb(input), spu_zero());
        input._u32[0] = 0xFFFFu;
        EXPECT_EQ(spu_fsmb(input), spu_splat_u8(0xFF));
    }


    TEST("fsmb dynamic path matches all 65536 exact masks");
    for (unsigned mask = 0; mask <= 0xFFFFu; ++mask) {
        u128 input = spu_zero();
        input._u32[0] = mask;
        EXPECT_EQ(spu_fsmb(input), spu_fsmbi((int32_t)mask));
    }
}

/* ===========================================================================
 * Gather halfword low bits
 * ===========================================================================*/
static void test_gbh(void) {
    TEST("gbh gathers all 256 halfword-bit combinations in SPU order");
    for (unsigned mask = 0; mask <= 0xFFu; ++mask) {
        u128 input = spu_splat_u16(0xA500u);
        for (int halfword = 0; halfword < 8; ++halfword) {
            input._u16[halfword ^ 1] |=
                (uint16_t)((mask >> (7 - halfword)) & 1u);
        }
        EXPECT_EQ(spu_gbh(input), spu_pref_u32(mask));
    }
}

/* ===========================================================================
 * Sign extension
 * ===========================================================================*/
static void test_sign_extend(void) {
    TEST("xsbh: sign-extend low byte of each halfword");
    /* halfword 0xFF80 -> low byte = 0x80 = -128 -> sign-extend to 0xFF80 */
    u128 in_xsbh = spu_ilh(0xFF80);
    EXPECT_EQ(spu_xsbh(in_xsbh), spu_ilh(0xFF80));

    TEST("xsbh: positive small low byte stays the same");
    EXPECT_EQ(spu_xsbh(spu_ilh(0x007F)), spu_ilh(0x007F));

    TEST("xshw: sign-extend low halfword of each word");
    EXPECT_EQ(spu_xshw(spu_il(0x0000FFFE)), spu_il((int32_t)0xFFFFFFFE));

    /* Words are in SPU order here, unlike native _u64: the low words of
     * architectural doublewords are word 1 and word 3. Distinct high words
     * catch accidental use of RPCS3's reversed host-word numbering. */
    TEST("xswd: architectural low words, distinct signs and poisoned high words");
    const uint8_t xswd_input[16] = {
        0x12,0x34,0x56,0x78, 0xFF,0xFF,0xFF,0xFE,
        0x87,0x65,0x43,0x21, 0x7F,0xFF,0xFF,0xFF};
    const uint8_t xswd_expected[16] = {
        0xFF,0xFF,0xFF,0xFF, 0xFF,0xFF,0xFF,0xFE,
        0x00,0x00,0x00,0x00, 0x7F,0xFF,0xFF,0xFF};
    EXPECT_EQ(spu_xswd(from_bytes(xswd_input)), from_bytes(xswd_expected));

    TEST("xswd: zero and minimum signed word preserve architectural byte order");
    const uint8_t xswd_edges[16] = {
        0xFF,0xFF,0xFF,0xFF, 0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00, 0x80,0x00,0x00,0x00};
    const uint8_t xswd_edges_expected[16] = {
        0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
        0xFF,0xFF,0xFF,0xFF, 0x80,0x00,0x00,0x00};
    EXPECT_EQ(spu_xswd(from_bytes(xswd_edges)), from_bytes(xswd_edges_expected));

    TEST("SGX mixer: original sample-counter sequence advances 128 samples");
    /* socjob2 LS 0x6C8..0x6E8: ROTQMBYI, XSWD, CG, SHUFB, ADDX,
     * SHUFB. The former helper read word 0 (zero after the byte shift),
     * leaving the movie stream's consumption counter permanently zero. */
    const uint8_t sgx_carry_control[16] = {
        4,5,6,7, 0x80,0x80,0x80,0x80,
        12,13,14,15, 0x80,0x80,0x80,0x80};
    const uint8_t sgx_counter_expected[16] = {
        0,0,0,0, 0,0,0,128, 0,0,0,0, 0,0,0,0};
    u128 sgx_count = spu_zero();
    u128 sgx_delta = spu_xswd(spu_rotqmbyi(spu_link(128), -4));
    u128 sgx_carry = spu_cg(sgx_count, sgx_delta);
    sgx_carry = spu_shufb(sgx_carry, sgx_carry, from_bytes(sgx_carry_control));
    u128 sgx_sum = spu_addx(sgx_count, sgx_delta, sgx_carry);
    sgx_count = spu_shufb(sgx_sum, sgx_count, spu_cdd(spu_zero(), 0));
    EXPECT_EQ(sgx_count, from_bytes(sgx_counter_expected));
}

/* ===========================================================================
 * Floating-point fused multiply / negative variants
 * ===========================================================================*/
static void float_splat(u128* r, float v) {
    for (int i = 0; i < 4; i++) r->_f32[i] = v;
}
static u128 f32x4(float a, float b, float c, float d) {
    u128 r; r._f32[0]=a; r._f32[1]=b; r._f32[2]=c; r._f32[3]=d; return r;
}
static u128 s32x4(int32_t a, int32_t b, int32_t c, int32_t d) {
    u128 r; r._s32[0]=a; r._s32[1]=b; r._s32[2]=c; r._s32[3]=d; return r;
}
static u128 u32x4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    u128 r; r._u32[0]=a; r._u32[1]=b; r._u32[2]=c; r._u32[3]=d; return r;
}
static int u128_f32_eq_all(u128 a, float v) {
    for (int i = 0; i < 4; i++) if (a._f32[i] != v) return 0;
    return 1;
}
static void test_float_int_conversions(void) {
    TEST("cflts neutral i8=173 truncates toward zero");
    EXPECT_EQ(spu_cflts(f32x4(1.75f, -1.75f, 0.0f, 42.0f), 173),
              s32x4(1, -1, 0, 42));

    TEST("cflts i8=172 scales by 2 and saturates signed output");
    EXPECT_EQ(spu_cflts(f32x4(1.875f, -1.875f, 1073741824.0f, -1073741824.0f), 172),
              s32x4(3, -3, INT32_MAX, INT32_MIN));

    TEST("cflts i8=0 float-scale overflow saturates smallest positive subnormal");
    EXPECT_EQ(spu_cflts(u32x4(1, 1, 1, 1), 0),
              s32x4(INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX));

    TEST("cflts maps NaN and negative infinity to minimum, positive infinity to maximum");
    EXPECT_EQ(spu_cflts(u32x4(0x7fc00000u, 0x7f800000u, 0xff800000u, 0), 173),
              s32x4(INT32_MIN, INT32_MAX, INT32_MIN, 0));

    TEST("cflts preserves the last in-range integer and saturates both exact bounds");
    EXPECT_EQ(spu_cflts(f32x4(2147483520.0f, 2147483648.0f,
                              -2147483648.0f, -2147483904.0f), 173),
              s32x4(2147483520, INT32_MAX, INT32_MIN, INT32_MIN));

    TEST("cfltu neutral i8=173 truncates and clamps negatives");
    EXPECT_EQ(spu_cfltu(f32x4(-1.5f, 1.75f, 42.0f, 0.0f), 173),
              u32x4(0, 1, 42, 0));

    TEST("cfltu i8=172 scales by 2 and saturates unsigned output");
    EXPECT_EQ(spu_cfltu(f32x4(-1.5f, 1.875f, 2147483648.0f, 1.0f), 172),
              u32x4(0, 3, UINT32_MAX, 2));

    TEST("cfltu i8=0 float-scale overflow saturates smallest positive subnormal");
    EXPECT_EQ(spu_cfltu(u32x4(1, 1, 1, 1), 0),
              u32x4(UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX));

    TEST("cfltu maps NaN and negative infinity to zero, positive infinity to maximum");
    EXPECT_EQ(spu_cfltu(u32x4(0x7fc00000u, 0x7f800000u, 0xff800000u, 0), 173),
              u32x4(0, UINT32_MAX, 0, 0));

    TEST("csflt neutral i8=155 preserves signed integer magnitude");
    EXPECT_EQ(spu_csflt(s32x4(1, -2, 0, 123456), 155),
              f32x4(1.0f, -2.0f, 0.0f, 123456.0f));

    TEST("csflt i8=156 scales signed output by 2");
    EXPECT_EQ(spu_csflt(s32x4(1, -2, 1073741824, -1073741824), 156),
              f32x4(2.0f, -4.0f, 2147483648.0f, -2147483648.0f));

    TEST("csflt i8=0 float scale underflows to zero");
    EXPECT_EQ(spu_csflt(s32x4(1, 2, 3, 4), 0), f32x4(0.0f, 0.0f, 0.0f, 0.0f));

    TEST("cuflt neutral i8=155 preserves unsigned integer magnitude");
    EXPECT_EQ(spu_cuflt(u32x4(1, 2, 0, 16777216), 155),
              f32x4(1.0f, 2.0f, 0.0f, 16777216.0f));

    TEST("cuflt i8=156 scales unsigned output by 2");
    EXPECT_EQ(spu_cuflt(u32x4(1, 2, 1073741824, 0), 156),
              f32x4(2.0f, 4.0f, 2147483648.0f, 0.0f));

    TEST("cuflt i8=0 float scale underflows to zero");
    EXPECT_EQ(spu_cuflt(u32x4(1, 2, 3, 4), 0), f32x4(0.0f, 0.0f, 0.0f, 0.0f));
}
static void test_float_fused(void) {
    u128 a, b, c;
    float_splat(&a, 2.0f);
    float_splat(&b, 3.0f);
    float_splat(&c, 1.0f);

    TEST("fma: a*b + c = 7.0");
    if (u128_f32_eq_all(spu_fma(a, b, c), 7.0f)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("fms: a*b - c = 5.0");
    if (u128_f32_eq_all(spu_fms(a, b, c), 5.0f)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("fnms: c - a*b = -5.0  (note: NEG-multiply-SUB convention)");
    if (u128_f32_eq_all(spu_fnms(a, b, c), -5.0f)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }
}

/* Extended-finite arithmetic used as a culling mask, plus ordinary lanes. */
static void test_extended_float_masks(void) {
    u128 a = {._u32 = {0, 0xffffffffu, 0, 0x40000000u}};
    u128 b = {._u32 = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0x40400000u}};
    u128 c = {._u32 = {0, 0, 0xffffffffu, 0x3f800000u}};
    u128 expected = {._u32 = {0, 0x7fffffffu, 0, 0x40c00000u}};
    TEST("extended fm: zero, saturation and ordinary lane");
    EXPECT_EQ(spu_fm(a, b), expected);
    expected._u32[2] = 0xffffffffu; expected._u32[3] = 0x40e00000u;
    TEST("extended fma: zero, saturation and ordinary lane");
    EXPECT_EQ(spu_fma(a, b, c), expected);
    expected._u32[2] = 0x7fffffffu; expected._u32[3] = 0x40a00000u;
    TEST("extended fms: subtract extended addend");
    EXPECT_EQ(spu_fms(a, b, c), expected);
    expected._u32[1] = 0xffffffffu; expected._u32[2] = 0xffffffffu;
    expected._u32[3] = 0xc0a00000u;
    TEST("extended fnms: mixed culling flags preserve zero lane");
    EXPECT_EQ(spu_fnms(a, b, c), expected);
    TEST("extended fnms then gb: only intended culling bits survive");
    expected = spu_zero(); expected._u32[0] = 6;
    EXPECT_EQ(spu_gb(spu_fnms(a, b, c)), expected);
    for (unsigned bits = 0; bits < 8; bits++) {
        a = spu_splat_u32(bits & 1 ? 0xffffffffu : 0);
        b = spu_splat_u32(bits & 2 ? 0xffffffffu : 0);
        c = spu_splat_u32(bits & 4 ? 0xffffffffu : 0);
        expected = spu_splat_u32((bits & 3) == 3 || (bits & 4) ? 0xffffffffu : 0);
        TEST("extended fnms: all eight Boolean-mask input combinations");
        EXPECT_EQ(spu_fnms(a, b, c), expected);
    }
}

/* ===========================================================================
 * SPURS task signal: one guest SIGNALLED bit, never a counted token queue
 * ===========================================================================*/
static void test_task_signal_pending(void) {
    uint16_t pending = 0;

    TEST("task signal first delivery sets pending bit");
    pending = spu_task_signal_merge_pending(pending);
    if (pending == 1) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s pending=%u\n", g_current, pending); }

    TEST("task signal duplicate delivery coalesces while pending");
    for (int i = 0; i < 65536; ++i)
        pending = spu_task_signal_merge_pending(pending);
    if (pending == 1) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s pending=%u\n", g_current, pending); }

    TEST("WAIT_SIGNAL consumes the pending bit exactly once");
    pending = spu_task_signal_consume_pending(pending);
    if (pending == 0) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s pending=%u\n", g_current, pending); }

    TEST("new delivery after consume is independently observable");
    pending = spu_task_signal_merge_pending(pending);
    pending = spu_task_signal_consume_pending(pending);
    if (pending == 0) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s pending=%u\n", g_current, pending); }

    TEST("no observed SPU edge adds no pending delivery");
    pending = spu_task_signal_merge_observed_edge(0, 0);
    if (pending == 0) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s pending=%u\n", g_current, pending); }

    TEST("current SPU guest edge becomes one pending delivery");
    pending = spu_task_signal_merge_observed_edge(0, 1);
    if (pending == 1) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s pending=%u\n", g_current, pending); }

    TEST("mixed PPU and SPU overlap still coalesces to one bit");
    pending = spu_task_signal_merge_observed_edge(pending, 1);
    if (pending == 1) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s pending=%u\n", g_current, pending); }

    TEST("guest signal bit remains authoritative without host shadow");
    if (spu_task_signal_is_pending(0, 1)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("generation retry requires parked expected worker with unchanged epochs");
    if (spu_task_signal_generation_retry_candidate(
            0, 0, 1, 15, 15, 41, 41, 19, 19)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("generation retry rejects pending bridge or guest signal");
    if (!spu_task_signal_generation_retry_candidate(
            1, 0, 1, 15, 15, 41, 41, 19, 19) &&
        !spu_task_signal_generation_retry_candidate(
            0, 1, 1, 15, 15, 41, 41, 19, 19)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("generation retry rejects wrong worker or changed delivery-consume epoch");
    if (!spu_task_signal_generation_retry_candidate(
            0, 0, 0, 15, 15, 41, 41, 19, 19) &&
        !spu_task_signal_generation_retry_candidate(
            0, 0, 1, 14, 15, 41, 41, 19, 19) &&
        !spu_task_signal_generation_retry_candidate(
            0, 0, 1, 15, 15, 42, 41, 19, 19) &&
        !spu_task_signal_generation_retry_candidate(
            0, 0, 1, 15, 15, 41, 41, 20, 19)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("generation retry claim is one-shot for current C904 generation");
    if (spu_task_signal_generation_can_claim(7, 7, 0) &&
        !spu_task_signal_generation_can_claim(7, 7, 7) &&
        !spu_task_signal_generation_can_claim(7, 6, 0) &&
        !spu_task_signal_generation_can_claim(0, 0, 0)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("new C904 generation rearms one retry claim");
    if (spu_task_signal_generation_can_claim(8, 8, 7) &&
        !spu_task_signal_generation_can_claim(8, 8, 8)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("wait-entry recovery waits until every exact waiter is parked");
    if (!spu_task_signal_generation_retry_batch_ready(
            7, 7, 0, 1, 0, 1) &&
        spu_task_signal_generation_retry_batch_ready(
            7, 7, 0, 1, 1, 1)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("wait-entry recovery claims only the exact generation once");
    if (!spu_task_signal_generation_retry_batch_ready(
            8, 7, 0, 1, 1, 1) &&
        !spu_task_signal_generation_retry_batch_ready(
            7, 7, 7, 1, 1, 1) &&
        spu_task_signal_generation_retry_batch_ready(
            7, 7, 0, 1, 1, 1)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("wait-entry recovery blocks stale epoch, pending, guest, completion");
    if (!spu_task_signal_generation_retry_candidate(
            0, 0, 1, 15, 15, 42, 41, 19, 19) &&
        !spu_task_signal_generation_retry_candidate(
            1, 0, 1, 15, 15, 41, 41, 19, 19) &&
        !spu_task_signal_generation_retry_candidate(
            0, 1, 1, 15, 15, 41, 41, 19, 19) &&
        !spu_task_signal_generation_retry_batch_ready(
            7, 7, 0, 1, 1, 0)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("follow-up ignores the arm's primary CA9C debt policy");
    if (spu_task_signal_generation_retry_policy_matches(1, 0, 1) &&
        spu_task_signal_generation_retry_policy_matches(1, 1, 0) &&
        !spu_task_signal_generation_retry_policy_matches(1, 0, 0)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("follow-up retry requires exactly one delivered-consumed first pass");
    if (spu_task_signal_generation_followup_retry_candidate(
            0, 0, 1, 15, 15, 11, 10, 21, 20) &&
        !spu_task_signal_generation_followup_retry_candidate(
            1, 0, 1, 15, 15, 11, 10, 21, 20) &&
        !spu_task_signal_generation_followup_retry_candidate(
            0, 1, 1, 15, 15, 11, 10, 21, 20) &&
        !spu_task_signal_generation_followup_retry_candidate(
            0, 0, 1, 14, 15, 11, 10, 21, 20) &&
        !spu_task_signal_generation_followup_retry_candidate(
            0, 0, 1, 15, 15, 12, 10, 21, 20) &&
        !spu_task_signal_generation_followup_retry_candidate(
            0, 0, 1, 15, 15, 11, 10, 20, 20)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("follow-up epoch state keeps partial primary passes retryable");
    if (spu_task_signal_generation_followup_epoch_state(10, 10, 20, 20) == 0 &&
        spu_task_signal_generation_followup_epoch_state(11, 10, 20, 20) == 0 &&
        spu_task_signal_generation_followup_epoch_state(11, 10, 21, 20) == 1 &&
        spu_task_signal_generation_followup_epoch_state(12, 10, 21, 20) == -1 &&
        spu_task_signal_generation_followup_epoch_state(11, 10, 22, 20) == -1)
        g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("wait-owned follow-up retry claim is one-shot for current generation");
    if (spu_task_signal_generation_followup_can_claim(7, 7, 0) &&
        !spu_task_signal_generation_followup_can_claim(7, 7, 7) &&
        !spu_task_signal_generation_followup_can_claim(8, 7, 0) &&
        !spu_task_signal_generation_followup_can_claim(0, 0, 0)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("exact CA9C substitution debt survives rollover and consumes FIFO");
    uint64_t debt = spu_task_signal_add_ca9c_substitution_debt(0);
    debt = spu_task_signal_add_ca9c_substitution_debt(debt);
    debt = spu_task_signal_consume_ca9c_substitution_debt(debt);
    uint64_t after_first_ca9c = debt;
    debt = spu_task_signal_consume_ca9c_substitution_debt(debt);
    if (after_first_ca9c == 1 && debt == 0 &&
        spu_task_signal_consume_ca9c_substitution_debt(0) == 0 &&
        spu_task_signal_add_ca9c_substitution_debt(UINT64_MAX) == UINT64_MAX)
        g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("CA9C atomically chooses debt consume or normal delivery");
    if (spu_task_signal_ca9c_action(1) == SPU_CA9C_ACTION_CONSUME_DEBT &&
        spu_task_signal_ca9c_action(UINT64_MAX) == SPU_CA9C_ACTION_CONSUME_DEBT &&
        spu_task_signal_ca9c_action(0) == SPU_CA9C_ACTION_DELIVER_SIGNAL) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("CA9C normal delivery edge blocks synthetic recovery");
    if (spu_task_signal_ca9c_action(0) == SPU_CA9C_ACTION_DELIVER_SIGNAL &&
        !spu_task_signal_generation_retry_candidate(
            spu_task_signal_merge_pending(0), 0, 1, 15, 15, 41, 41, 19, 19) &&
        !spu_task_signal_generation_retry_candidate(
            0, 1, 1, 15, 15, 41, 41, 19, 19)) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }

    TEST("guest signal without CA9C debt remains a new level edge");
    pending = spu_task_signal_merge_pending(0);
    pending = spu_task_signal_merge_pending(pending);
    uint16_t before_consume = pending;
    pending = spu_task_signal_consume_pending(pending);
    pending = spu_task_signal_merge_pending(pending);
    if (before_consume == 1 && pending == 1) g_pass++;
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", g_current); }
}

/* ===========================================================================
 * main: run all and report
 * ===========================================================================*/
int main(void) {
    test_immediate_loaders();
    test_simd_arith();
    test_multiply();
    test_compares();
    test_shift_rotate_boundaries();
    test_shufb();
    test_selb();
    test_constant_generators();
    test_fsmbi();
    test_gbh();
    test_sign_extend();
    test_float_int_conversions();
    test_float_fused();
    test_extended_float_masks();
    test_task_signal_pending();

    printf("\nSPU helper tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
