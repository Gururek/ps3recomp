/*
 * prx_loader.c — see prx_loader.h for the design.
 *
 * Two steps per module: place the relocated image into guest RAM at its load
 * base, then register every lifted function in the host dispatch table.
 */
#include "prx_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

/* The runtime's guest memory: host_ptr = vm_base + guest_addr. Declared by
 * runtime/memory/vm.h; we only need the symbol, so extern it directly to keep
 * the loader independent of the memory backend's init path. */
extern uint8_t* vm_base;

/* ---- export registry ---------------------------------------------------- *
 * Every writer is retained in order, so unloading/re-registering one dynamic
 * owner exposes the preceding static or dynamic writer for the same NID/kind. */
typedef struct {
    uint32_t nid;
    uint32_t guest_addr;
    uint64_t owner;
    prx_dynamic_export_kind kind;
    uint64_t order;
} prx_export_reg_entry;
#define PRX_EXPORT_REG_CAP 2048
static prx_export_reg_entry s_export_reg[PRX_EXPORT_REG_CAP];
static uint32_t             s_export_reg_n = 0;
static uint64_t             s_export_reg_order = 0;

#ifdef _WIN32
static SRWLOCK s_export_reg_lock = SRWLOCK_INIT;
static void prx_export_lock(void) { AcquireSRWLockExclusive(&s_export_reg_lock); }
static void prx_export_unlock(void) { ReleaseSRWLockExclusive(&s_export_reg_lock); }
#else
static pthread_mutex_t s_export_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static void prx_export_lock(void) { pthread_mutex_lock(&s_export_reg_lock); }
static void prx_export_unlock(void) { pthread_mutex_unlock(&s_export_reg_lock); }
#endif

static int prx_dynamic_export_valid(const prx_dynamic_export* e)
{
    return e && e->nid && e->guest_addr &&
           (e->kind == PRX_DYNAMIC_EXPORT_FUNCTION ||
            e->kind == PRX_DYNAMIC_EXPORT_VARIABLE);
}

static void prx_export_remove_owner_locked(uint64_t owner)
{
    uint32_t out = 0;
    for (uint32_t i = 0; i < s_export_reg_n; i++) {
        if (s_export_reg[i].owner != owner)
            s_export_reg[out++] = s_export_reg[i];
    }
    s_export_reg_n = out;
}

static int prx_export_register_locked(uint64_t owner,
                                      const prx_dynamic_export* export_entry)
{
    if (s_export_reg_n >= PRX_EXPORT_REG_CAP)
        return 0;
    s_export_reg[s_export_reg_n].nid = export_entry->nid;
    s_export_reg[s_export_reg_n].guest_addr = export_entry->guest_addr;
    s_export_reg[s_export_reg_n].owner = owner;
    s_export_reg[s_export_reg_n].kind = export_entry->kind;
    s_export_reg[s_export_reg_n].order = ++s_export_reg_order;
    s_export_reg_n++;
    return 1;
}

static void prx_export_register_static(uint32_t nid, uint32_t guest_addr,
                                       prx_dynamic_export_kind kind)
{
    const prx_dynamic_export export_entry = {
        nid, guest_addr, kind
    };
    if (!prx_dynamic_export_valid(&export_entry))
        return;
    prx_export_lock();
    if (!prx_export_register_locked(0, &export_entry))
        fprintf(stderr, "[prx] export registry full, dropping NID 0x%08X\n", nid);
    prx_export_unlock();
}

uint32_t prx_resolve_export(uint32_t nid)
{
    uint32_t guest_addr = 0;
    prx_export_lock();
    uint64_t latest = 0;
    for (uint32_t i = 0; i < s_export_reg_n; i++) {
        if (s_export_reg[i].nid == nid &&
            s_export_reg[i].kind == PRX_DYNAMIC_EXPORT_FUNCTION &&
            s_export_reg[i].order > latest) {
            latest = s_export_reg[i].order;
            guest_addr = s_export_reg[i].guest_addr;
        }
    }
    prx_export_unlock();
    return guest_addr;
}

uint32_t prx_resolve_variable_export(uint32_t nid)
{
    uint32_t guest_addr = 0;
    prx_export_lock();
    uint64_t latest = 0;
    for (uint32_t i = 0; i < s_export_reg_n; i++) {
        if (s_export_reg[i].nid == nid &&
            s_export_reg[i].kind == PRX_DYNAMIC_EXPORT_VARIABLE &&
            s_export_reg[i].order > latest) {
            latest = s_export_reg[i].order;
            guest_addr = s_export_reg[i].guest_addr;
        }
    }
    prx_export_unlock();
    return guest_addr;
}

