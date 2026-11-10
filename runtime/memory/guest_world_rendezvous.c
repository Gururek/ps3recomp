#include "guest_world_rendezvous.h"

#ifdef _WIN32

#include <windows.h>
#include <string.h>

#include "../spu/spu_reservation.h"

#define GUEST_WORLD_MAX_THREADS 128u

typedef struct GuestWorldSlot {
    HANDLE handle;
    DWORD tid;
    DWORD prior_suspend_count;
    unsigned char used;
    unsigned char suspended;
    char name[24];
} GuestWorldSlot;

static INIT_ONCE s_init_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_lock;
static GuestWorldSlot s_slots[GUEST_WORLD_MAX_THREADS];
static unsigned s_count;
static unsigned s_capacity_limit = GUEST_WORLD_MAX_THREADS;
static DWORD s_exclusive_owner;
static int s_exclusive;
static volatile LONG s_poisoned;
static __declspec(thread) int s_tls_slot = -1;
static __declspec(thread) int s_tls_exclusive;

#ifdef GUEST_WORLD_RENDEZVOUS_TESTING
static guest_world_test_suspend_fn s_test_suspend;
static guest_world_test_resume_fn s_test_resume;
static guest_world_test_live_fn s_test_live;
#endif

static BOOL CALLBACK guest_world_init_once(PINIT_ONCE once, PVOID param,
                                           PVOID* context)
{
    (void)once; (void)param; (void)context;
    InitializeCriticalSection(&s_lock);
    return TRUE;
}

static int guest_world_init(void)
{
    return InitOnceExecuteOnce(&s_init_once, guest_world_init_once, NULL, NULL) ? 1 : 0;
}

static void guest_world_poison_locked(void)
{
    InterlockedExchange(&s_poisoned, 1);
}

static int guest_world_slot_live(HANDLE handle)
{
#ifdef GUEST_WORLD_RENDEZVOUS_TESTING
    if (s_test_live) return s_test_live((void*)handle);
#endif
    DWORD exit_code = 0;
    return GetExitCodeThread(handle, &exit_code) && exit_code == STILL_ACTIVE;
}

static DWORD guest_world_suspend(HANDLE handle)
{
#ifdef GUEST_WORLD_RENDEZVOUS_TESTING
    if (s_test_suspend) return s_test_suspend((void*)handle);
#endif
    return SuspendThread(handle);
}

static DWORD guest_world_resume(HANDLE handle)
{
#ifdef GUEST_WORLD_RENDEZVOUS_TESTING
    if (s_test_resume) return s_test_resume((void*)handle);
#endif
    return ResumeThread(handle);
}

int guest_world_register_current(const char* name)
{
    HANDLE duplicate = NULL;
    DWORD tid;
    unsigned slot = GUEST_WORLD_MAX_THREADS;

    if (!guest_world_init()) return 0;
    if (s_tls_slot >= 0 || s_tls_exclusive) {
        InterlockedExchange(&s_poisoned, 1);
        return 0;
    }
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &duplicate, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        InterlockedExchange(&s_poisoned, 1);
        return 0;
    }
    tid = GetCurrentThreadId();
    EnterCriticalSection(&s_lock);
    if (s_exclusive || InterlockedCompareExchange(&s_poisoned, 0, 0) ||
        s_count >= s_capacity_limit) {
        guest_world_poison_locked();
    } else {
        for (unsigned i = 0; i < GUEST_WORLD_MAX_THREADS; ++i) {
            if (s_slots[i].used && s_slots[i].tid == tid) {
                guest_world_poison_locked();
                break;
            }
            if (!s_slots[i].used && slot == GUEST_WORLD_MAX_THREADS)
                slot = i;
        }
        if (!InterlockedCompareExchange(&s_poisoned, 0, 0) &&
            slot != GUEST_WORLD_MAX_THREADS) {
            GuestWorldSlot* s = &s_slots[slot];
            memset(s, 0, sizeof(*s));
            s->handle = duplicate;
            s->tid = tid;
            s->used = 1;
            if (name) {
                strncpy(s->name, name, sizeof(s->name) - 1);
                s->name[sizeof(s->name) - 1] = '\0';
            }
            s_count++;
            s_tls_slot = (int)slot;
            duplicate = NULL;
        } else {
            guest_world_poison_locked();
        }
    }
    LeaveCriticalSection(&s_lock);
    if (duplicate) CloseHandle(duplicate);
    return s_tls_slot >= 0 && !guest_world_is_poisoned();
}

int guest_world_unregister_current(void)
{
    HANDLE handle = NULL;
    int slot = s_tls_slot;
    if (!guest_world_init() || slot < 0 || s_tls_exclusive) {
        InterlockedExchange(&s_poisoned, 1);
        return 0;
    }
    EnterCriticalSection(&s_lock);
    if ((unsigned)slot >= GUEST_WORLD_MAX_THREADS || !s_slots[slot].used ||
        s_slots[slot].tid != GetCurrentThreadId() || s_exclusive) {
        guest_world_poison_locked();
    } else {
        handle = s_slots[slot].handle;
        memset(&s_slots[slot], 0, sizeof(s_slots[slot]));
        s_count--;
        s_tls_slot = -1;
    }
    LeaveCriticalSection(&s_lock);
    if (handle) CloseHandle(handle);
    return handle != NULL && !guest_world_is_poisoned();
}

