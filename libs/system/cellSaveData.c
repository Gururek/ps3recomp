/*
 * ps3recomp - cellSaveData HLE implementation
 *
 * Game save data management with callback-driven flow.
 * Save data is stored under: {root}/gamedata/dev_hdd0/home/00000001/savedata/{dirName}/
 */

#include "cellSaveData.h"
#include "savedata_guest_abi.h"
#include "ps3emu/guest_call.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../runtime/memory/vm.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

/* glibc's <sys/stat.h> exposes st_atime/st_mtime/st_ctime as macros (st_atim.tv_sec
 * etc.) that collide with the identically named members of CellSaveData*Stat; undef
 * them so the struct fields resolve, and read host times via the explicit timespec
 * fields in the POSIX branches below. */
#ifdef st_atime
#  undef st_atime
#endif
#ifdef st_mtime
#  undef st_mtime
#endif
#ifdef st_ctime
#  undef st_ctime
#endif

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  define HOST_MKDIR(p) _mkdir(p)
#  define HOST_STAT     _stat64
#  define HOST_STAT_T   struct __stat64
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/types.h>
#  define HOST_MKDIR(p) mkdir(p, 0755)
#  define HOST_STAT     stat
#  define HOST_STAT_T   struct stat
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

static char s_save_root[1024] = "./gamedata/dev_hdd0/home/00000001/savedata";

static const char* savedata_root(void)
{
    static int initialized = 0;
    if (!initialized) {
        const char* configured = getenv("PS3_SAVEDATA_ROOT");
        if (configured && *configured) {
            strncpy(s_save_root, configured, sizeof(s_save_root) - 1);
            s_save_root[sizeof(s_save_root) - 1] = '\0';
        }
        initialized = 1;
    }
    return s_save_root;
}

/* Ensure directory (and parents) exist. */
static int ensure_dirs(const char* path)
{
    char tmp[1024];
    if (!path || !*path || strlen(path) >= sizeof(tmp))
        return 0;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (HOST_MKDIR(tmp) != 0 && errno != EEXIST) {
                *p = saved;
                return 0;
            }
            *p = saved;
        }
    }
    return HOST_MKDIR(tmp) == 0 || errno == EEXIST;
}

/* Host and guest callbacks must name one bounded path component. */
static int save_component_valid(const char* name, size_t capacity)
{
    if (!name) return 0;
    size_t length = strnlen(name, capacity);
    if (!length || length >= capacity || !strcmp(name, ".") || !strcmp(name, ".."))
        return 0;
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) return 0;
#ifdef _WIN32
    /* Win32 strips these suffixes; never let '.. ' or a save alias through. */
    if (name[length - 1] == ' ' || name[length - 1] == '.') return 0;
#endif
    return 1;
}

static int build_save_path(char* buf, size_t buf_size, const char* dirName)
{
    if (!save_component_valid(dirName, CELL_SAVEDATA_DIRNAME_SIZE)) return 0;
    int length = snprintf(buf, buf_size, "%s/%s", savedata_root(), dirName);
    if (length < 0 || (size_t)length >= buf_size) {
        if (buf_size) buf[0] = '\0';
        return 0; /* A truncated path can name the root or another save. */
    }
#ifdef _WIN32
    for (char* p = buf; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif
    return 1;
}

static int dir_exists(const char* path)
{
    HOST_STAT_T st;
    if (HOST_STAT(path, &st) != 0)
        return 0;
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

/* ---------------------------------------------------------------------------
 * Guest-callback marshalling
 *
 * The funcStat / funcFile callbacks the game registers are GUEST function
 * pointers (OPD addresses), not host-callable. We have to:
 *   1. Allocate guest memory for the StatGet/StatSet/CBResult structures
 *   2. Write the structs in big-endian format
 *   3. Dispatch the OPD via g_ps3_guest_caller with the guest pointers
 *   4. Read the structures back (game may have updated CBResult.result)
 *
 * Layout (sizes in PS3 ABI, all big-endian, 32-bit pointers):
 *   CellSaveDataCBResult:    4 + 4 + 4 + 4 + 4                = 20 bytes
 *   CellSaveDataStatGet:     fields through fileList = 1640, reserved[64] = 1704
 *   CellSaveDataStatSet:     4 + 4 + 4                        = 12 bytes (pointers + reCreateMode)
 *
 * We use a fixed scratch region at 0x024E0000 (128 KB, just before the
 * cmdbuf region at 0x02500000). Only one savedata operation is in flight
 * at a time, so a bump allocator is sufficient.
 * -----------------------------------------------------------------------*/

#define SAVEDATA_SCRATCH_BASE  0x024E0000u
#define SAVEDATA_SCRATCH_SIZE  0x00020000u  /* 128 KB */

static uint32_t s_scratch_next = SAVEDATA_SCRATCH_BASE;

static void scratch_reset(void) { s_scratch_next = SAVEDATA_SCRATCH_BASE; }

static uint32_t scratch_alloc(uint32_t size)
{
    /* 16-byte align */
    size = (size + 0xF) & ~0xFu;
    if (s_scratch_next + size > SAVEDATA_SCRATCH_BASE + SAVEDATA_SCRATCH_SIZE)
        return 0;
    uint32_t addr = s_scratch_next;
    s_scratch_next += size;
    /* Zero-init */
    memset(vm_base + addr, 0, size);
    return addr;
}

/* Big-endian struct field writers. Offsets are in PS3 ABI layout.
 *
 * CellSaveDataCBResult (sysutil_savedata.h) is 20 bytes, and invalidMsg is a
 * POINTER, not an inline buffer:
 *   int   result;                +0
 *   unsigned int progressBarInc; +4
 *   int   errNeedSizeKB;         +8
 *   char *invalidMsg;            +12   (32-bit guest pointer, NULL when unused)
 *   void *userdata;              +16   (32-bit guest pointer)
 *
 * userdata initially comes from cellSaveData*(..., userdata). Each callback
 * may replace it; subsequent callbacks receive that updated value. A callback
 * typically recovers its own object
 * from it. LBP's funcStat is literally
 *     v = cbResult->userdata; r = (*(v+104))(cbResult, get, set); ...
 * so leaving it zero made it dispatch through guest address 104, read the VM's
 * zero-fill, and report CELL_SAVEDATA_CBRESULT_ERR_NODATA (-4) -- it never
 * reached its own handler at all. */
static void marshal_cbresult_init(uint32_t addr, s32 result, uint32_t userdata_ea)
{
    vm_write32(addr + 0,  (uint32_t)result);     /* result */
    vm_write32(addr + 4,  0);                    /* progressBarInc */
    vm_write32(addr + 8,  0);                    /* errNeedSizeKB */
    vm_write32(addr + 12, 0);                    /* invalidMsg = NULL */
    vm_write32(addr + 16, userdata_ea);          /* current callback state */
}

static s32 marshal_cbresult_read_result(uint32_t addr)
{
    return (s32)vm_read32(addr + 0);
}

/* PS3 ABI layout of CellSaveDataStatGet (all big-endian on PPC, 32-bit pointers):
 *   s32 hddFreeSizeKB                     +0
 *   u32 isNewData                         +4
 *   CellSaveDataDirStat dir               +8   (s64*3 + char[32] = 56 bytes)
 *   CellSaveDataSystemFileParam getParam  +64  (128+128+1024+4+4+8+256 = 1552 bytes)
 *   u32 bind                              +1616
 *   s32 sizeKB                            +1620
 *   s32 sysSizeKB                         +1624
 *   u32 fileNum                           +1628
 *   u32 fileListNum                       +1632
 *   ptr fileList                          +1636 (32-bit guest ptr)
 *   total                                 1640 bytes
 */
#define SAVEDATA_STATGET_SIZE   PS3_SAVEDATA_STATGET_SIZE
#define SAVEDATA_STATSET_SIZE   PS3_SAVEDATA_STATSET_SIZE
#define SAVEDATA_CBRESULT_SIZE  PS3_SAVEDATA_CBRESULT_SIZE

static u32 enumerate_save_files(const char* save_path,
                                CellSaveDataFileStat* fileList, u32 max);
static void read_param_sfo(const char* save_path,
                           CellSaveDataSystemFileParam* param);
static int write_guest_param_sfo(const char* save_path, uint32_t param_ea);

static int guest_range_valid(uint32_t ea, uint32_t size)
{
    if (!vm_base || !ea)
        return 0;
    const uint64_t vm_limit = ppu_vm_size ? ppu_vm_size : 0x100000000ull;
    if ((uint64_t)ea + size > vm_limit)
        return 0;
    return 1;
}

static void marshal_system_file_param(uint32_t addr,
                                      const CellSaveDataSystemFileParam* param)
{
    if (!param)
        return;
    memcpy(vm_base + addr, param->title, CELL_SAVEDATA_SYSP_TITLE_SIZE);
    memcpy(vm_base + addr + 128, param->subTitle,
           CELL_SAVEDATA_SYSP_SUBTITLE_SIZE);
    memcpy(vm_base + addr + 256, param->detail,
           CELL_SAVEDATA_SYSP_DETAIL_SIZE);
    vm_write32(addr + 1280, param->attribute);
    memcpy(vm_base + addr + 1288, param->listParam,
           CELL_SAVEDATA_SYSP_LPARAM_SIZE);
}

static void marshal_file_stat(uint32_t addr, const CellSaveDataFileStat* stat)
{
    vm_write32(addr + 0, stat->fileType);
    vm_write64(addr + 8, stat->st_size);
    vm_write64(addr + 16, (uint64_t)stat->st_atime);
    vm_write64(addr + 24, (uint64_t)stat->st_mtime);
    vm_write64(addr + 32, (uint64_t)stat->st_ctime);
    memcpy(vm_base + addr + 40, stat->fileName, CELL_SAVEDATA_FILENAME_SIZE);
}

static void marshal_statget_init(uint32_t addr, int is_new, const char* dirName,
                                  s32 sizeKB, u32 fileNum, u32 fileListNum,
                                  uint32_t fileListEa,
                                  const CellSaveDataSystemFileParam* param)
{
    vm_write32(addr + 0,    1024 * 1024);    /* hddFreeSizeKB = 1 GB */
    vm_write32(addr + 4,    is_new ? 1 : 0); /* isNewData */
    vm_write64(addr + 8,    0);              /* dir.st_atime */
    vm_write64(addr + 16,   0);              /* dir.st_mtime */
    vm_write64(addr + 24,   0);              /* dir.st_ctime */
    if (dirName) {
        size_t len = strnlen(dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1);
        memcpy(vm_base + addr + 32, dirName, len);
        ((char*)vm_base)[addr + 32 + len] = 0;
    }
    marshal_system_file_param(addr + 64, param);
    vm_write32(addr + 1616, 0);              /* bind */
    vm_write32(addr + 1620, (uint32_t)sizeKB); /* sizeKB */
    vm_write32(addr + 1624, 0);              /* sysSizeKB */
    vm_write32(addr + 1628, fileNum);        /* fileNum */
    vm_write32(addr + 1632, fileListNum);     /* fileListNum */
    vm_write32(addr + 1636, fileListEa);      /* fileList ptr */
}

/* Dispatch funcStat callback via the guest-caller hook.
 * Returns the cbResult.result value the callback wrote, or
 * CELL_SAVEDATA_CBRESULT_ERR_FAILURE if no dispatcher is installed. */
static s32 dispatch_func_stat(uint32_t func_opd, int is_new, const char* dirName,
                              const char* save_path, uint32_t set_buf_ea,
                              uint32_t* userdata_ea_io, uint32_t* set_param_ea_out,
                              uint32_t* recreate_mode_out)
{
    if (!g_ps3_guest_caller) return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;

    CellSaveDataFileStat* files = NULL;
    u32 file_num = 0;
    u32 file_list_num = 0;
    uint32_t file_list_ea = 0;
    uint64_t total_bytes = 0;
    CellSaveDataSystemFileParam param;
    memset(&param, 0, sizeof(param));

    if (!is_new && save_path) {
        files = (CellSaveDataFileStat*)calloc(CELL_SAVEDATA_DIRLIST_MAX,
                                              sizeof(CellSaveDataFileStat));
        if (!files)
            return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
        file_num = enumerate_save_files(save_path, files,
                                        CELL_SAVEDATA_DIRLIST_MAX);
        u32 summed = file_num < CELL_SAVEDATA_DIRLIST_MAX
                         ? file_num : CELL_SAVEDATA_DIRLIST_MAX;
        for (u32 i = 0; i < summed; i++)
            total_bytes += files[i].st_size;
        read_param_sfo(save_path, &param);

        if (guest_range_valid(set_buf_ea, PS3_SAVEDATA_SETBUF_SIZE)) {
            Ps3SaveDataGuestSetBuf set_buf =
                ps3_savedata_decode_set_buf(vm_base + set_buf_ea);
            u32 capacity = set_buf.file_list_max;
            u32 by_size = set_buf.buffer_size / PS3_SAVEDATA_FILESTAT_SIZE;
            if (capacity > by_size)
                capacity = by_size;
            if (capacity > file_num)
                capacity = file_num;
            if (capacity > CELL_SAVEDATA_DIRLIST_MAX)
                capacity = CELL_SAVEDATA_DIRLIST_MAX;
            if (capacity && guest_range_valid(
                    set_buf.buffer_ea, capacity * PS3_SAVEDATA_FILESTAT_SIZE)) {
                file_list_ea = set_buf.buffer_ea;
                file_list_num = capacity;
                memset(vm_base + file_list_ea, 0,
                       file_list_num * PS3_SAVEDATA_FILESTAT_SIZE);
                for (u32 i = 0; i < file_list_num; i++)
                    marshal_file_stat(file_list_ea +
                                          i * PS3_SAVEDATA_FILESTAT_SIZE,
                                      &files[i]);
            }
        }
    }

    scratch_reset();
    uint32_t cb_ea  = scratch_alloc(SAVEDATA_CBRESULT_SIZE);
    uint32_t get_ea = scratch_alloc(SAVEDATA_STATGET_SIZE);
    uint32_t set_ea = scratch_alloc(SAVEDATA_STATSET_SIZE);
    if (!cb_ea || !get_ea || !set_ea) {
        printf("[cellSaveData] scratch alloc failed\n");
        free(files);
        return CELL_SAVEDATA_CBRESULT_ERR_FAILURE;
    }

    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, *userdata_ea_io);
    s32 size_kb = (s32)((total_bytes + 1023u) / 1024u);
    marshal_statget_init(get_ea, is_new, dirName, size_kb, file_num,
                         file_list_num, file_list_ea, is_new ? NULL : &param);
    /* StatSet zero-init by scratch_alloc */

    printf("[cellSaveData] dispatching funcStat OPD=0x%08X (cb=0x%X get=0x%X set=0x%X, isNew=%d, userdata=0x%08X)\n",
           func_opd, cb_ea, get_ea, set_ea, is_new, *userdata_ea_io);
    g_ps3_guest_caller(func_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);

    /* userdata is callback-owned state. The title's stat callback replaces
     * the API's initial pointer with the state needed by its file callback. */
    *userdata_ea_io = vm_read32(cb_ea + 16);
    s32 result = marshal_cbresult_read_result(cb_ea);
    if (set_param_ea_out)
        *set_param_ea_out = vm_read32(set_ea + 0);
    if (recreate_mode_out)
        *recreate_mode_out = vm_read32(set_ea + 4);
    free(files);
    printf("[cellSaveData] funcStat returned cbResult.result=%d\n", result);
    return result;
}

/* Enumerate save directories matching a prefix. Returns count, fills dirList up to max. */
static u32 enumerate_save_dirs(const char* prefix, CellSaveDataDirList* dirList, u32 max)
{
    u32 count = 0;

    const char* root = savedata_root();
    ensure_dirs(root);

#ifdef _WIN32
    {
        char search[1024];
        snprintf(search, sizeof(search), "%s\\*", root);
        for (char* p = search; *p; p++) {
            if (*p == '/') *p = '\\';
        }

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            if (fd.cFileName[0] == '.')
                continue;
            if (prefix && prefix[0] && strncmp(fd.cFileName, prefix, strlen(prefix)) != 0)
                continue;
            if (count < max && dirList) {
                memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                strncpy(dirList[count].dirName, fd.cFileName,
                        CELL_SAVEDATA_DIRNAME_SIZE - 1);
            }
            count++;
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }
#else
    {
        DIR* dp = opendir(root);
        if (!dp)
            return 0;

        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            /* Check it's a directory */
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", root, de->d_name);
            if (!dir_exists(full))
                continue;
            if (prefix && prefix[0] && strncmp(de->d_name, prefix, strlen(prefix)) != 0)
                continue;
            if (count < max && dirList) {
                memset(&dirList[count], 0, sizeof(CellSaveDataDirList));
                strncpy(dirList[count].dirName, de->d_name,
                        CELL_SAVEDATA_DIRNAME_SIZE - 1);
            }
            count++;
        }
        closedir(dp);
    }
#endif

    return count;
}

