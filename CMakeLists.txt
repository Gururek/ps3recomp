cmake_minimum_required(VERSION 3.20)
project(ps3recomp_runtime
    VERSION 0.3.0
    LANGUAGES C CXX
    DESCRIPTION "ps3recomp - Static recompilation runtime for PS3 games"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PS3RECOMP_BUILD_TESTS "Build unit tests" OFF)

# ---------------------------------------------------------------------------
# Collect source files
# ---------------------------------------------------------------------------

# HLE library sources
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
)
# Exclude test harnesses (e.g. libs/video/tests/*): they define their own main().
list(FILTER HLE_SOURCES EXCLUDE REGEX "/tests/")

# Runtime core (syscalls, PPU, SPU, memory)
# The D3D12 backend is the reference implementation of the rsx_d3d12_backend_*
# surface that cellGcmSys calls. Hosts without D3D12 get libs/video/
# rsx_backend_shims.c, which maps that surface onto the Vulkan backend, so the
# D3D12 TU is compiled out there (its 16 definitions would otherwise collide
# with the shim's at link).
if(NOT WIN32)
    list(FILTER HLE_SOURCES EXCLUDE REGEX "rsx_d3d12_backend\\.c$")
endif()

# The Vulkan RSX backend is the POSIX graphics path. It implements the same
# rsx_backend vtable the null backend does, plus the rsx_d3d12_backend_*
# surface cellGcmSys calls, so enabling it swaps the window's content from a
# fixed-function clear to the real RSX command stream. It loads libvulkan at
# run time, so building needs only the Vulkan headers.
option(PS3RECOMP_RSX_VULKAN "Build the Vulkan RSX backend (Linux)" OFF)
list(FILTER HLE_SOURCES EXCLUDE REGEX "rsx_vulkan_backend\\.c$")
if(PS3RECOMP_RSX_VULKAN)
    if(WIN32 OR APPLE)
        message(FATAL_ERROR "PS3RECOMP_RSX_VULKAN is Linux-only for now")
    endif()
    find_path(PS3RECOMP_VULKAN_INCLUDE_DIR vulkan/vulkan.h)
    if(NOT PS3RECOMP_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "Vulkan headers not found. Debian/Ubuntu: "
            "libvulkan-dev, Fedora: vulkan-headers -- or pass "
            "-DPS3RECOMP_VULKAN_INCLUDE_DIR=<dir containing vulkan/vulkan.h>")
    endif()
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_vulkan_backend.c")
    # HLSL -> SPIR-V for guest programs: a stub unless glslang is found below.
    list(APPEND HLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/libs/video/rsx_shader_spirv.cpp")
endif()

file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
# Exclude:
#  - test harnesses (runtime/*/tests/*): they have their own main() and #include
#    per-game generated headers (spu_recomp.h) that a plain library build lacks.
#  - the PPU boot scaffold (runtime/ppu/, runtime/host/): these are compiled
#    per-game against the lifter-generated ppu_recomp.h (and the game's RSX/D3D12
#    backend), not as part of the game-agnostic runtime library. See
#    docs/PPU_RECOMP.md.
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/tests/|/runtime/ppu/|/runtime/host/")
# The runtime/ppu list is compiled per-game (see the note below), and the
# guest-pointer trap has no game-specific part, so it rides with the runtime.
list(APPEND RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppu/guest_ptr_trap.cpp")



# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(ps3recomp_runtime STATIC
    ${HLE_SOURCES}
    ${RUNTIME_SOURCES}
)

target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# ---------------------------------------------------------------------------
# POSIX build: the HLE modules and runtime core are written against the Win32
# threading/timing API (SRWLOCK, CONDITION_VARIABLE, QueryPerformanceCounter,
# GetTickCount64, Interlocked*, VirtualAlloc, ...). runtime/platform/win32_compat
# implements all of it on pthreads; force-including it means every TU sees the
# same declarations without touching the per-file `#ifdef _WIN32` blocks.
# ---------------------------------------------------------------------------
if(NOT WIN32)
    target_include_directories(ps3recomp_runtime PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    )
    # CPU_SET / pthread_setaffinity_np / REG_RIP in win32_compat, and the
    # __declspec(thread)/align spelling the PPU sources use under -fdeclspec.
    target_compile_definitions(ps3recomp_runtime PRIVATE _GNU_SOURCE)
    target_compile_options(ps3recomp_runtime PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform/win32_compat.h"
    )
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE -fdeclspec)
endif()

