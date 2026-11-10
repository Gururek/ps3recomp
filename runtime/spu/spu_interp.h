/* spu_interp.h — SPU interpreter: the correctness floor for the SPU recompiler.
 *
 * Statically lifted functions are the fast path. Computed/indirect branches
 * (`bi $reg`) and code that only exists at runtime — DMA'd overlays (LBP),
 * self-modified code (spu_0004's `stqd; sync; bi $3` trampoline at LS 0xC0) —
 * have no static bytes to lift. On a lifted-function-table miss we interpret
 * from live local store instead of falling off a cliff.
 *
 * The interpreter shares the EXACT spu_context (gpr, ls, channels) the lifted
 * code uses and calls the SAME spu_<mnemonic> helpers, so control and state
 * cross the boundary transparently.
 */
#ifndef SPU_INTERP_H
#define SPU_INTERP_H

#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A lifted SPU function: void f(spu_context*). Matches the lifter's signature. */
typedef void (*spu_lifted_fn)(spu_context*);

/* Registered lifted-function lookup by local-store entry address for the image
 * currently selected in ctx->image_id. Weak: the default returns NULL (pure
 * interpretation). The lifted image's generated spu_function_table registration
 * overrides this so computed branches reach compiled code. */
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa);

/* Interpret from `start_lsa` until the SPU stops (`stop`/`stopd`/halt) or reaches
 * a registered lifted entry. Handles all control flow internally. Returns the
 * stop code after a stop/halt, or the lifted entry address for re-dispatch.
 * The optional YDKJ_SPU_STEPCAP diagnostic returns 0x2000. ctx->pc is the next
 * instruction after stop, the offending instruction after halt, or that entry. */
uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa);

/* Execute exactly the instruction at ctx->pc from live local store. This does
 * not consult lifted code or alter ctx->status before execution. Returns 1
 * when the instruction stops/halts the SPU. When non-NULL, branch_taken is
 * reset to 0 then set to 1 only for a control-flow branch that is taken. */
int spu_interp_step(spu_context* ctx, int* branch_taken);

/* The single dispatch point every computed branch routes through. If `target`
 * is a registered lifted entry, runs it; otherwise interprets from `target`.
 * The lifter emits `bi $reg` / unresolved indirect branches as a call here. */
void spu_dispatch(spu_context* ctx, uint32_t target);

#ifdef __cplusplus
}
#endif
#endif /* SPU_INTERP_H */