static int guest_copy_bounded_string(uint32_t ea, char* out, uint32_t limit,
                                     int allow_empty)
{
    if (!out || limit < 2 || !guest_range_valid(ea, 1))
        return 0;

    for (uint32_t i = 0; i < limit; i++) {
        if (!guest_range_valid(ea + i, 1))
            return 0;
        out[i] = (char)vm_base[ea + i];
        if (!out[i])
            return allow_empty || i != 0;
    }

    out[0] = 0;
    return 0;
}

static int guest_copy_save_dir_name(uint32_t ea,
                                    char out[CELL_SAVEDATA_DIRNAME_SIZE])
{
    if (!guest_copy_bounded_string(ea, out, CELL_SAVEDATA_DIRNAME_SIZE, 0))
        return 0;
    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        return 0;
    for (const char* p = out; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':')
            return 0;
    }
    return 1;
}

/* Public HLE entry points receive raw guest EAs. Tests and internal callers
 * may pass a native string, so support both without ever exposing an
 * unchecked guest pointer to strlen/printf/path construction. */
static int copy_save_dir_name_arg(const char* arg,
                                  char out[CELL_SAVEDATA_DIRNAME_SIZE])
{
    uintptr_t value = (uintptr_t)arg;
    if (!arg)
        return 0;

    if (value < 0x100000000ull)
        return guest_copy_save_dir_name((uint32_t)value, out);

    size_t len = strnlen(arg, CELL_SAVEDATA_DIRNAME_SIZE);
    if (!len || len >= CELL_SAVEDATA_DIRNAME_SIZE)
        return 0;
    memcpy(out, arg, len + 1);
    if (strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        return 0;
    for (const char* p = out; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ':')
            return 0;
    }
    return 1;
}

static int validate_guest_auto_set_buf(CellSaveDataSetBuf* set_buf)
{
    uintptr_t value = (uintptr_t)set_buf;
    if (!value || value >= 0x100000000ull ||
        !guest_range_valid((uint32_t)value, PS3_SAVEDATA_SETBUF_SIZE))
        return 0;

    uint32_t set_buf_ea = (uint32_t)value;
    Ps3SaveDataGuestSetBuf decoded =
        ps3_savedata_decode_set_buf(vm_base + set_buf_ea);
    if (decoded.dir_list_max > CELL_SAVEDATA_DIRLIST_MAX ||
        decoded.file_list_max > CELL_SAVEDATA_DIRLIST_MAX)
        return 0;
    for (uint32_t i = 0; i < 6; i++) {
        if (vm_read32(set_buf_ea + PS3_SAVEDATA_SETBUF_RESERVED + i * 4))
            return 0;
    }
    if (decoded.buffer_size &&
        !guest_range_valid(decoded.buffer_ea, decoded.buffer_size))
        return 0;
    if (decoded.file_list_max >
        decoded.buffer_size / PS3_SAVEDATA_FILESTAT_SIZE)
        return 0;
    return 1;
}

