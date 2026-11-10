/* Guest-pointer trap: reserve the low 4 GB and report faults that land in it,
 * which means an HLE function dereferenced a raw guest pointer as a host address.
 * Moved verbatim from runtime/ppu/ppu_loader.cpp on origin/master so the pinned
 * branch exposes the same entry point the title calls at boot. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include "windows.h"

extern "C" void ppu_guest_caller(char* out, size_t cap);   /* ppu_loader.cpp */

/* ---------------------------------------------------------------------------
 * Guest-pointer trap
 *
 * The HLE ABI hands a guest function's pointer arguments through as raw 32-bit
 * GUEST addresses. An implementation that dereferences one directly -- instead
 * of translating through vm_base -- reads or writes host address 0x00000000 to
 * 0xFFFFFFFF. vm_base sits far above that, so those two worlds never overlap.
 *
 * That mistake has been the single most expensive bug class in this runtime.
 * cellFsRead read an entire sound bank to an untranslated pointer, and the
 * symptom was a title deadlocking in audio initialisation four subsystems away.
 * cellAvconfExt, cellSail and cellFs cost similar hunts. Found by crashing, they
 * are archaeology; found here, they are a one-line fix at a named function.
 *
 * Reserving the low 4 GB is what makes the report RELIABLE rather than lucky:
 * unreserved address space faults today, but nothing stops the CRT or a DLL from
 * allocating there later, at which point the same bug silently corrupts unrelated
 * memory instead of faulting. Reserve it and the fault is guaranteed.
 *
 * Deliberately does NOT swallow the exception: it reports and lets the normal
 * handling proceed, so a real bug still stops the run.
 * -----------------------------------------------------------------------*/
/* The report is identical on both platforms; only the trap mechanism differs
 * (vectored exception handler vs sigaction, VirtualAlloc vs mmap). Written
 * once so the two cannot drift apart. */
static void ps3_report_guest_ptr(uintptr_t at, const char* how)
{
    static LONG n = 0;
    if (InterlockedIncrement(&n) > 32) return;
    char who[64]; ppu_guest_caller(who, sizeof who);
    fprintf(stderr,
            "\n[ps3] UNTRANSLATED GUEST POINTER: %s of guest 0x%08X as a host "
            "address\n      (an HLE function dereferenced a pointer parameter without "
            "vm_base)\n      guest caller: %s\n", how, (uint32_t)at, who);
    fflush(stderr);
}

/* Guest addresses worth reporting: above the first page (a plain NULL deref is
 * someone else's bug) and below 4 GB. */
static inline int ps3_is_guest_ptr_fault(uintptr_t at)
{
    return at >= 0x10000u && at < 0x100000000ull;
}

#ifdef _WIN32

static LONG WINAPI ps3_guest_ptr_veh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = (uintptr_t)er->ExceptionInformation[1];
    if (!ps3_is_guest_ptr_fault(at)) return EXCEPTION_CONTINUE_SEARCH;

    ps3_report_guest_ptr(at, er->ExceptionInformation[0] == 0 ? "read"
                           : er->ExceptionInformation[0] == 1 ? "write" : "execute");
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" void ps3_install_guest_ptr_trap(void)
{
    if (getenv("PS3_NO_GUEST_PTR_TRAP")) return;
    /* Reserve the low 4 GB in chunks; a chunk already in use just fails and is
     * skipped, which is fine -- every chunk we do get is one the bug can no
     * longer land in silently. */
    size_t got = 0;
    for (uintptr_t a = 0x10000u; a < 0x100000000ull; a += 0x10000000ull) {
        SIZE_T len = 0x10000000u;
        if (a == 0x10000u) len -= 0x10000u;
        if (VirtualAlloc((void*)a, len, MEM_RESERVE, PAGE_NOACCESS)) got += len;
    }
    AddVectoredExceptionHandler(1, ps3_guest_ptr_veh);
    fprintf(stderr, "[ps3] guest-pointer trap armed (%zu MB of the low 4 GB reserved)\n",
            got >> 20);
}
#else  /* POSIX */

/* Both signals a fault in the reserved window can arrive on, each with its own
 * saved predecessor, because the two are separate sigaction slots and chaining
 * to the wrong one would run a handler that was never registered for this
 * signal. See the install below for which host produces which. */
enum { PS3_FAULT_SEGV = 0, PS3_FAULT_BUS = 1, PS3_FAULT_SLOTS = 2 };
static struct sigaction s_prev_fault[PS3_FAULT_SLOTS];
static int              s_prev_fault_valid[PS3_FAULT_SLOTS];

static inline int ps3_fault_slot(int sig)
{ return sig == SIGBUS ? PS3_FAULT_BUS : PS3_FAULT_SEGV; }

/* Not async-signal-safe, and deliberately so: this runs on a path that is about
 * to end the process anyway, and the whole value of the report is the guest
 * function name -- which means walking the stack and formatting. The Windows
 * handler makes exactly the same trade. */
