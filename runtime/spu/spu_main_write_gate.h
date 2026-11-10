/*
 * Narrow ordering gate for guest MAIN writes that race an RSX exact
 * LOCAL->MAIN completion.  The gate is intentionally independent of the
 * guest-world rendezvous: it delays only overlapping regular SPU PUTs.
 */
#ifndef SPU_MAIN_WRITE_GATE_H
#define SPU_MAIN_WRITE_GATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Publish [ea, ea + size) and wait for PUTs already in flight.  Returns an
 * opaque nonzero generation token, or zero for invalid/nested requests. */
uint64_t spu_main_write_gate_begin(uint32_t ea, uint32_t size);

/* Release a successful begin from its owner.  The exact begin token is
 * required.  Returns 1 on success, 0 for stale/invalid/wrong-owner release. */
int spu_main_write_gate_end(uint64_t token);

/* Called by a regular MFC PUT immediately before its memcpy.  Returns an
 * opaque nonzero writer token, or zero for invalid/overflow.  Every success
 * must pair with writer_exit(token), including a disjoint active-gate PUT. */
uint64_t spu_main_write_gate_writer_enter(uint32_t ea, uint32_t size);
void spu_main_write_gate_writer_exit(uint64_t writer_token);

#ifdef SPU_MAIN_WRITE_GATE_TESTING
/* 0 idle, 1 published/draining, 2 published/owned.  Test-only seam. */
int spu_main_write_gate_test_state(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
