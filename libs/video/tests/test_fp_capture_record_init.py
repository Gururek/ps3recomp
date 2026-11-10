"""Native equivalence and bounded cost check for vp_record_cb FP capture init.

Run with: python test_fp_capture_record_init.py

The old block below was extracted from rsx_d3d12_backend.c at renderer SHA
28B2B8B5E00B22D1932C90DAAD680141AEC440BCE37F16CDC2E7FDE1DC8942D9.
Its original byte hashes are recorded here so this fixture does not become a
whole-file backup. Prefix/suffix hashes bracket only the capture block.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


BASE_SOURCE_SHA256 = "28B2B8B5E00B22D1932C90DAAD680141AEC440BCE37F16CDC2E7FDE1DC8942D9"
BASE_PREFIX_SHA256 = "0ECE4416198B8E8E0801B08A116BB8C4B8D79191BF960377433138A4B9E6E597"
BASE_OLD_BLOCK_SHA256 = "21BCC09235C3E49D395D9B8FB3DCC84EF4A96A50161B45AAC08749AB128C08E1"
BASE_SUFFIX_SHA256 = "4377EB26B4AAE34320444F915EBE05EE24C32E748A6DBECB33F4AECE931EDEBA"

BEGIN = b"    /* Capture before the VP constant-bank availability check."
END = b"    /* Derive texture use from this exact program"

OLD_BLOCK = b"""    /* Capture before the VP constant-bank availability check.  FP replay must
     * retain this draw's program even when a later resource failure prevents
     * vertex constants from being uploaded. */
    {
        extern uint8_t* vm_base;
        u32 fp_off = 0;
        int fp_local = 0;
        int fp_resolved = dr->fp_addr &&
            rsx_shader_program_location_is_local(dr->fp_addr, &fp_local) &&
            cellGcmResolveLocatedChecked(fp_local, dr->fp_addr & ~0x3u, &fp_off);
        dr->fp_resolved_off = fp_resolved ? fp_off : 0xFFFFFFFFu;
        dr->fp_program_valid = 0;
        memset(&dr->fp_program, 0, sizeof(dr->fp_program));
        if (vm_base && fp_resolved &&
            rsx_fp_capture_structural(vm_base + fp_off,
                                      RSX_FP_MAX_PROGRAM_BYTES,
                                      &dr->fp_program) == 0)
            dr->fp_program_valid = 1;
    }
"""

NEW_BLOCK = b"""    /* Capture before the VP constant-bank availability check.  FP replay must
     * retain this draw's program even when a later resource failure prevents
     * vertex constants from being uploaded. */
    {
        extern uint8_t* vm_base;
        u32 fp_off = 0;
        int fp_local = 0;
        int fp_resolved = dr->fp_addr &&
            rsx_shader_program_location_is_local(dr->fp_addr, &fp_local) &&
            cellGcmResolveLocatedChecked(fp_local, dr->fp_addr & ~0x3u, &fp_off);
        dr->fp_resolved_off = fp_resolved ? fp_off : 0xFFFFFFFFu;
        dr->fp_program_valid = 0;
        if (vm_base && fp_resolved) {
            if (rsx_fp_capture_structural(vm_base + fp_off,
                                          RSX_FP_MAX_PROGRAM_BYTES,
                                          &dr->fp_program) == 0)
                dr->fp_program_valid = 1;
        } else {
            memset(&dr->fp_program, 0, sizeof(dr->fp_program));
        }
    }
