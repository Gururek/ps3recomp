/*
 * ps3recomp - RSX Vertex Program (NV40 ISA) -> HLSL decompiler
 *
 * See rsx_vp_decompiler.h. The 4-word instruction encoding (D0..D3 fields,
 * source operand layout, opcode numbers) follows the public NV40 vertex
 * program layout as documented in the NV_vertex_program3 extension spec and
 * the envytools rnndb; RPCS3 was consulted as a READ-ONLY semantics oracle
 * for the field positions and the dest-write rules (no code copied).
 *
 * Field facts used (verified against the oracle 2026-07-03):
 *   D0: mask_w/z/y/x pairs [2:9] are the CC swizzle, cond [10:12],
 *       cond_test_enable [13], dst_tmp [15:20], src0/1/2_abs [21:23],
 *       addr_reg_sel [24], saturate [26], vec_result [30]
 *   D1: src0h [0:7], input_src [8:11], const_src [12:21],
 *       vec_opcode [22:26], sca_opcode [27:31]
 *   D2: src2h [0:5], src1 [6:22], src0l [23:31]
 *   D3: end [0], index_const [1], dst [2:6], sca_dst_tmp [7:12],
 *       vec_writemask w/z/y/x [13:16], sca_writemask w/z/y/x [17:20],
 *       src2l [21:31]
 *   SRC (17 bits): reg_type [0:1] (1 = temp, 2 = input, 3 = constant),
 *       tmp_src [2:7], swz w/z/y/x [8:15] (2 bits each), neg [16]
 *   Dest rules: the VEC result goes to output o[D3.dst] when D0.vec_result
 *       (dst 0x1F = none) AND to temp r[D0.dst_tmp] (0x3F = none); the SCA
 *       result goes to o[D3.dst] when !D0.vec_result, else to
 *       r[D3.sca_dst_tmp]. The SCA unit reads SRC2.
 *   VEC ADD reads src0 + src2 (not src1).
 *   Output register map: o0 = HPOS, o1 = COL0, o2 = COL1, o3/o4 = back-face
 *       colors, o5.x = FOGC (z,0,0,0), o6 = point size / TEX9, o7..o14 = TEX0..7,
 *       o15 = TEX8. Unwritten outputs default to (0,0,0,1).
 */
#include "rsx_vp_decompiler.h"
#include <stdio.h>
#include <string.h>

/* ---- little-endian word read (RPCS3-capture native order) --------------- */
static u32 rd_le(const u8* p)
{ return (u32)p[0] | ((u32)p[1]<<8) | ((u32)p[2]<<16) | ((u32)p[3]<<24); }

/* ---- opcode names ------------------------------------------------------ */
const char* rsx_vp_vec_name(u32 op)
{
    static const char* n[] = {"NOP","MOV","MUL","ADD","MAD","DP3","DPH","DP4",
        "DST","MIN","MAX","SLT","SGE","ARL","FRC","FLR","SEQ","SFL","SGT","SLE",
        "SNE","STR","SSG","?","?","TXL"};
    return op < 26 ? n[op] : "?";
}
const char* rsx_vp_sca_name(u32 op)
{
    static const char* n[] = {"NOP","MOV","RCP","RCC","RSQ","EXP","LOG","LIT",
        "BRA","BRI","CAL","CLI","RET","LG2","EX2","SIN","COS","BRB","CLB","PSH","POP"};
    return op < 21 ? n[op] : "?";
}

u32 rsx_vp_program_size_instrs(const u8* ucode, u32 max_bytes)
{
    if (!ucode) return 0;
    u32 off = 0, instrs = 0;
    while (off + 16 <= max_bytes) {
        u32 d3 = rd_le(ucode + off + 12);
        instrs++;
        off += 16;
        if (d3 & 1u) return instrs;   /* D3.end */
    }
    return 0;
}

/* ---- output appender --------------------------------------------------- */
typedef struct { char* p; u32 cap; u32 len; int overflow; } Out;
static void emit(Out* o, const char* s)
{
    u32 n = (u32)strlen(s);
    if (o->len + n + 1 > o->cap) { o->overflow = 1; return; }
    memcpy(o->p + o->len, s, n); o->len += n; o->p[o->len] = '\0';
}

/* ---- source operand ---------------------------------------------------- */
static const char SWZ[4] = {'x','y','z','w'};

/* Build the HLSL expression for one 17-bit source field.
 * reg_type: 1 = temp r[], 2 = input v[], 3 = constant vp_c[]. */
