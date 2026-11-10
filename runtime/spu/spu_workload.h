/* spu_workload.h — SPU workload / task dispatch registry.
 *
 * The bridge cellSpurs needs: when a title registers a SPURS workload
 * (cellSpursAddWorkload, a "policy module" SPU binary) or a SPURS task
 * (cellSpursCreateTask, a task ELF), the runtime must actually RUN that SPU
 * program. We do not interpret SPU ELFs at runtime — we statically lift each
 * title's SPU binaries ahead of time (spu_lifter -> a native entry fn). This
 * layer maps a registered SPU image, by content FINGERPRINT, to its pre-lifted
 * native entry, loads the image into a fresh 256 KB local store, and runs it
 * with the SPURS task ABI (job/task arg EA in r3) via the lifted-job adapter.
 *
 * Flow:
 *   1) At init, the title's generated registration code calls
 *      spu_workload_register(fingerprint, lifted_entry, "name") once per lifted
 *      SPU binary. The fingerprint is FNV-1a-64 over the exact ELF image bytes
 *      (the same bytes the title later hands to cellSpurs), computed identically
 *      here and by the offline extractor (so a manifest can be emitted).
 *   2) cellSpursAddWorkload / cellSpursCreateTask call spu_workload_dispatch()
 *      with the guest image -> fingerprint -> lifted entry -> load LS -> run.
 *
 * This is title-agnostic ps3recomp runtime: the registry is populated by the
 * title's lifted SPU set; the dispatch mechanism is generic.
 */
#ifndef SPU_WORKLOAD_H
#define SPU_WORKLOAD_H

#include "spu_context.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_lifted_entry_fn)(spu_context*);

/* FNV-1a 64-bit over a byte range. The canonical workload fingerprint; the
 * offline extractor computes the same value so registrations and the images a
 * title passes to cellSpurs line up. */
uint64_t spu_workload_fingerprint(const void* data, size_t n);

/* Register one lifted SPU binary under its image fingerprint. `name` is borrowed
 * (kept by pointer for logging — pass a string literal / static). Idempotent on
 * fingerprint: a second register of the same fp updates the entry. */
void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name);

/* As spu_workload_register, but binds the image's indirect-branch table id
 * (the value passed to spu_begin_image() before this image's recomp_register).
 * dispatch sets the run context's image_id to this so spu_indirect_branch
 * resolves within the correct image when several share LS addresses. */
void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                               int image_id, const char* name);

/* Look up a lifted entry by fingerprint; NULL if none registered. */
spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint);

/* As spu_workload_find, but also returns the image's indirect-branch table id. */
spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint, int* image_id_out);

/* Run a SPURS policy module (raw PM binary loaded at LS 0xA00) under the HLE'd
 * kernel handoff: kernel context at LS 0x100, entry ABI of
 * cellSpursModuleEntry(context, ea) with the workload's u64 data in r4.
 * Returns 0 if the module exited back to the kernel, negative otherwise.
 * (spurs_policy.c) */
/* spu_num selects the module's VIRTUAL SPU (kernel context +0x1C8). The WWS
 * job manager keys its per-SPU ticket LANE off this (lane row = spuNum*16 in
 * the workload sync header) and its command lists contain cross-lane barrier
 * commands, so a workload configured for N SPUs must be dispatched once per
 * spu_num or the un-run lanes deadlock every barrier. */
int spu_run_policy_module(spu_lifted_entry_fn entry, int image_id,
                          const uint8_t* pm_host, uint32_t pm_size,
                          uint64_t wkl_data, uint32_t wid, uint32_t spurs_ea,
                          uint32_t spu_num);

/* Stage and enter one SPURS jobchain job (spurs_job.c). Unlike a policy module
 * or a task, a job binary is a raw image built by the SDK's job_elf-to-bin and
 * linked against Sony's job_start_w_crt.o, whose entry contract is
 *   cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
 * with r3 = ctx, r4 = job and BOTH pointers into local store. This lays out
 * LS the way jm2 does (per
 * _cellSpursCheckJob in the SDK's job_descriptor.h: binary+bss, ioBuffer,
 * oBuffer, unified scratch+stack, cacheBuffer[0..3], all 1024-aligned), fills
 * ioBuffer from the descriptor's input DMA list, builds the context, and runs
 * the lifted entry. `job_desc_size` is the jobchain's sizeJobDescriptor (0 =>
 * assume 256). Returns 0 if the job ran, negative if it could not be staged. */
