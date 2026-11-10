/*
 * ps3recomp - SPU instruction semantics helpers
 *
 * Pure-C, header-only implementation of the per-instruction semantics used
 * by lifter-generated code. Extracted from spu_lifter.py so the helpers
 * have one source of truth and can be unit-tested directly (see
 * runtime/spu/tests/test_spu_helpers.c).
 *
 * Each helper is `static inline u128 spu_<mnemonic>(...)`. Naming, lane
 * widths and big-endian conventions match runtime/spu/spu_context.h.
 */

#ifndef SPU_HELPERS_H
#define SPU_HELPERS_H

#include "spu_context.h"
#include <string.h>
#include <math.h>
#if defined(__SSSE3__)
#include <tmmintrin.h>
#define SPU_SHUFB_SSSE3 1
#endif
#if defined(__clang__)
#define SPU_SSSE3_TARGET __attribute__((target("ssse3")))
#else
#define SPU_SSSE3_TARGET
#endif
/* Hot float helpers: inline the common path, keep the rare
 * extended-range path out of line so callers stay small. */
#if defined(_MSC_VER)
#define SPU_FORCEINLINE __forceinline
#define SPU_NOINLINE __declspec(noinline)
#else
#define SPU_FORCEINLINE inline __attribute__((always_inline))
#define SPU_NOINLINE __attribute__((noinline))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#include <intrin.h>
static inline int spu_clz32(uint32_t x) {
    unsigned long idx;
    if (!x) return 32;
    _BitScanReverse(&idx, x);
    return 31 - (int)idx;
}
#else
static inline int spu_clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
#endif

/* Architectural SPU barrier -- the lifted `sync`/`dsync`/`syncc` emission.
 * SPU ISA v1.2 §13 (+ the per-instruction pages): order all earlier LS and
 * channel accesses before later ones; LS must be consistent "if it were
 * observed by another entity" (Table 13-6 is exactly this runtime's
 * host-thread producer -> SPU consumer handoff). A no-op is neither a
 * compiler nor a CPU barrier -- MSVC at /O2 may reorder plain LS accesses
 * across an empty statement (the PPU twin of this bug is LESSONS #11d). A
 * full fence: the locked exchange forces total order and is opaque to the
 * optimizer. */
#ifdef _MSC_VER
static inline void spu_arch_fence(void) { long spu_fence_tmp = 0; _InterlockedExchange(&spu_fence_tmp, 1); }
#else
static inline void spu_arch_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif

/* SPU byte position -> our _u8 index.
 * Our u128 is HOST-NATIVE little-endian (_u32[i]=SPU word i as a value via
 * spu_ls_read128's big-endian load), so within each 4-byte word the _u8[]
 * bytes are reversed vs SPU big-endian byte order. Any op defined in terms of
 * SPU *byte positions* (quadword byte rotates/shifts, gen-controls, shuffle
 * insertion) must map SPU byte P to our index W(P). Word-aligned operations
 * are unaffected (W is identity modulo the within-word reversal). */
#define SPU_W(P) (((P) & ~3) | (3 - ((P) & 3)))

/* ---- constructors ---- */
static inline u128 spu_splat_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=v; r._u32[2]=v; r._u32[3]=v; return r;
}
/* Link register value for brsl/bisl/bisled: the SPU writes the return address
 * into the PREFERRED word (word 0) and ZEROS the other three slots (matches
 * RPCS3 v128::from32r, SPUInterpreter.cpp BRSL). Splatting it (the old bug)
 * corrupts any code that saves/reloads/operates on the full 128-bit link. */
static inline u128 spu_link(uint32_t addr) {
    u128 r; r._u32[0]=addr; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
/* Preferred-slot scalar: value in word 0, remaining slots ZERO. CBEA's scalar
 * channel convention -- rchcnt returns the count this way (RPCS3 measured
 * {1,0,0,0} where the old splat gave {1,1,1,1}). Same bug class as spu_link. */
static inline u128 spu_pref_u32(uint32_t v) {
    u128 r; r._u32[0]=v; r._u32[1]=0; r._u32[2]=0; r._u32[3]=0; return r;
}
static inline u128 spu_splat_u16(uint16_t v) {
    u128 r; for (int i=0;i<8;i++) r._u16[i]=v; return r;
}
static inline u128 spu_splat_u8(uint8_t v) {
    u128 r; for (int i=0;i<16;i++) r._u8[i]=v; return r;
}
static inline u128 spu_zero(void) { u128 r; memset(&r,0,sizeof r); return r; }

/* ---- integer arithmetic (SIMD) ---- */
static inline u128 spu_a(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]; return r; }
static inline u128 spu_sf(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]-a._u32[i]; return r; }
static inline u128 spu_ah(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+b._u16[i]; return r; }
static inline u128 spu_sfh(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=b._u16[i]-a._u16[i]; return r; }
static inline u128 spu_ai(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+(uint32_t)imm; return r; }
static inline u128 spu_ahi(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]+(uint16_t)imm; return r; }
static inline u128 spu_sfi(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)imm-a._u32[i]; return r; }
static inline u128 spu_sfhi(u128 a, int32_t imm){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)imm-a._u16[i]; return r; }

/* ---- multiply (low halfword of each word × ... -> 32-bit, per SPU mpy) ----
 * Sub-lane indexing assumes a little-endian host (matches the recompiler's
 * target). The "low halfword of word i" in SPU BE semantics is _s16[2i] on
 * an LE host (NOT _s16[2i+1], which would be correct on a BE host). */
static inline u128 spu_mpy(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]; return r; }
/* mpya: 16x16 signed multiply of low halves + add rc (per word, RRR form). */
static inline u128 spu_mpya(u128 a, u128 b, u128 c) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int32_t)b._s16[i*2]+c._s32[i]; return r; }
/* sfx: extended subtract rb-ra-1+carry; carry-in = low bit of old rt (RT is 3rd src). */
static inline u128 spu_sfx(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=b._u32[i]+~a._u32[i]+(t._u32[i]&1u); return r; }
/* FI interpolates the base/slope payload returned by either estimate opcode.
 * It is not a Newton reciprocal step: that would destroy FRSQEST results
 * (for example FI(4, 1/sqrt(4)) would become zero). RA supplies only the
 * low 19 fraction bits; RB supplies sign/exponent, base and 10-bit slope.
 * Compute the fractional result exactly and truncate, rather than using host
 * round-to-nearest or an FMA. Published SPU tests distinguish this rounding
 * at FI(0x3fcccccd, 0x3fcccccd). No host floating-point state is changed. */
static inline u128 spu_fi(u128 a, u128 b) {
    u128 r;
    for (int i = 0; i < 4; ++i) {
        const uint64_t base = (uint64_t)(0x800000u | (b._u32[i] & 0x007ffc00u)) << 9;
        const uint32_t correction = (b._u32[i] & 0x3ffu) * (a._u32[i] & 0x7ffffu);
        const uint64_t interpolated = base - correction;
        /* The unsigned intermediate is in [0.875, 2) * 2^32. */
        const unsigned shift = interpolated >= (1ULL << 32) ? 9u : 8u;
        r._u32[i] = (b._u32[i] & 0xff800000u) |
                    ((uint32_t)(interpolated >> shift) & 0x007fffffu);
    }
    return r;
}
static inline u128 spu_mpyu(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[i*2]*(uint32_t)b._u16[i*2]; return r; }
static inline u128 spu_mpyi(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[i*2]*(int16_t)imm; return r; }