static void emit_src(u32 src, int is_abs, u32 input_src, u32 const_src,
                     int index_const, u32 addr_sel, u32 addr_swz,
                     char* buf, u32 bufsz)
{
    u32 reg_type = src & 3;
    u32 tmp_src  = (src >> 2) & 0x3F;
    u32 sw = (src >> 8) & 3, sz = (src >> 10) & 3;
    u32 sy = (src >> 12) & 3, sx = (src >> 14) & 3;
    int neg = (src >> 16) & 1;

    char base[64];
    switch (reg_type) {
    case 2:  snprintf(base, sizeof(base), "v[%u]", input_src & 15); break;
    case 3:
        if (index_const)
            snprintf(base, sizeof(base), "vp_c[(%uu + a%u.%c) & 511u]",
                     const_src, addr_sel & 1, SWZ[addr_swz & 3]);
        else
            snprintf(base, sizeof(base), "vp_c[%u]", const_src & 511);
        break;
    default: snprintf(base, sizeof(base), "r[%u]", tmp_src & 31); break;
    }

    char swz[8];
    swz[0]='.'; swz[1]=SWZ[sx]; swz[2]=SWZ[sy]; swz[3]=SWZ[sz]; swz[4]=SWZ[sw]; swz[5]='\0';

    const char* pre = neg ? "-" : "";
    if (is_abs) snprintf(buf, bufsz, "%sabs(%s)%s", pre, base, swz);
    else        snprintf(buf, bufsz, "%s(%s)%s", pre, base, swz);
}

/* writemask string from 4 bits (x,y,z,w order) -> "xyzw" subset */
static void mask_str(int mx,int my,int mz,int mw, char* m)
{
    int i=0;
    if (mx) m[i++]='x';
    if (my) m[i++]='y';
    if (mz) m[i++]='z';
    if (mw) m[i++]='w';
    m[i]='\0';
}

static int vp_sca_is_flow(u32 op)
{
    return (op >= 0x08u && op <= 0x0Cu) ||
           (op >= 0x11u && op <= 0x14u);
}

static u32 vp_flow_addr(u32 d0, u32 d2, u32 d3)
{
    return (((d0 >> 23) & 1u) << 9) |
           ((d2 & 0x3Fu) << 3) | ((d3 >> 29) & 7u);
}

static void vp_flow_cc_expr(u32 cond, u32 cond_reg, const char* swz,
                            char* out, u32 out_size)
{
    static const char* cmp[8] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
    if (cond == 0u) snprintf(out, out_size, "false");
    else if (cond == 7u) snprintf(out, out_size, "true");
    else snprintf(out, out_size, "any(cc%u.%s %s 0.0)",
                  cond_reg & 1u, swz, cmp[cond & 7u]);
}

/* Emit a destination write. When the instruction enables condition testing,
 * each lane uses the correspondingly swizzled CC lane captured in `_cc`. */
static void emit_store(Out* b, const char* dst_fmt, u32 idx, const char* m,
                       int cond_test, u32 cond)
{
    if (!m[0] || (cond_test && cond == 0)) return;
    char line[160];
    char dst[32];
    snprintf(dst, sizeof(dst), dst_fmt, idx);
    if (!cond_test || cond == 7) {
        snprintf(line, sizeof(line), "        %s.%s = _v.%s;\n", dst, m, m);
        emit(b, line);
        return;
    }

    static const char* cmp[8] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
    for (const char* lane = m; *lane; ++lane) {
        snprintf(line, sizeof(line),
                 "        %s.%c = (_cc.%c %s 0.0) ? _v.%c : %s.%c;\n",
                 dst, *lane, *lane, cmp[cond & 7u], *lane, dst, *lane);
        emit(b, line);
    }
}