int guest_world_exclusive_begin(void)
{
    unsigned suspended = 0;
    DWORD self = GetCurrentThreadId();

    if (!guest_world_init() || s_tls_slot < 0 || s_tls_exclusive ||
        guest_world_is_poisoned()) return 0;
    EnterCriticalSection(&s_lock);
    if (s_exclusive || s_exclusive_owner) {
        LeaveCriticalSection(&s_lock);
        return 0; /* nested/other-owner request: reject without disturbing owner */
    }
    if (s_tls_slot < 0 || !s_slots[s_tls_slot].used ||
        s_slots[s_tls_slot].tid != self) {
        guest_world_poison_locked();
        LeaveCriticalSection(&s_lock);
        return 0;
    }
    s_exclusive = 1;
    s_exclusive_owner = self;
    spu_reservation_lock();
    for (unsigned i = 0; i < GUEST_WORLD_MAX_THREADS; ++i) {
        GuestWorldSlot* s = &s_slots[i];
        if (!s->used || s->tid == self) continue;
        if (!guest_world_slot_live(s->handle)) {
            guest_world_poison_locked();
            break;
        }
        DWORD prior = guest_world_suspend(s->handle);
        if (prior == (DWORD)-1) {
            guest_world_poison_locked();
            break;
        }
        s->prior_suspend_count = prior;
        s->suspended = 1;
        suspended++;
    }
    if (InterlockedCompareExchange(&s_poisoned, 0, 0)) {
        /* Match exclusive_end ordering: no guest thread may resume while the
         * reservation line is still locked by this failed rendezvous. */
        spu_reservation_unlock();
        for (unsigned i = GUEST_WORLD_MAX_THREADS; i-- > 0;) {
            GuestWorldSlot* s = &s_slots[i];
            if (s->suspended) {
                if (guest_world_resume(s->handle) == (DWORD)-1)
                    guest_world_poison_locked();
                s->suspended = 0;
            }
        }
        s_exclusive = 0;
        s_exclusive_owner = 0;
        LeaveCriticalSection(&s_lock);
        return 0;
    }
    (void)suspended;
    s_tls_exclusive = 1;
    return 1; /* registry + reservation locks remain held until exclusive_end */
}

int guest_world_exclusive_end(void)
{
    int ok = 1;
    if (!guest_world_init() || !s_tls_exclusive || !s_exclusive ||
        s_exclusive_owner != GetCurrentThreadId()) {
        return 0;
    }
    /* Guest reservation writes are released before suspended guest threads run. */
    spu_reservation_unlock();
    for (unsigned i = GUEST_WORLD_MAX_THREADS; i-- > 0;) {
        GuestWorldSlot* s = &s_slots[i];
        if (!s->suspended) continue;
        if (guest_world_resume(s->handle) == (DWORD)-1) {
            guest_world_poison_locked();
            ok = 0;
        }
        s->suspended = 0;
    }
    s_exclusive = 0;
    s_exclusive_owner = 0;
    s_tls_exclusive = 0;
    LeaveCriticalSection(&s_lock);
    return ok && !guest_world_is_poisoned();
}

int guest_world_exclusive_is_owned(void)
{
    return s_tls_exclusive && s_exclusive_owner == GetCurrentThreadId();
}

int guest_world_is_poisoned(void)
{
    return InterlockedCompareExchange(&s_poisoned, 0, 0) != 0;
}

unsigned guest_world_registered_count(void)
{
    unsigned count = 0;
    if (!guest_world_init()) return 0;
    EnterCriticalSection(&s_lock);
    count = s_count;
    LeaveCriticalSection(&s_lock);
    return count;
}

#ifdef GUEST_WORLD_RENDEZVOUS_TESTING
void guest_world_test_reset(void)
{
    if (!guest_world_init()) return;
    EnterCriticalSection(&s_lock);
    if (!s_exclusive) {
        for (unsigned i = 0; i < GUEST_WORLD_MAX_THREADS; ++i) {
            if (s_slots[i].handle) CloseHandle(s_slots[i].handle);
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
        }
        s_count = 0;
        s_capacity_limit = GUEST_WORLD_MAX_THREADS;
        InterlockedExchange(&s_poisoned, 0);
        s_test_suspend = NULL; s_test_resume = NULL; s_test_live = NULL;
    }
    s_tls_slot = -1;
    s_tls_exclusive = 0;
    LeaveCriticalSection(&s_lock);
}

void guest_world_test_set_hooks(guest_world_test_suspend_fn suspend_fn,
                                guest_world_test_resume_fn resume_fn,
                                guest_world_test_live_fn live_fn)
{
    EnterCriticalSection(&s_lock);
    s_test_suspend = suspend_fn; s_test_resume = resume_fn; s_test_live = live_fn;
    LeaveCriticalSection(&s_lock);
}

void guest_world_test_set_capacity_limit(unsigned limit)
{
    EnterCriticalSection(&s_lock);
    s_capacity_limit = limit <= GUEST_WORLD_MAX_THREADS ? limit : GUEST_WORLD_MAX_THREADS;
    LeaveCriticalSection(&s_lock);
}
#endif

#else

int guest_world_register_current(const char* name) { (void)name; return 1; }
int guest_world_unregister_current(void) { return 1; }
int guest_world_exclusive_begin(void) { return 0; }
int guest_world_exclusive_end(void) { return 0; }
int guest_world_exclusive_is_owned(void) { return 0; }
int guest_world_is_poisoned(void) { return 0; }
unsigned guest_world_registered_count(void) { return 0; }

#endif
