/* spu_interp_selftest.c — end-to-end check of the SPU interpreter.
 *
 * Runs a snippet assembled by spu-lv2-as (bytes below) through the interpreter
 * and asserts the architectural results. Proves decode + execute + control flow
 * + local-store store, using the same helpers the lifter emits.
 *
 *   il $2,5; il $3,7; a $4,$2,$3; ai $4,$4,100; rotqbyi $5,$4,0;
 *   sf $6,$2,$3; stqd $4,0($1); stop
 *
 * Also executes signed-I10 OR/XOR byte/halfword forms to guard decoder table
 * coverage and immediate sign extension through the interpreter.
 */
#include "../spu_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* assert() is disabled by NDEBUG in Release builds; selftest failures must not
 * disappear there. This macro is used only from main, where failure is final. */
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "spu_interp_selftest:%d: CHECK(%s) failed\n", \
                __LINE__, #condition); \
        return 1; \
    } \
} while (0)

/* This snippet issues no channel ops; stub the channel ABI so the test links
 * without the full runtime (spu_channels.c pulls in unrelated globals). */
static const spu_context* s_lookup_sentinel_ctx;
static uint32_t s_lookup_sentinel_pc;
static int s_lookup_calls;
static void selftest_lifted_sentinel(spu_context* c) { (void)c; }

u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c;(void)ch; u128 z; memset(&z,0,sizeof z); return z; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c;(void)ch;(void)v; }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c;(void)ch; return 1; }
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t lsa) {
    s_lookup_calls++;
    return c == s_lookup_sentinel_ctx && lsa == s_lookup_sentinel_pc
        ? selftest_lifted_sentinel : 0;
}
void spu_spurs_taskset_syscall(spu_context* c) {
    (void)c;
    fputs("spu_interp_selftest: unexpected taskset syscall\n", stderr);
    exit(EXIT_FAILURE);
}
int g_wws_code_probe = 0;
int g_wws_read_probe = 0;

static void put_word(spu_context* c, uint32_t pc, uint32_t word) {
    c->ls[pc] = (unsigned char)(word >> 24);
    c->ls[pc + 1] = (unsigned char)(word >> 16);
    c->ls[pc + 2] = (unsigned char)(word >> 8);
    c->ls[pc + 3] = (unsigned char)word;
}

/* .text of the assembled snippet (big-endian words, verbatim). */
static const unsigned char PROG[] = {
    0x40,0x80,0x02,0x82, 0x40,0x80,0x03,0x83, 0x18,0x00,0xc1,0x04,
    0x1c,0x19,0x02,0x04, 0x3f,0x80,0x02,0x05, 0x08,0x00,0xc1,0x06,
    0x24,0x00,0x00,0x84, 0x00,0x00,0x00,0x00,
};