int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size,
                      uint32_t spurs_ea);

/* Fingerprint `image`, look up its lifted entry, and run it as a SPURS jobchain
 * job via spu_run_spurs_job. Runs SYNCHRONOUSLY on the caller's thread: a job
 * is not persistent (it runs to completion and returns to the manager), and a
 * chain's jobs are ordered by its own NEXT/CALL/RET commands, so the jobchain
 * thread is the right place to run them. Returns 1 if a lifted image matched
 * and ran, 0 if none is registered for this binary. */
int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size,
                              uint32_t job_ea, uint32_t job_desc_size,
                              uint32_t spurs_ea);

/* Load a 32-bit big-endian SPU ELF image into a 256 KB local store: each PT_LOAD
 * segment is copied to its p_vaddr (BSS zero-filled). Returns 1 on success and
 * writes the entry vaddr (e_entry, which may legitimately be 0) to *entry_out;
 * returns 0 if `image` is not a valid SPU ELF or a segment is out of range.
 * `ls` must point to SPU_LS_SIZE bytes (caller-zeroed if a clean BSS is wanted). */
int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls,
                       uint32_t* entry_out);

/* Compute the true on-disk size of a 32-bit BE SPU ELF: the extent that spans
 * the section-header table and all program/section content (max p_offset+p_filesz
 * and non-NOBITS sh_offset+sh_size). This matches the offline extractor's sizing,
 * so fingerprints agree. Use it when a caller (e.g. cellSpursCreateTask) is handed
 * an SPU ELF pointer without an explicit length. `max_avail` bounds the read in
 * case the header is corrupt; returns 0 if `image` is not a valid SPU ELF. */
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail);

/* Dispatch a registered SPU image: fingerprint `image`, find its lifted entry,
 * load it into a fresh local store, and run it with `args_ea` in r3 (SPURS task
 * ABI). Returns 1 if an image matched and ran, 0 if no lifted entry is
 * registered for it (caller may fall back to logging/stubbing). */
int spu_workload_dispatch(const uint8_t* image, uint32_t image_size,
                          uint32_t args_ea);

/* Same, but runs the SPU job on its own detached host thread so the PPU caller
 * is not blocked. Required for persistent SPURS service/worker tasks that loop
 * waiting on PPU-side signals (running them inline deadlocks). */
int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size,
                                uint32_t args_ea);

/* Taskset-owned async dispatch.  Identity is explicit so concurrent PPU task
 * creation cannot overwrite a process-global handoff before the worker starts. */
int spu_workload_dispatch_async_task(const uint8_t* image, uint32_t image_size,
                                     uint32_t args_ea, uint32_t taskset_ea,
                                     uint32_t task_id);

/* Host-side delivery state for SPURS task WAIT_SIGNAL. The guest ABI exposes
 * one SIGNALLED bit per task: delivery is idempotent while that bit remains
 * set, and WAIT_SIGNAL consumes the bit. The host shadow must preserve those
 * level semantics instead of turning repeated sends into queued tokens. */
static inline uint16_t spu_task_signal_merge_pending(uint16_t pending)
{
    (void)pending;
    return 1;
}

static inline uint16_t spu_task_signal_consume_pending(uint16_t pending)
{
    (void)pending;
    return 0;
}

static inline uint16_t spu_task_signal_merge_observed_edge(
    uint16_t pending, int edge_observed)
{
    return edge_observed ? spu_task_signal_merge_pending(pending) : pending;
}

static inline int spu_task_signal_is_pending(
    uint16_t pending, int guest_bit_set)
{
    return pending != 0 || guest_bit_set;
}

/* A recovery retry is eligible only when the task is still parked on the
 * expected image, has no host/guest signal pending, and neither delivery nor
 * consumption advanced since this C904 generation was armed. */
static inline int spu_task_signal_generation_retry_candidate(
    uint16_t pending, int guest_bit_set, int waiter_active,
    int waiter_image_id, int expected_image_id,
    uint64_t delivery_epoch, uint64_t armed_delivery_epoch,
    uint64_t consume_epoch, uint64_t armed_consume_epoch)
{
    return !spu_task_signal_is_pending(pending, guest_bit_set) &&
           waiter_active && waiter_image_id == expected_image_id &&
           delivery_epoch == armed_delivery_epoch &&
           consume_epoch == armed_consume_epoch;
}