# HLE modules include their own headers
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/input"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/audio"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/network"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spurs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/sync"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/codec"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/font"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/misc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Vulkan: headers for the backend, and the definition is PUBLIC because the
# application picks its backend (null / Vulkan / D3D12) on it.
if(PS3RECOMP_RSX_VULKAN)
    target_include_directories(ps3recomp_runtime PRIVATE
        "${PS3RECOMP_VULKAN_INCLUDE_DIR}")
    target_compile_definitions(ps3recomp_runtime PUBLIC PS3RECOMP_RSX_VULKAN)
    target_link_libraries(ps3recomp_runtime PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "  Vulkan:       RSX backend enabled (headers in ${PS3RECOMP_VULKAN_INCLUDE_DIR})")
endif()

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(ps3recomp_runtime PRIVATE
        ws2_32      # Winsock for network modules
        xinput      # XInput for gamepad
        ole32       # COM for WASAPI audio
        bcrypt      # BCryptGenRandom for SSL RNG
    )
endif()

# On non-Windows hosts cellPad/cellAudio are SDL2-backed (cellPad uses the
# controllers, cellAudio the audio device), so the runtime needs SDL2 there.
if(NOT WIN32)
    find_package(SDL2 QUIET)               # distro config, exports SDL2::SDL2
    if(NOT SDL2_FOUND)
        find_package(SDL2 CONFIG QUIET)    # e.g. /usr/lib/cmake/SDL2 on Arch
    endif()
    # The null RSX backend's POSIX branch clears through the fixed-function
    # OpenGL pipeline, which SDL's context provides.
    find_package(OpenGL QUIET)
    if(TARGET OpenGL::GL)
        target_link_libraries(ps3recomp_runtime PUBLIC OpenGL::GL)
        message(STATUS "  OpenGL: null RSX backend window clear")
    endif()
    if(TARGET SDL2::SDL2)
        target_link_libraries(ps3recomp_runtime PUBLIC SDL2::SDL2)
        if(TARGET SDL2::SDL2main)
            # A static lib must not drag in SDL_main's main() remangling.
            target_link_libraries(ps3recomp_runtime PUBLIC SDL2::SDL2)
        endif()
        message(STATUS "  SDL2: $<TARGET_FILE:SDL2::SDL2> (cellPad/cellAudio backend)")
    elseif(SDL2_INCLUDE_DIRS)
        target_include_directories(ps3recomp_runtime PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(ps3recomp_runtime PUBLIC ${SDL2_LIBRARIES})
        message(STATUS "  SDL2: via pkg-config ${SDL2_LIBRARIES}")
    else()
        message(FATAL_ERROR
            "cellPad/cellAudio need SDL2 on this platform. Install it (Arch: "
            "lib32-sdl2/sdl2) and reconfigure.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(ps3recomp_runtime PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(ps3recomp_runtime PRIVATE
        /W3 /wd4100 /wd4244 /wd4267 /wd4996
        /Zc:__cplusplus
        /experimental:c11atomics   # Enable C11 <stdatomic.h> support
    )
    target_compile_definitions(ps3recomp_runtime PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS3RECOMP_BUILD_TESTS)
    include(CTest)
    if(WIN32)
        add_executable(test_d3d12_depth_sample
            libs/video/tests/test_d3d12_depth_sample.c
            libs/video/rsx_d3d12_depth_sample.c)
        target_link_libraries(test_d3d12_depth_sample PRIVATE d3d12 d3dcompiler)
        add_test(NAME d3d12_depth_sample COMMAND test_d3d12_depth_sample)
        set_tests_properties(d3d12_depth_sample PROPERTIES TIMEOUT 30)
    endif()
    add_executable(test_fp_decompiler
        libs/video/tests/test_fp_decompiler.c
        libs/video/rsx_fp_decompiler.c
        libs/video/rsx_vp_decompiler.c
    )
    target_include_directories(test_fp_decompiler PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/video"
    )
    add_test(NAME fp_decompiler COMMAND test_fp_decompiler)
    add_executable(spu_interp_selftest
        runtime/spu/tests/spu_interp_selftest.c
        runtime/spu/spu_interp.c
    )
    target_include_directories(spu_interp_selftest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spu"
    )
    if(UNIX)
        target_link_libraries(spu_interp_selftest PRIVATE m)
    endif()
    add_test(NAME spu_interp_selftest COMMAND spu_interp_selftest)
    add_subdirectory(tests/sync_stress)
    add_subdirectory(tests/spurs_event_flag)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ps3recomp_runtime
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/ps3emu
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ps3recomp runtime v${PROJECT_VERSION}")
message(STATUS "  C compiler:   ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "")
