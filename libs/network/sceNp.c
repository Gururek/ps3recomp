/*
 * ps3recomp - sceNp HLE implementation
 *
 * Provides fake PSN identity so games can proceed through NP checks.
 * The username defaults to "PS3Player" but is configurable.
 */

#include "sceNp.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32 -- out-params are guest EAs */
#include <stdio.h>
#include <string.h>

#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_np_initialized = 0;
static int  s_np_score_initialized = 0;
static char s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH + 1] = "PS3Player";

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void sceNpSetFakeUsername(const char* username)
{
    if (username) {
        strncpy(s_fake_username, username, SCE_NP_ONLINEID_MAX_LENGTH);
        s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH] = '\0';
    }
}

/* Build a fake NP ID from the current username */
static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, s_fake_username, SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr)
{
    (void)poolSize;
    (void)poolPtr;

    printf("[sceNp] Init(poolSize=%u, username=\"%s\")\n",
           poolSize, s_fake_username);

    if (s_np_initialized)
        return SCE_NP_ERROR_ALREADY_INITIALIZED;

    s_np_initialized = 1;
    return CELL_OK;
}

s32 sceNpTerm(void)
{
    printf("[sceNp] Term()\n");

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_score_initialized = 0;
    s_np_initialized = 0;
    return CELL_OK;
}

s32 sceNpScoreInit(void)
{
    printf("[sceNp] ScoreInit()\n");

    if (s_np_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_score_initialized = 1;
    return CELL_OK;
}

s32 sceNpScoreTerm(void)
{
    printf("[sceNp] ScoreTerm()\n");

    if (!s_np_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_score_initialized = 0;
    return CELL_OK;
}

s32 sceNpDrmIsAvailable(const u8* k_licensee, const char* drm_path)
{
    uint32_t key_ea = (uint32_t)(uintptr_t)k_licensee;
    uint32_t path_ea = (uint32_t)(uintptr_t)drm_path;
    if (!key_ea || !path_ea)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    uint8_t key[16];
    char path[256];
    for (uint32_t i = 0; i < sizeof key; i++) key[i] = vm_read8(key_ea + i);
    uint32_t i = 0;
    for (; i + 1 < sizeof path; i++) {
        path[i] = (char)vm_read8(path_ea + i);
        if (!path[i]) break;
    }
    path[sizeof path - 1] = '\0';

    /* Hardware checks that the NPDRM target exists before accepting its
     * license.  Returning CELL_OK for Tokyo Jungle's absent DLC made it spawn
     * and join an NPDRM-open thread for every DLC slot, stretching a simple
     * no-DLC scan into several minutes of blank frames. */
    extern int ppu_fs_guest_exists(const char* guest_path);
    if (!ppu_fs_guest_exists(path)) {
        printf("[sceNp] DrmIsAvailable(path='%s') -> CELL_ENOENT\n", path);
        return (s32)CELL_ENOENT;
    }

    extern void ppu_fs_set_npdrm_key(const uint8_t key[16]);
    ppu_fs_set_npdrm_key(key);
    printf("[sceNp] DrmIsAvailable(path='%s') -> licensed\n", path);
    return CELL_OK;
}

s32 sceNpGetNpId(s32 userId, SceNpId* npId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    npId = GUEST_PTR(npId, SceNpId*);
    np_build_fake_id(npId);
    printf("[sceNp] GetNpId(user=%d) -> \"%s\"\n", userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    onlineId = GUEST_PTR(onlineId, SceNpOnlineId*);
    memset(onlineId, 0, sizeof(SceNpOnlineId));
    strncpy(onlineId->data, s_fake_username, SCE_NP_ONLINEID_MAX_LENGTH);
    onlineId->term = '\0';

    printf("[sceNp] GetOnlineId(user=%d) -> \"%s\"\n", userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineName)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    onlineName = GUEST_PTR(onlineName, SceNpOnlineName*);
    memset(onlineName, 0, sizeof(SceNpOnlineName));
    strncpy(onlineName->data, s_fake_username,
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);

    printf("[sceNp] GetOnlineName(user=%d) -> \"%s\"\n",
           userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!userInfo)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    userInfo = GUEST_PTR(userInfo, SceNpUserInfo*);
    memset(userInfo, 0, sizeof(SceNpUserInfo));
    np_build_fake_id(&userInfo->npId);
    strncpy(userInfo->onlineName.data, s_fake_username,
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);
    /* Leave avatar URL empty */

    printf("[sceNp] GetUserProfile(user=%d) -> \"%s\"\n",
           userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetAccountRegion(s32 userId, u32* region)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!region)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Region: US (SCEA) = 0x5553 ('US') */
    vm_write32((uint32_t)(uintptr_t)region, 0x5553);

    printf("[sceNp] GetAccountRegion(user=%d) -> US\n", userId);
    return CELL_OK;
}

s32 sceNpGetAccountAge(s32 userId, s32* age)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    vm_write32((uint32_t)(uintptr_t)age, 25); /* default adult age */
    printf("[sceNp] GetAccountAge(user=%d) -> 25\n", userId);
    return CELL_OK;
}

s32 sceNpGetMyLanguages(SceNpMyLanguages* langs)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!langs)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    uint32_t langs_ea = (uint32_t)(uintptr_t)langs;
    vm_write32(langs_ea + 0, SCE_NP_LANG_ENGLISH);
    vm_write32(langs_ea + 4, 0);
    vm_write32(langs_ea + 8, 0);

    printf("[sceNp] GetMyLanguages() -> English\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager (sign-in state)
 *
 * The toolkit runs with a fake local profile but no live PSN connection. So the
 * manager reports OFFLINE — games gate their online flows on GetStatus and skip
 * them cleanly — while the identity getters still hand back the fake account,
 * matching how NP behaves on a real signed-in-but-disconnected console.
 * Imported by most online-capable titles.
 * -----------------------------------------------------------------------*/

static SceNpManagerCallback s_npmgr_cb = NULL;
static void*                s_npmgr_cb_arg = NULL;

s32 sceNpManagerGetStatus(s32* status)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    vm_write32((uint32_t)(uintptr_t)status, SCE_NP_MANAGER_STATUS_OFFLINE);
    printf("[sceNp] ManagerGetStatus() -> OFFLINE\n");
    return CELL_OK;
}

s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    /* Stored for bookkeeping; we never transition online so never fire it. */
    s_npmgr_cb     = callback;
    s_npmgr_cb_arg = arg;
    printf("[sceNp] ManagerRegisterCallback()\n");
    return CELL_OK;
}

s32 sceNpManagerUnregisterCallback(void)
{
    s_npmgr_cb     = NULL;
    s_npmgr_cb_arg = NULL;
    return CELL_OK;
}

/* Identity getters: reuse the fake-profile implementations (offline-with-account). */
s32 sceNpManagerGetNpId(SceNpId* npId)               { return sceNpGetNpId(0, npId); }
s32 sceNpManagerGetOnlineId(SceNpOnlineId* onlineId) { return sceNpGetOnlineId(0, onlineId); }
s32 sceNpManagerGetOnlineName(SceNpOnlineName* name) { return sceNpGetOnlineName(0, name); }
s32 sceNpManagerGetAccountAge(s32* age)              { return sceNpGetAccountAge(0, age); }