/* ---- bitwise logic (whole 128 bits) ---- */
static inline u128 spu_and(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]&b._u64[0]; r._u64[1]=a._u64[1]&b._u64[1]; return r; }
static inline u128 spu_or(u128 a, u128 b)  { u128 r; r._u64[0]=a._u64[0]|b._u64[0]; r._u64[1]=a._u64[1]|b._u64[1]; return r; }
static inline u128 spu_xor(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]^b._u64[0]; r._u64[1]=a._u64[1]^b._u64[1]; return r; }
static inline u128 spu_nand(u128 a, u128 b){ u128 r; r._u64[0]=~(a._u64[0]&b._u64[0]); r._u64[1]=~(a._u64[1]&b._u64[1]); return r; }
static inline u128 spu_nor(u128 a, u128 b) { u128 r; r._u64[0]=~(a._u64[0]|b._u64[0]); r._u64[1]=~(a._u64[1]|b._u64[1]); return r; }
static inline u128 spu_andc(u128 a, u128 b){ u128 r; r._u64[0]=a._u64[0]&~b._u64[0]; r._u64[1]=a._u64[1]&~b._u64[1]; return r; }
static inline u128 spu_orc(u128 a, u128 b) { u128 r; r._u64[0]=a._u64[0]|~b._u64[0]; r._u64[1]=a._u64[1]|~b._u64[1]; return r; }
static inline u128 spu_andi(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]&(uint32_t)imm; return r; }
static inline u128 spu_ori(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r; }
static inline u128 spu_xori(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]^(uint32_t)imm; return r; }

/* ---- count leading zeros / population count per byte ---- */
static inline u128 spu_clz(u128 a)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)spu_clz32(a._u32[i]); return r; }
static inline SPU_SSSE3_TARGET u128 spu_cntb(u128 a) {
#ifdef SPU_SHUFB_SSSE3
    /* Population count is lane-local, so host/SPU byte order is irrelevant.
     * Two nibble-table shuffles replace 16 variable-length scalar loops. */
    const __m128i value = _mm_loadu_si128((const __m128i*)&a);
    const __m128i mask = _mm_set1_epi8(0x0F);
    const __m128i lut = _mm_setr_epi8(
        0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
    const __m128i low = _mm_and_si128(value, mask);
    const __m128i high = _mm_and_si128(_mm_srli_epi16(value, 4), mask);
    u128 r;
    _mm_storeu_si128((__m128i*)&r,
                     _mm_add_epi8(_mm_shuffle_epi8(lut, low),
                                  _mm_shuffle_epi8(lut, high)));
    return r;
#else
    u128 r;
    for(int i=0;i<16;i++){
        uint8_t v=a._u8[i],c=0;
        while(v){c+=v&1;v>>=1;}
        r._u8[i]=c;
    }
    return r;
#endif
}

/* ---- compares: all-ones / all-zeros per lane ---- */
static inline u128 spu_ceq(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]==b._u32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_ceqh(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]==b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_ceqb(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_cgt(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>b._s32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_cgth(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>b._s16[i])?0xFFFFu:0; return r; }
static inline u128 spu_cgtb(u128 a, u128 b) { u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>b._s8[i])?0xFFu:0; return r; }
static inline u128 spu_clgt(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>b._u32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_clgth(u128 a, u128 b){ u128 r; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>b._u16[i])?0xFFFFu:0; return r; }
static inline u128 spu_clgtb(u128 a, u128 b){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>b._u8[i])?0xFFu:0; return r; }
static inline u128 spu_ceqi(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]==imm)?0xFFFFFFFFu:0; return r; }
static inline u128 spu_cgti(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._s32[i]>imm)?0xFFFFFFFFu:0; return r; }
static inline u128 spu_clgti(u128 a, int32_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._u32[i]>(uint32_t)imm)?0xFFFFFFFFu:0; return r; }

/* ---- select / shuffle ---- */
static inline u128 spu_selb(u128 a, u128 b, u128 c) {
    u128 r;
    r._u64[0]=(a._u64[0]&~c._u64[0])|(b._u64[0]&c._u64[0]);
    r._u64[1]=(a._u64[1]&~c._u64[1])|(b._u64[1]&c._u64[1]);
    return r;
}
/* shufb special selectors per Cell BE ISA:
 *   sel & 0xE0 == 0xE0 -> 0x80
 *   sel & 0xC0 == 0xC0 -> 0xFF
 *   sel & 0xC0 == 0x80 -> 0x00
 *   otherwise           -> concat{a,b}[sel & 0x1F] */
static inline SPU_SSSE3_TARGET u128 spu_shufb(u128 a, u128 b, u128 c) {
#ifdef SPU_SHUFB_SSSE3
    /* Raw host output and control lanes already correspond. For source byte
     * selection only, SPU byte position P maps to raw lane P xor 3. */
    const __m128i va = _mm_loadu_si128((const __m128i*)&a);
    const __m128i vb = _mm_loadu_si128((const __m128i*)&b);
    const __m128i vc = _mm_loadu_si128((const __m128i*)&c);
    const __m128i base = _mm_xor_si128(
        _mm_and_si128(vc, _mm_set1_epi8(0x0F)), _mm_set1_epi8(0x03));
    const __m128i choose_b = _mm_slli_epi16(
        _mm_and_si128(vc, _mm_set1_epi8(0x10)), 3);
    const __m128i special_bit = _mm_and_si128(vc, _mm_set1_epi8(-128));
    const __m128i ctrl_a = _mm_or_si128(
        base, _mm_or_si128(special_bit, choose_b));
    const __m128i ctrl_b = _mm_or_si128(base, _mm_or_si128(
        special_bit, _mm_xor_si128(choose_b, _mm_set1_epi8(-128))));
    const __m128i normal = _mm_or_si128(
        _mm_shuffle_epi8(va, ctrl_a), _mm_shuffle_epi8(vb, ctrl_b));
    const __m128i top = _mm_and_si128(vc, _mm_set1_epi8(-32));
    const __m128i make_80 = _mm_cmpeq_epi8(top, _mm_set1_epi8(-32));
    const __m128i make_ff = _mm_cmpeq_epi8(top, _mm_set1_epi8(-64));
    const __m128i special = _mm_or_si128(
        make_ff, _mm_and_si128(make_80, _mm_set1_epi8(-128)));
    const __m128i out = _mm_or_si128(normal, special);
    u128 r;
    _mm_storeu_si128((__m128i*)&r, out);
    return r;
#else
    /* shufb is defined on SPU byte positions. Our u128 is host-native LE, so SPU
     * byte P lives at _u8[SPU_W(P)]; map every access (the concat source, the
     * control, and the result) through SPU_W so a control supplied as an immediate
     * (ila/il) or an LS-loaded constant -- already in true SPU byte order -- is
     * interpreted correctly. The cbd/chd/cwd/cdd generators below produce true
     * SPU-byte-order selectors to match. */
    uint8_t cat[32];
    for (int j=0;j<16;j++) cat[j]    = a._u8[SPU_W(j)];   /* concat SPU byte j  = a SPU byte j */
    for (int j=0;j<16;j++) cat[16+j] = b._u8[SPU_W(j)];   /* concat SPU byte 16+j = b SPU byte j */
    u128 r;
    for (int t=0;t<16;t++) {                              /* result SPU byte t */
        uint8_t s = c._u8[SPU_W(t)];                      /* control SPU byte t */
        uint8_t v;
        if      ((s & 0xE0)==0xE0) v=0x80;
        else if ((s & 0xC0)==0xC0) v=0xFF;
        else if ((s & 0xC0)==0x80) v=0x00;
        else                       v=cat[s & 0x1F];       /* concat SPU byte (s & 0x1F) */
        r._u8[SPU_W(t)] = v;
    }
    return r;
#endif
}