static void marshal_dir_list(uint32_t ea, const CellSaveDataDirList* entry)
{
    memset(vm_base + ea, 0, PS3_SAVEDATA_DIRLIST_SIZE);
    memcpy(vm_base + ea, entry->dirName, CELL_SAVEDATA_DIRNAME_SIZE);
    memcpy(vm_base + ea + CELL_SAVEDATA_DIRNAME_SIZE, entry->listParam,
           CELL_SAVEDATA_SYSP_LPARAM_SIZE);
}

static uint64_t savedata_aligned_kib_bytes(uint64_t size)
{
    return (size + 1023u) & ~(uint64_t)1023u;
}

/* Measure and remove without following directory links/reparse points.  The
 * caller additionally requires the name to exactly match an enumerated save,
 * so this cannot escape the isolated save-data root through a crafted name. */
static int savedata_measure_tree(const char* path, uint64_t* bytes)
{
#ifdef _WIN32
    DWORD root_attr = GetFileAttributesA(path);
    if (root_attr == INVALID_FILE_ATTRIBUTES)
        return 0;
    if (root_attr & FILE_ATTRIBUTE_REPARSE_POINT)
        return 1;

    char search[1024];
    int n = snprintf(search, sizeof(search), "%s\\*", path);
    if (n < 0 || (size_t)n >= sizeof(search))
        return 0;

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND;

    int ok = 1;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        char child[1024];
        n = snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            ok = 0;
            break;
        }

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (!savedata_measure_tree(child, bytes)) {
                ok = 0;
                break;
            }
        } else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ULARGE_INTEGER size;
            size.HighPart = fd.nFileSizeHigh;
            size.LowPart = fd.nFileSizeLow;
            *bytes += savedata_aligned_kib_bytes((uint64_t)size.QuadPart);
        }
    } while (FindNextFileA(hFind, &fd));

    if (ok && GetLastError() != ERROR_NO_MORE_FILES)
        ok = 0;
    FindClose(hFind);
    return ok;
#else
    struct stat root_st;
    if (lstat(path, &root_st) != 0)
        return 0;
    if (S_ISLNK(root_st.st_mode))
        return 1;
    if (!S_ISDIR(root_st.st_mode)) {
        if (S_ISREG(root_st.st_mode))
            *bytes += savedata_aligned_kib_bytes((uint64_t)root_st.st_size);
        return 1;
    }

    DIR* dp = opendir(path);
    if (!dp)
        return 0;

    int ok = 1;
    struct dirent* de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            ok = 0;
            break;
        }

        struct stat st;
        if (lstat(child, &st) != 0) {
            ok = 0;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!savedata_measure_tree(child, bytes)) {
                ok = 0;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            *bytes += savedata_aligned_kib_bytes((uint64_t)st.st_size);
        }
    }
    closedir(dp);
    return ok;
#endif
}

static int savedata_remove_tree(const char* path)
{
#ifdef _WIN32
    DWORD root_attr = GetFileAttributesA(path);
    if (root_attr == INVALID_FILE_ATTRIBUTES)
        return 0;
    if (!(root_attr & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileA(path) != 0;
    }
    if (root_attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return RemoveDirectoryA(path) != 0;
    }

    char search[1024];
    int n = snprintf(search, sizeof(search), "%s\\*", path);
    if (n < 0 || (size_t)n >= sizeof(search))
        return 0;

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_NOT_FOUND)
        return 0;

    int ok = 1;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0)
                continue;

            char child[1024];
            n = snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
            if (n < 0 || (size_t)n >= sizeof(child)) {
                ok = 0;
                break;
            }

            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                if (!savedata_remove_tree(child)) {
                    ok = 0;
                    break;
                }
            } else {
                SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (!RemoveDirectoryA(child)) {
                        ok = 0;
                        break;
                    }
                } else if (!DeleteFileA(child)) {
                    ok = 0;
                    break;
                }
            }
        } while (FindNextFileA(hFind, &fd));

        if (ok && GetLastError() != ERROR_NO_MORE_FILES)
            ok = 0;
        FindClose(hFind);
    }

    if (!ok)
        return 0;
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryA(path) != 0;
#else
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;

    DIR* dp = opendir(path);
    if (!dp)
        return 0;

    int ok = 1;
    struct dirent* de;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(child) ||
            !savedata_remove_tree(child)) {
            ok = 0;
            break;
        }
    }
    closedir(dp);
    return ok && rmdir(path) == 0;
#endif
}

static int is_internal_save_file(const char* name)
{
    return strcmp(name, "PARAM.SFO") == 0 || strcmp(name, "PARAM.PFD") == 0;
}

static u32 save_file_type(const char* name)
{
    if (strcmp(name, "ICON0.PNG") == 0)
        return CELL_SAVEDATA_FILETYPE_CONTENT_ICON0;
    if (strcmp(name, "ICON1.PAM") == 0)
        return CELL_SAVEDATA_FILETYPE_CONTENT_ICON1;
    if (strcmp(name, "PIC1.PNG") == 0)
        return CELL_SAVEDATA_FILETYPE_CONTENT_PIC1;
    if (strcmp(name, "SND0.AT3") == 0)
        return CELL_SAVEDATA_FILETYPE_CONTENT_SND0;
    return CELL_SAVEDATA_FILETYPE_NORMALFILE;
}

/* Enumerate game/content files. PARAM.SFO/PFD are firmware-owned metadata and
 * are not exposed through CellSaveDataStatGet.fileList. */
static u32 enumerate_save_files(const char* save_path,
                                 CellSaveDataFileStat* fileList, u32 max)
{
    u32 count = 0;

#ifdef _WIN32
    {
        char search[1024];
        snprintf(search, sizeof(search), "%s\\*", save_path);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search, &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return 0;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (is_internal_save_file(fd.cFileName))
                continue;
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = save_file_type(fd.cFileName);
                strncpy(fileList[count].fileName, fd.cFileName,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                ULARGE_INTEGER sz;
                sz.HighPart = fd.nFileSizeHigh;
                sz.LowPart  = fd.nFileSizeLow;
                fileList[count].st_size = (u64)sz.QuadPart;
            }
            count++;
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }
#else
    {
        DIR* dp = opendir(save_path);
        if (!dp)
            return 0;

        struct dirent* de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            if (is_internal_save_file(de->d_name))
                continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", save_path, de->d_name);
            HOST_STAT_T st;
            if (HOST_STAT(full, &st) != 0)
                continue;
#ifdef _WIN32
            if (st.st_mode & _S_IFDIR)
                continue;
#else
            if (S_ISDIR(st.st_mode))
                continue;
#endif
            if (count < max && fileList) {
                memset(&fileList[count], 0, sizeof(CellSaveDataFileStat));
                fileList[count].fileType = save_file_type(de->d_name);
                strncpy(fileList[count].fileName, de->d_name,
                        CELL_SAVEDATA_FILENAME_SIZE - 1);
                fileList[count].st_size = (u64)st.st_size;
#ifdef _WIN32
                fileList[count].st_atime = (s64)st.st_atime;
                fileList[count].st_mtime = (s64)st.st_mtime;
                fileList[count].st_ctime = (s64)st.st_ctime;
#else
                fileList[count].st_atime = (s64)st.st_atim.tv_sec;
                fileList[count].st_mtime = (s64)st.st_mtim.tv_sec;
                fileList[count].st_ctime = (s64)st.st_ctim.tv_sec;
#endif
            }
            count++;
        }
        closedir(dp);
    }
#endif

    return count;
}

