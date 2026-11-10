#pragma once

/* Windows host-thread handoff for an asynchronous SPURS job chain.
 *
 * The worker publishes a parked JobGuard before it can clear `running`. A PPU
 * notification can therefore request a rerun while `running` is still one.
 * Keep that request latched until either the existing worker consumes it or a
 * new worker successfully claims ownership. */
#ifdef _WIN32
#include <Windows.h>

static inline int jobchain_request_run(volatile long* running,
                                       volatile long* run_requested)
{
    InterlockedExchange(run_requested, 1);
    return InterlockedCompareExchange(running, 1, 0) == 0;
}

static inline void jobchain_worker_begin(volatile long* run_requested)
{
    InterlockedExchange(run_requested, 0);
}

static inline int jobchain_worker_should_continue(volatile long* running,
                                                  volatile long* run_requested)
{
    if (InterlockedCompareExchange(run_requested, 0, 0) != 0)
        return 1;

    InterlockedExchange(running, 0);

    /* Close the race between the requester's running-state check and the
     * worker's release. If another requester already claimed `running`, that
     * requester owns the new host thread and this worker may exit. */
    if (InterlockedCompareExchange(run_requested, 0, 0) == 0)
        return 0;
    return InterlockedCompareExchange(running, 1, 0) == 0;
}
#endif