/* ---- shift / rotate immediate (word lanes) ---- */
static inline u128 spu_shli(u128 a, int sh)  { u128 r; sh&=0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); return r; }
static inline u128 spu_shlhi(u128 a, int sh) { u128 r; sh&=0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); return r; }
static inline u128 spu_roti(u128 a, int sh)  { u128 r; sh&=31; for(int i=0;i<4;i++) r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; return r; }
static inline u128 spu_rothi(u128 a, int sh) { u128 r; sh&=15; for(int i=0;i<8;i++) r._u16[i]=(uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))); return r; }
static inline u128 spu_rotmi(u128 a, int i7)  { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); return r; }
static inline u128 spu_rotmai(u128 a, int i7) { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); return r; }
static inline u128 spu_rotmhi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
/* Quadword byte moves for the SSSE3 path. Host lane J holds SPU byte J^3
 * (SPU_W), so "result SPU byte t <- source SPU byte t+d" is one pshufb whose
 * lane-J index is ((J^3)+d)^3. Out-of-range sources get bit 7 set, which
 * pshufb turns into a zero lane. With a constant count (the immediate forms)
 * the whole control folds to a constant. */
#ifdef SPU_SHUFB_SSSE3
static inline SPU_SSSE3_TARGET __m128i spu_qby_spu_pos(void) {
    return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
}
static inline SPU_SSSE3_TARGET u128 spu_qby_apply(u128 a, __m128i ctl) {
    u128 r;
    _mm_storeu_si128((__m128i*)&r,
                     _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)&a), ctl));
    return r;
}
/* result SPU byte t <- a SPU byte t+sh, zero past byte 15; sh in 0..31. */
static inline SPU_SSSE3_TARGET u128 spu_qby_shl(u128 a, int sh) {
    const __m128i src = _mm_add_epi8(spu_qby_spu_pos(), _mm_set1_epi8((char)sh));
    const __m128i ctl = _mm_or_si128(_mm_xor_si128(src, _mm_set1_epi8(3)),
                                     _mm_cmpgt_epi8(src, _mm_set1_epi8(15)));
    return spu_qby_apply(a, ctl);
}
/* result SPU byte t <- a SPU byte t-sh, zero below byte 0; sh in 0..31
 * (t-sh >= -31 is negative as int8, so bit 7 is already set). */
static inline SPU_SSSE3_TARGET u128 spu_qby_shr(u128 a, int sh) {
    const __m128i src = _mm_sub_epi8(spu_qby_spu_pos(), _mm_set1_epi8((char)sh));
    return spu_qby_apply(a, _mm_xor_si128(src, _mm_set1_epi8(3)));
}
/* result SPU byte t <- a SPU byte (t+sh)&15; sh in 0..15. t+sh <= 30 keeps
 * bit 7 clear, and pshufb reads only index bits 0-3, so no &15 is needed. */
static inline SPU_SSSE3_TARGET u128 spu_qby_rot(u128 a, int sh) {
    const __m128i src = _mm_add_epi8(spu_qby_spu_pos(), _mm_set1_epi8((char)sh));
    return spu_qby_apply(a, _mm_xor_si128(src, _mm_set1_epi8(3)));
}
#endif
static inline u128 spu_shlqbyi(u128 a, int sh) {
#ifdef SPU_SHUFB_SSSE3
    return spu_qby_shl(a, sh & 0x1F);
#else
    u128 r=spu_zero(); sh&=0x1F; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r;
#endif
}
static inline u128 spu_rotqbyi(u128 a, int sh) { u128 r; sh&=0x0F; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r; }
/* Bit-level quadword shifts/rotates operate on the WHOLE 128-bit big-endian
 * value. Our _u64[0]/_u64[1] are word-SCRAMBLED (host-LE: _u64[0] = word0 |
 * word1<<32, but SPU's MS half is word0<<32 | word1), so native shifts on them
 * corrupt any bits crossing a 32-bit boundary. Assemble the logical hi/lo halves
 * with _u32[0] as most-significant, shift, then disassemble. */