int prx_register_dynamic_exports(uint64_t owner,
                                 const prx_dynamic_export* exports,
                                 uint32_t export_count)
{
    if (!owner || !exports || !export_count || export_count > PRX_EXPORT_REG_CAP)
        return PRX_DYNAMIC_REGISTER_INVAL;
    for (uint32_t i = 0; i < export_count; i++)
        if (!prx_dynamic_export_valid(&exports[i]))
            return PRX_DYNAMIC_REGISTER_INVAL;

    prx_export_lock();
    uint32_t owner_entries = 0;
    for (uint32_t i = 0; i < s_export_reg_n; i++)
        if (s_export_reg[i].owner == owner)
            owner_entries++;

    uint32_t needed_entries = 0;
    for (uint32_t i = 0; i < export_count; i++) {
        int superseded = 0;
        for (uint32_t j = i + 1; j < export_count; j++) {
            if (exports[j].nid == exports[i].nid && exports[j].kind == exports[i].kind) {
                superseded = 1;
                break;
            }
        }
        if (!superseded)
            needed_entries++;
    }
    if (s_export_reg_n - owner_entries + needed_entries > PRX_EXPORT_REG_CAP) {
        prx_export_unlock();
        return PRX_DYNAMIC_REGISTER_FULL;
    }

    /* Capacity and inputs were preflighted, so no partial owner can escape. */
    prx_export_remove_owner_locked(owner);
    for (uint32_t i = 0; i < export_count; i++) {
        int superseded = 0;
        for (uint32_t j = i + 1; j < export_count; j++) {
            if (exports[j].nid == exports[i].nid && exports[j].kind == exports[i].kind) {
                superseded = 1;
                break;
            }
        }
        if (!superseded)
            (void)prx_export_register_locked(owner, &exports[i]);
    }
    prx_export_unlock();
    return PRX_DYNAMIC_REGISTER_OK;
}

uint32_t prx_unregister_dynamic_owner(uint64_t owner)
{
    if (!owner)
        return 0;
    prx_export_lock();
    const uint32_t before = s_export_reg_n;
    prx_export_remove_owner_locked(owner);
    const uint32_t removed = before - s_export_reg_n;
    prx_export_unlock();
    return removed;
}

uint32_t prx_export_registry_count(void)
{
    uint32_t count;
    prx_export_lock();
    count = s_export_reg_n;
    prx_export_unlock();
    return count;
}

prx_load_result prx_load_module(const prx_module* m, prx_register_fn reg)
{
    prx_load_result r;
    memset(&r, 0, sizeof(r));

    if (!m || !m->image || m->image_size == 0) {
        fprintf(stderr, "[prx] load failed: null/empty module\n");
        return r;
    }
    if (!vm_base) {
        fprintf(stderr, "[prx] load failed: vm_base not initialized "
                        "(call vm_init before loading PRX modules)\n");
        return r;
    }

    r.base       = m->base;
    r.image_size = m->image_size;

    /* Step A — place the relocated image into guest RAM at the load base.
     * The bytes already carry base-applied addresses (prx_relocate.py), so the
     * module's data, OPDs and pointer tables are immediately consistent with
     * the lifted code and the OPD-walking dispatcher. */
    memcpy(vm_base + m->base, m->image, m->image_size);

    /* Step B — register every lifted function at its guest address. The lift
     * baked the load base in (--base B), so e->addr is already B + offset and
     * needs no adjustment here. A null host function (rare: a table hole) is
     * skipped rather than registered. */
    if (reg) {
        for (uint64_t i = 0; i < m->func_count; i++) {
            const prx_func_entry* e = &m->funcs[i];
            if (!e->func)
                continue;
            reg((uint32_t)e->addr, e->func);
            r.funcs_registered++;
        }
    }

    /* Step C — publish exports as NID -> (base + vaddr) so a title's import
     * resolver can dispatch into this module by NID. Function exports point at
     * an OPD in the freshly-placed image; ps3_indirect_call walks it. */
    uint32_t exports_published = 0;
    if (m->exports) {
        for (uint32_t i = 0; i < m->export_count; i++) {
            const prx_export* e = &m->exports[i];
            if (e->nid == 0)
                continue;
            prx_export_register_static(e->nid, m->base + e->vaddr,
                e->kind == PRX_EXPORT_KIND_VARIABLE
                    ? PRX_DYNAMIC_EXPORT_VARIABLE : PRX_DYNAMIC_EXPORT_FUNCTION);
            exports_published++;
        }
    }

    r.ok = 1;
    fprintf(stderr,
            "[prx] loaded %-12s base=0x%08X size=%u (0x%X) funcs=%u/%llu exports=%u\n",
            m->name ? m->name : "?", m->base, m->image_size, m->image_size,
            r.funcs_registered, (unsigned long long)m->func_count,
            exports_published);
    return r;
}

uint8_t* prx_image_load_file(const char* path, uint32_t* out_size)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[prx] cannot open image '%s'\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "[prx] short read on '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_size)
        *out_size = (uint32_t)n;
    return buf;
}
