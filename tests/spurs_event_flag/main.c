/*
 * Host-only regression for the SPURS SPU->PPU event-flag handshake used by
 * Tokyo Jungle's Survival loading path. No title executable or game assets are
 * loaded. A synthetic SPU completion updates the real 128-byte guest structure
 * through the shared reservation primitive, then uses the normal
 * cellSpursDeliverSpuEvent delivery path.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cellSpurs.h"
#include "spu_workload.h"
#include "../../runtime/ppu/ppu_context.h"

extern __declspec(thread) ppu_context* g_active_ctx;

#define GUEST_MEM_SIZE (4u * 1024u * 1024u)
#define SPURS_EA       0x00010000u
#define EVENT_FLAG_EA  0x00020000u
#define WAIT_MASK_EA   0x00030000u
#define JOB_ATTR_EA    0x00040000u
#define JOB_CHAIN_EA   0x00050000u
#define JOB_ENTRY_EA   0x00060000u
#define JOB_DESC_EA    0x00070000u
#define JOB_IMAGE_EA   0x00071000u
#define GUARD_FIRST_EA 0x00080000u
#define GUARD_SECOND_EA 0x00080100u

#define EF_PPU_WAIT_MASK 0x04u
#define EF_SPU_PORT      0x0Cu
#define EF_PENDING_RECV  0x30u

static uint8_t g_guest_mem[GUEST_MEM_SIZE];
uint8_t* vm_base = g_guest_mem;
static volatile LONG g_force_wait_registration_race;
static volatile LONG g_wait_registration_race_failed;
static volatile LONG g_queued_callback;
static volatile LONG g_callback_failed;

extern int cellSpursDeliverSpuEvent(uint32_t spurs_ea, uint32_t port,
                                    uint32_t data0, uint32_t data1);
extern int32_t _cellSpursEventFlagInitialize(void* spurs, void* taskset,
                                             CellSpursEventFlag* event_flag,
                                             uint32_t clear_mode,
                                             uint32_t direction);
extern int32_t _cellSpursJobChainAttributeInitialize(
    uint32_t jm_revision, uint32_t sdk_revision, uint64_t attr_ea,
    uint64_t entry_ea, uint32_t size_job_descriptor,
    uint32_t max_grabbed_job, uint64_t priority_ea,
    uint32_t max_contention);
extern int32_t cellSpursCreateJobChainWithAttribute(
    uint64_t spurs_ea, uint64_t job_chain_ea, uint64_t attr_ea);
extern int32_t cellSpursJobChainAttributeSetHaltOnError(
    uint64_t attr_ea, uint32_t halt_on_error);
extern int32_t cellSpursRunJobChain(uint64_t job_chain_ea);
extern int32_t cellSpursShutdownJobChain(uint64_t job_chain_ea);
extern int32_t cellSpursJoinJobChain(uint64_t job_chain_ea);
extern int32_t cellSpursJobGuardInitialize(uint64_t job_chain_ea, uint64_t guard_ea,
                                          uint32_t notify_count, uint8_t spu_count,
                                          uint8_t auto_reset);
extern int32_t cellSpursJobGuardNotify(uint64_t guard_ea);
extern volatile long g_test_job_dispatches;

/* Title-only diagnostic hooks referenced by the generic runtime. */
void lbp_breadcrumb_dump(const char* tag) { (void)tag; }
void lbp_hle_complete_pending(void) {}
void ppu_guest_callstack(const char* tag) { (void)tag; }
void ppu_log_host_chain(const char* tag) { (void)tag; }
void ydkj_release_pending_threads(void) {}
void ydkj_wake_all_event_flags(void) {}

static uint16_t read_be16(uint32_t ea)
{
    return (uint16_t)(((uint16_t)vm_base[ea] << 8) | vm_base[ea + 1]);
}

static void write_be16(uint32_t ea, uint16_t value)
{
    vm_base[ea] = (uint8_t)(value >> 8);
    vm_base[ea + 1] = (uint8_t)value;
}

static void write_be64(uint32_t ea, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        vm_base[ea + 7 - i] = (uint8_t)value;
        value >>= 8;
    }
}