static inline u128 spu_shlqbii(u128 a, int sh) { sh&=7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbii(u128 a, int sh) { sh&=7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }

/* ---- single-precision float (4 lanes) ---- */
static inline u128 spu_fa(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._f32[i]=a._f32[i]+b._f32[i]; return r; }
static inline u128 spu_fs(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._f32[i]=a._f32[i]-b._f32[i]; return r; }

/* Float<->int conversions with scale (RI8): CFLTS/CFLTU use 2^(173-i8);
 * CSFLT/CUFLT use 2^(i8-155). The scale table and product are float, so its
 * overflow/underflow behavior is architectural before the integer clamps. */
static inline SPU_SSSE3_TARGET u128 spu_cflts(u128 a, int i8){
    const float f=exp2f((float)(173-i8));
#ifdef SPU_SHUFB_SSSE3
    /* cvttps already yields INT_MIN for NaN, negative overflow, and the exact
     * lower bound. Only positive overflow needs replacing with INT_MAX. */
    const __m128 v=_mm_mul_ps(_mm_loadu_ps(a._f32), _mm_set1_ps(f));
    const __m128 upper=_mm_cmpge_ps(v, _mm_set1_ps(2147483648.0f));
    const __m128i upper_mask=_mm_castps_si128(upper);
    const __m128i converted=_mm_cvttps_epi32(v);
    const __m128i saturated=_mm_or_si128(
        _mm_and_si128(upper_mask, _mm_set1_epi32(INT32_MAX)),
        _mm_andnot_si128(upper_mask, converted));
    u128 r;
    _mm_storeu_si128((__m128i*)&r, saturated);
    return r;
#else
    u128 r;
    for(int i=0;i<4;i++){
        float v=a._f32[i]*f;
        if(isnan(v)||v<=-2147483648.0f) r._s32[i]=INT32_MIN;
        else if(v>=2147483648.0f) r._s32[i]=INT32_MAX;
        else r._s32[i]=(int32_t)v;
    }
    return r;
#endif
}
static inline u128 spu_cfltu(u128 a, int i8){ u128 r; float f=exp2f((float)(173-i8));
    for(int i=0;i<4;i++){ float v=a._f32[i]*f; if(isnan(v)||v<0.0f) r._u32[i]=0; else if(v>=4294967296.0f) r._u32[i]=UINT32_MAX; else r._u32[i]=(uint32_t)v; } return r; }
static inline u128 spu_csflt(u128 a, int i8){ u128 r; float f=exp2f((float)(i8-155));
    for(int i=0;i<4;i++) r._f32[i]=(float)a._s32[i]*f; return r; }
static inline u128 spu_cuflt(u128 a, int i8){ u128 r; float f=exp2f((float)(i8-155));
    for(int i=0;i<4;i++) r._f32[i]=(float)a._u32[i]*f; return r; }
/* SPU single precision has an extra finite exponent, not IEEE Inf/NaN.
 * In particular, 0 * 0xffffffff is zero, not a NaN whose low bit rejects
 * every triangle in image-17's FNMS/GB culling sequence at 0x4f60.
 * Keep ordinary lanes on the existing fast path. Extended multiply/add
 * uses original integer arithmetic: exact 48-bit product, guard/sticky
 * alignment, truncation toward zero, extended saturation, denormal flush.
 * It neither changes host rounding mode nor depends on NaN payloads.
 * Architectural cross-check: RPCS3 SPUInterpreter.cpp precise FMA family.
 * This does not claim to repair ordinary-lane rounding or FPSCR tracking. */
static inline int spu__sf_extended(uint32_t v) {
    return (v & 0x7f800000u) == 0x7f800000u;
}
static inline int spu__sf_any_extended(u128 v) {
    return spu__sf_extended(v._u32[0]) | spu__sf_extended(v._u32[1]) |
           spu__sf_extended(v._u32[2]) | spu__sf_extended(v._u32[3]);
}
/* Both helpers below receive nonzero significands. */
static inline int spu__sf_top64(uint64_t v) {
    uint32_t hi = (uint32_t)(v >> 32);
    return hi ? 63 - spu_clz32(hi) : 31 - spu_clz32((uint32_t)v);
}
static inline uint64_t spu__sf_shift_jam(uint64_t v, unsigned shift) {
    if (!shift) return v;
    if (shift >= 64) return v != 0;
    return (v >> shift) | ((v << (64 - shift)) != 0);
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
static uint32_t spu__sf_extended_madd(uint32_t a, uint32_t b, uint32_t c) {
    int ae = (a >> 23) & 255, be = (b >> 23) & 255, ce = (c >> 23) & 255;
    uint32_t am = ae ? (a & 0x7fffffu) | 0x800000u : 0;
    uint32_t bm = be ? (b & 0x7fffffu) | 0x800000u : 0;
    uint32_t cm = ce ? (c & 0x7fffffu) | 0x800000u : 0;
    uint64_t product = (uint64_t)am * bm;
    uint32_t sign = (a ^ b) & 0x80000000u;
    if (!product) return cm ? c : 0;
    int top = spu__sf_top64(product);
    int exponent = ae + be - 300 + top;
    uint64_t magnitude = product << (62 - top);
    if (cm) {
        int c_exponent = ce - 127;
        uint64_t addend = (uint64_t)cm << 39;
        if (exponent < c_exponent) {
            magnitude = spu__sf_shift_jam(magnitude, (unsigned)(c_exponent - exponent));
            exponent = c_exponent;
        } else {
            addend = spu__sf_shift_jam(addend, (unsigned)(exponent - c_exponent));
        }
        if (sign == (c & 0x80000000u)) magnitude += addend;
        else if (magnitude >= addend) magnitude -= addend;
        else { magnitude = addend - magnitude; sign = c & 0x80000000u; }
    }
    if (!magnitude) return 0;
    top = spu__sf_top64(magnitude);
    exponent += top - 62 + 127;
    if (exponent <= 0) return 0;
    if (exponent > 255) return sign | 0x7fffffffu;
    uint64_t mantissa = top >= 23 ? magnitude >> (top - 23) : magnitude << (23 - top);
    return sign | ((uint32_t)exponent << 23) | ((uint32_t)mantissa & 0x7fffffu);
}
static SPU_NOINLINE u128 spu__fm_extended(u128 a, u128 b) {
    u128 r;
    for(int i=0;i<4;i++) {
        if (spu__sf_extended(a._u32[i]) | spu__sf_extended(b._u32[i]))
            r._u32[i]=spu__sf_extended_madd(a._u32[i],b._u32[i],0);
        else r._f32[i]=a._f32[i]*b._f32[i];
    }
    return r;
}
static SPU_FORCEINLINE u128 spu_fm(u128 a, u128 b) {
    if (spu__sf_any_extended(a) | spu__sf_any_extended(b)) return spu__fm_extended(a, b);
    u128 r;
    for(int i=0;i<4;i++) r._f32[i]=a._f32[i]*b._f32[i];
    return r;
}
static SPU_NOINLINE u128 spu__fma_extended(u128 a, u128 b, u128 c) {
    u128 r;
    for(int i=0;i<4;i++) {
        if (spu__sf_extended(a._u32[i]) | spu__sf_extended(b._u32[i]) | spu__sf_extended(c._u32[i]))
            r._u32[i]=spu__sf_extended_madd(a._u32[i],b._u32[i],c._u32[i]);
        else r._f32[i]=a._f32[i]*b._f32[i]+c._f32[i];
    }
    return r;
}
static SPU_FORCEINLINE u128 spu_fma(u128 a, u128 b, u128 c) {
    if (spu__sf_any_extended(a) | spu__sf_any_extended(b) | spu__sf_any_extended(c)) return spu__fma_extended(a, b, c);
    u128 r;
    for(int i=0;i<4;i++) r._f32[i]=a._f32[i]*b._f32[i]+c._f32[i];
    return r;
}
static SPU_NOINLINE u128 spu__fms_extended(u128 a, u128 b, u128 c) {
    u128 r;
    for(int i=0;i<4;i++) {
        if (spu__sf_extended(a._u32[i]) | spu__sf_extended(b._u32[i]) | spu__sf_extended(c._u32[i]))
            r._u32[i]=spu__sf_extended_madd(a._u32[i],b._u32[i],c._u32[i]^0x80000000u);
        else r._f32[i]=a._f32[i]*b._f32[i]-c._f32[i];
    }
    return r;
}
static SPU_FORCEINLINE u128 spu_fms(u128 a, u128 b, u128 c) {
    if (spu__sf_any_extended(a) | spu__sf_any_extended(b) | spu__sf_any_extended(c)) return spu__fms_extended(a, b, c);
    u128 r;
    for(int i=0;i<4;i++) r._f32[i]=a._f32[i]*b._f32[i]-c._f32[i];
    return r;
}
static SPU_NOINLINE u128 spu__fnms_extended(u128 a, u128 b, u128 c) {
    u128 r;
    for(int i=0;i<4;i++) {
        if (spu__sf_extended(a._u32[i]) | spu__sf_extended(b._u32[i]) | spu__sf_extended(c._u32[i]))
            r._u32[i]=spu__sf_extended_madd(a._u32[i],b._u32[i]^0x80000000u,c._u32[i]);
        else r._f32[i]=c._f32[i]-a._f32[i]*b._f32[i];
    }
    return r;
}
static SPU_FORCEINLINE u128 spu_fnms(u128 a, u128 b, u128 c) {
    if (spu__sf_any_extended(a) | spu__sf_any_extended(b) | spu__sf_any_extended(c)) return spu__fnms_extended(a, b, c);
    u128 r;
    for(int i=0;i<4;i++) r._f32[i]=c._f32[i]-a._f32[i]*b._f32[i];
    return r;
}
static inline u128 spu_fceq(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._f32[i]==b._f32[i])?0xFFFFFFFFu:0; return r; }
static inline u128 spu_fcgt(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(a._f32[i]>b._f32[i])?0xFFFFFFFFu:0; return r; }

/* ---- SPU double-precision (2 doubles/reg; dword i = words 2i(high),2i+1(low)).
 * Reassemble via _u32 -- our u128 is host-LE, so a naive _f64[i] would SWAP the
 * two words. Semantics from RPCS3 SPUInterpreter (DFASM / DFMA / FESD / FRDS). ---- */
static inline double spu__dget(u128 r, int i) {
    uint64_t u = ((uint64_t)r._u32[i*2] << 32) | r._u32[i*2+1];
    double d; memcpy(&d, &u, sizeof d); return d;
}
static inline void spu__dset(u128* r, int i, double d) {
    uint64_t u; memcpy(&u, &d, sizeof u);
    r->_u32[i*2] = (uint32_t)(u >> 32); r->_u32[i*2+1] = (uint32_t)u;
}
static inline u128 spu_dfa(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)+spu__dget(b,i)); return r; }
static inline u128 spu_dfs(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)-spu__dget(b,i)); return r; }
static inline u128 spu_dfm(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)*spu__dget(b,i)); return r; }
/* double FMA family: c = rt (accumulator, 3-register). */
static inline u128 spu_dfma(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)*spu__dget(b,i)+spu__dget(t,i)); return r; }
static inline u128 spu_dfms(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(a,i)*spu__dget(b,i)-spu__dget(t,i)); return r; }
static inline u128 spu_dfnms(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, spu__dget(t,i)-spu__dget(a,i)*spu__dget(b,i)); return r; }
static inline u128 spu_dfnma(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<2;i++) spu__dset(&r,i, -(spu__dget(a,i)*spu__dget(b,i)+spu__dget(t,i))); return r; }
/* double compares -> per-lane 64-bit mask (RPCS3 stubs these; sane impl here). */
static inline u128 spu_dfceq(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)==spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmeq(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x==y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* fesd: extend single (word slots 1,3) -> double (dwords 0,1). frds: round
 * double -> single in slots 1,3 and zero slots 0,2 (RPCS3 FESD/FRDS). */
static inline u128 spu_fesd(u128 a) { u128 r; memset(&r,0,sizeof r); for(int i=0;i<2;i++) spu__dset(&r,i,(double)a._f32[i*2+1]); return r; }
static inline u128 spu_frds(u128 a) { u128 r; memset(&r,0,sizeof r); for(int i=0;i<2;i++) r._f32[i*2+1]=(float)spu__dget(a,i); return r; }
/* mpyhhu: high-16 x high-16 of each word, unsigned, full 32-bit product. */
static inline u128 spu_mpyhhu(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* cgx: extended carry-generate, carry-in = low bit of old rt (RPCS3 CGX). */
static inline u128 spu_cgx(u128 a, u128 b, u128 t){ u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)(t._u32[i]&1u)+a._u32[i]+b._u32[i])>>32); return r; }