/* Execute file callback: read/write/delete files in the save directory */
static s32 process_file_op(const char* save_path, CellSaveDataFileSet* set)
{
    if (!set || !save_component_valid(set->fileName, CELL_SAVEDATA_FILENAME_SIZE))
        return -1;

    char file_path[1024];
    int path_len = snprintf(file_path, sizeof(file_path), "%s/%s",
                            save_path, set->fileName);
    if (path_len < 0 || (size_t)path_len >= sizeof(file_path))
        return -1;
#ifdef _WIN32
    for (char* p = file_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    switch (set->fileOperation) {
    case CELL_SAVEDATA_FILEOP_READ: {
        FILE* fp = fopen(file_path, "rb");
        if (!fp) {
            printf("[cellSaveData] file read: cannot open '%s'\n", file_path);
            return -1;
        }
        if (set->fileOffset > 0) {
#ifdef _MSC_VER
            int seek_result = _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            int seek_result = fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
            if (seek_result != 0) {
                fclose(fp);
                return -1;
            }
        }
        size_t read_size = (size_t)set->fileSize;
        if (read_size > set->fileBufSize) read_size = set->fileBufSize;
        if (read_size && !set->fileBuf) {
            fclose(fp);
            return -1;
        }
        size_t got = read_size ? fread(set->fileBuf, 1, read_size, fp) : 0;
        int failed = ferror(fp);
        fclose(fp);
        if (failed)
            return -1;
        return (s32)got;
    }

    case CELL_SAVEDATA_FILEOP_WRITE:
    case CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC: {
        /* Reject malformed requests before opening/truncating the payload.
         * A smaller buffer is not permission to report a partial save as OK. */
        if (set->fileSize > set->fileBufSize ||
            (!set->fileBuf && set->fileBufSize))
            return -1;
        if (!ensure_dirs(save_path))
            return -1;
        const char* mode;
        if (set->fileOperation == CELL_SAVEDATA_FILEOP_WRITE && set->fileOffset == 0) {
            mode = "wb";
        } else {
            mode = "r+b";
        }
        FILE* fp = fopen(file_path, mode);
        if (!fp && errno == ENOENT) {
            fp = fopen(file_path, "wb");
        }
        if (!fp) {
            printf("[cellSaveData] file write: cannot open '%s'\n", file_path);
            return -1;
        }
        if (set->fileOffset > 0) {
#ifdef _MSC_VER
            int seek_result = _fseeki64(fp, (long long)set->fileOffset, SEEK_SET);
#else
            int seek_result = fseeko(fp, (off_t)set->fileOffset, SEEK_SET);
#endif
            if (seek_result != 0) {
                fclose(fp);
                return -1;
            }
        }
        size_t write_size = (size_t)set->fileSize;
        size_t wrote = write_size ? fwrite(set->fileBuf, 1, write_size, fp) : 0;
        int flush_failed = fflush(fp) != 0;
        int truncate_failed = 0;
        if (wrote == write_size && !flush_failed &&
            set->fileOperation == CELL_SAVEDATA_FILEOP_WRITE) {
            /* WRITE ends the file at offset + bytes written, including an
             * empty write at a nonzero offset. NOTRUNC preserves the tail. */
            uint64_t end = (uint64_t)set->fileOffset + wrote;
#ifdef _WIN32
            truncate_failed = _chsize_s(_fileno(fp), (long long)end) != 0;
#else
            truncate_failed = ftruncate(fileno(fp), (off_t)end) != 0;
#endif
        }
        int close_failed = fclose(fp) != 0;
        if (wrote != write_size || flush_failed || truncate_failed || close_failed)
            return -1;
        return (s32)wrote;
    }

    case CELL_SAVEDATA_FILEOP_DELETE:
        return remove(file_path) == 0 ? 0 : -1;

    default:
        return -1;
    }
}

static int guest_copy_save_filename(uint32_t name_ea, char out[16])
{
    if (!guest_range_valid(name_ea, 1))
        return 0;

    for (uint32_t i = 0; i < CELL_SAVEDATA_FILENAME_SIZE; i++) {
        if (!guest_range_valid(name_ea + i, 1))
            return 0;
        char ch = (char)vm_base[name_ea + i];
        if (!ch) {
            out[i] = 0;
            return i > 0 && strcmp(out, ".") != 0 && strcmp(out, "..") != 0;
        }
        if (ch == '/' || ch == '\\')
            return 0;
        out[i] = ch;
    }
    return 0; /* The maximum 12-character name must still be NUL-terminated. */
}

static const char* content_filename(uint32_t file_type)
{
    switch (file_type) {
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON0: return "ICON0.PNG";
    case CELL_SAVEDATA_FILETYPE_CONTENT_ICON1: return "ICON1.PAM";
    case CELL_SAVEDATA_FILETYPE_CONTENT_PIC1:  return "PIC1.PNG";
    case CELL_SAVEDATA_FILETYPE_CONTENT_SND0:  return "SND0.AT3";
    default: return NULL;
    }
}

/* Decode one file operation produced by a guest funcFile callback and perform
 * it against the isolated host save root. File payloads are opaque bytes: they
 * are copied without endian conversion. */
static s32 process_guest_file_op(const char* save_path, uint32_t set_ea)
{
    if (!guest_range_valid(set_ea, PS3_SAVEDATA_FILESET_SIZE))
        return -1;

    Ps3SaveDataGuestFileRequest request =
        ps3_savedata_decode_file_request(vm_base + set_ea);
    if (request.operation > CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC)
        return -1;

    if ((request.operation == CELL_SAVEDATA_FILEOP_WRITE ||
         request.operation == CELL_SAVEDATA_FILEOP_WRITE_NOTRUNC) &&
        (request.size > request.buffer_size ||
         (!request.buffer_ea && request.buffer_size))) {
        printf("[cellSaveData] invalid write buffer EA=0x%08X size=%u capacity=%u\n",
               request.buffer_ea, request.size, request.buffer_size);
        return CELL_SAVEDATA_ERROR_PARAM;
    }

    char file_name[16] = {0};
    const char* fixed_name = content_filename(request.file_type);
    if (request.name_ea) {
        if (!guest_copy_save_filename(request.name_ea, file_name)) {
            printf("[cellSaveData] invalid guest file name EA=0x%08X\n",
                   request.name_ea);
            return -1;
        }
    } else if (fixed_name) {
        strncpy(file_name, fixed_name, sizeof(file_name) - 1);
    } else {
        printf("[cellSaveData] file callback omitted a file name\n");
        return -1;
    }

    if (request.operation != CELL_SAVEDATA_FILEOP_DELETE) {
        uint32_t transfer_size = request.size;
        if (transfer_size > request.buffer_size)
            transfer_size = request.buffer_size;
        if (transfer_size &&
            !guest_range_valid(request.buffer_ea, transfer_size)) {
            printf("[cellSaveData] invalid guest file buffer EA=0x%08X size=%u\n",
                   request.buffer_ea, transfer_size);
            return -1;
        }
    }

    CellSaveDataFileSet host_set;
    memset(&host_set, 0, sizeof(host_set));
    host_set.fileOperation = request.operation;
    host_set.fileType = request.file_type;
    host_set.fileName = file_name;
    host_set.fileOffset = request.offset;
    host_set.fileSize = request.size;
    host_set.fileBufSize = request.buffer_size;
    host_set.fileBuf = request.buffer_ea ? vm_base + request.buffer_ea : NULL;

    s32 result = process_file_op(save_path, &host_set);
    printf("[cellSaveData] file op=%u name='%s' offset=%u requested=%u excSize=%d\n",
           request.operation, file_name, request.offset, request.size, result);
    return result;
}

static s32 callback_result_error(s32 result)
{
    if (result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
        return CELL_SAVEDATA_ERROR_NODATA;
    return CELL_SAVEDATA_ERROR_CBRESULT;
}

static s32 dispatch_func_file(uint32_t func_opd, const char* save_path,
                              uint32_t userdata_ea)
{
    if (!g_ps3_guest_caller || !func_opd)
        return CELL_SAVEDATA_ERROR_PARAM;

    scratch_reset();
    uint32_t cb_ea = scratch_alloc(PS3_SAVEDATA_CBRESULT_SIZE);
    uint32_t get_ea = scratch_alloc(PS3_SAVEDATA_FILEGET_SIZE);
    uint32_t set_ea = scratch_alloc(PS3_SAVEDATA_FILESET_SIZE);
    if (!cb_ea || !get_ea || !set_ea)
        return CELL_SAVEDATA_ERROR_INTERNAL;

    uint32_t exc_size = 0;
    for (uint32_t iteration = 0; iteration < 1024; iteration++) {
        marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT, userdata_ea);
        memset(vm_base + get_ea, 0, PS3_SAVEDATA_FILEGET_SIZE);
        memset(vm_base + set_ea, 0, PS3_SAVEDATA_FILESET_SIZE);
        vm_write32(get_ea, exc_size);

        g_ps3_guest_caller(func_opd, cb_ea, get_ea, set_ea, 0, 0, 0, 0, 0);
        userdata_ea = vm_read32(cb_ea + 16);
        s32 cb_result = marshal_cbresult_read_result(cb_ea);
        if (cb_result < 0)
            return callback_result_error(cb_result);
        if (cb_result != CELL_SAVEDATA_CBRESULT_OK_NEXT &&
            cb_result != CELL_SAVEDATA_CBRESULT_OK_LAST &&
            cb_result != CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM) {
            printf("[cellSaveData] invalid funcFile callback result %d\n", cb_result);
            return CELL_SAVEDATA_ERROR_CBRESULT;
        }

        uint32_t name_ea = vm_read32(set_ea + PS3_SAVEDATA_FILESET_NAME);
        uint32_t file_type = vm_read32(set_ea + PS3_SAVEDATA_FILESET_TYPE);
        if (name_ea || content_filename(file_type)) {
            s32 processed = process_guest_file_op(save_path, set_ea);
            if (processed < 0)
                return processed == (s32)CELL_SAVEDATA_ERROR_PARAM
                    ? processed : CELL_SAVEDATA_ERROR_ACCESS_ERROR;
            exc_size = (uint32_t)processed;
        } else if (cb_result == CELL_SAVEDATA_CBRESULT_OK_NEXT) {
            printf("[cellSaveData] funcFile requested NEXT without a file operation\n");
            return CELL_SAVEDATA_ERROR_CBRESULT;
        }

        if (cb_result != CELL_SAVEDATA_CBRESULT_OK_NEXT)
            return CELL_OK;
    }

    printf("[cellSaveData] funcFile exceeded 1024 operations\n");
    return CELL_SAVEDATA_ERROR_INTERNAL;
}

/* In-flight save/load operations. On window close the presenter waits for
 * zero before ending the process, so a save is never cut off mid-write. */
static volatile long s_savedata_busy;

int cellSaveData_busy(void)
{
    return __atomic_load_n(&s_savedata_busy, __ATOMIC_SEQ_CST) != 0;
}

static s32 savedata_execute_guest_auto_impl(const char* dir_name, int is_save,
                                            CellSaveDataSetBuf* set_buf,
                                            CellSaveDataStatCallback func_stat,
                                            CellSaveDataFileCallback func_file,
                                            void* userdata);

static s32 savedata_execute_guest_auto(const char* dir_name, int is_save,
                                       CellSaveDataSetBuf* set_buf,
                                       CellSaveDataStatCallback func_stat,
                                       CellSaveDataFileCallback func_file,
                                       void* userdata)
{
    __atomic_add_fetch(&s_savedata_busy, 1, __ATOMIC_SEQ_CST);
    s32 result = savedata_execute_guest_auto_impl(dir_name, is_save, set_buf,
                                                  func_stat, func_file, userdata);
    __atomic_sub_fetch(&s_savedata_busy, 1, __ATOMIC_SEQ_CST);
    return result;
}

static s32 savedata_execute_guest_auto_impl(const char* dir_name, int is_save,
                                            CellSaveDataSetBuf* set_buf,
                                            CellSaveDataStatCallback func_stat,
                                            CellSaveDataFileCallback func_file,
                                            void* userdata)
{
    char save_path[1024];
    if (!build_save_path(save_path, sizeof(save_path), dir_name))
        return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
    int is_new = !dir_exists(save_path);
    uint32_t userdata_ea = (uint32_t)(uintptr_t)userdata;
    uint32_t set_param_ea = 0;
    uint32_t recreate_mode = CELL_SAVEDATA_RECREATE_NO;

    s32 cb_result = dispatch_func_stat((uint32_t)(uintptr_t)func_stat,
                                       is_new, dir_name, save_path,
                                       (uint32_t)(uintptr_t)set_buf,
                                       &userdata_ea, &set_param_ea,
                                       &recreate_mode);
    if (cb_result < 0)
        return callback_result_error(cb_result);

    /* Validate the callback before recreation can remove an existing save. */
    if (cb_result != CELL_SAVEDATA_CBRESULT_OK_NEXT &&
        cb_result != CELL_SAVEDATA_CBRESULT_OK_LAST &&
        cb_result != CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM)
        return CELL_SAVEDATA_ERROR_CBRESULT;
    if (is_save && set_param_ea && !guest_range_valid(set_param_ea, 1552u))
        return CELL_SAVEDATA_ERROR_PARAM;

    recreate_mode &= CELL_SAVEDATA_RECREATE_MASK;
    if (recreate_mode > CELL_SAVEDATA_RECREATE_YES_RESET_OWNER)
        return CELL_SAVEDATA_ERROR_PARAM;
    if (is_save && recreate_mode >= CELL_SAVEDATA_RECREATE_YES) {
        if (!set_param_ea)
            return CELL_SAVEDATA_ERROR_PARAM;
        if (!is_new && !savedata_remove_tree(save_path)) {
            printf("[cellSaveData] failed to recreate save '%s'\n", save_path);
            return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
        }
        printf("[cellSaveData] recreating save '%s' mode=%u\n",
               dir_name, recreate_mode);
    }
    if (is_save && set_param_ea &&
        !write_guest_param_sfo(save_path, set_param_ea))
        return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
    if (cb_result == CELL_SAVEDATA_CBRESULT_OK_LAST ||
        cb_result == CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM)
        return CELL_OK;

    s32 result = dispatch_func_file((uint32_t)(uintptr_t)func_file,
                                    save_path, userdata_ea);
    if (result == CELL_OK) {
        printf("[cellSaveData] guest %s complete for '%s'\n",
               is_save ? "SAVE" : "LOAD", dir_name);
    }
    return result;
}

/* Write a simplified PARAM.SFO with the save's title/subtitle/detail */
static int write_param_sfo(const char* save_path,
                           const CellSaveDataSystemFileParam* param)
{
    if (!param)
        return 0;

    char sfo_path[1024];
    int path_len = snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO",
                            save_path);
    if (path_len < 0 || (size_t)path_len >= sizeof(sfo_path))
        return 0;
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    FILE* fp = fopen(sfo_path, "wb");
    if (!fp)
        return 0;

    /* Write a simplified text-based PARAM.SFO for easy debugging.
       Games don't read this directly - our stat callback fills it from here. */
    int failed = fprintf(fp, "TITLE=%.*s\n", (int)sizeof(param->title), param->title) < 0 ||
                 fprintf(fp, "SUB_TITLE=%.*s\n", (int)sizeof(param->subTitle), param->subTitle) < 0 ||
                 fprintf(fp, "DETAIL=%.*s\n", (int)sizeof(param->detail), param->detail) < 0 ||
                 fprintf(fp, "ATTRIBUTE=%u\n", param->attribute) < 0 ||
                 fprintf(fp, "LIST_PARAM=%.*s\n", (int)sizeof(param->listParam), param->listParam) < 0 ||
                 fflush(fp) != 0;
    if (fclose(fp) != 0)
        failed = 1;
    return !failed;
}

