#ifndef SPU_RESERVATION_H
#define SPU_RESERVATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Shared lock/version table for all SPU GETLLAR/PUTLLC operations and HLE
 * writers that touch the same 128-byte guest reservation lines. The *_locked
 * helpers require the caller to hold spu_reservation_lock(). Version queries
 * create persistent tracked entries; writes invalidate existing entries only. */
void spu_reservation_lock(void);
void spu_reservation_unlock(void);
int spu_reservation_version_locked(uint32_t line_ea, uint64_t* version);
void spu_reservation_bump_locked(uint32_t line_ea);

void spu_reservation_notify_write(uint32_t ea, uint32_t size);

/* Ordered plain DMA PUT. On hardware a PUT that touches a reserved line kills
 * the reservation, so a PUTLLC racing it fails and retries. A plain host
 * memcpy can instead land between a PUTLLC's compare and its commit and be
 * overwritten. While the calling thread's flag is set (job-chain lane windows
 * only, TJ_JOBCHAIN_LANES>=2), the DMA PUT path copies under the reservation
 * lock and bumps every touched line's version. Off, nothing changes.
 * set_put_ordered returns the previous value for the caller to restore. */
int spu_reservation_set_put_ordered(int on);
int spu_reservation_put_ordered(void);
void spu_reservation_put_copy(uint32_t ea, void* dst, const void* src,
                              uint32_t size);
int spu_reservation_compare_exchange64(uint32_t ea, uint64_t* expected,
                                       uint64_t desired);

#ifdef __cplusplus
}
#endif

#endif /* SPU_RESERVATION_H */