static uint64_t read_be64(uint32_t ea)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value = (value << 8) | vm_base[ea + i];
    return value;
}

/* Deterministically place a synthetic SPU Set between EventFlagWait's first
 * control-word read and registration CAS.  The outer CAS must retry, consume
 * the queued event, and return without publishing a waiter. */
void ps3recomp_test_before_eventflag_wait_cas(uint32_t ea, uint64_t ctrl)
{
    if (InterlockedCompareExchange(&g_force_wait_registration_race, 0, 1) != 1)
        return;

    uint64_t expected = ctrl;
    const uint64_t queued = (UINT64_C(0x0001) << 48) | UINT64_C(0x0100);
    if (!spu_reservation_compare_exchange64(ea, &expected, queued))
        InterlockedExchange(&g_wait_registration_race_failed, 1);
}

typedef struct WaitArgs {
    int32_t rc;
    uint64_t thread_id;
} WaitArgs;

static DWORD WINAPI wait_thread(LPVOID opaque)
{
    WaitArgs* args = (WaitArgs*)opaque;
    ppu_context ctx = {0};
    ctx.thread_id = args->thread_id;
    g_active_ctx = args->thread_id ? &ctx : NULL;
    args->rc = cellSpursEventFlagWait(
        (CellSpursEventFlag*)(uintptr_t)EVENT_FLAG_EA,
        (uint16_t*)(uintptr_t)WAIT_MASK_EA,
        CELL_SPURS_EVENT_FLAG_OR);
    g_active_ctx = NULL;
    return 0;
}

static DWORD WINAPI callback_spu_completion(LPVOID unused)
{
    (void)unused;
    const uint16_t completion = read_be16(WAIT_MASK_EA);
    write_be16(EVENT_FLAG_EA + EF_PENDING_RECV, completion);
    uint64_t expected = read_be64(EVENT_FLAG_EA);
    if (!spu_reservation_compare_exchange64(EVENT_FLAG_EA, &expected, 1u) ||
        !cellSpursDeliverSpuEvent(SPURS_EA, vm_base[EVENT_FLAG_EA + EF_SPU_PORT], 0, 0))
        InterlockedExchange(&g_callback_failed, 1);
    return 0;
}

/* The real loader policy is separately compiled by test_ppu_callback_safe_point.
 * Here a queued main-thread callback starts an independent SPU producer. Its
 * delivery must acquire the EventFlag lock before the callback can return. */
void ppu_poll_display_callbacks(ppu_context* ctx)
{
    if (!ctx || ctx->thread_id != 1 ||
        InterlockedCompareExchange(&g_queued_callback, 0, 1) != 1) return;
    HANDLE producer = CreateThread(NULL, 0, callback_spu_completion, NULL, 0, NULL);
    if (!producer || WaitForSingleObject(producer, 1000) != WAIT_OBJECT_0)
        InterlockedExchange(&g_callback_failed, 1);
    if (producer) CloseHandle(producer);
}

