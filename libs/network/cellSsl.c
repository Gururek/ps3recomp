/*
 * ps3recomp - cellSsl HLE implementation
 *
 * SSL module with lifecycle, firmware certificate loader, stub certificate
 * metadata getters, and host OS entropy.
 */

#include "cellSsl.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32 -- out-params are guest EAs */
#include "../../runtime/memory/vm.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* VFS owns all /dev_flash mount translation. This resolver returns a host
 * path only when the requested guest file exists. */
extern int ppu_fs_guest_to_host(const char* guest, char* out, size_t cap);

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_ssl_initialized = 0;

#define CELL_SSL_CERT_COUNT 58u
#define CELL_SSL_CERT_FLAG_MASK ((UINT64_C(1) << CELL_SSL_CERT_COUNT) - UINT64_C(1))

static int ssl_loader_guest_range_valid(u32 ea, uint64_t length)
{
    const uint64_t end = (uint64_t)ea + length;
    if (!vm_base || end > (UINT64_C(1) << 32))
        return 0;
    return !ppu_vm_size || end <= ppu_vm_size;
}

static u32 ssl_loader_cert_id(u32 selected_id, int normal_set)
{
    if (!normal_set || selected_id < 6 || selected_id > 23)
        return selected_id;
    if (selected_id == 6)
        return 23;
    if (selected_id == 13)
        return 6;
    if (selected_id == 18)
        return 15;
    return selected_id - 1;
}

static s32 ssl_loader_resolve_cert(u32 cert_id, char* host_path, size_t host_cap)
{
    char guest_path[64];
    const int written = snprintf(guest_path, sizeof(guest_path),
                                 "/dev_flash/data/cert/CA%02u.cer", cert_id);
    if (written < 0 || (size_t)written >= sizeof(guest_path))
        return (s32)CELL_SSL_ERROR_UNRETRIEVABLE;
    return ppu_fs_guest_to_host(guest_path, host_path, host_cap)
        ? CELL_OK : (s32)CELL_SSL_ERROR_NOT_FOUND;
}

