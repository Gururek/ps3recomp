#include "spu_main_write_gate.h"

#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* A writer registered before begin() is allowed to finish; begin waits for
 * those preexisting tokens. A later overlap waits for end(); a later disjoint
 * PUT proceeds, but stays registered for the next gate generation. */
typedef struct spu_main_write_gate_state {
#ifdef _WIN32
    INIT_ONCE once;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE changed;
    DWORD owner;
#else
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t owner;
#endif
    uint64_t begin;
    uint64_t end;
    uint64_t generation;
    uint32_t active_writers;
    uint32_t draining_writers;
    int active;
    int owner_valid;
} spu_main_write_gate_state;

#ifdef _WIN32
static spu_main_write_gate_state s_gate = { INIT_ONCE_STATIC_INIT };

static BOOL CALLBACK spu_main_write_gate_init_once(PINIT_ONCE once, PVOID arg,
                                                    PVOID *context)
{
    (void)once;
    (void)arg;
    (void)context;
    InitializeCriticalSection(&s_gate.lock);
    InitializeConditionVariable(&s_gate.changed);
    return TRUE;
}

static int spu_main_write_gate_init(void)
{
    return InitOnceExecuteOnce(&s_gate.once, spu_main_write_gate_init_once,
                               NULL, NULL) ? 1 : 0;
}

#define GATE_LOCK() EnterCriticalSection(&s_gate.lock)
#define GATE_UNLOCK() LeaveCriticalSection(&s_gate.lock)
#define GATE_WAIT() SleepConditionVariableCS(&s_gate.changed, &s_gate.lock, INFINITE)
#define GATE_WAKE_ALL() WakeAllConditionVariable(&s_gate.changed)
#else
static spu_main_write_gate_state s_gate = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
    (pthread_t)0, 0, 0, 0, 0, 0, 0, 0
};

static int spu_main_write_gate_init(void) { return 1; }

#define GATE_LOCK() pthread_mutex_lock(&s_gate.lock)
#define GATE_UNLOCK() pthread_mutex_unlock(&s_gate.lock)
#define GATE_WAIT() pthread_cond_wait(&s_gate.changed, &s_gate.lock)
#define GATE_WAKE_ALL() pthread_cond_broadcast(&s_gate.changed)
#endif

static int gate_range(uint32_t ea, uint32_t size, uint64_t *begin,
                      uint64_t *end)
{
    const uint64_t first = ea;
    const uint64_t last = first + (uint64_t)size;
    if (!size || last <= first || last > (UINT64_C(1) << 32))
        return 0;
    *begin = first;
    *end = last;
    return 1;
}

static int gate_overlaps(uint64_t begin, uint64_t end)
{
    return begin < s_gate.end && end > s_gate.begin;
}

uint64_t spu_main_write_gate_begin(uint32_t ea, uint32_t size)
{
    uint64_t begin, end, token;
    if (!gate_range(ea, size, &begin, &end) || !spu_main_write_gate_init())
        return 0;

    GATE_LOCK();
    if (s_gate.active) {
        GATE_UNLOCK();
        return 0;
    }
    if (s_gate.generation >= UINT64_MAX - 1u) {
        GATE_UNLOCK();
        return 0;
    }
    ++s_gate.generation;
    s_gate.begin = begin;
    s_gate.end = end;
    s_gate.active = 1;
    s_gate.draining_writers = s_gate.active_writers;
    while (s_gate.draining_writers != 0)
        GATE_WAIT();
#ifdef _WIN32
    s_gate.owner = GetCurrentThreadId();
#else
    s_gate.owner = pthread_self();
#endif
    s_gate.owner_valid = 1;
    token = s_gate.generation;
    GATE_UNLOCK();
    return token;
}

int spu_main_write_gate_end(uint64_t token)
{
    if (!spu_main_write_gate_init())
        return 0;

    GATE_LOCK();
    if (!token || !s_gate.active || !s_gate.owner_valid ||
        token != s_gate.generation
#ifdef _WIN32
        || s_gate.owner != GetCurrentThreadId()
#else
        || !pthread_equal(s_gate.owner, pthread_self())
#endif
        ) {
        GATE_UNLOCK();
        return 0;
    }
    s_gate.active = 0;
    s_gate.owner_valid = 0;
    s_gate.draining_writers = 0;
    s_gate.begin = 0;
    s_gate.end = 0;
    GATE_WAKE_ALL();
    GATE_UNLOCK();
    return 1;
}

uint64_t spu_main_write_gate_writer_enter(uint32_t ea, uint32_t size)
{
    uint64_t begin, end, token;
    if (!gate_range(ea, size, &begin, &end) || !spu_main_write_gate_init())
        return 0;

    GATE_LOCK();
    while (s_gate.active && gate_overlaps(begin, end))
        GATE_WAIT();
    /* Every valid PUT stays accounted until its memcpy finishes.  A disjoint
     * PUT does not wait for the current gate, but a later gate still drains it
     * before publishing a new authoritative range. */
    if (s_gate.active_writers == UINT32_MAX) {
        GATE_UNLOCK();
        return 0;
    }
    ++s_gate.active_writers;
    token = s_gate.generation + 1u;
    GATE_UNLOCK();
    return token;
}

void spu_main_write_gate_writer_exit(uint64_t writer_token)
{
    if (!writer_token || !spu_main_write_gate_init())
        return;

    GATE_LOCK();
    if (s_gate.active_writers != 0) {
        --s_gate.active_writers;
        if (s_gate.active && writer_token <= s_gate.generation &&
            s_gate.draining_writers != 0)
            --s_gate.draining_writers;
        if (s_gate.draining_writers == 0)
            GATE_WAKE_ALL();
    }
    GATE_UNLOCK();
}

#ifdef SPU_MAIN_WRITE_GATE_TESTING
int spu_main_write_gate_test_state(void)
{
    int state;
    if (!spu_main_write_gate_init())
        return -1;
    GATE_LOCK();
    state = s_gate.active ? (s_gate.owner_valid ? 2 : 1) : 0;
    GATE_UNLOCK();
    return state;
}
#endif
