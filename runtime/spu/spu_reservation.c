/* Shared SPU reservation-line ownership and HLE atomic guest-memory updates. */
/* Coherency bitmap: an SPU that owns lines marks them here so main-memory
 * readers know which SPU-local lines are live. NULL until the first reserve.
 * (Newer runtimes keep this in spu_coh_bitmap.h / spu_coherency.c.) */
const unsigned char* g_spu_coh_bits = 0;

#include "spu_reservation.h"

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef _WIN32
#include <intrin.h>
#else
#include <x86intrin.h>   /* _mm_lfence et al. */
#endif
#include <stddef.h>
#include <string.h>

extern uint8_t* vm_base;

static volatile long g_resv_lock = 0;

/* Test-and-test-and-set: waiters spin on a plain read (no locked RMW bouncing
 * the line away from the holder) with a pause, and after a bounded spin yield
 * the core so a holder preempted inside its critical section can finish. */
#define RESV_LOCK_SPINS_BEFORE_YIELD 1024u

void spu_reservation_lock(void)
{
    unsigned spins = 0;
    while (_InterlockedExchange(&g_resv_lock, 1)) {
        while (g_resv_lock) {
            _mm_pause();
            if (++spins >= RESV_LOCK_SPINS_BEFORE_YIELD) {
                spins = 0;
#ifdef _WIN32
                SwitchToThread();
#endif
            }
        }
    }
}

void spu_reservation_unlock(void)
{
    _InterlockedExchange(&g_resv_lock, 0);
}

/* Comparing only bytes misses ABA: track every touched 128-byte line so an
 * intervening writer invalidates a stale GETLLAR reservation even if it later
 * restores the same data. */
#define RESV_VERSION_SLOTS 65536u
typedef struct ResvVersionSlot {
    uint32_t line_ea;
    uint32_t used;
    uint64_t version;
} ResvVersionSlot;

static ResvVersionSlot g_resv_versions[RESV_VERSION_SLOTS];

static ResvVersionSlot* version_slot_locked(uint32_t line_ea, int create)
{
    line_ea &= ~127u;
    uint32_t i = ((line_ea >> 7) * 2654435761u) &
                 (RESV_VERSION_SLOTS - 1u);
    for (uint32_t probe = 0; probe < RESV_VERSION_SLOTS; probe++) {
        ResvVersionSlot* slot = &g_resv_versions[i];
        if (!slot->used) {
            if (!create) return NULL;
            slot->used = 1;
            slot->line_ea = line_ea;
            slot->version = 0;
            return slot;
        }
        if (slot->line_ea == line_ea)
            return slot;
        i = (i + 1u) & (RESV_VERSION_SLOTS - 1u);
    }
    return NULL;
}

int spu_reservation_version_locked(uint32_t line_ea, uint64_t* version)
{
    ResvVersionSlot* slot = version_slot_locked(line_ea, 1);
    if (!slot) return 0;
    if (version) *version = slot->version;
    return 1;
}

void spu_reservation_bump_locked(uint32_t line_ea)
{
    /* Only queried lines can have an outstanding GETLLAR reservation. Bulk
     * image/HLE writes must not fill the finite table with unrelated lines and
     * prevent later atomic reads from acquiring a reservation. The shared lock
     * orders query and writer; entries are never removed, so every existing
     * reservation still receives its ABA-invalidating version increment. */
    ResvVersionSlot* slot = version_slot_locked(line_ea, 0);
    if (slot) slot->version++;
}

void spu_reservation_notify_write(uint32_t ea, uint32_t size)
{
    if (!size) return;
    uint32_t first = ea & ~127u;
    uint32_t last = (ea + size - 1u) & ~127u;
    spu_reservation_lock();
    for (uint32_t line = first;; line += 128u) {
        spu_reservation_bump_locked(line);
        if (line == last || line > UINT32_MAX - 128u) break;
    }
    spu_reservation_unlock();
}

/* Ordered plain PUT (see spu_reservation.h). Per host thread: only the thread
 * running a job of a lane window sets it, around that one job. */
#if defined(_MSC_VER)
static __declspec(thread) int t_put_ordered;
#else
static __thread int t_put_ordered;
#endif

int spu_reservation_set_put_ordered(int on)
{
    int old = t_put_ordered;
    t_put_ordered = on;
    return old;
}

int spu_reservation_put_ordered(void)
{
    return t_put_ordered;
}

void spu_reservation_put_copy(uint32_t ea, void* dst, const void* src,
                              uint32_t size)
{
    if (!size) return;
    uint32_t first = ea & ~127u;
    uint32_t last = (ea + size - 1u) & ~127u;
    spu_reservation_lock();
    memcpy(dst, src, size);
    for (uint32_t line = first;; line += 128u) {
        spu_reservation_bump_locked(line);
        if (line == last || line > UINT32_MAX - 128u) break;
    }
    spu_reservation_unlock();
}

int spu_reservation_compare_exchange64(uint32_t ea, uint64_t* expected,
                                       uint64_t desired)
{
    if (!expected || (ea & 7u)) return 0;

    spu_reservation_lock();
    const uint8_t* p = vm_base + ea;
    uint64_t current = ((uint64_t)p[0] << 56) |
                       ((uint64_t)p[1] << 48) |
                       ((uint64_t)p[2] << 40) |
                       ((uint64_t)p[3] << 32) |
                       ((uint64_t)p[4] << 24) |
                       ((uint64_t)p[5] << 16) |
                       ((uint64_t)p[6] << 8)  |
                       (uint64_t)p[7];
    int exchanged = current == *expected;
    if (exchanged) {
        uint8_t* d = vm_base + ea;
        d[0] = (uint8_t)(desired >> 56);
        d[1] = (uint8_t)(desired >> 48);
        d[2] = (uint8_t)(desired >> 40);
        d[3] = (uint8_t)(desired >> 32);
        d[4] = (uint8_t)(desired >> 24);
        d[5] = (uint8_t)(desired >> 16);
        d[6] = (uint8_t)(desired >> 8);
        d[7] = (uint8_t)desired;
        spu_reservation_bump_locked(ea);
    } else {
        *expected = current;
    }
    spu_reservation_unlock();
    return exchanged;
}