static s32 ssl_loader_file_size(const char* host_path, u32* size_out)
{
    FILE* file = fopen(host_path, "rb");
    if (!file)
        return (s32)CELL_SSL_ERROR_UNRETRIEVABLE;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return (s32)CELL_SSL_ERROR_UNRETRIEVABLE;
    }
    const long file_size = ftell(file);
    fclose(file);
    if (file_size < 0)
        return (s32)CELL_SSL_ERROR_UNRETRIEVABLE;
    if ((unsigned long)file_size > UINT32_MAX)
        return (s32)CELL_SSL_ERROR_NO_MEMORY;
    if (file_size == 0)
        return (s32)CELL_SSL_ERROR_UNRETRIEVABLE;
    *size_out = (u32)file_size;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellSslInit(void* pool, u32 poolSize)
{
    (void)pool;
    (void)poolSize;
    printf("[cellSsl] Init(poolSize=%u)\n", poolSize);

    if (s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_ALREADY_INITIALIZED;

    s_ssl_initialized = 1;
    return CELL_OK;
}

s32 cellSslEnd(void)
{
    printf("[cellSsl] End()\n");

    if (!s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_NOT_INITIALIZED;

    s_ssl_initialized = 0;
    return CELL_OK;
}

s32 cellSslCertificateLoader(u64 flags, char* buffer, u32 size, u32* required)
{
    printf("[cellSsl] CertificateLoader(flags=0x%llX)\n", (unsigned long long)flags);

    if (!s_ssl_initialized)
        return (s32)CELL_SSL_ERROR_NOT_INITIALIZED;
    if (flags & ~CELL_SSL_CERT_FLAG_MASK)
        return (s32)CELL_SSL_ERROR_UNKNOWN_LOAD_CERT;

    const int normal_set = (flags & (UINT64_C(1) << (6 - 1))) != 0;
    uint64_t total = 0;
    char host_path[1100];
    for (u32 selected_id = 1; selected_id <= CELL_SSL_CERT_COUNT; selected_id++) {
        if (!(flags & (UINT64_C(1) << (selected_id - 1))))
            continue;

        u32 cert_size = 0;
        s32 rc = ssl_loader_resolve_cert(ssl_loader_cert_id(selected_id, normal_set),
                                         host_path, sizeof(host_path));
        if (rc != CELL_OK)
            return rc;
        rc = ssl_loader_file_size(host_path, &cert_size);
        if (rc != CELL_OK)
            return rc;
        total += cert_size;
        if (total > UINT32_MAX)
            return (s32)CELL_SSL_ERROR_NO_MEMORY;
    }

    const u32 total_size = (u32)total;
    const u32 required_ea = (u32)(uintptr_t)required;
    if (required_ea) {
        if (!ssl_loader_guest_range_valid(required_ea, sizeof(u32)))
            return (s32)CELL_SSL_ERROR_NO_BUFFER;
        vm_write32(required_ea, total_size);
        return CELL_OK;
    }

    const u32 buffer_ea = (u32)(uintptr_t)buffer;
    if (total_size && !buffer_ea)
        return (s32)CELL_SSL_ERROR_NO_BUFFER;
    if (size < total_size)
        return (s32)CELL_SSL_ERROR_NO_BUFFER;
    if (!total_size)
        return CELL_OK;
    if (!ssl_loader_guest_range_valid(buffer_ea, total_size))
        return (s32)CELL_SSL_ERROR_NO_BUFFER;

    u32 offset = 0;
    for (u32 selected_id = 1; selected_id <= CELL_SSL_CERT_COUNT; selected_id++) {
        if (!(flags & (UINT64_C(1) << (selected_id - 1))))
            continue;

        s32 rc = ssl_loader_resolve_cert(ssl_loader_cert_id(selected_id, normal_set),
                                         host_path, sizeof(host_path));
        if (rc != CELL_OK)
            return rc;
        FILE* file = fopen(host_path, "rb");
        if (!file)
            return (s32)CELL_SSL_ERROR_UNRETRIEVABLE;

        u32 cert_size = 0;
        rc = ssl_loader_file_size(host_path, &cert_size);
        if (rc != CELL_OK) {
            fclose(file);
            return rc;
        }
        if (cert_size > total_size - offset) {
            fclose(file);
            return (s32)CELL_SSL_ERROR_UNRETRIEVABLE;
        }
        if (cert_size && fread(vm_base + buffer_ea + offset, 1, cert_size, file) != cert_size) {
            fclose(file);
            return (s32)CELL_SSL_ERROR_UNRETRIEVABLE;
        }
        fclose(file);
        offset += cert_size;
    }
    return offset == total_size ? CELL_OK : (s32)CELL_SSL_ERROR_UNRETRIEVABLE;
}

s32 cellSslCertGetSerialNumber(CellSslCertId certId, u8* serial, u32* serialSize)
{
    (void)certId;
    printf("[cellSsl] CertGetSerialNumber()\n");

    if (!serial || !serialSize)
        return (s32)CELL_SSL_ERROR_INVALID_ARG;

    /* Fake serial number */
    if (*serialSize >= 4) {
        serial[0] = 0x01;
        serial[1] = 0x00;
        serial[2] = 0x00;
        serial[3] = 0x01;
        *serialSize = 4;
    }
    return CELL_OK;
}

s32 cellSslCertGetPublicKey(CellSslCertId certId, u8* key, u32* keySize)
{
    (void)certId;
    (void)key;
    printf("[cellSsl] CertGetPublicKey()\n");

    if (keySize)
        *keySize = 0;

    return CELL_OK;
}

s32 cellSslCertGetNotBefore(CellSslCertId certId, u64* time)
{
    (void)certId;
    if (!time) return (s32)CELL_SSL_ERROR_INVALID_ARG;
    *time = 0;
    return CELL_OK;
}

s32 cellSslCertGetNotAfter(CellSslCertId certId, u64* time)
{
    (void)certId;
    if (!time) return (s32)CELL_SSL_ERROR_INVALID_ARG;
    *time = 0xFFFFFFFFFFFFFFFFULL;
    return CELL_OK;
}

s32 cellSslGetRandomNumber(u8* buf, u32 size)
{
    if (!buf || size == 0)
        return (s32)CELL_SSL_ERROR_INVALID_ARG;

#ifdef _WIN32
    NTSTATUS status = BCryptGenRandom(NULL, buf, size,
                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        /* Fallback: fill with pseudo-random */
        for (u32 i = 0; i < size; i++)
            buf[i] = (u8)(i * 37 + 13);
    }
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, buf, size);
        close(fd);
    } else {
        for (u32 i = 0; i < size; i++)
            buf[i] = (u8)(i * 37 + 13);
    }
#endif

    return CELL_OK;
}