static int rsx_vp_decompile_impl(const u8* ucode, u32 max_bytes,
                                 u32 entry_instr, u32 texture_dimensions,
                                 char* out, u32 out_size)
{
    if (!ucode || !out || out_size < 256) return -1;

    u32 total_instrs = max_bytes / 16u;
    if (total_instrs > 1024u) total_instrs = 1024u;
    if (!total_instrs || entry_instr >= total_instrs) return -1;

    /* Straight-line programs retain the compact old output. Flow programs
     * use a bounded PC state machine, which also handles backward branches
     * and conditional calls without trying to force arbitrary NV40 control
     * flow into nested HLSL if/loop scopes. */
    int has_flow = 0;
    for (u32 pc = entry_instr; pc < total_instrs; ++pc) {
        u32 d1 = rd_le(ucode + pc * 16u + 4u);
        u32 d3 = rd_le(ucode + pc * 16u + 12u);
        if (vp_sca_is_flow((d1 >> 27) & 0x1Fu)) { has_flow = 1; break; }
        if (d3 & 1u) break;
    }

    u8 reachable[1024] = {0};
    if (has_flow) {
        u16 queue[1024];
        u32 head = 0, tail = 0;
        reachable[entry_instr] = 1;
        queue[tail++] = (u16)entry_instr;
        while (head < tail) {
            u32 pc = queue[head++];
            const u8* ins = ucode + pc * 16u;
            u32 d0 = rd_le(ins + 0), d1 = rd_le(ins + 4);
            u32 d2 = rd_le(ins + 8), d3 = rd_le(ins + 12);
            u32 op = (d1 >> 27) & 0x1Fu;
            u32 cond = (d0 >> 10) & 7u;
            int mark_fallthrough = !(d3 & 1u);

            if (op == 0x08u) {
                /* BRA reads an address register at runtime. Any resident slot
                 * can be selected, so keep the entire loaded bank available. */
                for (u32 i = 0; i < total_instrs; ++i) {
                    if (!reachable[i]) { reachable[i] = 1; queue[tail++] = (u16)i; }
                }
                break;
            }
            if (op == 0x09u || op == 0x0Au || op == 0x0Bu ||
                op == 0x11u || op == 0x12u) {
                u32 target = vp_flow_addr(d0, d2, d3);
                if (target < total_instrs && !reachable[target]) {
                    reachable[target] = 1;
                    queue[tail++] = (u16)target;
                }
                /* Calls return to the following instruction. BRI is only
                 * fallthrough-free when its CC predicate is always true;
                 * BRB/CLB depend on mutable boolean constants. */
                if (op == 0x0Au || op == 0x0Bu || op == 0x12u)
                    mark_fallthrough = 1;
                else if (op == 0x09u && cond == 7u)
                    mark_fallthrough = 0;
                else if (op == 0x11u)
                    mark_fallthrough = 1;
            } else if (op == 0x0Cu && cond == 7u) {
                mark_fallthrough = 0;
            }
            if (mark_fallthrough && pc + 1u < total_instrs && !reachable[pc + 1u]) {
                reachable[pc + 1u] = 1;
                queue[tail++] = (u16)(pc + 1u);
            }
        }
    }

    /* Body is built first, preamble/epilogue wrap it. */
    static char body[512 * 1024];
    Out b = { body, sizeof(body), 0, 0 };
    body[0] = '\0';

    int n_flow_skipped = 0;

    if (has_flow) {
        char line[256];
        snprintf(line, sizeof(line),
                 "    int _pc = %u; int _flow_sp = 0; int _flow_stack[4]; int _flow_kind[4]; int4 _flow_addr[4]; bool _flow_done = false;\n"
                 "    [loop] for (int _flow_guard = 0; _flow_guard < 65536 && !_flow_done; ++_flow_guard) {\n"
                 "        switch (_pc) {\n",
                 entry_instr);
        emit(&b, line);
    }

    u32 off = has_flow ? 0u : entry_instr * 16u;
    int instrs = 0;
    while (off + 16u <= total_instrs * 16u) {
        u32 pc = off / 16u;
        if (has_flow && !reachable[pc]) { off += 16u; continue; }
        u32 d0 = rd_le(ucode+off+0), d1 = rd_le(ucode+off+4);
        u32 d2 = rd_le(ucode+off+8), d3 = rd_le(ucode+off+12);
        off += 16; instrs++;

        if (has_flow) {
            char line[160];
            snprintf(line, sizeof(line),
                     "        case %u: { int _next = %u; bool _flow_taken = false;\n",
                     pc, pc + 1u);
            emit(&b, line);
        }

        u32 vec_op = (d1 >> 22) & 0x1F;
        u32 sca_op = (d1 >> 27) & 0x1F;
        u32 input_src = (d1 >> 8) & 0xF;
        u32 const_src = (d1 >> 12) & 0x3FF;
        int index_const = (d3 >> 1) & 1;

        /* assemble the three 17-bit source fields */
        u32 src0 = ((d2 >> 23) & 0x1FF) | ((d1 & 0xFF) << 9);
        u32 src1 = (d2 >> 6) & 0x1FFFF;
        u32 src2 = ((d3 >> 21) & 0x7FF) | ((d2 & 0x3F) << 11);
        int a0 = (d0 >> 21) & 1, a1 = (d0 >> 22) & 1, a2 = (d0 >> 23) & 1;
        u32 addr_sel = (d0 >> 24) & 1;
        u32 addr_swz = d0 & 3;
        int saturate = (d0 >> 26) & 1;
        int cond_test = (d0 >> 13) & 1;
        u32 cond = (d0 >> 10) & 7;
        int cond_update_0 = (d0 >> 14) & 1;
        int cond_update_1 = (d0 >> 29) & 1;
        u32 cond_reg = (d0 >> 25) & 1;
        char cc_swz[5];
        cc_swz[0] = SWZ[(d0 >> 8) & 3];
        cc_swz[1] = SWZ[(d0 >> 6) & 3];
        cc_swz[2] = SWZ[(d0 >> 4) & 3];
        cc_swz[3] = SWZ[(d0 >> 2) & 3];
        cc_swz[4] = '\0';

        char A[96], B[96], C[96];
        emit_src(src0, a0, input_src, const_src, index_const, addr_sel, addr_swz, A, sizeof(A));
        emit_src(src1, a1, input_src, const_src, index_const, addr_sel, addr_swz, B, sizeof(B));
        emit_src(src2, a2, input_src, const_src, index_const, addr_sel, addr_swz, C, sizeof(C));

        u32 dst_tmp     = (d0 >> 15) & 0x3F;
        u32 dst_out     = (d3 >> 2) & 0x1F;
        u32 sca_dst_tmp = (d3 >> 7) & 0x3F;
        int vec_result  = (d0 >> 30) & 1;

        int vmx=(d3>>16)&1, vmy=(d3>>15)&1, vmz=(d3>>14)&1, vmw=(d3>>13)&1;
        int smx=(d3>>20)&1, smy=(d3>>19)&1, smz=(d3>>18)&1, smw=(d3>>17)&1;

        /* ---- vector ALU ---- */
        if ((vmx|vmy|vmz|vmw) && vec_op != 0x00) {
            char rhs[512];
            int handled = 1;
            switch (vec_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",A); break;                 /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"(%s)*(%s)",A,B); break;        /* MUL */
            case 0x03: snprintf(rhs,sizeof rhs,"(%s)+(%s)",A,C); break;        /* ADD: src0 + src2 */
            case 0x04: snprintf(rhs,sizeof rhs,"(%s)*(%s)+(%s)",A,B,C); break; /* MAD */
            case 0x05: snprintf(rhs,sizeof rhs,"dot((%s).xyz,(%s).xyz)",A,B); break; /* DP3 */
            case 0x06: snprintf(rhs,sizeof rhs,"(dot((%s).xyz,(%s).xyz)+(%s).w)",A,B,B); break; /* DPH */
            case 0x07: snprintf(rhs,sizeof rhs,"dot((%s),(%s))",A,B); break; /* DP4 */
            case 0x08: snprintf(rhs,sizeof rhs,"float4(1,(%s).y*(%s).y,(%s).z,(%s).w)",A,B,A,B); break; /* DST */
            case 0x09: snprintf(rhs,sizeof rhs,"min((%s),(%s))",A,B); break;   /* MIN */
            case 0x0A: snprintf(rhs,sizeof rhs,"max((%s),(%s))",A,B); break;   /* MAX */
            case 0x0B: snprintf(rhs,sizeof rhs,"(float4)((%s)<(%s))",A,B); break;  /* SLT */
            case 0x0C: snprintf(rhs,sizeof rhs,"(float4)((%s)>=(%s))",A,B); break; /* SGE */
            case 0x0E: snprintf(rhs,sizeof rhs,"frac(%s)",A); break;           /* FRC */
            case 0x0F: snprintf(rhs,sizeof rhs,"floor(%s)",A); break;          /* FLR */
            case 0x10: snprintf(rhs,sizeof rhs,"(float4)((%s)==(%s))",A,B); break; /* SEQ */
            case 0x11: snprintf(rhs,sizeof rhs,"(float4)0"); break;            /* SFL */
            case 0x12: snprintf(rhs,sizeof rhs,"(float4)((%s)>(%s))",A,B); break;  /* SGT */
            case 0x13: snprintf(rhs,sizeof rhs,"(float4)((%s)<=(%s))",A,B); break; /* SLE */
            case 0x14: snprintf(rhs,sizeof rhs,"(float4)((%s)!=(%s))",A,B); break; /* SNE */
            case 0x15: snprintf(rhs,sizeof rhs,"float4(1,1,1,1)"); break;      /* STR */
            case 0x16: snprintf(rhs,sizeof rhs,"sign(%s)",A); break;           /* SSG */
            case 0x19: { /* TXL: vertex texture fetch, fixed LOD zero */
                u32 unit = (d2 >> 8) & 3u;
                u32 dim = (texture_dimensions >> (unit * 3u)) & 7u;
                if (dim == 1u)
                    snprintf(rhs, sizeof rhs,
                             "vp_tex%u.SampleLevel(vp_smp%u, (%s).x * vp_texscale[%u].x, 0.0)",
                             unit, unit, A, unit);
                else if (dim == 3u)
                    snprintf(rhs, sizeof rhs,
                             "vp_tex%u.SampleLevel(vp_smp%u, (%s).xyz * vp_texscale[%u].xyz, 0.0)",
                             unit, unit, A, unit);
                else if (dim == 4u)
                    snprintf(rhs, sizeof rhs,
                             "vp_tex%u.SampleLevel(vp_smp%u, (%s).xyz, 0.0)",
                             unit, unit, A);
                else
                    snprintf(rhs, sizeof rhs,
                             "vp_tex%u.SampleLevel(vp_smp%u, (%s).xy * vp_texscale[%u].xy, 0.0)",
                             unit, unit, A, unit);
                break; }
            case 0x0D: /* ARL: address register load, rounds down */
                if (vmx|vmy|vmz|vmw) {
                    char m[6]; mask_str(vmx,vmy,vmz,vmw,m);
                    char line[768];
                    int pos = snprintf(line, sizeof line,
                        "    { int4 _a = (int4)floor(%s);", A);
                    if (cond_test && cond > 0 && cond < 7)
                        pos += snprintf(line + pos, sizeof line - pos,
                            " float4 _cc = cc%u.%s;", cond_reg, cc_swz);
                    if (!cond_test || cond == 7) {
                        pos += snprintf(line + pos, sizeof line - pos,
                            " a%u.%s = _a.%s;", addr_sel, m, m);
                    } else if (cond != 0) {
                        static const char* cmp[8] = { "", "<", "==", "<=", ">", "!=", ">=", "" };
                        for (const char* lane = m; *lane; ++lane)
                            pos += snprintf(line + pos, sizeof line - pos,
                                " a%u.%c = (_cc.%c %s 0.0) ? _a.%c : a%u.%c;",
                                addr_sel, *lane, *lane, cmp[cond], *lane, addr_sel, *lane);
                    }
                    snprintf(line + pos, sizeof line - pos, " }\n");
                    emit(&b, line);
                }
                handled = 0;
                break;
            default:
                n_flow_skipped++;
                emit(&b, "    /* TODO: unhandled VP vec op */\n");
                handled = 0;
                break;
            }

            if (handled) {
                char m[6]; mask_str(vmx,vmy,vmz,vmw,m);
                char line[700];
                snprintf(line,sizeof line,"    { float4 _v = (float4)(%s);%s%s\n",
                         rhs, saturate ? " _v = saturate(_v);" : "",
                         cond_test && cond > 0 && cond < 7
                            ? " /* predicated write; _cc follows */" : "");
                emit(&b, line);
                if (cond_test && cond > 0 && cond < 7) {
                    snprintf(line, sizeof line, "        float4 _cc = cc%u.%s;\n",
                             cond_reg, cc_swz);
                    emit(&b, line);
                }
                /* The output register o[dst_out] receives the VEC result when
                 * vec_result selects the VEC unit OR when the SCA unit is idle
                 * (no SCA op) so nothing else can claim the output. The old code
                 * wrote o[] only on vec_result==1, so a VEC op targeting an
                 * output with vec_result==0 and no paired SCA -- exactly how
                 * vkcube's VP writes HPOS (o[0]) and the colours -- was dropped
                 * as "CC-only", leaving o[0]=(0,0,0,1) so every vertex collapsed
                 * to the origin (black cube). cellmark's VP either sets
                 * vec_result or pairs a SCA op, so it is unaffected. */
                int _sca_active = (smx|smy|smz|smw) && sca_op != 0x00;
                int _wrote_o = (dst_out != 0x1F) && (vec_result || !_sca_active);
                if (_wrote_o)
                    emit_store(&b, "o[%u]", dst_out & 15, m, cond_test, cond);
                if (dst_tmp != 0x3F)
                    emit_store(&b, "r[%u]", dst_tmp & 31, m, cond_test, cond);
                if (!_wrote_o && dst_tmp == 0x3F && (cond_update_0 || cond_update_1))
                    emit_store(&b, "cc%u", cond_reg, m, cond_test, cond);
                emit(&b, "    }\n");
            }
        }

        /* ---- scalar ALU (reads SRC2) ---- */
        if ((smx|smy|smz|smw) && sca_op != 0x00 &&
            !vp_sca_is_flow(sca_op)) {
            char rhs[256];
            switch (sca_op) {
            case 0x01: snprintf(rhs,sizeof rhs,"%s",C); break;                /* MOV */
            case 0x02: snprintf(rhs,sizeof rhs,"(1.0/(%s).x)",C); break;      /* RCP */
            case 0x03: snprintf(rhs,sizeof rhs,"clamp(1.0/(%s).x, 5.42101e-20, 1.884467e19)",C); break; /* RCC */
            case 0x04: snprintf(rhs,sizeof rhs,"rsqrt(max(abs((%s).x), 1e-10))",C); break; /* RSQ */
            case 0x05: /* EXP: (2^floor(x), frac(x), 2^x, 1) */
                snprintf(rhs,sizeof rhs,
                    "float4(exp2(floor((%s).x)), frac((%s).x), exp2((%s).x), 1.0)",
                    C, C, C);
                break;
            case 0x06: /* LOG: (floor(log2|x|), |x|/2^floor(log2|x|), log2|x|, 1) */
                snprintf(rhs,sizeof rhs,
                    "float4(floor(log2(max(abs((%s).x), 1e-38))), "
                    "abs((%s).x) / exp2(floor(log2(max(abs((%s).x), 1e-38)))), "
                    "log2(max(abs((%s).x), 1e-38)), 1.0)",
                    C, C, C, C);
                break;
            case 0x0D: snprintf(rhs,sizeof rhs,"log2(max(abs((%s).x), 1e-10))",C); break; /* LG2 */
            case 0x0E: snprintf(rhs,sizeof rhs,"exp2((%s).x)",C); break;      /* EX2 */
            case 0x0F: snprintf(rhs,sizeof rhs,"sin((%s).x)",C); break;       /* SIN */
            case 0x10: snprintf(rhs,sizeof rhs,"cos((%s).x)",C); break;       /* COS */
            case 0x07: /* LIT: (1, max(s.x,0), s.x>0 ? 2^(s.w*log2(max(s.y,eps))) : 0, 1) */
                snprintf(rhs,sizeof rhs,
                    "float4(1.0, max((%s).x, 0.0), ((%s).x > 0.0) ? "
                    "exp2(clamp((%s).w, -127.996, 127.996) * log2(max((%s).y, 1e-10))) : 0.0, 1.0)",
                    C, C, C, C);
                break;
            default:
                emit(&b, "    /* TODO: unhandled VP scalar op */\n");
                rhs[0]='\0';
                break;
            }
            if (rhs[0]) {
                char m[6]; mask_str(smx,smy,smz,smw,m);
                char line[700];
                snprintf(line,sizeof line,"    { float4 _v = (float4)(%s);%s\n",
                         rhs, saturate ? " _v = saturate(_v);" : "");
                emit(&b, line);
                if (cond_test && cond > 0 && cond < 7) {
                    snprintf(line, sizeof line, "        float4 _cc = cc%u.%s;\n",
                             cond_reg, cc_swz);
                    emit(&b, line);
                }
                /* SCA writes the output register when the VEC unit does not
                 * own it; otherwise it targets its own temp. */
                int _wrote_o = !vec_result && dst_out != 0x1F;
                if (_wrote_o)
                    emit_store(&b, "o[%u]", dst_out & 15, m, cond_test, cond);
                if (sca_dst_tmp != 0x3F)
                    emit_store(&b, "r[%u]", sca_dst_tmp & 31, m, cond_test, cond);
                if (!_wrote_o && sca_dst_tmp == 0x3F && (cond_update_0 || cond_update_1))
                    emit_store(&b, "cc%u", cond_reg, m, cond_test, cond);
                emit(&b, "    }\n");
            }
        }

        /* Flow executes after the co-issued vector op. Branch predicates use
         * CC regardless of the ordinary destination-predication enable bit. */
        if (has_flow && vp_sca_is_flow(sca_op)) {
            char cond_expr[128], line[384];
            u32 target = vp_flow_addr(d0, d2, d3);
            vp_flow_cc_expr(cond, cond_reg, cc_swz,
                            cond_expr, sizeof(cond_expr));
            switch (sca_op) {
            case 0x08: /* BRA: dynamic address register target */
                snprintf(line, sizeof(line),
                         "            if (%s) { _next = a%u.%c; _flow_taken = true; }\n",
                         cond_expr, addr_sel & 1u, SWZ[addr_swz & 3u]);
                emit(&b, line);
                break;
            case 0x09: /* BRI: immediate target, CC predicate */
                snprintf(line, sizeof(line),
                         "            if (%s) { _next = %u; _flow_taken = true; }\n",
                         cond_expr, target);
                emit(&b, line);
                break;
            case 0x0A: /* CAL */
            case 0x0B: /* CLI */
                snprintf(line, sizeof(line),
                         "            if (%s) { if (_flow_sp < 4) { _flow_stack[_flow_sp] = %u; _flow_kind[_flow_sp++] = 1; _next = %u; _flow_taken = true; } else _flow_done = true; }\n",
                         cond_expr, pc + 1u, target);
                emit(&b, line);
                break;
            case 0x0C: /* RET */
                snprintf(line, sizeof(line),
                         "            if (%s) { if (_flow_sp > 0 && _flow_kind[_flow_sp - 1] == 1) { _next = _flow_stack[--_flow_sp]; _flow_taken = true; } else { _flow_done = true; _flow_taken = true; } }\n",
                         cond_expr);
                emit(&b, line);
                break;
            case 0x11: { /* BRB: boolean constant predicate */
                u32 bit = (d3 >> 23) & 31u;
                int branch_true = (int)((d3 >> 28) & 1u);
                snprintf(line, sizeof(line),
                         "            if (((transform_branch_bits >> %u) & 1u) %s 0u) { _next = %u; _flow_taken = true; }\n",
                         bit, branch_true ? "!=" : "==", target);
                emit(&b, line);
                break; }
            case 0x12: { /* CLB: boolean-constant call */
                u32 bit = (d3 >> 23) & 31u;
                int branch_true = (int)((d3 >> 28) & 1u);
                snprintf(line, sizeof(line),
                         "            if (((transform_branch_bits >> %u) & 1u) %s 0u) { if (_flow_sp < 4) { _flow_stack[_flow_sp] = %u; _flow_kind[_flow_sp++] = 1; _next = %u; _flow_taken = true; } else _flow_done = true; }\n",
                         bit, branch_true ? "!=" : "==", pc + 1u, target);
                emit(&b, line);
                break; }
            case 0x13: /* PUSHA: push the selected whole address register */
                snprintf(line, sizeof(line),
                         "            if (_flow_sp < 4) { _flow_addr[_flow_sp] = a%u; _flow_kind[_flow_sp++] = 2; } else _flow_done = true;\n",
                         addr_sel & 1u);
                emit(&b, line);
                break;
            case 0x14: /* POPA: top entry must have come from PUSHA */
                snprintf(line, sizeof(line),
                         "            if (_flow_sp > 0 && _flow_kind[_flow_sp - 1] == 2) a%u = _flow_addr[--_flow_sp]; else _flow_done = true;\n",
                         addr_sel & 1u);
                emit(&b, line);
                break;
            }
        }

        if (has_flow) {
            if (d3 & 1u)
                emit(&b, "            if (!_flow_taken) _flow_done = true;\n");
            emit(&b,
                 "            if (!_flow_done) _pc = _next;\n"
                 "            break; }\n");
        } else if (d3 & 1u) {
            break; /* end */
        }
    }

    if (has_flow) {
        emit(&b,
             "        default: _flow_done = true; break;\n"
             "        }\n"
             "    }\n");
    }

    if (b.overflow) return -1;

    /* ---- assemble full shader ---- */
    Out o = { out, out_size, 0, 0 };
    for (u32 unit = 0; unit < 4u; ++unit) {
        char decl[128];
        u32 dim = (texture_dimensions >> (unit * 3u)) & 7u;
        const char* type = dim == 1u ? "Texture1D<float4>" :
                           dim == 3u ? "Texture3D<float4>" :
                           dim == 4u ? "TextureCube<float4>" :
                                       "Texture2D<float4>";
        snprintf(decl, sizeof(decl),
                 "%s vp_tex%u : register(t%u); SamplerState vp_smp%u : register(s%u);\n",
                 /* Vertex SRVs remain t16-t19 in the shared 20-wide table,
                  * while the vertex-only sampler table maps its four entries
                  * to legal VS s0-s3 registers. */
                 type, unit, 16u + unit, unit, unit);
        emit(&o, decl);
    }
    emit(&o,
        /* All 16 RSX vertex attributes arrive as float4 (the harness fetches
         * and converts them on the CPU). */
        "struct VSInput {\n"
        "    float4 a0:ATTR0;  float4 a1:ATTR1;  float4 a2:ATTR2;  float4 a3:ATTR3;\n"
        "    float4 a4:ATTR4;  float4 a5:ATTR5;  float4 a6:ATTR6;  float4 a7:ATTR7;\n"
        "    float4 a8:ATTR8;  float4 a9:ATTR9;  float4 a10:ATTR10; float4 a11:ATTR11;\n"
        "    float4 a12:ATTR12; float4 a13:ATTR13; float4 a14:ATTR14; float4 a15:ATTR15;\n"
        "};\n"
        "struct VSOutput {\n"
        "    float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1;\n"
        "    float4 bcol0:COLOR2; float4 bcol1:COLOR3;\n"
        "    float4 fog:FOG;\n"
        "    float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "    float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7;\n"
        "    float4 t8:TEXCOORD8; float4 t9:TEXCOORD9;\n"
        "    float4 clip0:SV_ClipDistance0; float2 clip1:SV_ClipDistance1;\n"
        "};\n"
        /* b0: 512 vec4 transform constants + the RSX viewport transform,
         * pre-mapped by the harness to D3D clip space:
         *   ndc.xyz = clip.xyz * vp_posscale.xyz + clip.w * vp_posoffset.xyz */
        "cbuffer VPConst : register(b0) {\n"
        "    float4 vp_c[512];\n"
        "    float4 vp_posscale;\n"
        "    float4 vp_posoffset;\n"
        "    uint transform_branch_bits;\n"
        "    uint vertex_output_mask;\n"
        "    uint user_clip_control;\n"
        "    float4 vp_texscale[4];\n"
        "};\n"
        "float rsx_user_clip(float value, uint plane) {\n"
        "    uint mode = (user_clip_control >> (plane * 4u)) & 15u;\n"
        "    if ((vertex_output_mask & (1u << (6u + plane))) == 0u) return 0.5;\n"
        "    if (mode == 1u) return -value;\n"
        "    if (mode == 2u) return value;\n"
        "    return 0.5;\n"
        "}\n"
        "VSOutput main(VSInput input) {\n"
        "    float4 v[16];\n"
        "    v[0]=input.a0;  v[1]=input.a1;  v[2]=input.a2;  v[3]=input.a3;\n"
        "    v[4]=input.a4;  v[5]=input.a5;  v[6]=input.a6;  v[7]=input.a7;\n"
        "    v[8]=input.a8;  v[9]=input.a9;  v[10]=input.a10; v[11]=input.a11;\n"
        "    v[12]=input.a12; v[13]=input.a13; v[14]=input.a14; v[15]=input.a15;\n"
        "    float4 r[32]; float4 o[16];\n"
        "    [unroll] for (int _i=0;_i<32;_i++) r[_i]=(float4)0;\n"
        "    [unroll] for (int _j=0;_j<16;_j++) o[_j]=float4(0,0,0,1);\n"
        "    int4 a0 = (int4)0; int4 a1 = (int4)0;\n"
        "    float4 cc0 = (float4)0; float4 cc1 = (float4)0;\n");

    emit(&o, body);

    /* Output register map (see header comment): o0 = HPOS through the RSX
     * viewport transform, o1/o2 = colors, o5.x = FOGC (z,0,0,0), o7..o14 = TEX0..7,
     * o15 = TEX8, and o6 is point-size/TEX9. */
    emit(&o,
        "    VSOutput Out;\n"
        "    float4 _p = o[0];\n"
        "    Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);\n"
        "    Out.col0 = o[1]; Out.col1 = o[2]; Out.bcol0 = o[3]; Out.bcol1 = o[4];\n"
        "    Out.fog = float4(o[5].x,0,0,0);\n"
        "    Out.t0=o[7];  Out.t1=o[8];  Out.t2=o[9];  Out.t3=o[10];\n"
        "    Out.t4=o[11]; Out.t5=o[12]; Out.t6=o[13]; Out.t7=o[14];\n"
        "    Out.t8=o[15]; Out.t9=o[6];\n"
        "    Out.clip0 = float4(rsx_user_clip(o[5].y,0u), rsx_user_clip(o[5].z,1u),\n"
        "                       rsx_user_clip(o[5].w,2u), rsx_user_clip(o[6].y,3u));\n"
        "    Out.clip1 = float2(rsx_user_clip(o[6].z,4u), rsx_user_clip(o[6].w,5u));\n"
        "    return Out;\n"
        "}\n");

    (void)n_flow_skipped;
    if (o.overflow) return -1;
    return instrs;
}

int rsx_vp_decompile_entry(const u8* ucode, u32 max_bytes, u32 entry_instr,
                           char* out, u32 out_size)
{
    return rsx_vp_decompile_impl(ucode, max_bytes, entry_instr, 0x492u,
                                 out, out_size);
}

int rsx_vp_decompile_entry_tex(const u8* ucode, u32 max_bytes, u32 entry_instr,
                               u32 texture_dimensions, char* out, u32 out_size)
{
    return rsx_vp_decompile_impl(ucode, max_bytes, entry_instr,
                                 texture_dimensions, out, out_size);
}

int rsx_vp_decompile(const u8* ucode, u32 max_bytes, char* out, u32 out_size)
{
    return rsx_vp_decompile_impl(ucode, max_bytes, 0, 0x492u, out, out_size);
}
