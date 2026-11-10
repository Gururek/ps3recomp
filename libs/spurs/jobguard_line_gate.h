#pragma once

/* The JobGuard count and parked flag live on one 128-byte SPU reservation
 * line.  Call these only while that line is serialized. */
#include <stdint.h>

typedef struct jobguard_line_state {
    uint32_t count;
    uint32_t parked;
} jobguard_line_state;

/* Returns nonzero when the chain must park. A final notification which wins
 * first leaves count zero, so this clears a stale parked flag and lets the
 * worker keep walking instead of creating a missed wake. */
static inline int jobguard_line_try_park(jobguard_line_state* state)
{
    if (!state->count) {
        state->parked = 0;
        return 0;
    }
    state->parked = 1;
    return 1;
}

/* Returns -1 for an empty guard, 0 for a non-final notification, or 1 when
 * the final notification must restart an already parked chain. */
static inline int jobguard_line_notify(jobguard_line_state* state)
{
    if (!state->count) return -1;
    state->count--;
    return state->count == 0 && state->parked != 0;
}