/* Follow-up epoch state: 1 means exactly one first pass was delivered and
 * consumed; 0 means a current-generation pass is still pending/partial; -1
 * means overshoot or an impossible ordering. Unsigned subtraction preserves
 * the monotonic wraparound relation. */
static inline int spu_task_signal_generation_followup_epoch_state(
    uint64_t delivery_epoch, uint64_t armed_delivery_epoch,
    uint64_t consume_epoch, uint64_t armed_consume_epoch)
{
    uint64_t delivered = delivery_epoch - armed_delivery_epoch;
    uint64_t consumed = consume_epoch - armed_consume_epoch;
    if (delivered == 1u && consumed == 1u) return 1;
    if ((delivered == 0u && consumed == 0u) ||
        (delivered == 1u && consumed == 0u)) return 0;
    return -1;
}

/* A bounded follow-up is eligible only after every managed worker consumed
 * exactly one first delivery, re-parked on the expected image, and no signal
 * or completion-producing pass remains pending. */
static inline int spu_task_signal_generation_followup_retry_candidate(
    uint16_t pending, int guest_bit_set, int waiter_active,
    int waiter_image_id, int expected_image_id,
    uint64_t delivery_epoch, uint64_t armed_delivery_epoch,
    uint64_t consume_epoch, uint64_t armed_consume_epoch)
{
    return !spu_task_signal_is_pending(pending, guest_bit_set) &&
           waiter_active && waiter_image_id == expected_image_id &&
           spu_task_signal_generation_followup_epoch_state(
               delivery_epoch, armed_delivery_epoch,
               consume_epoch, armed_consume_epoch) == 1;
}

/* A generation owns at most one retry. Caller atomically records the claimed
 * generation after this predicate succeeds. */
static inline int spu_task_signal_generation_can_claim(
    uint64_t current_generation, uint64_t requested_generation,
    uint64_t retry_delivered_generation)
{
    return current_generation != 0 &&
           current_generation == requested_generation &&
           retry_delivered_generation != current_generation;
}

/* Batch recovery may claim only for the exact live generation, after every
 * managed waiter passed the per-task guard, while completion remains zero. */
static inline int spu_task_signal_generation_retry_batch_ready(
    uint64_t current_generation, uint64_t requested_generation,
    uint64_t retry_delivered_generation, int metadata_matches,
    int all_waiters_ready, int completion_is_zero)
{
    return spu_task_signal_generation_can_claim(
               current_generation, requested_generation,
               retry_delivered_generation) &&
           metadata_matches && all_waiters_ready && completion_is_zero;
}

/* Follow-up recovery never creates CA9C debt, so it must not be rejected for
 * differing from the primary arm's stored debt policy. */
static inline int spu_task_signal_generation_retry_policy_matches(
    int armed_create_ca9c_substitution_debt,
    int requested_create_ca9c_substitution_debt, int followup_recovery)
{
    return followup_recovery ||
           armed_create_ca9c_substitution_debt ==
               (requested_create_ca9c_substitution_debt != 0);
}

static inline int spu_task_signal_generation_followup_can_claim(
    uint64_t current_generation, uint64_t requested_generation,
    uint64_t followup_retry_delivered_generation)
{
    return current_generation != 0 &&
           current_generation == requested_generation &&
           followup_retry_delivered_generation != current_generation;
}

/* Wait-owned synthetic batches replace future exact CA9C callbacks FIFO.
 * Debt persists across C904 arms because CA9C is an asynchronous callback;
 * generic HLE/EventFlag/SPU-atomic signals never consume it. */
static inline uint64_t spu_task_signal_add_ca9c_substitution_debt(
    uint64_t debt)
{
    return debt == UINT64_MAX ? debt : debt + 1u;
}

static inline uint64_t spu_task_signal_consume_ca9c_substitution_debt(
    uint64_t debt)
{
    return debt ? debt - 1u : 0;
}

enum {
    SPU_CA9C_ACTION_CONSUME_DEBT = 1,
    SPU_CA9C_ACTION_DELIVER_SIGNAL = 2,
};