/* ---- remaining SPU ISA ops (RPCS3 SPUInterpreter: EQV/ABSDB/AVGB/MPYHHA/
 * MPYHHAU/DFCGT/DFCMGT/XORBI/DFTSV). Completes the lifter's opcode coverage. ---- */
static inline u128 spu_eqv(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=~(a._u32[i]^b._u32[i]); return r; }
static inline u128 spu_xorbi(u128 a, int32_t imm){ u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(a._u8[i]^(uint8_t)imm); return r; }
static inline u128 spu_absdb(u128 a, u128 b)  { u128 r; for(int i=0;i<16;i++){ uint8_t x=a._u8[i],y=b._u8[i]; r._u8[i]=(uint8_t)(x>y?x-y:y-x); } return r; }
static inline u128 spu_avgb(u128 a, u128 b)   { u128 r; for(int i=0;i<16;i++) r._u8[i]=(uint8_t)(((uint32_t)a._u8[i]+(uint32_t)b._u8[i]+1u)>>1); return r; }
/* mpyhha/mpyhhau: high-16 x high-16, ACCUMULATE into rt (3-register). */
static inline u128 spu_mpyhha(u128 a, u128 b, u128 t)  { u128 r; for(int i=0;i<4;i++) r._s32[i]=t._s32[i]+(int32_t)a._s16[2*i+1]*(int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpyhhau(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=t._u32[i]+(uint32_t)a._u16[2*i+1]*(uint32_t)b._u16[2*i+1]; return r; }
/* double compare greater (RPCS3 stubs these; sane impl) -> per-lane mask. */
static inline u128 spu_dfcgt(u128 a, u128 b)  { u128 r; for(int i=0;i<2;i++){ uint64_t m=(spu__dget(a,i)>spu__dget(b,i))?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
static inline u128 spu_dfcmgt(u128 a, u128 b) { u128 r; for(int i=0;i<2;i++){ double x=spu__dget(a,i),y=spu__dget(b,i); if(x<0)x=-x; if(y<0)y=-y; uint64_t m=(x>y)?~0ull:0ull; r._u32[i*2]=(uint32_t)(m>>32); r._u32[i*2+1]=(uint32_t)m; } return r; }
/* dftsv: test each IEEE-754 double lane against the u7 class mask.
 * SPU_SV_{NEG_DENORM,POS_DENORM,NEG_ZERO,POS_ZERO,NEG_INFINITY,
 * POS_INFINITY,NAN} are bits 0..6 respectively.  Classify the raw payload so
 * signalling NaNs and signed zero survive host floating-point handling. */
static inline u128 spu_dftsv(u128 a, int32_t imm) {
    u128 r;
    for (int i = 0; i < 2; i++) {
        uint64_t raw = ((uint64_t)a._u32[i * 2] << 32) |
                       (uint64_t)a._u32[i * 2 + 1];
        uint32_t sign = (uint32_t)(raw >> 63);
        uint32_t exp = (uint32_t)((raw >> 52) & 0x7ffu);
        uint64_t frac = raw & UINT64_C(0x000fffffffffffff);
        uint32_t class_bit;

        if (exp == 0)
            class_bit = frac ? (sign ? 0x01u : 0x02u)
                             : (sign ? 0x04u : 0x08u);
        else if (exp == 0x7ffu)
            class_bit = frac ? 0x40u : (sign ? 0x10u : 0x20u);
        else
            class_bit = 0;

        uint64_t mask = ((uint32_t)imm & class_bit) ? UINT64_MAX : 0;
        r._u32[i * 2] = (uint32_t)(mask >> 32);
        r._u32[i * 2 + 1] = (uint32_t)mask;
    }
    return r;
}

/* ---- Phase 2: register-variable shifts/rotates ---- */
static inline u128 spu_shl(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); } return r; }
static inline u128 spu_shlh(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]<<sh); } return r; }
static inline u128 spu_rot(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=b._u32[i]&31; r._u32[i]= sh ? ((a._u32[i]<<sh)|(a._u32[i]>>(32-sh))) : a._u32[i]; } return r; }
static inline u128 spu_roth(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=b._u16[i]&15; r._u16[i]= sh ? (uint16_t)((a._u16[i]<<sh)|(a._u16[i]>>(16-sh))) : a._u16[i]; } return r; }
static inline u128 spu_shlqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqbi(u128 a, u128 b){ int sh=b._u32[0]&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nhi=(hi<<sh)|(lo>>(64-sh)), nlo=(lo<<sh)|(hi>>(64-sh));
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_shlqby(u128 a, u128 b){
#ifdef SPU_SHUFB_SSSE3
    return spu_qby_shl(a, (int)(b._u32[0]&0x1F));
#else
    int sh=b._u32[0]&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r;
#endif
}
static inline u128 spu_rotqby(u128 a, u128 b){
#ifdef SPU_SHUFB_SSSE3
    return spu_qby_rot(a, (int)(b._u32[0]&0x0F));
#else
    int sh=b._u32[0]&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r;
#endif
}
static inline u128 spu_shlqbybi(u128 a, u128 b){
#ifdef SPU_SHUFB_SSSE3
    return spu_qby_shl(a, (int)((b._u32[0]>>3)&0x1F));
#else
    int sh=(b._u32[0]>>3)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i+sh; if(s<16) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r;
#endif
}
static inline u128 spu_rotqbybi(u128 a, u128 b){
#ifdef SPU_SHUFB_SSSE3
    return spu_qby_rot(a, (int)((b._u32[0]>>3)&0x0F));
#else
    int sh=(b._u32[0]>>3)&0x0F; u128 r; for(int i=0;i<16;i++) r._u8[SPU_W(i)]=a._u8[SPU_W((i+sh)&0x0F)]; return r;
#endif
}

/* ---- Phase 2: rotmahi ---- */
static inline u128 spu_rotmahi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); return r; }