static int write_guest_param_sfo(const char* save_path, uint32_t param_ea)
{
    if (!guest_range_valid(param_ea, 1552u)) {
        printf("[cellSaveData] invalid guest setParam EA=0x%08X\n", param_ea);
        return 0;
    }

    CellSaveDataSystemFileParam param;
    memset(&param, 0, sizeof(param));
    memcpy(param.title, vm_base + param_ea, CELL_SAVEDATA_SYSP_TITLE_SIZE);
    memcpy(param.subTitle, vm_base + param_ea + 128,
           CELL_SAVEDATA_SYSP_SUBTITLE_SIZE);
    memcpy(param.detail, vm_base + param_ea + 256,
           CELL_SAVEDATA_SYSP_DETAIL_SIZE);
    param.attribute = vm_read32(param_ea + 1280);
    memcpy(param.listParam, vm_base + param_ea + 1288,
           CELL_SAVEDATA_SYSP_LPARAM_SIZE);
    param.title[CELL_SAVEDATA_SYSP_TITLE_SIZE - 1] = 0;
    param.subTitle[CELL_SAVEDATA_SYSP_SUBTITLE_SIZE - 1] = 0;
    param.detail[CELL_SAVEDATA_SYSP_DETAIL_SIZE - 1] = 0;
    param.listParam[CELL_SAVEDATA_SYSP_LPARAM_SIZE - 1] = 0;

    if (!ensure_dirs(save_path) || !write_param_sfo(save_path, &param)) {
        printf("[cellSaveData] failed to write save metadata for '%s'\n",
               save_path);
        return 0;
    }
    printf("[cellSaveData] wrote save metadata for '%s'\n", save_path);
    return 1;
}

/* Read simplified PARAM.SFO */
static void read_param_sfo(const char* save_path, CellSaveDataSystemFileParam* param)
{
    if (!param) return;
    memset(param, 0, sizeof(CellSaveDataSystemFileParam));

    char sfo_path[1024];
    snprintf(sfo_path, sizeof(sfo_path), "%s/PARAM.SFO", save_path);
#ifdef _WIN32
    for (char* p = sfo_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
#endif

    FILE* fp = fopen(sfo_path, "rb");
    if (!fp) return;

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strncmp(line, "TITLE=", 6) == 0)
            strncpy(param->title, line + 6, CELL_SAVEDATA_SYSP_TITLE_SIZE - 1);
        else if (strncmp(line, "SUB_TITLE=", 10) == 0)
            strncpy(param->subTitle, line + 10, CELL_SAVEDATA_SYSP_SUBTITLE_SIZE - 1);
        else if (strncmp(line, "DETAIL=", 7) == 0)
            strncpy(param->detail, line + 7, CELL_SAVEDATA_SYSP_DETAIL_SIZE - 1);
        else if (strncmp(line, "ATTRIBUTE=", 10) == 0)
            param->attribute = (u32)atoi(line + 10);
        else if (strncmp(line, "LIST_PARAM=", 11) == 0)
            strncpy(param->listParam, line + 11, CELL_SAVEDATA_SYSP_LPARAM_SIZE - 1);
    }
    fclose(fp);
}

static s32 savedata_execute_impl(const char* dirName, int is_save,
                                 CellSaveDataSetBuf* setBuf,
                                 CellSaveDataStatCallback funcStat,
                                 CellSaveDataFileCallback funcFile,
                                 void* userdata);

/* Core save/load implementation shared by List/Fixed/Auto variants */
static s32 savedata_execute(const char* dirName, int is_save,
                             CellSaveDataSetBuf* setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile,
                             void* userdata)
{
    __atomic_add_fetch(&s_savedata_busy, 1, __ATOMIC_SEQ_CST);
    s32 result = savedata_execute_impl(dirName, is_save, setBuf, funcStat,
                                       funcFile, userdata);
    __atomic_sub_fetch(&s_savedata_busy, 1, __ATOMIC_SEQ_CST);
    return result;
}