/* Registered CA9C delivery is atomic: consume a recovery-created debt, or
 * perform one normal task signal while still holding the signal lock. */
static inline int spu_task_signal_ca9c_action(uint64_t debt)
{
    return debt ? SPU_CA9C_ACTION_CONSUME_DEBT
                : SPU_CA9C_ACTION_DELIVER_SIGNAL;
}

void spu_taskset_register_signal_state(uint32_t taskset_ea);
void spu_taskset_unregister_signal_state(uint32_t taskset_ea);
void spu_taskset_signal_task(uint32_t taskset_ea, uint32_t taskId);
/* Opt-in Tokyo Jungle signal provenance. The title arms this only after its
 * established scene-ready boundary; no delivery or guest state is changed. */
void spu_taskset_signal_trace_arm_after_scene(void);
void spu_taskset_mark_task_started(uint32_t taskset_ea, uint32_t taskId);
void spu_taskset_mark_task_exit(uint32_t taskset_ea, uint32_t taskId,
                                int32_t exit_code);
int spu_taskset_wait_task_exit(uint32_t taskset_ea, uint32_t taskId,
                               int32_t* exit_code);
int spu_taskset_request_shutdown(uint32_t taskset_ea);
int spu_taskset_wait_all_tasks_exit(uint32_t taskset_ea);
uint64_t spu_taskset_arm_signal_generation(uint32_t owner_ea,
                                            uint32_t taskset_ea,
                                            uint32_t task_ids_ea,
                                            uint32_t count,
                                            int expected_image_id,
                                            uint32_t completion_ea,
                                            int create_ca9c_substitution_debt);
/* Atomically arm and deliver a complete synthetic manager batch. The
 * matching future CA9C callbacks are consumed instead of double-signalling. */
uint64_t spu_taskset_dispatch_signal_generation(
    uint32_t owner_ea, uint32_t taskset_ea, uint32_t task_ids_ea,
    uint32_t count, int expected_image_id, uint32_t completion_ea,
    int create_ca9c_substitution_debt);
uint64_t spu_taskset_current_signal_generation(uint32_t owner_ea,
                                                uint32_t taskset_ea);
int spu_taskset_retry_signal_generation(uint32_t owner_ea,
                                         uint32_t taskset_ea,
                                         uint32_t task_ids_ea,
                                         uint32_t count,
                                         int expected_image_id,
                                         uint32_t completion_ea,
                                         uint64_t generation,
                                         int create_ca9c_substitution_debt,
                                         int followup_recovery);
/* Returns handled for a registered valid task: it consumed debt or delivered
 * the normal signal. Returns unhandled for the generated CA9C HLE fallback. */
int spu_taskset_consume_ca9c_substitution(
    uint32_t taskset_ea, uint32_t taskId);
/* Bracket SPU PUTLLC verdict/commit so taskset SIGNALLED guest bits and the
 * host wait shadow publish atomically. Lock order is signal then reservation. */
void spu_taskset_putllc_begin(void);
void spu_taskset_putllc_end(uint32_t taskset_ea,
                           const uint8_t before[128],
                           const uint8_t after[128], int success);
/* Returns 0 after consuming a signal, 1 when taskset shutdown interrupts the
 * wait. The syscall bridge turns shutdown into a clean task EXIT. */
int spu_taskset_wait_signal(uint32_t taskset_ea, uint32_t taskId,
                            int image_id, uint32_t wait_return_pc);

/* Read-only diagnostic snapshot. Captures registered taskset signal state
 * under the signal lock, then prints it after releasing that lock. */
void spu_taskset_signal_snapshot(FILE* out);

/* Read-only Tokyo Jungle completion diagnostic. Prints the retained image15
 * atomic history for the 128-byte line containing `completion_ea`. */
void spu_completion_atomic_history_snapshot(FILE* out, uint32_t completion_ea);

/* Invalidate GETLLAR reservations overlapping a non-SPU guest-memory write.
 * HLE code that modifies lock-line-owned structures calls this after writing. */
void spu_reservation_notify_write(uint32_t ea, uint32_t size);

/* Reservation-safe compare/exchange for the first 8 bytes of an SPU-owned
 * lock line. Values are expressed in guest (big-endian logical) order. */
int spu_reservation_compare_exchange64(uint32_t ea, uint64_t* expected,
                                       uint64_t desired);