/* ---- Phase 2: byte/half immediate compares ---- */
static inline u128 spu_ceqbi(u128 a, int32_t imm)  { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]==v)?0xFFu:0; return r; }
static inline u128 spu_ceqhi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]==v)?0xFFFFu:0; return r; }
static inline u128 spu_clgtbi(u128 a, int32_t imm) { u128 r; uint8_t v=(uint8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._u8[i]>v)?0xFFu:0; return r; }
static inline u128 spu_clgthi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._u16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgthi(u128 a, int32_t imm)  { u128 r; int16_t v=(int16_t)imm; for(int i=0;i<8;i++) r._u16[i]=(a._s16[i]>v)?0xFFFFu:0; return r; }
static inline u128 spu_cgtbi(u128 a, int32_t imm)  { u128 r; int8_t v=(int8_t)imm; for(int i=0;i<16;i++) r._u8[i]=(a._s8[i]>v)?0xFFu:0; return r; }

/* ---- Phase 2: misc one-offs ---- */
static inline u128 spu_fscrrd(u128 a) { (void)a; return spu_zero(); }
static inline u128 spu_gb(u128 a) {
    uint32_t v = ((a._u32[0]&1)<<3)|((a._u32[1]&1)<<2)|((a._u32[2]&1)<<1)|(a._u32[3]&1);
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline SPU_SSSE3_TARGET u128 spu_gbh(u128 a) {
#ifdef SPU_SHUFB_SSSE3
    /* Move each host halfword's low bit to its sign position, reorder those
     * eight sign bytes into architectural SPU bit order, then gather them. */
    const __m128i low_bits = _mm_slli_epi16(
        _mm_loadu_si128((const __m128i*)&a), 15);
    const __m128i ordered = _mm_shuffle_epi8(
        low_bits,
        _mm_setr_epi8(13, 15, 9, 11, 5, 7, 1, 3,
                      -1, -1, -1, -1, -1, -1, -1, -1));
    u128 r = spu_zero();
    r._u32[0] = (uint32_t)(_mm_movemask_epi8(ordered) & 0xFF);
    return r;
#else
    /* gather LSB of each SPU halfword H into bit (7-H). SPU halfword H maps to
     * our _u16[H^1] (the within-word halfword swap of the value layout). */
    uint32_t v=0; for(int i=0;i<8;i++) v |= ((uint32_t)(a._u16[i^1]&1) << (7-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
#endif
}
/* gather LSB of each SPU byte i into bit (15-i); exact inverse of spu_fsmb.
 * SPU byte i lives at our _u8[SPU_W(i)] (byte-reversed within each word). */
static inline u128 spu_gbb(u128 a) {
    uint32_t v=0; for(int i=0;i<16;i++) v |= ((uint32_t)(a._u8[SPU_W(i)]&1) << (15-i));
    u128 r = spu_zero(); r._u32[0]=v; return r;
}
static inline u128 spu_cg(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)a._u32[i]+(uint64_t)b._u32[i])>>32); return r; }
/* sumb: sum the 4 bytes of each word. Per CBEA, RT halfword 2i (the HIGH half of
 * word i) = sum of RB's 4 bytes of word i; halfword 2i+1 (LOW half) = sum of RA's
 * 4 bytes of word i. Computed on word VALUES so byte order is irrelevant. */
static inline u128 spu_sumb(u128 a, u128 b) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint32_t wa=a._u32[i], wb=b._u32[i];
        uint32_t sa=((wa>>24)&0xFF)+((wa>>16)&0xFF)+((wa>>8)&0xFF)+(wa&0xFF);
        uint32_t sb=((wb>>24)&0xFF)+((wb>>16)&0xFF)+((wb>>8)&0xFF)+(wb&0xFF);
        r._u32[i]=(sb<<16)|sa;
    }
    return r;
}
/* Borrow generate: carry-out of (b + ~a + 1) == (b >= a unsigned ? 1 : 0).
 * The subtract-side sibling of cg; pairs with sf/sfx for extended subtraction. */
static inline u128 spu_bg(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)(((uint64_t)b._u32[i]+(uint64_t)(~a._u32[i])+1u)>>32); return r; }
static inline u128 spu_addx(u128 a, u128 b, u128 t) { u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]+b._u32[i]+(t._u32[i]&1); return r; }
/* LE host: high half of word i = _s16[2i+1], low half = _s16[2i]. */
static inline u128 spu_mpyh(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++) r._s32[i]=((int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i]) << 16; return r; }
static inline u128 spu_mpyhh(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++) r._s32[i]=(int32_t)a._s16[2*i+1] * (int32_t)b._s16[2*i+1]; return r; }
static inline u128 spu_mpys(u128 a, u128 b) { u128 r; for(int i=0;i<4;i++){ int32_t p=(int32_t)a._s16[2*i]*(int32_t)b._s16[2*i]; r._s32[i]=(int16_t)(p>>16); } return r; }
static inline u128 spu_mpyui(u128 a, int32_t imm) { u128 r; for(int i=0;i<4;i++) r._u32[i]=(uint32_t)a._u16[2*i]*(uint32_t)(uint16_t)imm; return r; }
static inline u128 spu_fcmeq(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++){ float fa=fabsf(a._f32[i]),fb=fabsf(b._f32[i]); r._u32[i]=(fa==fb)?0xFFFFFFFFu:0; } return r; }
static inline u128 spu_fcmgt(u128 a, u128 b){ u128 r; for(int i=0;i<4;i++){ float fa=fabsf(a._f32[i]),fb=fabsf(b._f32[i]); r._u32[i]=(fa>fb)?0xFFFFFFFFu:0; } return r; }
/* Hardware estimate payloads include FI interpolation slopes in low bits.
 * A host 1/x or 1/sqrt(x) is not an equivalent encoding. The lookup data and
 * exponent mapping are documented by RPCS3, SPUThread.cpp/SPUInterpreter.cpp:
 * https://github.com/RPCS3/rpcs3/tree/54014a7de4b2ccec98c9c0cb7dbebec0606c5cd6/rpcs3/Emu/Cell
 * The sign is ignored by FRSQEST, including negative finite inputs. Integer
 * assembly also preserves SPU extended-exponent and exponent-zero payloads
 * instead of introducing host NaNs/infinities or division exceptions. */
