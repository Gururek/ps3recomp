#ifndef GUEST_WORLD_RENDEZVOUS_H
#define GUEST_WORLD_RENDEZVOUS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serializes a host-side, whole-guest-memory commit with guest execution.
 * A registered thread owns a duplicated real thread HANDLE until it unregisters.
 * On Windows exclusive_begin suspends every other registered live thread while
 * retaining the registry and SPU reservation locks.  It is intentionally a
 * fail-closed primitive: any registry/suspend/resume failure poisons it.
 */
int guest_world_register_current(const char* name);
int guest_world_unregister_current(void);
int guest_world_exclusive_begin(void);
int guest_world_exclusive_end(void);
int guest_world_exclusive_is_owned(void);
int guest_world_is_poisoned(void);
unsigned guest_world_registered_count(void);

#ifdef GUEST_WORLD_RENDEZVOUS_TESTING
typedef unsigned long (*guest_world_test_suspend_fn)(void* handle);
typedef unsigned long (*guest_world_test_resume_fn)(void* handle);
typedef int (*guest_world_test_live_fn)(void* handle);
void guest_world_test_reset(void);
void guest_world_test_set_hooks(guest_world_test_suspend_fn suspend_fn,
                                guest_world_test_resume_fn resume_fn,
                                guest_world_test_live_fn live_fn);
void guest_world_test_set_capacity_limit(unsigned limit);
#endif

#ifdef __cplusplus
}
#endif

#endif /* GUEST_WORLD_RENDEZVOUS_H */
