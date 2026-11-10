/* Non-local control flow used by guest sys_ppu_thread_exit.  This remains a
 * header-only C++ tag so title targets do not need a new runtime source file. */
#ifndef PPU_THREAD_EXIT_SIGNAL_H
#define PPU_THREAD_EXIT_SIGNAL_H

#ifdef __cplusplus
struct ppu_thread_exit_signal {};

[[noreturn]] inline void ppu_throw_thread_exit()
{
    throw ppu_thread_exit_signal{};
}
#endif

#endif