static inline u128 spu_frsqest(u128 a) {
    static const uint32_t fractions[64] = {
        0x350160,0x34e954,0x2f993d,0x2f993d,0x2aa523,0x2aa523,0x26190d,0x26190d,
        0x21e4f9,0x21e4f9,0x1e00e9,0x1e00e9,0x1a5cd9,0x1a5cd9,0x16f8cb,0x16f8cb,
        0x13ccc0,0x13ccc0,0x10ccb3,0x10ccb3,0x0e00aa,0x0e00aa,0x0b58a1,0x0b58a1,
        0x08d498,0x08d498,0x067491,0x067491,0x043089,0x043089,0x020c83,0x020c83,
        0x7ffdf4,0x7fd1de,0x7859c8,0x783dba,0x71559c,0x71559c,0x6ae57c,0x6ae57c,
        0x64f561,0x64f561,0x5f7149,0x5f7149,0x5a4d33,0x5a4d33,0x55811f,0x55811f,
        0x51050f,0x51050f,0x4cc8fe,0x4cc8fe,0x48d0f0,0x48d0f0,0x4510e4,0x4510e4,
        0x4180d7,0x4180d7,0x3e24cc,0x3e24cc,0x3af4c3,0x3af4c3,0x37e8ba,0x37e8ba
    };
    u128 r;
    for (int i = 0; i < 4; ++i) {
        const uint32_t exponent = (a._u32[i] >> 23) & 255u;
        const uint32_t result_exponent = exponent ? (380u - exponent) / 2u : 255u;
        r._u32[i] = (result_exponent << 23) | fractions[(a._u32[i] >> 18) & 63u];
    }
    return r;
}
static inline u128 spu_frest(u128 a) {
    static const uint32_t fractions[32] = {
        0x7ffbe0,0x7f87a6,0x70ef72,0x708b40,0x638b12,0x633aea,0x5792c4,0x574aa0,
        0x4cca7e,0x4c9262,0x430a44,0x42d62a,0x3a2e12,0x39fdfa,0x3215e4,0x31f1d2,
        0x2aa9be,0x2a85ac,0x23d59a,0x23bd8e,0x1d8576,0x1d8576,0x17ad5a,0x17ad5a,
        0x124543,0x124543,0x0d392d,0x0d392d,0x08851a,0x08851a,0x041d07,0x041d07
    };
    u128 r;
    for (int i = 0; i < 4; ++i) {
        const uint32_t exponent = (a._u32[i] >> 23) & 255u;
        const uint32_t result_exponent = !exponent ? 255u : exponent < 253u ? 253u - exponent : 0u;
        r._u32[i] = (a._u32[i] & 0x80000000u) | (result_exponent << 23) |
                    fractions[(a._u32[i] >> 18) & 31u];
    }
    return r;
}

/* ---- Phase 3: sign extension ----
 * LE host: low sub-lane = _u8[2i] / _s16[2i] / _s32[2i] (the byte/half/word
 * at the *lower* storage address). Same caveat as the mpy family. */
static inline u128 spu_xsbh(u128 a) { u128 r; for(int i=0;i<8;i++) r._s16[i] = (int8_t)a._u8[2*i]; return r; }
static inline u128 spu_xshw(u128 a) { u128 r; for(int i=0;i<4;i++) r._s32[i] = (int16_t)a._s16[2*i]; return r; }
/* XSWD extends each architectural doubleword's low word (SPU words 1/3).
 * Our word order is architectural, unlike RPCS3's reversed native u128: a
 * native _s64 assignment both selects the wrong word and swaps the result.
 * Preserve explicit high/low word order for guest 64-bit counters. */
static inline u128 spu_xswd(u128 a) {
    u128 r;
    for (int i = 0; i < 2; ++i) {
        r._u32[2*i] = a._s32[2*i+1] < 0 ? UINT32_MAX : 0;
        r._u32[2*i+1] = a._u32[2*i+1];
    }
    return r;
}

/* ---- Phase 3: OR across ---- */
static inline u128 spu_orx(u128 a) {
    u128 r = spu_zero();
    r._u32[0] = a._u32[0] | a._u32[1] | a._u32[2] | a._u32[3];
    return r;
}

/* ---- Phase 3: form-select mask from bits ---- */
static inline u128 spu_fsm(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xF;
    for(int i=0;i<4;i++) r._u32[i] = ((v>>(3-i))&1) ? 0xFFFFFFFFu : 0;
    return r;
}
/* fsmh: per-halfword mask from 8 bits. SPU halfword H <- bit (7-H); store at
 * our _u16[H^1] (within-word halfword swap of the value layout). Per-halfword
 * both bytes are identical so byte order within a halfword is irrelevant, but
 * the halfword ORDER within each word must be swapped. */
static inline u128 spu_fsmh(u128 a) {
    u128 r; uint32_t v = a._u32[0] & 0xFF;
    for(int i=0;i<8;i++) r._u16[i^1] = ((v>>(7-i))&1) ? 0xFFFFu : 0;
    return r;
}
/* fsmb/fsmbi: per-byte mask from 16 bits. SPU byte position P <- bit (15-P);
 * store at our _u8[SPU_W(P)] (byte-reversed within each word). A bit set at SPU
 * byte P must land at our _u8[SPU_W(P)] so that downstream word arithmetic (e.g.
 * andbi(fsmbi(0x101),-128) building a +0x80 EA offset in the low byte of words
 * 1,3) places the value in the correct byte lane. Raw _u8[P] would put 0x80 in
 * the high byte (bit 31) instead -> wrong DMA EA. */
static inline SPU_SSSE3_TARGET u128 spu_fsmb(u128 a) {
#ifdef SPU_SHUFB_SSSE3
    const __m128i value = _mm_set1_epi16((short)(a._u32[0] & 0xFFFFu));
    /* Host lane J represents SPU byte J^3. Therefore output lane J tests
     * source bit 15-(J^3), split across two groups of eight halfwords. */
    const __m128i mask0 = _mm_setr_epi16(
        1 << 12, 1 << 13, 1 << 14, (short)(1u << 15),
        1 << 8, 1 << 9, 1 << 10, 1 << 11);
    const __m128i mask1 = _mm_setr_epi16(
        1 << 4, 1 << 5, 1 << 6, 1 << 7,
        1 << 0, 1 << 1, 1 << 2, 1 << 3);
    const __m128i lanes0 = _mm_cmpeq_epi16(_mm_and_si128(value, mask0), mask0);
    const __m128i lanes1 = _mm_cmpeq_epi16(_mm_and_si128(value, mask1), mask1);
    u128 r;
    _mm_storeu_si128((__m128i*)&r, _mm_packs_epi16(lanes0, lanes1));
    return r;
#else
    u128 r; uint32_t v = a._u32[0] & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
#endif
}
static inline u128 spu_fsmbi(int32_t imm) {
    u128 r; uint32_t v = imm & 0xFFFF;
    for(int i=0;i<16;i++) r._u8[SPU_W(i)] = ((v>>(15-i))&1) ? 0xFFu : 0;
    return r;
}

/* ---- Phase 3: constant generators (insertion shuffle patterns) ---- */
/* Generate-controls (cbd/chd/cwd/cdd) — insertion selectors for the SPU-byte-
 * correct shufb. Each builds a control in TRUE SPU byte order: the base selects
 * b's SPU byte t at result SPU byte t (selector 0x10+t), and insert positions
 * select a's preferred scalar bytes by source byte. Every store is mapped
 * through SPU_W to land at the right host index. */
#ifdef SPU_SHUFB_SSSE3
/* SSSE3 generate-control: base lane J = 0x10 + (J^3) (b's SPU byte J^3);
 * the n-byte aligned group holding SPU byte (pos & 0xF) instead selects a's
 * SPU bytes off..off+n-1. n/off are constants at every call site, so base,
 * key and insert values fold to constants: one compare, AND and XOR. */