/* Tokyo Jungle's late Survival EventFlag probe is dormant until cellSpurs
 * registers its exact 0x0001 waiter. Counters are process-global; the waiter
 * snapshots them before arming and reports only deltas. */
#define TJ_LATE_EVENTFLAG_TRANSITION_CAPACITY 8u

enum TJLateEventFlagTransitionKind {
    TJ_LATE_EVENTFLAG_TRANSITION_PPU_REGISTER = 1,
    TJ_LATE_EVENTFLAG_TRANSITION_GETLLAR,
    TJ_LATE_EVENTFLAG_TRANSITION_PUTLLC_SUCCESS,
    TJ_LATE_EVENTFLAG_TRANSITION_PUTLLC_FAIL,
    TJ_LATE_EVENTFLAG_TRANSITION_PPU_PENDING_CLEAR,
};

/* Fixed diagnostic ring. `memory`, `reservation`, and `store` are the first
 * eight bytes in guest big-endian logical order.  PPU records use memory/store
 * for before/after; GETLLAR uses memory/reservation for the loaded line. */
typedef struct TJLateEventFlagTransition {
    uint64_t sequence;
    uint64_t memory;
    uint64_t reservation;
    uint64_t store;
    uint64_t line_version;
    uint64_t reservation_version;
    uint32_t image_id;
    uint32_t pc;
    uint8_t kind;
    uint8_t success;
    uint8_t reservation_valid;
    uint8_t reserved;
} TJLateEventFlagTransition;

typedef struct TJLateEventFlagCounters {
    uint64_t image13_dispatch_begin;
    uint64_t image13_dispatch_end;
    uint64_t committed_atomics;
    uint64_t jobchain_requests;
    uint64_t jobchain_start_workers;
    uint64_t jobchain_thread_created;
    uint64_t jobchain_thread_create_failed;
    uint64_t jobchain_worker_register_failed;
    uint32_t last_image13_job;
    uint32_t last_image13_pc;
    uint32_t last_atomic_pc;
} TJLateEventFlagCounters;

int tj_late_eventflag_trace_matches(uint32_t eventflag_ea);
int tj_late_eventflag_trace_matches_jobchain(uint32_t jobchain_ea);
int tj_late_eventflag_trace_arm(uint32_t eventflag_ea);
void tj_late_eventflag_trace_disarm(void);
int tj_late_eventflag_trace_is_active(void);
void tj_late_eventflag_trace_snapshot(TJLateEventFlagCounters* out);
uint32_t tj_late_eventflag_trace_transition_snapshot(
    TJLateEventFlagTransition out[TJ_LATE_EVENTFLAG_TRANSITION_CAPACITY]);
void tj_late_eventflag_trace_note_ppu_registration(uint32_t eventflag_ea,
                                                   uint64_t before,
                                                   uint64_t after);
void tj_late_eventflag_trace_note_ppu_pending_clear(uint32_t eventflag_ea,
                                                    uint64_t before,
                                                    uint64_t after);
void tj_late_eventflag_trace_note_getllar(uint32_t eventflag_ea,
                                          uint64_t memory,
                                          uint64_t line_version,
                                          int reservation_valid,
                                          uint32_t image_id,
                                          uint32_t pc);
void tj_late_eventflag_trace_note_putllc(uint32_t eventflag_ea,
                                         int success,
                                         uint64_t memory,
                                         uint64_t reservation,
                                         uint64_t store,
                                         uint64_t line_version,
                                         uint64_t reservation_version,
                                         int reservation_valid,
                                         uint32_t image_id,
                                         uint32_t pc);
void tj_late_eventflag_trace_note_image13_dispatch(uint32_t job_ea,
                                                    uint32_t pc, int began);
void tj_late_eventflag_trace_note_committed_atomic(uint32_t eventflag_ea,
                                                    uint32_t pc);
void tj_late_eventflag_trace_note_jobchain_request(uint32_t jobchain_ea,
                                                    int start_worker);
void tj_late_eventflag_trace_note_jobchain_thread_result(uint32_t jobchain_ea,
                                                         int created);
void tj_late_eventflag_trace_note_jobchain_worker_register_failure(
    uint32_t jobchain_ea);

/* Number of currently registered lifted SPU binaries (diagnostics/tests). */
unsigned spu_workload_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_WORKLOAD_H */