static int fail(const char* message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static int wait_guard_parked(uint32_t guard_ea)
{
    ULONGLONG deadline = GetTickCount64() + 2000;
    while (read_be16(guard_ea + 0x32) != 1 && GetTickCount64() < deadline) Sleep(1);
    return read_be16(guard_ea + 0x32) == 1;
}

int main(void)
{
    const uint16_t completion = 0x0100u;
    memset(g_guest_mem, 0, sizeof(g_guest_mem));

    if (cellSpursInitialize((CellSpurs*)(uintptr_t)SPURS_EA,
                            1, 100, 100, 0) != CELL_OK)
        return fail("cellSpursInitialize");

    if (_cellSpursEventFlagInitialize(
            (void*)(uintptr_t)SPURS_EA, NULL,
            (CellSpursEventFlag*)(uintptr_t)EVENT_FLAG_EA,
            CELL_SPURS_EVENT_FLAG_CLEAR_AUTO,
            CELL_SPURS_EVENT_FLAG_SPU2PPU) != CELL_OK)
        return fail("_cellSpursEventFlagInitialize");

    if (cellSpursEventFlagAttachLv2EventQueue(
            (CellSpursEventFlag*)(uintptr_t)EVENT_FLAG_EA) != CELL_OK)
        return fail("cellSpursEventFlagAttachLv2EventQueue");

    write_be16(WAIT_MASK_EA, completion);
    WaitArgs args = { .rc = -1 };
    HANDLE thread = CreateThread(NULL, 0, wait_thread, &args, 0, NULL);
    if (!thread)
        return fail("CreateThread");

    /* Wait until the PPU waiter has atomically published its mask. */
    unsigned elapsed_ms = 0;
    while (read_be16(EVENT_FLAG_EA + EF_PPU_WAIT_MASK) != completion &&
           elapsed_ms < 2000) {
        Sleep(1);
        elapsed_ms++;
    }
    if (elapsed_ms == 2000)
        return fail("PPU wait mask was not registered");

    /* First synthetic SPU completion: publish the received bits, clear
     * ppuWaitMask, and set ppuPendingRecv. */
    write_be16(EVENT_FLAG_EA + EF_PENDING_RECV, completion);
    uint64_t expected = read_be64(EVENT_FLAG_EA);
    if (!spu_reservation_compare_exchange64(EVENT_FLAG_EA, &expected, 1u))
        return fail("synthetic SPU completion CAS");

    /* A second completion of the SAME bit arrives before the PPU consumes the
     * first. With no waiter currently registered, real SPU Set preserves it in
     * ctrl.events while leaving ppuPendingRecv set for the first completion. */
    expected = 1u;
    const uint64_t queued_completion = ((uint64_t)completion << 48) | 1u;
    if (!spu_reservation_compare_exchange64(EVENT_FLAG_EA, &expected,
                                             queued_completion))
        return fail("queued duplicate SPU completion CAS");
    const uint8_t port = vm_base[EVENT_FLAG_EA + EF_SPU_PORT];
    if (!cellSpursDeliverSpuEvent(SPURS_EA, port, 0, 0))
        return fail("SPU event delivery");

    if (WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0)
        return fail("PPU waiter did not wake");
    CloseHandle(thread);

    if (args.rc != CELL_OK)
        return fail("cellSpursEventFlagWait return code");
    if (read_be16(WAIT_MASK_EA) != completion)
        return fail("PPU waiter received wrong completion mask");
    if (read_be64(EVENT_FLAG_EA) != ((uint64_t)completion << 48))
        return fail("duplicate completion was lost while consuming pending state");

    /* The next wait must consume the queued duplicate immediately, without a
     * second wake or timeout. This is the regression for Survival's repeated
     * same-worker completion bits. */
    write_be16(WAIT_MASK_EA, completion);
    if (cellSpursEventFlagWait(
            (CellSpursEventFlag*)(uintptr_t)EVENT_FLAG_EA,
            (uint16_t*)(uintptr_t)WAIT_MASK_EA,
            CELL_SPURS_EVENT_FLAG_OR) != CELL_OK)
        return fail("queued duplicate wait return code");
    if (read_be16(WAIT_MASK_EA) != completion)
        return fail("queued duplicate returned wrong completion mask");
    if (read_be64(EVENT_FLAG_EA) != 0)
        return fail("queued duplicate was not consumed");

    /* Regression for the real Survival freeze: image13 queues completion in
     * the narrow window before the PPU registration CAS.  The failed first CAS
     * must not leave stale `registered` state on the immediate-success retry. */
    const uint16_t raced_completion = 0x0001u;
    write_be16(WAIT_MASK_EA, raced_completion);
    InterlockedExchange(&g_wait_registration_race_failed, 0);
    InterlockedExchange(&g_force_wait_registration_race, 1);
    WaitArgs raced_args = { .rc = -1 };
    HANDLE raced_thread = CreateThread(NULL, 0, wait_thread, &raced_args, 0, NULL);
    if (!raced_thread)
        return fail("CreateThread registration-race");
    if (WaitForSingleObject(raced_thread, 2000) != WAIT_OBJECT_0) {
        /* Release a broken implementation so the test exits cleanly instead
         * of leaving its waiter and EventFlag lock alive until CTest timeout. */
        write_be16(EVENT_FLAG_EA + EF_PENDING_RECV, raced_completion);
        uint64_t stuck = read_be64(EVENT_FLAG_EA);
        (void)spu_reservation_compare_exchange64(
            EVENT_FLAG_EA, &stuck, (stuck & UINT64_C(0xFF00)) | UINT64_C(1));
        (void)cellSpursDeliverSpuEvent(SPURS_EA, port, 0, 0);
        (void)WaitForSingleObject(raced_thread, 2000);
        CloseHandle(raced_thread);
        return fail("queued completion CAS retry left a stale waiter");
    }
    CloseHandle(raced_thread);
    if (InterlockedCompareExchange(&g_wait_registration_race_failed, 0, 0))
        return fail("registration-race producer hook");
    if (raced_args.rc != CELL_OK)
        return fail("registration-race wait return code");
    if (read_be16(WAIT_MASK_EA) != raced_completion)
        return fail("registration-race returned wrong completion mask");
    if (read_be64(EVENT_FLAG_EA) != UINT64_C(0x0100))
        return fail("registration-race did not auto-clear queued event");
    write_be64(EVENT_FLAG_EA, 0);
    spu_reservation_notify_write(EVENT_FLAG_EA, 8);

    /* A stale compare must fail and return the current guest-order value. */
    expected = UINT64_C(0x1122334455667788);
    if (spu_reservation_compare_exchange64(EVENT_FLAG_EA, &expected, 0))
        return fail("stale reservation compare unexpectedly succeeded");
    if (expected != 0)
        return fail("failed reservation compare returned wrong current value");

    /* Queue only after the main PPU has entered the native blocking wait.
     * HLE-entry polling alone cannot deliver this later callback. */
    write_be16(WAIT_MASK_EA, 0x0200);
    WaitArgs callback_args = { .rc = -1, .thread_id = 1 };
    HANDLE callback_thread = CreateThread(NULL, 0, wait_thread, &callback_args, 0, NULL);
    if (!callback_thread) return fail("CreateThread callback wait");
    ULONGLONG deadline = GetTickCount64() + 2000;
    while (read_be16(EVENT_FLAG_EA + EF_PPU_WAIT_MASK) != 0x0200 &&
           GetTickCount64() < deadline) Sleep(1);
    if (read_be16(EVENT_FLAG_EA + EF_PPU_WAIT_MASK) != 0x0200)
        return fail("callback wait did not register");
    InterlockedExchange(&g_queued_callback, 1);
    DWORD callback_wait = WaitForSingleObject(callback_thread, 2000);
    if (callback_wait != WAIT_OBJECT_0) {
        /* Release the baseline implementation before reporting its failure. */
        InterlockedExchange(&g_queued_callback, 0);
        callback_spu_completion(NULL);
        WaitForSingleObject(callback_thread, 2000);
    }
    CloseHandle(callback_thread);
    if (callback_wait != WAIT_OBJECT_0)
        return fail("callback arriving inside EventFlagWait was starved");
    if (g_callback_failed || callback_args.rc != CELL_OK ||
        read_be16(WAIT_MASK_EA) != 0x0200 || read_be64(EVENT_FLAG_EA) != 0)
        return fail("callback completion or lock handoff failed");

    /* Imported by Tokyo Jungle's animation-chain teardown. Shutdown is an
     * asynchronous request; Join is the completion barrier. */
    write_be64(JOB_ENTRY_EA, JOB_DESC_EA);       /* JOB */
    write_be64(JOB_ENTRY_EA + 8, 0x7Fu);         /* END */
    write_be64(JOB_DESC_EA, JOB_IMAGE_EA);       /* eaBinary */
    write_be16(JOB_DESC_EA + 8, 1);              /* 16-byte image */
    if (_cellSpursJobChainAttributeInitialize(
            2, 1, JOB_ATTR_EA, JOB_ENTRY_EA, 256, 8, 0, 1) != CELL_OK)
        return fail("_cellSpursJobChainAttributeInitialize");
    if (cellSpursJobChainAttributeSetHaltOnError(JOB_ATTR_EA, 1) != CELL_OK)
        return fail("cellSpursJobChainAttributeSetHaltOnError");
    if (vm_base[JOB_ATTR_EA + 0x34] != 0 ||
        vm_base[JOB_ATTR_EA + 0x35] != 0 ||
        vm_base[JOB_ATTR_EA + 0x36] != 0 ||
        vm_base[JOB_ATTR_EA + 0x37] != 1)
        return fail("halt-on-error attribute layout");
    if (cellSpursCreateJobChainWithAttribute(
            SPURS_EA, JOB_CHAIN_EA, JOB_ATTR_EA) != CELL_OK)
        return fail("cellSpursCreateJobChainWithAttribute");
    if (cellSpursShutdownJobChain(0) !=
            (int32_t)CELL_SPURS_JOB_ERROR_NULL_POINTER)
        return fail("cellSpursShutdownJobChain null validation");
    if (cellSpursShutdownJobChain(JOB_CHAIN_EA) != CELL_OK)
        return fail("cellSpursShutdownJobChain");
    if (cellSpursRunJobChain(JOB_CHAIN_EA) != CELL_OK)
        return fail("RunJobChain after shutdown");
    Sleep(10);
    if (g_test_job_dispatches != 0)
        return fail("shutdown job chain restarted and dispatched work");
    if (cellSpursJoinJobChain(JOB_CHAIN_EA) != CELL_OK)
        return fail("JoinJobChain after shutdown");

    /* The real animation chain has two alternating guards. A notification for
     * the second guard must resume there, not revisit the already-reset first
     * guard. A CALL/RET around them also proves return-PC preservation. */
    const uint32_t sub = JOB_ENTRY_EA + 0x80;
    write_be64(JOB_ENTRY_EA, sub | 4u);              /* CALL */
    write_be64(JOB_ENTRY_EA + 8, JOB_DESC_EA);       /* third JOB */
    write_be64(JOB_ENTRY_EA + 16, JOB_ENTRY_EA | 3u); /* NEXT */
    write_be64(sub, GUARD_FIRST_EA | 0x0Fu);
    write_be64(sub + 8, JOB_DESC_EA);
    write_be64(sub + 16, GUARD_SECOND_EA | 0x0Fu);
    write_be64(sub + 24, JOB_DESC_EA);
    write_be64(sub + 32, 0x77u);                    /* RET */
    if (cellSpursCreateJobChainWithAttribute(SPURS_EA, JOB_CHAIN_EA, JOB_ATTR_EA) != CELL_OK ||
        cellSpursJobGuardInitialize(JOB_CHAIN_EA, GUARD_FIRST_EA, 1, 1, 1) != CELL_OK ||
        cellSpursJobGuardInitialize(JOB_CHAIN_EA, GUARD_SECOND_EA, 1, 1, 1) != CELL_OK ||
        cellSpursRunJobChain(JOB_CHAIN_EA) != CELL_OK)
        return fail("two-guard chain setup");
    int resume_failed = !wait_guard_parked(GUARD_FIRST_EA);
    for (long cycle = 0; cycle < 3 && !resume_failed; ++cycle) {
        if (cellSpursJobGuardNotify(GUARD_FIRST_EA) != CELL_OK ||
            !wait_guard_parked(GUARD_SECOND_EA) ||
            InterlockedCompareExchange(&g_test_job_dispatches, 0, 0) != cycle * 3 + 1 ||
            cellSpursJobGuardNotify(GUARD_SECOND_EA) != CELL_OK ||
            !wait_guard_parked(GUARD_FIRST_EA) ||
            InterlockedCompareExchange(&g_test_job_dispatches, 0, 0) != (cycle + 1) * 3)
            resume_failed = 1;
    }
    if (cellSpursShutdownJobChain(JOB_CHAIN_EA) != CELL_OK ||
        cellSpursJoinJobChain(JOB_CHAIN_EA) != CELL_OK)
        return fail("two-guard chain cleanup");
    if (resume_failed)
        return fail("parked job chain did not resume its guard and CALL return position");

    cellSpursFinalize((CellSpurs*)(uintptr_t)SPURS_EA);
    puts("PASS: SPURS event flag handshake, callback wait, parked-chain resume and shutdown");
    return 0;
}
