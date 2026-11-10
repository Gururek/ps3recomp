/* Small non-branch opcode set used by runtime-generated WWS SPU stubs. */
#ifndef PS3RECOMP_SPU_SMC_OPS_H
#define PS3RECOMP_SPU_SMC_OPS_H

#include "spu_context.h"
#include "spu_helpers.h"

static inline int spu_smc_execute_nonbranch(spu_context* ctx, uint32_t word)
{
    const uint32_t op11 = word >> 21;
    const uint32_t op7 = word >> 25;
    const uint32_t op4 = word >> 28;
    const uint32_t rt = word & 0x7F;
    const uint32_t rt4 = (word >> 21) & 0x7F;
    const uint32_t ra = (word >> 7) & 0x7F;
    const uint32_t rb = (word >> 14) & 0x7F;
    const uint32_t rc = word & 0x7F;
    const uint32_t i7 = (word >> 14) & 0x7F;
    const int32_t i10 = (int32_t)(word << 8) >> 22;
    const uint32_t i16 = (word >> 7) & 0xFFFF;

    if (op4 == 0xB) { /* shufb rt,ra,rb,rc */
        ctx->gpr[rt4] = spu_shufb(ctx->gpr[ra], ctx->gpr[rb], ctx->gpr[rc]);
        return 1;
    }
    if (op11 == 0x1DC) { /* rotqby rt,ra,rb */
        ctx->gpr[rt] = spu_rotqby(ctx->gpr[ra], ctx->gpr[rb]);
        return 1;
    }
    if (op11 == 0x144 || op11 == 0x1C4) { /* stqx / lqx rt,ra,rb */
        uint32_t lsa = ctx->gpr[ra]._u32[0] + ctx->gpr[rb]._u32[0];
        if (op11 == 0x144)
            spu_ls_write128(ctx, lsa, ctx->gpr[rt]);
        else
            ctx->gpr[rt] = spu_ls_read128(ctx, lsa);
        return 1;
    }
    if (op11 == 0x1FC) { /* rotqbyi rt,ra,i7 */
        ctx->gpr[rt] = spu_rotqbyi(ctx->gpr[ra], (int)(int8_t)i7);
        return 1;
    }
    if (op11 == 0x2B6) { /* xsbh rt,ra */
        ctx->gpr[rt] = spu_xsbh(ctx->gpr[ra]);
        return 1;
    }
    if (op11 == 0x2C2) { /* fcgt rt,ra,rb */
        ctx->gpr[rt] = spu_fcgt(ctx->gpr[ra], ctx->gpr[rb]);
        return 1;
    }
    if (op11 == 0x2C6) { /* fm rt,ra,rb */
        ctx->gpr[rt] = spu_fm(ctx->gpr[ra], ctx->gpr[rb]);
        return 1;
    }
    if ((word >> 24) == 0x4E) { /* cgtbi rt,ra,i10 */
        ctx->gpr[rt] = spu_cgtbi(ctx->gpr[ra], i10);
        return 1;
    }
    if ((word >> 23) == 0x065) { /* fsmbi rt,i16 */
        ctx->gpr[rt] = spu_fsmbi((int32_t)i16);
        return 1;
    }
    if (op11 >= 0x1F4 && op11 <= 0x1F7) { /* cbd/chd/cwd/cdd rt,ra,i7 */
        switch (op11) {
        case 0x1F4:
            ctx->gpr[rt] = spu_cbd(ctx->gpr[ra], (int)i7);
            break;
        case 0x1F5:
            ctx->gpr[rt] = spu_chd(ctx->gpr[ra], (int)i7);
            break;
        case 0x1F6:
            ctx->gpr[rt] = spu_cwd(ctx->gpr[ra], (int)i7);
            break;
        default:
            ctx->gpr[rt] = spu_cdd(ctx->gpr[ra], (int)i7);
            break;
        }
        return 1;
    }
    if (op11 == 0x1FF) { /* shlqbyi rt,ra,i7 */
        ctx->gpr[rt] = spu_shlqbyi(ctx->gpr[ra], (int)(int8_t)i7);
        return 1;
    }
    if (op7 == 0x08 || op7 == 0x09) { /* hbra / hbrr performance hints */
        return 1;
    }
    return 0;
}

#endif
