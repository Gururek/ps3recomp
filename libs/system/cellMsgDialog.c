/*
 * ps3recomp - cellMsgDialog HLE implementation
 *
 * Prints dialog messages to stdout and queues callbacks for sysutil polling.
 * No actual UI is rendered.
 */

#include "cellMsgDialog.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "ps3emu/guest_call.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int               s_dialog_open = 0;
static u32               s_callback_opd = 0;
static u32               s_userdata_ea = 0;
static CellMsgDialogType s_type = 0;
static int               s_close_pending = 0;
static u64               s_close_deadline_ms = 0;

typedef struct {
    u32 callback_opd;
    u32 userdata_ea;
    s32 result;
    u64 sequence;
} PendingDialogCallback;

#define PENDING_DIALOG_CALLBACK_QUEUE_SIZE 32

static PendingDialogCallback s_pending_callbacks[PENDING_DIALOG_CALLBACK_QUEUE_SIZE];
static unsigned int          s_pending_callback_head = 0;
static unsigned int          s_pending_callback_tail = 0;
static u64                   s_next_pending_callback_sequence = 1;
static int                   s_pending_callback_sequence_exhausted = 0;

#ifdef _WIN32
static SRWLOCK s_pending_callback_lock = SRWLOCK_INIT;

static void pending_callback_lock(void)
{
    AcquireSRWLockExclusive(&s_pending_callback_lock);
}

static void pending_callback_unlock(void)
{
    ReleaseSRWLockExclusive(&s_pending_callback_lock);
}
#else
static pthread_mutex_t s_pending_callback_lock = PTHREAD_MUTEX_INITIALIZER;

static void pending_callback_lock(void)
{
    pthread_mutex_lock(&s_pending_callback_lock);
}

static void pending_callback_unlock(void)
{
    pthread_mutex_unlock(&s_pending_callback_lock);
}
#endif

/* Progress bar state */
#define MAX_PROGRESS_BARS 2

typedef struct {
    u32  value;
    char message[256];
} ProgressBarState;

static ProgressBarState s_progress[MAX_PROGRESS_BARS];

static const char* copy_guest_string(const char* guest_string,
                                     char* host_buffer, size_t host_size)
{
    const u32 guest_ea = (u32)(uintptr_t)guest_string;
    if (!guest_ea || !host_size)
        return "<null>";

    size_t i = 0;
    for (; i + 1 < host_size; ++i) {
        host_buffer[i] = (char)vm_read8(guest_ea + (u32)i);
        if (!host_buffer[i])
            break;
    }
    host_buffer[i < host_size ? i : host_size - 1] = '\0';
    return host_buffer;
}

static u64 monotonic_milliseconds(void)
{
#ifdef _WIN32
    return (u64)GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * 1000u + (u64)now.tv_nsec / 1000000u;
#endif
}

static int pending_callback_queue_has_space_locked(void)
{
    return !s_pending_callback_sequence_exhausted &&
        (s_pending_callback_tail + 1) % PENDING_DIALOG_CALLBACK_QUEUE_SIZE
            != s_pending_callback_head;
}

static void enqueue_dialog_callback_locked(u32 callback_opd, u32 userdata_ea,
                                           s32 result)
{
    PendingDialogCallback* callback =
        &s_pending_callbacks[s_pending_callback_tail];

    callback->callback_opd = callback_opd;
    callback->userdata_ea = userdata_ea;
    callback->result = result;
    callback->sequence = s_next_pending_callback_sequence;
    if (s_next_pending_callback_sequence == UINT64_MAX)
        s_pending_callback_sequence_exhausted = 1;
    else
        ++s_next_pending_callback_sequence;
    s_pending_callback_tail =
        (s_pending_callback_tail + 1) % PENDING_DIALOG_CALLBACK_QUEUE_SIZE;
}