static void ps3_guest_ptr_fault(int sig, siginfo_t* si, void* uctx)
{
    /* si_addr is the faulting address on both signals and on both hosts, which
     * is the only thing this needs out of the frame. */
    if (si && ps3_is_guest_ptr_fault((uintptr_t)si->si_addr)) {
        /* Neither the signal nor si_code says read or write -- they separate
         * unmapped from protected -- so unlike the Windows report this cannot
         * name the direction without decoding a machine-specific trap frame.
         * The address and the guest caller are what actually locate the bug. */
        ps3_report_guest_ptr((uintptr_t)si->si_addr, "access");
    }

    /* Do NOT swallow it, matching the Windows handler: hand back to whoever was
     * installed before, or restore the default so the faulting instruction
     * re-runs and the process dies exactly as it would have. */
    const int slot = ps3_fault_slot(sig);
    if (s_prev_fault_valid[slot]) {
        const struct sigaction& prev = s_prev_fault[slot];
        if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction) {
            prev.sa_sigaction(sig, si, uctx);
            return;
        }
        if (prev.sa_handler && prev.sa_handler != SIG_DFL &&
            prev.sa_handler != SIG_IGN) {
            prev.sa_handler(sig);
            return;
        }
    }
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
}

extern "C" void ps3_install_guest_ptr_trap(void)
{
    if (getenv("PS3_NO_GUEST_PTR_TRAP")) return;

    /* Leave the chunk holding the program break alone. Reserving the space just
     * above it would stop brk from ever growing and silently push every small
     * allocation onto mmap -- a steep price for a diagnostic. */
    uintptr_t brk_now = (uintptr_t)sbrk(0);

    size_t got = 0;
    for (uintptr_t a = 0x10000u; a < 0x100000000ull; a += 0x10000000ull) {
        size_t len = 0x10000000u;
        if (a == 0x10000u) len -= 0x10000u;
        if (brk_now >= a && brk_now < a + len) continue;

        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#ifdef MAP_FIXED_NOREPLACE
        flags |= MAP_FIXED_NOREPLACE;   /* fail rather than evict a live mapping */
#endif
        void* at = mmap((void*)a, len, PROT_NONE, flags, -1, 0);
        if (at == MAP_FAILED) continue;                 /* chunk already in use */
        if ((uintptr_t)at != a) { munmap(at, len); continue; }  /* hint ignored */
        got += len;
    }

    /* SIGSEGV *and* SIGBUS. Which signal an inaccessible page produces is the
     * host's choice, and the two hosts do not agree:
     *
     *   Linux delivers an access to a mapped-but-forbidden page as SIGSEGV
     *   (si_code SEGV_ACCERR) and one to nothing at all as SIGSEGV
     *   (SEGV_MAPERR), so one signal covers the reservation and the gaps in it
     *   alike.
     *
     *   Darwin turns the Mach EXC_BAD_ACCESS into SIGBUS whenever the page WAS
     *   mapped and its protection refused the access, and into SIGSEGV only
     *   where nothing is mapped. A PROT_NONE mapping is the first case exactly:
     *   the reservation made just above is what converts the fault this exists
     *   to report into one a SIGSEGV-only handler cannot see. Measured, arm64:
     *   SIGBUS with si_code 1 -- the value that spells BUS_ADRALN -- for a read
     *   and for a write of a perfectly aligned PROT_NONE byte.
     *
     * The reason macOS nevertheless survived the SIGSEGV-only handler this
     * replaces is worth writing down, because it is luck and not design. A
     * 64-bit Mach-O carries a __PAGEZERO segment over 0 .. 0x100000000 with no
     * protections at all, and mmap will not place anything inside it, so every
     * chunk of the loop above fails and the guest window stays __PAGEZERO --
     * which faults as SIGSEGV, si_code 2, a zero-maxprot region reading as
     * unmapped rather than as forbidden. The report fired on that accident, and
     * the accident stops holding the moment a process shrinks its __PAGEZERO or
     * anything else maps low.
     *
     * si_code discriminates on neither host and is not consulted. si_addr
     * carries the faulting address on both signals and both hosts, which is all
     * the report needs. runtime/platform/tests/test_guest_ptr_trap.c is the
     * evidence, and runtime/platform/win32_compat.c's dispatcher takes both
     * signals for the same reason. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ps3_guest_ptr_fault;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &s_prev_fault[PS3_FAULT_SEGV]) == 0)
        s_prev_fault_valid[PS3_FAULT_SEGV] = 1;
    if (sigaction(SIGBUS, &sa, &s_prev_fault[PS3_FAULT_BUS]) == 0)
        s_prev_fault_valid[PS3_FAULT_BUS] = 1;

    /* 0 MB is the expected answer on macOS, for the __PAGEZERO reason above,
     * and does not mean the trap is off: the fault lands on __PAGEZERO instead
     * of on a reservation and si_addr is the same either way. */
    fprintf(stderr, "[ps3] guest-pointer trap armed (%zu MB of the low 4 GB reserved)\n",
            got >> 20);
}

#endif /* _WIN32 */