static s32 savedata_execute_impl(const char* dirName, int is_save,
                                 CellSaveDataSetBuf* setBuf,
                                 CellSaveDataStatCallback funcStat,
                                 CellSaveDataFileCallback funcFile,
                                 void* userdata)
{
    if (!dirName || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    char save_path[1024];
    if (!build_save_path(save_path, sizeof(save_path), dirName))
        return CELL_SAVEDATA_ERROR_ACCESS_ERROR;

    int is_new = !dir_exists(save_path);

    printf("[cellSaveData] %s dir='%s' (new=%d)\n",
           is_save ? "SAVE" : "LOAD", dirName, is_new);

    /* If loading and directory doesn't exist, it's not an error -
       the stat callback will see isNewData=1 and can handle it */

    /* Prepare stat get */
    u32 file_list_max = setBuf ? setBuf->fileListMax : 64;
    CellSaveDataFileStat* fileList = NULL;
    u32 fileNum = 0;

    if (file_list_max > 0) {
        fileList = (CellSaveDataFileStat*)calloc(file_list_max, sizeof(CellSaveDataFileStat));
        if (!fileList)
            return CELL_SAVEDATA_ERROR_INTERNAL;
        if (!is_new) {
            fileNum = enumerate_save_files(save_path, fileList, file_list_max);
        }
    }

    CellSaveDataStatGet statGet;
    memset(&statGet, 0, sizeof(statGet));
    statGet.hddFreeSizeKB = 1024 * 1024; /* report 1GB free */
    statGet.isNewData = is_new ? 1 : 0;
    strncpy(statGet.dir.dirName, dirName, CELL_SAVEDATA_DIRNAME_SIZE - 1);

    if (!is_new) {
        HOST_STAT_T hst;
        if (HOST_STAT(save_path, &hst) == 0) {
#ifdef _WIN32
            statGet.dir.st_atime = (s64)hst.st_atime;
            statGet.dir.st_mtime = (s64)hst.st_mtime;
            statGet.dir.st_ctime = (s64)hst.st_ctime;
#else
            statGet.dir.st_atime = (s64)hst.st_atim.tv_sec;
            statGet.dir.st_mtime = (s64)hst.st_mtim.tv_sec;
            statGet.dir.st_ctime = (s64)hst.st_ctim.tv_sec;
#endif
        }
        read_param_sfo(save_path, &statGet.getParam);
    }

    statGet.fileNum = fileNum;
    statGet.fileListNum = fileNum < file_list_max ? fileNum : file_list_max;
    statGet.fileList = fileList;
    statGet.bind = 0;
    statGet.sizeKB = 0;
    statGet.sysSizeKB = 0;

    /* Call stat callback */
    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;
    cbResult.userdata = userdata;

    CellSaveDataStatSet statSet;
    memset(&statSet, 0, sizeof(statSet));

    funcStat(&cbResult, &statGet, &statSet);

    if (cbResult.result < 0) {
        printf("[cellSaveData] stat callback returned error %d\n", cbResult.result);
        free(fileList);
        if (cbResult.result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    /* Write PARAM.SFO if stat set provided params and we're saving */
    if (cbResult.result != CELL_SAVEDATA_CBRESULT_OK_NEXT &&
        cbResult.result != CELL_SAVEDATA_CBRESULT_OK_LAST &&
        cbResult.result != CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM) {
        free(fileList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }
    if (is_save && statSet.setParam) {
        if (!ensure_dirs(save_path) ||
            !write_param_sfo(save_path, statSet.setParam)) {
            free(fileList);
            return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
        }
    }

    /* Call file callback repeatedly */
    if (funcFile && cbResult.result == CELL_SAVEDATA_CBRESULT_OK_NEXT) {
        u32 exc_size = 0;
        while (1) {
            CellSaveDataFileGet fileGet;
            memset(&fileGet, 0, sizeof(fileGet));
            fileGet.excSize = exc_size;

            CellSaveDataFileSet fileSet;
            memset(&fileSet, 0, sizeof(fileSet));

            cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;

            funcFile(&cbResult, &fileGet, &fileSet);

            /* A failing callback cannot authorize its leftover file request. */
            if (cbResult.result < 0) {
                free(fileList);
                return callback_result_error(cbResult.result);
            }
            if (cbResult.result != CELL_SAVEDATA_CBRESULT_OK_NEXT &&
                cbResult.result != CELL_SAVEDATA_CBRESULT_OK_LAST &&
                cbResult.result != CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM) {
                free(fileList);
                return CELL_SAVEDATA_ERROR_CBRESULT;
            }

            if (cbResult.result != CELL_SAVEDATA_CBRESULT_OK_NEXT) {
                /* Process last operation if set */
                if (fileSet.fileName) {
                    s32 exc = process_file_op(save_path, &fileSet);
                    if (exc < 0) {
                        free(fileList);
                        return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
                    }
                }
                break;
            }

            if (fileSet.fileName) {
                s32 exc = process_file_op(save_path, &fileSet);
                if (exc < 0) {
                    free(fileList);
                    return CELL_SAVEDATA_ERROR_ACCESS_ERROR;
                }
                exc_size = (u32)exc;
            } else {
                /* No file operation requested, done */
                break;
            }
        }
    }

    free(fileList);

    if (cbResult.result < 0) {
        if (cbResult.result == CELL_SAVEDATA_CBRESULT_ERR_NODATA)
            return CELL_SAVEDATA_ERROR_NODATA;
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    printf("[cellSaveData] %s complete for '%s'\n",
           is_save ? "SAVE" : "LOAD", dirName);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellSaveDataListSave2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] ListSave2(version=%u)\n", version);

    if (!setList || !setBuf || !funcList || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    /* Enumerate existing save dirs */
    u32 dir_max = setBuf->dirListMax;
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));
    if (!dirList)
        return CELL_SAVEDATA_ERROR_INTERNAL;

    u32 dirCount = enumerate_save_dirs(
        setList->dirNamePrefix ? setList->dirNamePrefix : "",
        dirList, dir_max);

    /* Call list callback */
    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;
    cbResult.userdata = userdata;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirNum = dirCount;
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataListSet listSet;
    memset(&listSet, 0, sizeof(listSet));

    funcList(&cbResult, &listGet, &listSet);

    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    /* Determine selected directory name */
    const char* selectedDir = NULL;
    if (listSet.fixedList && listSet.fixedListNum > 0) {
        selectedDir = listSet.fixedList[0].dirName;
    } else if (listSet.focusDirName) {
        selectedDir = listSet.focusDirName;
    } else if (dirCount > 0) {
        selectedDir = dirList[0].dirName;
    }

    if (!selectedDir || selectedDir[0] == '\0') {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 1, setBuf, funcStat, funcFile, cbResult.userdata);
    free(dirList);
    return result;
}

s32 cellSaveDataListLoad2(u32 version, CellSaveDataSetList* setList,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataListCallback funcList,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    printf("[cellSaveData] ListLoad2(version=%u)\n", version);

    if (!setList || !setBuf || !funcList || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    u32 dir_max = setBuf->dirListMax;
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));
    if (!dirList)
        return CELL_SAVEDATA_ERROR_INTERNAL;

    u32 dirCount = enumerate_save_dirs(
        setList->dirNamePrefix ? setList->dirNamePrefix : "",
        dirList, dir_max);

    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;
    cbResult.userdata = userdata;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirNum = dirCount;
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataListSet listSet;
    memset(&listSet, 0, sizeof(listSet));

    funcList(&cbResult, &listGet, &listSet);

    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    const char* selectedDir = NULL;
    if (listSet.fixedList && listSet.fixedListNum > 0) {
        selectedDir = listSet.fixedList[0].dirName;
    } else if (listSet.focusDirName) {
        selectedDir = listSet.focusDirName;
    } else if (dirCount > 0) {
        selectedDir = dirList[0].dirName;
    }

    if (!selectedDir || selectedDir[0] == '\0') {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 0, setBuf, funcStat, funcFile, cbResult.userdata);
    free(dirList);
    return result;
}

s32 cellSaveDataFixedSave2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata)
{
    printf("[cellSaveData] FixedSave2(version=%u)\n", version);

    if (!setList || !setBuf || !funcFixed || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    u32 dir_max = setBuf->dirListMax;
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));
    if (!dirList)
        return CELL_SAVEDATA_ERROR_INTERNAL;

    u32 dirCount = enumerate_save_dirs(
        setList->dirNamePrefix ? setList->dirNamePrefix : "",
        dirList, dir_max);

    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;
    cbResult.userdata = userdata;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirNum = dirCount;
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataFixedSet fixedSet;
    memset(&fixedSet, 0, sizeof(fixedSet));

    funcFixed(&cbResult, &listGet, &fixedSet);

    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    const char* selectedDir = fixedSet.dirName;

    if (!selectedDir || selectedDir[0] == '\0') {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 1, setBuf, funcStat, funcFile, cbResult.userdata);
    free(dirList);
    return result;
}

s32 cellSaveDataFixedLoad2(u32 version, CellSaveDataSetList* setList,
                            CellSaveDataSetBuf* setBuf,
                            CellSaveDataFixedCallback funcFixed,
                            CellSaveDataStatCallback funcStat,
                            CellSaveDataFileCallback funcFile,
                            u32 container, void* userdata)
{
    printf("[cellSaveData] FixedLoad2(version=%u)\n", version);

    if (!setList || !setBuf || !funcFixed || !funcStat)
        return CELL_SAVEDATA_ERROR_PARAM;

    u32 dir_max = setBuf->dirListMax;
    if (dir_max == 0) dir_max = CELL_SAVEDATA_DIRLIST_MAX;
    CellSaveDataDirList* dirList = (CellSaveDataDirList*)calloc(dir_max, sizeof(CellSaveDataDirList));
    if (!dirList)
        return CELL_SAVEDATA_ERROR_INTERNAL;

    u32 dirCount = enumerate_save_dirs(
        setList->dirNamePrefix ? setList->dirNamePrefix : "",
        dirList, dir_max);

    CellSaveDataCBResult cbResult;
    memset(&cbResult, 0, sizeof(cbResult));
    cbResult.result = CELL_SAVEDATA_CBRESULT_OK_NEXT;
    cbResult.userdata = userdata;

    CellSaveDataListGet listGet;
    memset(&listGet, 0, sizeof(listGet));
    listGet.dirNum = dirCount;
    listGet.dirListNum = dirCount < dir_max ? dirCount : dir_max;
    listGet.dirList = dirList;

    CellSaveDataFixedSet fixedSet;
    memset(&fixedSet, 0, sizeof(fixedSet));

    funcFixed(&cbResult, &listGet, &fixedSet);

    if (cbResult.result < 0) {
        free(dirList);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    const char* selectedDir = fixedSet.dirName;

    if (!selectedDir || selectedDir[0] == '\0') {
        free(dirList);
        return CELL_SAVEDATA_ERROR_NODATA;
    }

    s32 result = savedata_execute(selectedDir, 0, setBuf, funcStat, funcFile, cbResult.userdata);
    free(dirList);
    return result;
}

s32 cellSaveDataAutoSave2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    char checked_dir[CELL_SAVEDATA_DIRNAME_SIZE];
    if (!copy_save_dir_name_arg(dirName, checked_dir) ||
        !validate_guest_auto_set_buf(setBuf) || !funcStat || !funcFile)
        return CELL_SAVEDATA_ERROR_PARAM;

    printf("[cellSaveData] AutoSave2(version=%u, dir='%s')\n",
           version, checked_dir);

    return savedata_execute_guest_auto(checked_dir, 1, setBuf, funcStat, funcFile,
                                       userdata);
}

s32 cellSaveDataAutoLoad2(u32 version, const char* dirName,
                           u32 errDialog,
                           CellSaveDataSetBuf* setBuf,
                           CellSaveDataStatCallback funcStat,
                           CellSaveDataFileCallback funcFile,
                           u32 container, void* userdata)
{
    char checked_dir[CELL_SAVEDATA_DIRNAME_SIZE];
    (void)errDialog; (void)container;
    if (!copy_save_dir_name_arg(dirName, checked_dir) ||
        !validate_guest_auto_set_buf(setBuf) || !funcStat || !funcFile)
        return CELL_SAVEDATA_ERROR_PARAM;

    printf("[cellSaveData] AutoLoad2(version=%u, dir='%s')\n",
           version, checked_dir);

    s32 result = savedata_execute_guest_auto(checked_dir, 0, setBuf, funcStat,
                                              funcFile, userdata);
    if (result == CELL_SAVEDATA_ERROR_NODATA) {
        /* Tokyo Jungle's first-boot funcStat returns ERR_NODATA after observing
         * isNewData=1. It then expects the synchronous AutoLoad call itself to
         * succeed so its state machine can continue into new-profile setup. */
        return CELL_OK;
    }
    return result;
}