"""

C_TEMPLATE = r'''#include "rsx_fp_decompiler.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif

typedef struct {
    u32 fp_addr;
    u32 fp_resolved_off;
    int fp_program_valid;
    RSXFPStructuralProgram fp_program;
} Record;

u8* vm_base;
static int g_location_ok = 1;
static int g_resolve_ok = 1;
static int g_local_value;
static u32 g_offset;
static unsigned g_location_calls;
static unsigned g_resolve_calls;

int rsx_shader_program_location_is_local(u32 addr, int* local)
{
    (void)addr;
    ++g_location_calls;
    if (!g_location_ok) return 0;
    *local = g_local_value;
    return 1;
}

int cellGcmResolveLocatedChecked(int local, u32 addr, u32* off)
{
    (void)local;
    (void)addr;
    ++g_resolve_calls;
    if (!g_resolve_ok) return 0;
    *off = g_offset;
    return 1;
}

static void run_old(Record* dr)
{
__OLD_BLOCK__
}

static void run_new(Record* dr)
{
__NEW_BLOCK__
}

#define OP(x) ((u32)(x) << 24)
#define END_BIT 1u
#define CONST_TYPE 2u
#define TEXTURE_UNIT(u) ((u32)(u) << 17)

static void put_word(u8* p, u32 host)
{
    u32 be = (host << 16) | (host >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16);
    p[2] = (u8)(be >> 8); p[3] = (u8)be;
}

static u8 g_programs[11][RSX_FP_MAX_PROGRAM_BYTES + 16u];

static void build_programs(void)
{
    for (unsigned n = 0; n < 11; ++n) {
        u8* p = g_programs[n];
        for (unsigned i = 0; i < RSX_FP_MAX_PROGRAM_BYTES + 16u; ++i)
            p[i] = 0;
        if (n == 8) {
            for (unsigned i = 0; i < 128; ++i) {
                u8* q = p + i * 32u;
                put_word(q, OP(0x04) | (i == 127u ? END_BIT : 0u));
                put_word(q + 4, CONST_TYPE); put_word(q + 8, 0); put_word(q + 12, 0);
                put_word(q + 16, 0x3F000000u + i); put_word(q + 20, 0x80000000u + i);
                put_word(q + 24, 0x7FC10000u + i); put_word(q + 28, 1u + i);
            }
        } else if (n == 9) {
            put_word(p, OP(0x01)); /* no END */
        } else if (n == 10) {
            put_word(p + RSX_FP_MAX_PROGRAM_BYTES - 16u, OP(0x04) | END_BIT);
            put_word(p + RSX_FP_MAX_PROGRAM_BYTES - 12u, CONST_TYPE);
        } else if (n == 1 || n == 3 || n == 5 || n == 7) {
            put_word(p, OP(0x04) | END_BIT);
            put_word(p + 4, CONST_TYPE);
            put_word(p + 8, 0); put_word(p + 12, 0);
            put_word(p + 16, 0x3F000000u + n);
            put_word(p + 20, 0x80000000u + n);
            put_word(p + 24, 0x7FC10000u + n);
            put_word(p + 28, 0x00000001u + n);
        } else if (n == 2 || n == 6) {
            put_word(p, OP(0x3F) | END_BIT); /* unknown opcode: all bindings */
        } else {
            put_word(p, OP(0x17) | TEXTURE_UNIT(n) | END_BIT);
        }
    }
}

static void configure(unsigned variant, u32 addr)
{
    g_location_ok = 1;
    g_resolve_ok = 1;
    g_local_value = (int)(variant & 1u);
    g_offset = (variant % 11u) * (RSX_FP_MAX_PROGRAM_BYTES + 16u);
    vm_base = &g_programs[0][0];
    g_location_calls = g_resolve_calls = 0;
    (void)addr;
}

static int compare_case(const char* name, unsigned variant, u32 addr,
                        int has_vm, int location_ok, int resolve_ok,
                        int overlap, int expected_valid,
                        u32 expected_inline_count, u32 expected_mask)
{
    Record old_record, new_record;
    memset(&old_record, 0xA5, sizeof(old_record));
    memset(&new_record, 0xA5, sizeof(new_record));
    old_record.fp_addr = new_record.fp_addr = addr;
    configure(variant, addr);
    g_location_ok = location_ok;
    g_resolve_ok = resolve_ok;
    if (!has_vm) vm_base = NULL;
    if (overlap) {
        /* Each variant aliases its own output bytes as guest source. */
        vm_base = (u8*)&old_record.fp_program;
        run_old(&old_record);
        vm_base = (u8*)&new_record.fp_program;
        run_new(&new_record);
    } else {
        run_old(&old_record);
        run_new(&new_record);
    }
    u32 expected_off = addr && location_ok && resolve_ok
        ? (variant % 11u) * (RSX_FP_MAX_PROGRAM_BYTES + 16u) : 0xFFFFFFFFu;
    int same = old_record.fp_resolved_off == new_record.fp_resolved_off &&
        old_record.fp_resolved_off == expected_off &&
        old_record.fp_program_valid == new_record.fp_program_valid &&
        memcmp(&old_record.fp_program, &new_record.fp_program,
               sizeof(old_record.fp_program)) == 0 &&
        old_record.fp_program_valid == expected_valid &&
        (expected_valid == 0 ||
         (old_record.fp_program.inline_count == expected_inline_count &&
          old_record.fp_program.texture_read_mask == expected_mask));
    if (!expected_valid) {
        const u8* bytes = (const u8*)&old_record.fp_program;
        for (size_t i = 0; same && i < sizeof(old_record.fp_program); ++i)
            if (bytes[i] != 0) same = 0;
    }
    if (!same) {
        fprintf(stderr, "FAIL %s: output differs\n", name);
        return 1;
    }
    printf("PASS %s valid=%d resolved_off=%08X bytes=%zu\n", name,
           old_record.fp_program_valid, old_record.fp_resolved_off,
           sizeof(old_record.fp_program));
    return 0;
}

#if defined(COUNTED_BUILD)
static u64 g_cleared_bytes;
static u64 g_clear_calls;
static int g_count_enabled;
void* counted_memset(void* dst, int value, size_t bytes)
{
    u8* p = (u8*)dst;
    if (g_count_enabled) { g_cleared_bytes += bytes; ++g_clear_calls; }
    for (size_t i = 0; i < bytes; ++i) p[i] = (u8)value;
    return dst;
}

static void count_one(const char* name, int candidate, unsigned variant, int has_vm,
                      int location_ok, int resolve_ok)
{
    Record record;
    for (size_t i = 0; i < sizeof(record); ++i) ((u8*)&record)[i] = 0xA5;
    record.fp_addr = 0x100u;
    configure(variant, record.fp_addr);
    g_location_ok = location_ok;
    g_resolve_ok = resolve_ok;
    if (!has_vm) vm_base = NULL;
    g_cleared_bytes = g_clear_calls = 0;
    g_count_enabled = 1;
    (candidate ? run_new : run_old)(&record);
    g_count_enabled = 0;
    printf("COUNT %s bytes=%llu calls=%llu valid=%d\n", name,
           (unsigned long long)g_cleared_bytes,
           (unsigned long long)g_clear_calls, record.fp_program_valid);
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    build_programs();
    count_one("valid-old", 0, 0, 1, 1, 1);
    count_one("valid-new", 1, 0, 1, 1, 1);
    count_one("unresolved-old", 0, 0, 1, 1, 0);
    count_one("unresolved-new", 1, 0, 1, 1, 0);
    count_one("null-vm-old", 0, 0, 0, 1, 1);
    count_one("null-vm-new", 1, 0, 0, 1, 1);
    count_one("capture-error-old", 0, 9, 1, 1, 1);
    count_one("capture-error-new", 1, 9, 1, 1, 1);
    return 0;
}
#else
typedef void (*record_fn)(Record*);
static volatile u64 g_sink;

static u64 now_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count); QueryPerformanceFrequency(&frequency);
    return (u64)(count.QuadPart / frequency.QuadPart) * 1000000000ull +
        (u64)(count.QuadPart % frequency.QuadPart) * 1000000000ull /
            (u64)frequency.QuadPart;
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}

static u64 bench(record_fn fn, unsigned rounds)
{
    u64 hash = 0;
    u64 start = now_ns();
    for (unsigned i = 0; i < rounds; ++i) {
        Record record;
        unsigned variant = (i * 7u + (i >> 4)) & 7u;
        record.fp_addr = 1u + (u32)(i * 13u);
        configure(variant, record.fp_addr);
        fn(&record);
        hash ^= record.fp_program.structural_hash +
            record.fp_program.raw_words[i & (RSX_FP_MAX_RAW_WORDS - 1u)] + i;
    }
    g_sink = g_sink * 31u + hash + rounds;
    return now_ns() - start;
}

static void sort_u64(u64* a, unsigned n)
{
    for (unsigned i = 1; i < n; ++i) {
        u64 v = a[i]; unsigned j = i;
        while (j && a[j - 1] > v) { a[j] = a[j - 1]; --j; }
        a[j] = v;
    }
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    int failures = 0;
    build_programs();
    failures += compare_case("short-valid", 0, 0x100u, 1, 1, 1, 0, 1, 0, 1u);
    failures += compare_case("inline-valid", 1, 0x104u, 1, 1, 1, 0, 1, 1, 0u);
    failures += compare_case("unknown-op-valid", 2, 0x108u, 1, 1, 1, 0, 1, 0, 0xFFFFu);
    failures += compare_case("malformed-no-end", 9, 0x10Cu, 1, 1, 1, 0, 0, 0, 0u);
    failures += compare_case("truncated-inline", 10, 0x10Eu, 1, 1, 1, 0, 0, 0, 0u);
    failures += compare_case("null-vm", 0, 0x110u, 0, 1, 1, 0, 0, 0, 0u);
    failures += compare_case("unresolved-location", 0, 0x114u, 1, 0, 1, 0, 0, 0, 0u);
    failures += compare_case("unresolved-address", 0, 0x118u, 1, 1, 0, 0, 0, 0, 0u);
    failures += compare_case("address-zero", 0, 0, 1, 1, 1, 0, 0, 0, 0u);
    failures += compare_case("maximum-inline-program", 8, 0x11Cu, 1, 1, 1, 0, 1, 128, 0u);
    failures += compare_case("overlap-source-output", 0, 0x120u, 1, 1, 1, 1, 0, 0, 0u);

    enum { ROUNDS = 100000, TRIALS = 5 };
    u64 old_times[TRIALS], new_times[TRIALS];
    for (unsigned trial = 0; trial < TRIALS; ++trial) {
        (void)bench(run_old, 2000u);
        (void)bench(run_new, 2000u);
        if (trial & 1u) {
            new_times[trial] = bench(run_new, ROUNDS);
            old_times[trial] = bench(run_old, ROUNDS);
        } else {
            old_times[trial] = bench(run_old, ROUNDS);
            new_times[trial] = bench(run_new, ROUNDS);
        }
    }
    sort_u64(old_times, TRIALS); sort_u64(new_times, TRIALS);
    printf("BENCH rounds=%u trials=%u old_median_ns=%llu new_median_ns=%llu sink=%llu\n",
           ROUNDS, TRIALS,
           (unsigned long long)old_times[TRIALS / 2],
           (unsigned long long)new_times[TRIALS / 2],
           (unsigned long long)g_sink);
    return failures ? 1 : 0;
}
#endif
'''


def extract_block(source: bytes) -> tuple[bytes, bytes, bytes]:
    start = source.index(BEGIN)
    end = source.index(END, start)
    return source[:start], source[start:end], source[end:]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()


def compile_fixture(cc: str, video_dir: Path, temp_dir: Path,
                    block_old: bytes, block_new: bytes, counted: bool) -> Path:
    source = C_TEMPLATE.replace("__OLD_BLOCK__", block_old.decode("utf-8"))
    source = source.replace("__NEW_BLOCK__", block_new.decode("utf-8"))
    c_path = temp_dir / ("count.c" if counted else "paired.c")
    exe_path = temp_dir / ("count.exe" if counted else "paired.exe")
    c_path.write_text(source, encoding="utf-8", newline="\n")
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-O0" if counted else "-O3",
           "-I", str(video_dir.parent.parent / "include"),
           "-I", str(video_dir), str(c_path), str(video_dir / "rsx_fp_decompiler.c")]
    if counted:
        cmd += ["-DCOUNTED_BUILD", "-Dmemset=counted_memset", "-fno-builtin-memset"]
    if os.name != "nt":
        cmd += ["-lm"]
    cmd += ["-o", str(exe_path)]
    subprocess.run(cmd, check=True, capture_output=True, text=True)
    return exe_path


def main() -> int:
    video_dir = Path(__file__).resolve().parents[1]
    backend = video_dir / "rsx_d3d12_backend.c"
    source = backend.read_bytes()
    prefix, current, suffix = extract_block(source)
    current_lf = current.replace(b"\r\n", b"\n")
    old_lf = OLD_BLOCK.replace(b"\r\n", b"\n")
    new_lf = NEW_BLOCK.replace(b"\r\n", b"\n")
    if current_lf == old_lf:
        old_block, new_block = current_lf, new_lf
        state = "baseline source extracted; candidate block paired"
    elif current_lf == new_lf:
        old_block, new_block = old_lf, current_lf
        state = "patched source extracted; stored baseline block paired"
    else:
        raise SystemExit("capture block differs from both stored baseline and candidate")
    print(f"SOURCE_STATE {state}")
    print(f"SOURCE_SHA256 {sha256(source)}")
    print(f"CURRENT_BLOCK_SHA256_LF {sha256(current_lf)}")
    print(f"PREFIX_SHA256 {sha256(prefix)}")
    print(f"SUFFIX_SHA256 {sha256(suffix)}")
    cc = (shutil.which("clang") or shutil.which("clang-cl") or
          shutil.which("gcc") or
          (r"C:\Program Files\LLVM\bin\clang.exe"
           if Path(r"C:\Program Files\LLVM\bin\clang.exe").exists() else None))
    if not cc:
        raise SystemExit("native compiler not found (need clang, clang-cl, or gcc)")
    with tempfile.TemporaryDirectory(prefix="fp_record_init_") as temp:
        temp_dir = Path(temp)
        paired = compile_fixture(cc, video_dir, temp_dir, old_block, new_block, False)
        count = compile_fixture(cc, video_dir, temp_dir, old_block, new_block, True)
        subprocess.run([str(paired)], check=True)
        subprocess.run([str(count)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