int main(void) {
    static spu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    memcpy(ctx.ls, PROG, sizeof PROG);
    ctx.gpr[1]._u32[0] = 0x100;   /* stack pointer (store target base) */

    spu_interp_run(&ctx, 0);

    #define P(i) (ctx.gpr[i]._u32[0])
    printf("r2=%u r3=%u r4=%u r5=%u r6=%u stop=0x%X\n",
           P(2), P(3), P(4), P(5), P(6), ctx.stop_code);

    CHECK(P(2) == 5);
    CHECK(P(3) == 7);
    CHECK(P(4) == 112);          /* 5+7+100 */
    CHECK(P(5) == 112);          /* rotqbyi 0 = identity */
    CHECK(P(6) == 2);            /* sf: b - a = 7 - 5 */
    CHECK(ctx.status == SPU_STATUS_STOPPED_BY_STOP);

    /* stqd $4,0($1): LS[0x100] holds 112 big-endian in the preferred word. */
    const unsigned char* q = &ctx.ls[0x100];
    unsigned w = ((unsigned)q[0]<<24)|((unsigned)q[1]<<16)|((unsigned)q[2]<<8)|q[3];
    CHECK(w == 112);

    /* stqd/lqd use signed RI10 quadword offsets. -1 must address base - 16,
     * not invoke undefined signed-left-shift behavior in the decoder. Check
     * both the independently known LS line and the loaded word lanes. */
    static const uint32_t NEG_QW[4] = {
        0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    };
    static spu_context nc;
    memset(&nc, 0, sizeof nc);
    nc.gpr[1]._u32[0] = 0x110;
    for (int i = 0; i < 4; i++) nc.gpr[2]._u32[i] = NEG_QW[i];
    put_word(&nc, 0, 0x24ffc082u); /* stqd $2,-1($1): 0x110 - 16 = 0x100 */
    put_word(&nc, 4, 0x34ffc083u); /* lqd  $3,-1($1) */
    put_word(&nc, 8, 0x00000000u);
    spu_interp_run(&nc, 0);
    CHECK(nc.status == SPU_STATUS_STOPPED_BY_STOP);
    CHECK(nc.ls[0x100] == 0x11 && nc.ls[0x101] == 0x22 &&
          nc.ls[0x102] == 0x33 && nc.ls[0x103] == 0x44);
    for (int i = 0; i < 4; i++) CHECK(nc.gpr[3]._u32[i] == NEG_QW[i]);

    /* --- backward-branch loop (the SPU counter-loop mechanism) via spu_dispatch --
     * il $2,0; il $3,5; loop: ai $2,$2,1; ceq $4,$2,$3; brz $4,loop; stop */
    static const unsigned char LOOP[] = {
        0x40,0x80,0x00,0x02, 0x40,0x80,0x02,0x83, 0x1c,0x00,0x41,0x02,
        0x78,0x00,0xc1,0x04, 0x20,0x7f,0xff,0x04, 0x00,0x00,0x00,0x00,
    };
    static spu_context lc;
    memset(&lc, 0, sizeof lc);
    memcpy(lc.ls, LOOP, sizeof LOOP);
    spu_dispatch(&lc, 0);                 /* enters via the computed-branch entry point */
    printf("loop r2=%u stop=0x%X\n", lc.gpr[2]._u32[0], lc.stop_code);
    CHECK(lc.gpr[2]._u32[0] == 5);       /* loop terminated at the bound */
    CHECK(lc.status == SPU_STATUS_STOPPED_BY_STOP);

    /* orbi $11,$10,-128; xorhi $13,$12,-2; xorbi $15,$14,-1; stop */
    static const unsigned char RI10_LOGIC[] = {
        0x06,0xe0,0x05,0x0b, 0x45,0xff,0x86,0x0d,
        0x46,0xff,0xc7,0x0f, 0x00,0x00,0x00,0x00,
    };
    static spu_context ic;
    memset(&ic, 0, sizeof ic);
    memcpy(ic.ls, RI10_LOGIC, sizeof RI10_LOGIC);
    for (int i = 0; i < 16; i++) {
        ic.gpr[10]._u8[i] = (uint8_t)(0x10 + i);
        ic.gpr[14]._u8[i] = (uint8_t)(0xa0 + i);
    }
    for (int i = 0; i < 8; i++) ic.gpr[12]._u16[i] = (uint16_t)(0x1234 + i);
    spu_interp_run(&ic, 0);
    for (int i = 0; i < 16; i++) {
        CHECK(ic.gpr[11]._u8[i] == (uint8_t)(ic.gpr[10]._u8[i] | 0x80));
        CHECK(ic.gpr[15]._u8[i] == (uint8_t)(ic.gpr[14]._u8[i] ^ 0xff));
    }
    for (int i = 0; i < 8; i++)
        CHECK(ic.gpr[13]._u16[i] == (uint16_t)(ic.gpr[12]._u16[i] ^ 0xfffe));
    CHECK(ic.status == SPU_STATUS_STOPPED_BY_STOP);

    /* Raw SMC-style step: xsbh $49,$50 (0x56C01931). Its current PC has a
     * lifted-table sentinel, proving spu_interp_step never performs lookup. */
    static spu_context sc;
    memset(&sc, 0, sizeof sc);
    sc.pc = 0x180;
    put_word(&sc, sc.pc, 0x56C01931u);
    for (int i = 0; i < 8; i++) sc.gpr[50]._u8[2 * i] = (uint8_t)(0x80 + i);
    s_lookup_sentinel_ctx = &sc;
    s_lookup_sentinel_pc = sc.pc;
    s_lookup_calls = 0;
    int branch_taken = 99;
    CHECK(spu_interp_step(&sc, &branch_taken) == 0);
    CHECK(s_lookup_calls == 0);
    CHECK(sc.pc == 0x184);
    CHECK(branch_taken == 0);
    for (int i = 0; i < 8; i++)
        CHECK(sc.gpr[49]._s16[i] == (int8_t)(0x80 + i));
    s_lookup_sentinel_ctx = 0;

    /* A taken conditional may target its own fall-through PC; it is still a
     * branch and must request trampoline re-dispatch. */
    static spu_context bc;
    memset(&bc, 0, sizeof bc);
    bc.pc = 0x100;
    put_word(&bc, bc.pc, 0x20000084u); /* brz $4,+1 -> pc + 4 */
    bc.gpr[4]._u32[0] = 0;
    branch_taken = 0;
    CHECK(spu_interp_step(&bc, &branch_taken) == 0);
    CHECK(bc.pc == 0x104 && branch_taken == 1);
    bc.pc = 0x100;
    bc.gpr[4]._u32[0] = 1;
    branch_taken = 1;
    CHECK(spu_interp_step(&bc, &branch_taken) == 0);
    CHECK(bc.pc == 0x104 && branch_taken == 0);
    put_word(&bc, bc.pc, 0x32000080u); /* br +1 -> pc + 4 */
    branch_taken = 0;
    CHECK(spu_interp_step(&bc, &branch_taken) == 0);
    CHECK(bc.pc == 0x108 && branch_taken == 1);

    static spu_context tc;
    memset(&tc, 0, sizeof tc);
    put_word(&tc, 0, 0x00000000u); /* stop 0 */
    CHECK(spu_interp_step(&tc, &branch_taken) == 1);
    CHECK(tc.status == SPU_STATUS_STOPPED_BY_STOP && tc.pc == 4);
    memset(&tc, 0, sizeof tc);
    put_word(&tc, 0, 0x01000000u); /* decoder tables reject this as .word */
    CHECK(spu_interp_step(&tc, &branch_taken) == 1);
    CHECK(tc.status == SPU_STATUS_STOPPED_BY_HALT && tc.pc == 0);

    printf("spu_interp_selftest: PASS\n");
    return 0;
}
