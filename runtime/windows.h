/*
 * windows.h - POSIX stand-in for the Win32 API
 *
 * Lets Windows-authored HLE/library sources include <windows.h> unconditionally
 * on Linux/macOS. Include-guarded through win32_compat.h so it is a no-op after
 * the target-wide force-include of that header.
 */
#ifndef PS3RECOMP_WINDOWS_SHIM_H
#define PS3RECOMP_WINDOWS_SHIM_H
#include "platform/win32_compat.h"
#endif