static inline SPU_SSSE3_TARGET u128 spu_cgen_ctl(int pos, int n, int off) {
    const __m128i spu_pos = spu_qby_spu_pos();
    const __m128i base = _mm_add_epi8(spu_pos, _mm_set1_epi8(0x10));
    const __m128i key = _mm_and_si128(spu_pos, _mm_set1_epi8((char)~(n - 1)));
    const __m128i ins = _mm_add_epi8(_mm_and_si128(spu_pos, _mm_set1_epi8((char)(n - 1))),
                                     _mm_set1_epi8((char)off));
    const __m128i hit = _mm_cmpeq_epi8(key, _mm_set1_epi8((char)((pos & 0xF) & ~(n - 1))));
    u128 r;
    _mm_storeu_si128((__m128i*)&r,
                     _mm_xor_si128(base, _mm_and_si128(hit, _mm_xor_si128(base, ins))));
    return r;
}
#endif
static inline u128 spu_cbd_pos(int pos) {
#ifdef SPU_SHUFB_SSSE3
    return spu_cgen_ctl(pos, 1, 3);
#else
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    r._u8[SPU_W(pos & 0xF)] = 0x03;                 /* a's preferred byte = SPU byte 3 */
    return r;
#endif
}
static inline u128 spu_chd_pos(int pos) {
#ifdef SPU_SHUFB_SSSE3
    return spu_cgen_ctl(pos, 2, 2);
#else
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~1;                       /* SPU halfword byte positions p, p+1 */
    r._u8[SPU_W(p)]   = 0x02;                        /* a SPU byte 2 (hi byte of preferred hw) */
    r._u8[SPU_W(p+1)] = 0x03;                        /* a SPU byte 3 (lo byte) */
    return r;
#endif
}
static inline u128 spu_cwd_pos(int pos) {
#ifdef SPU_SHUFB_SSSE3
    return spu_cgen_ctl(pos, 4, 0);
#else
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~3;
    for(int k=0;k<4;k++) r._u8[SPU_W(p+k)] = (uint8_t)k;   /* a SPU bytes 0..3 */
    return r;
#endif
}
static inline u128 spu_cdd_pos(int pos) {
#ifdef SPU_SHUFB_SSSE3
    return spu_cgen_ctl(pos, 8, 0);
#else
    u128 r; for(int t=0;t<16;t++) r._u8[SPU_W(t)] = (uint8_t)(0x10 + t);
    int p = (pos & 0xF) & ~7;
    for(int k=0;k<8;k++) r._u8[SPU_W(p+k)] = (uint8_t)k;   /* a SPU bytes 0..7 */
    return r;
#endif
}
static inline u128 spu_cbd(u128 a, int i7){ return spu_cbd_pos((int)a._u32[0]+i7); }
static inline u128 spu_chd(u128 a, int i7){ return spu_chd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cwd(u128 a, int i7){ return spu_cwd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cdd(u128 a, int i7){ return spu_cdd_pos((int)a._u32[0]+i7); }
static inline u128 spu_cbx(u128 a, u128 b){ return spu_cbd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_chx(u128 a, u128 b){ return spu_chd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cwx(u128 a, u128 b){ return spu_cwd_pos((int)(a._u32[0]+b._u32[0])); }
static inline u128 spu_cdx(u128 a, u128 b){ return spu_cdd_pos((int)(a._u32[0]+b._u32[0])); }

/* ---- Phase 3: rotate-and-mask family ---- */
static inline u128 spu_rotm(u128 a, u128 b)   { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); } return r; }
static inline u128 spu_rotma(u128 a, u128 b)  { u128 r; for(int i=0;i<4;i++){ uint32_t sh=(0-b._u32[i])&0x3F; r._s32[i]=(sh>31)?(a._s32[i]>>31):(a._s32[i]>>sh); } return r; }
static inline u128 spu_rothm(u128 a, u128 b)  { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); } return r; }
static inline u128 spu_rothma(u128 a, u128 b) { u128 r; for(int i=0;i<8;i++){ uint32_t sh=(0-b._u16[i])&0x1F; r._s16[i]=(sh>15)?(a._s16[i]>>15):(a._s16[i]>>sh); } return r; }
static inline u128 spu_rothmi(u128 a, int i7) { u128 r; int sh=(0-i7)&0x1F; for(int i=0;i<8;i++) r._u16[i]=(sh>15)?0:(uint16_t)(a._u16[i]>>sh); return r; }
static inline u128 spu_rotqmbi(u128 a, u128 b)   { int sh=(0-(int)b._u32[0])&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmby(u128 a, u128 b)   {
#ifdef SPU_SHUFB_SSSE3
    return spu_qby_shr(a, (0-(int)b._u32[0])&0x1F);
#else
    int sh=(0-(int)b._u32[0])&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r;
#endif
}
static inline u128 spu_rotqmbybi(u128 a, u128 b) {
#ifdef SPU_SHUFB_SSSE3
    return spu_qby_shr(a, (0-((int)b._u32[0]>>3))&0x1F);
#else
    int sh=(0-((int)b._u32[0]>>3))&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r;
#endif
}
static inline u128 spu_rotqmbii(u128 a, int i7)  { int sh=(0-i7)&7; if(!sh) return a;
    uint64_t hi=((uint64_t)a._u32[0]<<32)|a._u32[1], lo=((uint64_t)a._u32[2]<<32)|a._u32[3];
    uint64_t nlo=(lo>>sh)|(hi<<(64-sh)), nhi=(hi>>sh);
    u128 r; r._u32[0]=(uint32_t)(nhi>>32); r._u32[1]=(uint32_t)nhi; r._u32[2]=(uint32_t)(nlo>>32); r._u32[3]=(uint32_t)nlo; return r; }
static inline u128 spu_rotqmbyi(u128 a, int i7)  {
#ifdef SPU_SHUFB_SSSE3
    return spu_qby_shr(a, (0-i7)&0x1F);
#else
    int sh=(0-i7)&0x1F; u128 r=spu_zero(); if(sh>=16) return r; for(int i=0;i<16;i++){ int s=i-sh; if(s>=0) r._u8[SPU_W(i)]=a._u8[SPU_W(s)]; } return r;
#endif
}

/* ---- Phase 3: halfword/byte immediate logic ---- */
static inline u128 spu_andhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]&v; return r; }
static inline u128 spu_andbi(u128 a, int32_t imm) { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]&v;   return r; }
static inline u128 spu_orhi(u128 a, int32_t imm)  { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]|v; return r; }
static inline u128 spu_orbi(u128 a, int32_t imm)  { u128 r; uint8_t  v=(uint8_t)imm;  for(int i=0;i<16;i++) r._u8[i]=a._u8[i]|v;   return r; }
static inline u128 spu_xorhi(u128 a, int32_t imm) { u128 r; uint16_t v=(uint16_t)imm; for(int i=0;i<8;i++) r._u16[i]=a._u16[i]^v; return r; }

/* ---- Phase 3: borrow-generate extended ---- */
static inline u128 spu_bgx(u128 a, u128 b, u128 t) {
    u128 r;
    for(int i=0;i<4;i++) {
        uint64_t s = (uint64_t)b._u32[i] + (uint64_t)(~a._u32[i]) + (uint64_t)(t._u32[i]&1);
        r._u32[i] = (uint32_t)(s >> 32);
    }
    return r;
}

/* ---- Phase 3: mfspr stub ---- */
static inline u128 spu_mfspr(u128 a) { (void)a; return spu_zero(); }

/* ---- immediate loaders ---- */
static inline u128 spu_il(int32_t imm)   { return spu_splat_u32((uint32_t)imm); }
static inline u128 spu_ila(uint32_t i18) { return spu_splat_u32(i18 & 0x3FFFF); }
static inline u128 spu_ilh(uint16_t imm) { return spu_splat_u16(imm); }
static inline u128 spu_ilhu(uint16_t imm){ return spu_splat_u32((uint32_t)imm << 16); }
static inline u128 spu_iohl(u128 a, uint16_t imm){ u128 r; for(int i=0;i<4;i++) r._u32[i]=a._u32[i]|(uint32_t)imm; return r; }

#ifdef __cplusplus
}
#endif

#endif /* SPU_HELPERS_H */
