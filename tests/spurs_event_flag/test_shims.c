/* Unused title/runtime services referenced by cellSpurs.c's other sections. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "spu_workload.h"
#include "spurs_taskset.h"

int g_resv_store_active = 0;
volatile unsigned g_spurs_pm_polls = 0;
volatile uint32_t g_spu_event_flag_watch_ea = 0;
volatile long g_test_job_dispatches = 0;
volatile long g_rsx_survival_trace_armed = 0;
uint32_t ppu_vm_size = 0;

int tj_late_eventflag_trace_matches(uint32_t eventflag_ea) { (void)eventflag_ea; return 0; }
int tj_late_eventflag_trace_matches_jobchain(uint32_t jobchain_ea) { (void)jobchain_ea; return 0; }
int tj_late_eventflag_trace_arm(uint32_t eventflag_ea) { (void)eventflag_ea; return 0; }
void tj_late_eventflag_trace_disarm(void) {}
int tj_late_eventflag_trace_is_active(void) { return 0; }
void tj_late_eventflag_trace_snapshot(TJLateEventFlagCounters* out) { if (out) *out = (TJLateEventFlagCounters){0}; }
uint32_t tj_late_eventflag_trace_transition_snapshot(TJLateEventFlagTransition out[TJ_LATE_EVENTFLAG_TRANSITION_CAPACITY]) { (void)out; return 0; }
void tj_late_eventflag_trace_note_ppu_registration(uint32_t eventflag_ea, uint64_t before, uint64_t after) { (void)eventflag_ea; (void)before; (void)after; }
void tj_late_eventflag_trace_note_ppu_pending_clear(uint32_t eventflag_ea, uint64_t before, uint64_t after) { (void)eventflag_ea; (void)before; (void)after; }
void tj_late_eventflag_trace_note_getllar(uint32_t eventflag_ea, uint64_t memory, uint64_t line_version, int reservation_valid, uint32_t image_id, uint32_t pc) { (void)eventflag_ea; (void)memory; (void)line_version; (void)reservation_valid; (void)image_id; (void)pc; }
void tj_late_eventflag_trace_note_putllc(uint32_t eventflag_ea, int success, uint64_t memory, uint64_t reservation, uint64_t store, uint64_t line_version, uint64_t reservation_version, int reservation_valid, uint32_t image_id, uint32_t pc) { (void)eventflag_ea; (void)success; (void)memory; (void)reservation; (void)store; (void)line_version; (void)reservation_version; (void)reservation_valid; (void)image_id; (void)pc; }
void tj_late_eventflag_trace_note_image13_dispatch(uint32_t job_ea, uint32_t pc, int began) { (void)job_ea; (void)pc; (void)began; }
void tj_late_eventflag_trace_note_committed_atomic(uint32_t eventflag_ea, uint32_t pc) { (void)eventflag_ea; (void)pc; }
void tj_late_eventflag_trace_note_jobchain_request(uint32_t jobchain_ea, int start_worker) { (void)jobchain_ea; (void)start_worker; }
void tj_late_eventflag_trace_note_jobchain_thread_result(uint32_t jobchain_ea, int created) { (void)jobchain_ea; (void)created; }
void tj_late_eventflag_trace_note_jobchain_worker_register_failure(uint32_t jobchain_ea) { (void)jobchain_ea; }
typedef struct ppu_context ppu_context;
#ifdef _MSC_VER
__declspec(thread) ppu_context* g_active_ctx = NULL;
#else
__thread ppu_context* g_active_ctx = NULL;
#endif

void ppu_resv_break_store(uint64_t ea, uint32_t width) { (void)ea; (void)width; }
int guest_world_register_current(const char* name) { (void)name; return 1; }
int guest_world_unregister_current(void) { return 1; }
int guest_world_is_poisoned(void) { return 0; }
unsigned guest_world_registered_count(void) { return 0; }

uint64_t spu_workload_fingerprint(const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint,
                                          int* image_id_out)
{
    (void)fingerprint;
    if (image_id_out) *image_id_out = -1;
    return NULL;
}

int spu_run_policy_module(spu_lifted_entry_fn entry, int image_id,
                          const uint8_t* pm_host, uint32_t pm_size,
                          uint64_t wkl_data, uint32_t wid, uint32_t spurs_ea,
                          uint32_t spu_num)
{
    (void)entry; (void)image_id; (void)pm_host; (void)pm_size;
    (void)wkl_data; (void)wid; (void)spurs_ea; (void)spu_num;
    return -1;
}

int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size,
                              uint32_t job_ea, uint32_t job_desc_size,
                              uint32_t spurs_ea)
{
    (void)image; (void)image_size; (void)job_ea;
    (void)job_desc_size; (void)spurs_ea;
    g_test_job_dispatches++;
    return 0;
}

size_t spu_elf_image_size(const uint8_t* image, size_t max_avail)
{
    (void)image; (void)max_avail;
    return 0;
}

int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size,
                                uint32_t args_ea)
{
    (void)image; (void)image_size; (void)args_ea;
    return 0;
}

int spu_workload_dispatch_async_task(const uint8_t* image, uint32_t image_size,
                                     uint32_t args_ea, uint32_t taskset_ea,
                                     uint32_t task_id)
{
    (void)image; (void)image_size; (void)args_ea;
    (void)taskset_ea; (void)task_id;
    return 0;
}

void spu_taskset_register_signal_state(uint32_t taskset_ea)
{
    (void)taskset_ea;
}

void spu_taskset_unregister_signal_state(uint32_t taskset_ea)
{
    (void)taskset_ea;
}

void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t task_id)
{
    (void)taskset_ea; (void)task_id;
}

/* These unrelated task-lifecycle paths must not run in the EventFlag test. */
void spu_taskset_mark_task_started(uint32_t taskset_ea, uint32_t task_id)
{ (void)taskset_ea; (void)task_id; abort(); }
int spu_taskset_wait_task_exit(uint32_t taskset_ea, uint32_t task_id, int32_t* exit_code)
{ (void)taskset_ea; (void)task_id; (void)exit_code; abort(); }
int spu_taskset_request_shutdown(uint32_t taskset_ea)
{ (void)taskset_ea; abort(); }
int spu_taskset_wait_all_tasks_exit(uint32_t taskset_ea)
{ (void)taskset_ea; abort(); }

void spurs_taskset_init(uint32_t taskset_ea, uint32_t spurs_ea, uint64_t args,
                        uint32_t wid, uint32_t size, uint32_t evf1,
                        uint32_t evf2)
{
    (void)taskset_ea; (void)spurs_ea; (void)args; (void)wid;
    (void)size; (void)evf1; (void)evf2;
}

void spurs_taskset_add_task(uint32_t taskset_ea, uint32_t task_id,
                            uint64_t elf_ea, uint64_t context,
                            const uint32_t arg[4],
                            const uint32_t ls_pattern[4])
{
    (void)taskset_ea; (void)task_id; (void)elf_ea;
    (void)context; (void)arg; (void)ls_pattern;
}

int sys_event_queue_push_by_id(uint32_t queue_id, uint64_t source,
                               uint64_t data1, uint64_t data2, uint64_t data3)
{
    (void)queue_id; (void)source; (void)data1; (void)data2; (void)data3;
    return 0;
}