static u64 close_deadline_milliseconds(u64 now_ms, float delay_ms)
{
    u64 delay;

    /* NaN and nonpositive delays close on the next callback poll. */
    if (!(delay_ms > 0.0f))
        return now_ms;
    if (isinf(delay_ms) || delay_ms >= (float)UINT64_MAX)
        return UINT64_MAX;

    delay = (u64)delay_ms;
    if (delay > UINT64_MAX - now_ms)
        return UINT64_MAX;
    return now_ms + delay;
}

static void clear_dialog_state_locked(void)
{
    s_dialog_open = 0;
    s_callback_opd = 0;
    s_userdata_ea = 0;
    s_type = 0;
    s_close_pending = 0;
    s_close_deadline_ms = 0;
    memset(s_progress, 0, sizeof(s_progress));
}

static s32 close_dialog_and_queue_callback_locked(s32 result)
{
    const u32 callback_opd = s_callback_opd;
    const u32 userdata_ea = s_userdata_ea;

    if (callback_opd && !pending_callback_queue_has_space_locked())
        return (s32)CELL_SYSUTIL_ERROR_BUSY;

    /* Capture callback/userdata before releasing this dialog's ownership. */
    clear_dialog_state_locked();
    if (callback_opd)
        enqueue_dialog_callback_locked(callback_opd, userdata_ea, result);

    return CELL_OK;
}

static void abort_dialog_locked(void)
{
    /* Abort suppresses the callback. */
    clear_dialog_state_locked();
}

void cellMsgDialogDispatchPending(void)
{
    u64 poll_sequence = 0;
    ps3_guest_caller_fn guest_caller;

    pending_callback_lock();
    if (s_dialog_open && s_close_pending &&
        monotonic_milliseconds() >= s_close_deadline_ms) {
        /* Keep dialog state intact if capacity disappeared before expiry. */
        (void)close_dialog_and_queue_callback_locked(CELL_MSGDIALOG_BUTTON_NONE);
    }
    if (s_pending_callback_head != s_pending_callback_tail) {
        const unsigned int newest =
            (s_pending_callback_tail + PENDING_DIALOG_CALLBACK_QUEUE_SIZE - 1) %
            PENDING_DIALOG_CALLBACK_QUEUE_SIZE;
        poll_sequence = s_pending_callbacks[newest].sequence;
    }
    guest_caller = g_ps3_guest_caller;
    pending_callback_unlock();

    if (!guest_caller)
        return;

    for (;;) {
        PendingDialogCallback callback;

        pending_callback_lock();
        if (s_pending_callback_head == s_pending_callback_tail ||
            s_pending_callbacks[s_pending_callback_head].sequence > poll_sequence) {
            pending_callback_unlock();
            break;
        }

        callback = s_pending_callbacks[s_pending_callback_head];
        s_pending_callback_head =
            (s_pending_callback_head + 1) % PENDING_DIALOG_CALLBACK_QUEUE_SIZE;
        pending_callback_unlock();

        guest_caller(callback.callback_opd, (u64)(s64)callback.result,
                     callback.userdata_ea, 0, 0, 0, 0, 0, 0);
    }
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellMsgDialogOpen2(CellMsgDialogType type, const char* msgString,
                        CellMsgDialogCallback callback, void* userdata,
                        void* extParam)
{
    char message[1024];
    const char* host_message = copy_guest_string(msgString, message, sizeof(message));
    const u32 callback_opd = (u32)(uintptr_t)callback;
    const u32 userdata_ea = (u32)(uintptr_t)userdata;
    const u32 button_type = type & 0x000000F0;
    const int has_progress = (type & 0x0000F000) != 0;
    s32 result = CELL_MSGDIALOG_BUTTON_NONE;

    printf("[cellMsgDialog] Open2(type=0x%08X, msg='%s')\n",
           type, host_message);

    if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO) {
        result = CELL_MSGDIALOG_BUTTON_YES;
    } else if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK) {
        result = CELL_MSGDIALOG_BUTTON_OK;
    }

    pending_callback_lock();
    if (s_dialog_open) {
        pending_callback_unlock();
        return (s32)CELL_SYSUTIL_ERROR_BUSY;
    }

    if (!has_progress && callback_opd && !pending_callback_queue_has_space_locked()) {
        pending_callback_unlock();
        return (s32)CELL_SYSUTIL_ERROR_BUSY;
    }

    s_dialog_open = 1;
    s_callback_opd = callback_opd;
    s_userdata_ea = userdata_ea;
    s_type        = type;
    s_close_pending = 0;
    s_close_deadline_ms = 0;
    memset(s_progress, 0, sizeof(s_progress));

    if (!has_progress)
        (void)close_dialog_and_queue_callback_locked(result);
    pending_callback_unlock();

    /* Print the message so developers can see it */
    if (msgString) {
        printf("========================================\n");
        printf("[DIALOG] %s\n", host_message);
        printf("========================================\n");
    }

    if (!has_progress) {
        if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_YESNO) {
            printf("[cellMsgDialog] Auto-responding: YES\n");
        } else if (button_type == CELL_MSGDIALOG_TYPE_BUTTON_TYPE_OK) {
            printf("[cellMsgDialog] Auto-responding: OK\n");
        } else {
            printf("[cellMsgDialog] Auto-responding: NONE (no buttons)\n");
        }
    } else {
        printf("[cellMsgDialog] Progress bar dialog opened (will close on explicit Close/Abort)\n");
    }

    return CELL_OK;
}