s32 cellSaveDataDelete2(u32 container)
{
    printf("[cellSaveData] Delete2(container=%u)\n", container);

    /* This API owns a firmware deletion UI and does not identify a directory.
     * Never report a deletion that did not happen: callers may discard their
     * in-memory slot after CELL_OK.  A real selector can replace this explicit
     * result later without risking unrelated host save directories. */
    return CELL_SAVEDATA_ERROR_NOTSUPPORTED;
}

/* ---------------------------------------------------------------------------
 * Old / non-_2 variants — same wrapper pattern as the _2 versions, with
 * the same guest-callback caveat.
 *
 * Older PS3 SDK builds (e.g. our flOw NPUA80001 dump) link the original
 * cellSaveDataAutoSave / AutoLoad / Delete instead of the _2 variants
 * RPCS3's flOw build uses. Same semantics, different NID.
 * -----------------------------------------------------------------------*/
s32 cellSaveDataAutoSave(u32 version, const char* dirName,
                          u32 errDialog,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile,
                          u32 container, void* userdata)
{
    char checked_dir[CELL_SAVEDATA_DIRNAME_SIZE];
    (void)errDialog; (void)container;
    if (!copy_save_dir_name_arg(dirName, checked_dir) ||
        !validate_guest_auto_set_buf(setBuf) || !funcStat || !funcFile)
        return CELL_SAVEDATA_ERROR_PARAM;

    printf("[cellSaveData] AutoSave(version=%u, dir='%s')\n",
           version, checked_dir);

    return savedata_execute_guest_auto(checked_dir, 1, setBuf, funcStat, funcFile,
                                       userdata);
}

s32 cellSaveDataAutoLoad(u32 version, const char* dirName,
                          u32 errDialog,
                          CellSaveDataSetBuf* setBuf,
                          CellSaveDataStatCallback funcStat,
                          CellSaveDataFileCallback funcFile,
                          u32 container, void* userdata)
{
    char checked_dir[CELL_SAVEDATA_DIRNAME_SIZE];
    (void)version; (void)errDialog; (void)container;
    if (!copy_save_dir_name_arg(dirName, checked_dir) ||
        !validate_guest_auto_set_buf(setBuf) || !funcStat || !funcFile)
        return CELL_SAVEDATA_ERROR_PARAM;

    printf("[cellSaveData] AutoLoad(version=%u, dir='%s')\n",
           version, checked_dir);

    s32 result = savedata_execute_guest_auto(checked_dir, 0, setBuf, funcStat,
                                              funcFile, userdata);
    if (result == CELL_SAVEDATA_ERROR_NODATA) {
        /* Same first-boot compat return as cellSaveDataAutoLoad2 above (flОw
         * calls this old non-_2 variant): ERR_NODATA from funcStat on a new
         * profile must not surface as an error, or the title parks in
         * MODE_AUTO_LOAD. See the longer note there. */
        return CELL_OK;
    }
    return result;
}

s32 cellSaveDataDelete(u32 version, const char* dirName,
                        u32 container)
{
    (void)version;
    char checked_dir[CELL_SAVEDATA_DIRNAME_SIZE];
    if (!copy_save_dir_name_arg(dirName, checked_dir))
        return CELL_SAVEDATA_ERROR_PARAM;
    printf("[cellSaveData] Delete(dir='%s', container=%u)\n",
           checked_dir, container);
    return CELL_SAVEDATA_ERROR_NOTSUPPORTED;
}

/* ---------------------------------------------------------------------------
 * User variants (cellSaveDataUserAutoSave/Load): same as the non-User forms
 * with a userId inserted after version. The trailing userdata is the guest's
 * NINTH argument (on the guest stack, not in r3-r10) — the generic adapter
 * only passes 8 registers, so declare 8 params and pass NULL through.
 * LittleBigPlanet auto-loads its profile through UserAutoLoad at boot.
 * -----------------------------------------------------------------------*/
s32 cellSaveDataUserAutoSave(u32 version, u32 userId, const char* dirName,
                             u32 errDialog, CellSaveDataSetBuf* setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile, u32 container)
{
    (void)userId;
    printf("[cellSaveData] UserAutoSave(user=%u)\n", userId);
    return cellSaveDataAutoSave(version, dirName, errDialog, setBuf,
                                funcStat, funcFile, container, NULL);
}

/* NOTE: the User* entry points take NINE arguments -- cellSaveDataUserAutoLoad
 * inserts `userId` ahead of dirName, which pushes `userdata` into the ninth
 * slot. The generic HLE adapter only forwards r3..r10 (eight), so the ninth
 * argument is unreachable here and MUST come from the caller's parameter save
 * area. That is what hle_cellSaveDataUserAutoLoad (below) is for; this plain-C
 * entry only sees eight and cannot know userdata. It stays for direct/internal
 * callers, and the ctx handler overrides it on the NID path. */
/* ---------------------------------------------------------------------------
 * Nine-argument entry points
 *
 * cellSaveDataUserAutoLoad/UserAutoSave take nine arguments; the generic HLE
 * adapter forwards only r3..r10, so the ninth (userdata) never arrives and used
 * to be hardcoded to NULL. That is fatal: a funcStat callback typically recovers
 * its own object from cbResult->userdata. LBP's is
 *     v = cbResult->userdata;  r = sub(v + 104, cbResult, get, set);
 * so a NULL made it work off guest address 104, read the VM's zero-fill, and
 * return CELL_SAVEDATA_CBRESULT_ERR_NODATA (-4) -- it never reached its handler.
 *
 * Take the full context and read the ninth argument from the caller's parameter
 * save area. SAVEDATA_ARG9_SP_OFF can be overridden at runtime
 * (PS3_SAVEDATA_ARG9_OFF) and the surrounding window is dumped once, so the
 * offset is confirmed from a real call rather than assumed.
 * -----------------------------------------------------------------------*/
#include "../../runtime/ppu/ppu_context.h"

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

/* PPC64 ELFv1: back chain, CR, LR, 2 reserved, TOC = 48 bytes, then the
 * parameter save area shadowing r3.. one doubleword each -> arg9 at SP+112. */
#define SAVEDATA_ARG9_SP_OFF 112u

static uint32_t savedata_arg9(ppu_context* ctx)
{
    uint32_t sp = (uint32_t)ctx->gpr[1];
    static int off = -1;
    if (off < 0) {
        const char* e = getenv("PS3_SAVEDATA_ARG9_OFF");
        off = e ? (int)strtol(e, NULL, 0) : (int)SAVEDATA_ARG9_SP_OFF;
    }
    { static int dumped = 0;
      if (!dumped) { dumped = 1;
        printf("[cellSaveData] caller SP=0x%08X param-save-area window:\n", sp);
        for (uint32_t o = 48; o <= 136; o += 8)
            printf("    SP+%3u = 0x%016llX%s\n", o,
                   (unsigned long long)vm_read64(sp + o),
                   o == (uint32_t)off ? "   <- taking as arg9 (userdata)" : "");
      } }
    return (uint32_t)vm_read64(sp + (uint32_t)off);
}