s32 cellMsgDialogClose(float delayMs)
{
    const u64 now_ms = monotonic_milliseconds();

    printf("[cellMsgDialog] Close(delay=%.1f ms)\n", delayMs);

    pending_callback_lock();
    if (!s_dialog_open) {
        pending_callback_unlock();
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }

    if (s_callback_opd && !pending_callback_queue_has_space_locked()) {
        pending_callback_unlock();
        return (s32)CELL_SYSUTIL_ERROR_BUSY;
    }

    s_close_pending = 1;
    s_close_deadline_ms = close_deadline_milliseconds(now_ms, delayMs);
    pending_callback_unlock();

    return CELL_OK;
}

s32 cellMsgDialogAbort(void)
{
    printf("[cellMsgDialog] Abort()\n");

    pending_callback_lock();
    if (s_dialog_open)
        abort_dialog_locked();
    pending_callback_unlock();

    return CELL_OK;
}

s32 cellMsgDialogProgressBarSetMsg(u32 progressBarIndex, const char* msgString)
{
    char message[256];

    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    if (msgString)
        copy_guest_string(msgString, message, sizeof(message));

    pending_callback_lock();
    if (!s_dialog_open)
    {
        pending_callback_unlock();
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }

    if (msgString) {
        memcpy(s_progress[progressBarIndex].message, message, sizeof(message));
    }
    pending_callback_unlock();

    if (msgString)
        printf("[cellMsgDialog] ProgressBar[%u] msg='%s'\n", progressBarIndex,
               message);

    return CELL_OK;
}

s32 cellMsgDialogProgressBarReset(u32 progressBarIndex)
{
    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    pending_callback_lock();
    if (!s_dialog_open)
    {
        pending_callback_unlock();
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }

    s_progress[progressBarIndex].value = 0;
    pending_callback_unlock();
    printf("[cellMsgDialog] ProgressBar[%u] reset to 0%%\n", progressBarIndex);

    return CELL_OK;
}

s32 cellMsgDialogProgressBarInc(u32 progressBarIndex, u32 delta)
{
    u32 value;

    if (progressBarIndex >= MAX_PROGRESS_BARS)
        return CELL_MSGDIALOG_ERROR_PARAM;

    pending_callback_lock();
    if (!s_dialog_open)
    {
        pending_callback_unlock();
        return CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED;
    }

    s_progress[progressBarIndex].value += delta;
    if (s_progress[progressBarIndex].value > 100)
        s_progress[progressBarIndex].value = 100;
    value = s_progress[progressBarIndex].value;
    pending_callback_unlock();

    printf("[cellMsgDialog] ProgressBar[%u] = %u%%\n",
           progressBarIndex, value);

    return CELL_OK;
}