static s32 savedata_fixed_delete_guest(uint32_t set_list_ea,
                                       uint32_t set_buf_ea,
                                       uint32_t func_fixed_opd,
                                       uint32_t func_done_opd,
                                       uint32_t container,
                                       uint32_t userdata_ea)
{
    (void)container;
    if (!g_ps3_guest_caller || !func_fixed_opd || !func_done_opd ||
        !guest_range_valid(set_list_ea, PS3_SAVEDATA_SETLIST_SIZE) ||
        !guest_range_valid(set_buf_ea, PS3_SAVEDATA_SETBUF_SIZE))
        return CELL_SAVEDATA_ERROR_PARAM;

    uint32_t sort_type = vm_read32(set_list_ea +
                                   PS3_SAVEDATA_SETLIST_SORT_TYPE);
    uint32_t sort_order = vm_read32(set_list_ea +
                                    PS3_SAVEDATA_SETLIST_SORT_ORDER);
    uint32_t prefix_ea = vm_read32(set_list_ea +
                                   PS3_SAVEDATA_SETLIST_PREFIX);
    if (sort_type > CELL_SAVEDATA_SORTTYPE_SUBTITLE ||
        sort_order > CELL_SAVEDATA_SORTORDER_ASCENT || !prefix_ea ||
        vm_read32(set_list_ea + PS3_SAVEDATA_SETLIST_RESERVED) != 0)
        return CELL_SAVEDATA_ERROR_PARAM;

    char prefix[CELL_SAVEDATA_PREFIX_SIZE + 1];
    memset(prefix, 0, sizeof(prefix));
    if (!guest_copy_bounded_string(prefix_ea, prefix,
                                   CELL_SAVEDATA_PREFIX_SIZE, 1))
        return CELL_SAVEDATA_ERROR_PARAM;

    Ps3SaveDataGuestSetBuf set_buf =
        ps3_savedata_decode_set_buf(vm_base + set_buf_ea);
    if (set_buf.dir_list_max > CELL_SAVEDATA_DIRLIST_MAX ||
        set_buf.file_list_max != 0)
        return CELL_SAVEDATA_ERROR_PARAM;
    for (uint32_t i = 0; i < 6; i++) {
        if (vm_read32(set_buf_ea + PS3_SAVEDATA_SETBUF_RESERVED + i * 4))
            return CELL_SAVEDATA_ERROR_PARAM;
    }

    uint32_t required = set_buf.dir_list_max * PS3_SAVEDATA_DIRLIST_SIZE;
    if (required > set_buf.buffer_size ||
        (set_buf.buffer_size &&
         !guest_range_valid(set_buf.buffer_ea, set_buf.buffer_size)))
        return CELL_SAVEDATA_ERROR_PARAM;

    CellSaveDataDirList* entries = NULL;
    if (set_buf.dir_list_max) {
        entries = (CellSaveDataDirList*)calloc(set_buf.dir_list_max,
                                               sizeof(*entries));
        if (!entries)
            return CELL_SAVEDATA_ERROR_INTERNAL;
    }

    uint32_t dir_num = enumerate_save_dirs(prefix, entries,
                                           set_buf.dir_list_max);
    uint32_t dir_list_num = dir_num < set_buf.dir_list_max
                                ? dir_num : set_buf.dir_list_max;
    if (set_buf.buffer_size)
        memset(vm_base + set_buf.buffer_ea, 0, set_buf.buffer_size);
    for (uint32_t i = 0; i < dir_list_num; i++)
        marshal_dir_list(set_buf.buffer_ea +
                             i * PS3_SAVEDATA_DIRLIST_SIZE,
                         &entries[i]);

    scratch_reset();
    uint32_t cb_ea = scratch_alloc(PS3_SAVEDATA_CBRESULT_SIZE);
    uint32_t list_get_ea = scratch_alloc(PS3_SAVEDATA_LISTGET_SIZE);
    uint32_t fixed_set_ea = scratch_alloc(PS3_SAVEDATA_FIXEDSET_SIZE);
    uint32_t done_get_ea = scratch_alloc(PS3_SAVEDATA_DONEGET_SIZE);
    if (!cb_ea || !list_get_ea || !fixed_set_ea || !done_get_ea) {
        free(entries);
        return CELL_SAVEDATA_ERROR_INTERNAL;
    }

    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT,
                          userdata_ea);
    vm_write32(list_get_ea + PS3_SAVEDATA_LISTGET_DIR_NUM, dir_num);
    vm_write32(list_get_ea + PS3_SAVEDATA_LISTGET_LIST_NUM, dir_list_num);
    vm_write32(list_get_ea + PS3_SAVEDATA_LISTGET_LIST,
               set_buf.buffer_ea);

    printf("[cellSaveData] FixedDelete list prefix='%s' total=%u listed=%u userdata=0x%08X\n",
           prefix, dir_num, dir_list_num, userdata_ea);
    g_ps3_guest_caller(func_fixed_opd, cb_ea, list_get_ea, fixed_set_ea,
                       0, 0, 0, 0, 0);

    s32 cb_result = marshal_cbresult_read_result(cb_ea);
    if (cb_result < 0) {
        free(entries);
        return callback_result_error(cb_result);
    }
    if (cb_result == CELL_SAVEDATA_CBRESULT_OK_LAST) {
        free(entries);
        return CELL_OK;
    }
    if (cb_result != CELL_SAVEDATA_CBRESULT_OK_NEXT) {
        free(entries);
        return CELL_SAVEDATA_ERROR_CBRESULT;
    }

    uint32_t selected_ea = vm_read32(
        fixed_set_ea + PS3_SAVEDATA_FIXEDSET_DIRNAME);
    uint32_t option = vm_read32(fixed_set_ea +
                                PS3_SAVEDATA_FIXEDSET_OPTION);
    char selected[CELL_SAVEDATA_DIRNAME_SIZE];
    memset(selected, 0, sizeof(selected));
    if (!selected_ea || !guest_copy_save_dir_name(selected_ea, selected)) {
        free(entries);
        return CELL_SAVEDATA_ERROR_PARAM;
    }
    if (option == CELL_SAVEDATA_OPTION_NONE) {
        /* Firmware normally owns the yes/no dialog. No native dialog is wired
         * here, so report cancel and preserve the save instead of deleting it. */
        free(entries);
        return 1;
    }
    if (option != CELL_SAVEDATA_OPTION_NOCONFIRM) {
        free(entries);
        return CELL_SAVEDATA_ERROR_PARAM;
    }

    int selected_found = 0;
    for (uint32_t i = 0; i < dir_list_num; i++) {
        if (strcmp(entries[i].dirName, selected) == 0) {
            selected_found = 1;
            break;
        }
    }
    free(entries);

    s32 exc_result = CELL_SAVEDATA_ERROR_NODATA;
    uint64_t size_bytes = 0;
    char save_path[1024];
    save_path[0] = 0;
    if (selected_found) {
        if (!build_save_path(save_path, sizeof(save_path), selected)) {
            exc_result = CELL_SAVEDATA_ERROR_ACCESS_ERROR;
        } else {
            if (!dir_exists(save_path)) {
                exc_result = CELL_SAVEDATA_ERROR_NODATA;
            } else if (!savedata_measure_tree(save_path, &size_bytes) ||
                       !savedata_remove_tree(save_path)) {
                exc_result = CELL_SAVEDATA_ERROR_ACCESS_ERROR;
            } else {
                exc_result = CELL_OK;
            }
        }
    }

    memset(vm_base + done_get_ea, 0, PS3_SAVEDATA_DONEGET_SIZE);
    vm_write32(done_get_ea + PS3_SAVEDATA_DONEGET_RESULT,
               (uint32_t)exc_result);
    memcpy(vm_base + done_get_ea + PS3_SAVEDATA_DONEGET_DIRNAME,
           selected, strnlen(selected, CELL_SAVEDATA_DIRNAME_SIZE - 1));
    uint64_t size_kb64 = size_bytes / 1024u;
    uint32_t size_kb = size_kb64 > 0x7FFFFFFFu
                           ? 0x7FFFFFFFu : (uint32_t)size_kb64;
    vm_write32(done_get_ea + PS3_SAVEDATA_DONEGET_SIZE_KB, size_kb);
    vm_write32(done_get_ea + PS3_SAVEDATA_DONEGET_FREE_KB, 1024u * 1024u);

    marshal_cbresult_init(cb_ea, CELL_SAVEDATA_CBRESULT_OK_NEXT,
                          userdata_ea);
    g_ps3_guest_caller(func_done_opd, cb_ea, done_get_ea, 0,
                       0, 0, 0, 0, 0);
    cb_result = marshal_cbresult_read_result(cb_ea);
    if (cb_result < 0)
        return callback_result_error(cb_result);
    if (cb_result == CELL_SAVEDATA_CBRESULT_OK_NEXT ||
        cb_result == CELL_SAVEDATA_CBRESULT_OK_LAST ||
        cb_result == CELL_SAVEDATA_CBRESULT_OK_LAST_NOCONFIRM)
        return CELL_OK;
    return CELL_SAVEDATA_ERROR_CBRESULT;
}

void cellSaveDataFixedDelete_ctx(ppu_context* ctx)
{
    if (!ctx)
        return;
    s32 result = savedata_fixed_delete_guest(
        (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
        (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6],
        (uint32_t)ctx->gpr[7], (uint32_t)ctx->gpr[8]);
    ctx->gpr[3] = (uint64_t)(int64_t)result;
}

static void hle_cellSaveDataUserAutoSave(ppu_context* ctx)
{
    u32   version   = (u32)ctx->gpr[3];
    u32   userId    = (u32)ctx->gpr[4];
    const char* dir = (const char*)(uintptr_t)(u32)ctx->gpr[5];
    u32   errDialog = (u32)ctx->gpr[6];
    void* setBuf    = (void*)(uintptr_t)(u32)ctx->gpr[7];
    void* funcStat  = (void*)(uintptr_t)(u32)ctx->gpr[8];
    void* funcFile  = (void*)(uintptr_t)(u32)ctx->gpr[9];
    u32   container = (u32)ctx->gpr[10];
    uint32_t userdata = savedata_arg9(ctx);

    printf("[cellSaveData] UserAutoSave(user=%u, userdata=0x%08X)\n", userId, userdata);
    ctx->gpr[3] = (uint64_t)(int64_t)(s32)
        cellSaveDataAutoSave2(version, dir, errDialog,
                              (CellSaveDataSetBuf*)setBuf,
                              (CellSaveDataStatCallback)funcStat,
                              (CellSaveDataFileCallback)funcFile,
                              container, (void*)(uintptr_t)userdata);
}

static void hle_cellSaveDataUserAutoLoad(ppu_context* ctx)
{
    u32   version   = (u32)ctx->gpr[3];
    u32   userId    = (u32)ctx->gpr[4];
    const char* dir = (const char*)(uintptr_t)(u32)ctx->gpr[5];
    u32   errDialog = (u32)ctx->gpr[6];
    void* setBuf    = (void*)(uintptr_t)(u32)ctx->gpr[7];
    void* funcStat  = (void*)(uintptr_t)(u32)ctx->gpr[8];
    void* funcFile  = (void*)(uintptr_t)(u32)ctx->gpr[9];
    u32   container = (u32)ctx->gpr[10];
    uint32_t userdata = savedata_arg9(ctx);

    printf("[cellSaveData] UserAutoLoad(user=%u, userdata=0x%08X)\n", userId, userdata);
    ctx->gpr[3] = (uint64_t)(int64_t)(s32)
        cellSaveDataAutoLoad2(version, dir, errDialog,
                              (CellSaveDataSetBuf*)setBuf,
                              (CellSaveDataStatCallback)funcStat,
                              (CellSaveDataFileCallback)funcFile,
                              container, (void*)(uintptr_t)userdata);
}

void cellSaveData_register_ctx_handlers(void)
{
    /* Overrides the generated 8-arg registration: ctx handlers are dispatched
     * before the generic table. */
    ps3_hle_register_ctx(0x52AAC4FAu, "cellSaveDataUserAutoSave",
                         hle_cellSaveDataUserAutoSave);
    ps3_hle_register_ctx(0xCDC6AEFDu, "cellSaveDataUserAutoLoad",
                         hle_cellSaveDataUserAutoLoad);
}

s32 cellSaveDataUserAutoLoad(u32 version, u32 userId, const char* dirName,
                             u32 errDialog, CellSaveDataSetBuf* setBuf,
                             CellSaveDataStatCallback funcStat,
                             CellSaveDataFileCallback funcFile, u32 container)
{
    (void)userId;
    printf("[cellSaveData] UserAutoLoad(user=%u) [8-arg path: userdata unavailable]\n", userId);
    return cellSaveDataAutoLoad(version, dirName, errDialog, setBuf,
                                funcStat, funcFile, container, NULL);
}
