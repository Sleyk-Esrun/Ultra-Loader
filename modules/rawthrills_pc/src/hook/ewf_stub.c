/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  ewf_stub.c -- Enhanced Write Filter (ewfapi.dll) IAT stub.
 * =============================================================================
 *
 *  ENGINE FACTS (from static analysis of sdaemon.exe + I:/Raw Thrills Dumps/
 *  The Fast and the Furious Super Bikes/ewfapi.dll):
 *    ewfapi.dll exports 21 names (ordinal-based, EwfMgr*). sdaemon imports 4:
 *        EwfMgrGetProtectedVolumeList
 *        EwfMgrOpenProtected
 *        EwfMgrCommit
 *        EwfMgrVolumeNameListDelete
 *
 *    On real hardware EWF is the WinXP-Embedded write filter: the game commits
 *    changes on shutdown so cabinet state persists across reboots. On a
 *    non-XPE host EwfMgrOpenProtected returns NULL and the game's error path
 *    treats "no protected volume" as benign -- but its noisy log complaint
 *    keeps triggering periodically. Cleaner to stub the 4 imports.
 *
 *  WHAT THIS STUB DOES:
 *    IAT-patches those 4 imports to safe no-ops. The remaining 17 EwfMgr*
 *    exports are not touched -- if the game (or a shipped ewfapi.dll) resolves
 *    them at runtime via GetProcAddress they will still work; the stub only
 *    displaces the direct-imports the game actually uses.
 *
 *    Return values chosen so sdaemon's "no protected volume" path takes
 *    silently:
 *      EwfMgrGetProtectedVolumeList -> NULL   (no protected volumes)
 *      EwfMgrOpenProtected          -> INVALID_HANDLE_VALUE (open failed)
 *      EwfMgrCommit                 -> TRUE   (pretend commit succeeded)
 *      EwfMgrVolumeNameListDelete   -> void   (nothing to free)
 *
 *  Env gate: ULTRA_EWF_STUB=1. Default OFF.
 * =============================================================================
 */
#if defined(_WIN32)

#include <windows.h>

#include "rawthrills_hook.h"

static int g_enabled = 0;

/* Signatures per the ewfapi.dll PDB (verified from the shipped ewfapi.dll
 * exports). We return safe defaults; the caller's error-path handles NULL
 * gracefully because that's the "no EWF present" case Microsoft documents. */
static PVOID  WINAPI Hook_EwfMgrGetProtectedVolumeList(void) {
    return NULL;   /* "no protected volumes" */
}
static HANDLE WINAPI Hook_EwfMgrOpenProtected(LPCWSTR vol) {
    (void)vol;
    return INVALID_HANDLE_VALUE;   /* "no such protected volume" */
}
static BOOL WINAPI Hook_EwfMgrCommit(HANDLE h) {
    (void)h;
    return TRUE;   /* "commit succeeded" -- there is nothing to commit */
}
static void WINAPI Hook_EwfMgrVolumeNameListDelete(PVOID lst) {
    (void)lst;   /* nothing was allocated; nothing to free */
}

/* Additional 17 stubs so an ewfapi.dll shim rebuilt from this file exports the
 * full surface. They are compiled in but only wired via IAT-patch if the game
 * imports them. Return safe defaults. */
static BOOL   WINAPI Hook_EwfMgrCheckpoint(HANDLE h, PVOID d, ULONG n) { (void)h;(void)d;(void)n; return TRUE; }
static BOOL   WINAPI Hook_EwfMgrClearCommand(HANDLE h) { (void)h; return TRUE; }
static BOOL   WINAPI Hook_EwfMgrClose(HANDLE h) { (void)h; return TRUE; }
static BOOL   WINAPI Hook_EwfMgrDisable(HANDLE h) { (void)h; return TRUE; }
static BOOL   WINAPI Hook_EwfMgrEnable(HANDLE h) { (void)h; return TRUE; }
static BOOL   WINAPI Hook_EwfMgrGetDriveLetterFromVolumeName(LPCWSTR v, LPWSTR o, ULONG n) { (void)v;(void)o;(void)n; return FALSE; }
static PVOID  WINAPI Hook_EwfMgrGetOverlayStoreConfig(void) { return NULL; }
static PVOID  WINAPI Hook_EwfMgrGetOverlayStoreVolumeName(void) { return NULL; }
static BOOL   WINAPI Hook_EwfMgrGetPersistentData(HANDLE h, PVOID d, ULONG* n) { (void)h;(void)d; if(n) *n=0; return TRUE; }
static PVOID  WINAPI Hook_EwfMgrGetProtectedVolumeConfig(HANDLE h) { (void)h; return NULL; }
static HANDLE WINAPI Hook_EwfMgrOpenOverlayStore(LPCWSTR v) { (void)v; return INVALID_HANDLE_VALUE; }
static BOOL   WINAPI Hook_EwfMgrRegisterLowSpaceNotification(HANDLE h, HANDLE ev) { (void)h;(void)ev; return TRUE; }
static BOOL   WINAPI Hook_EwfMgrRestore(HANDLE h) { (void)h; return TRUE; }
static BOOL   WINAPI Hook_EwfMgrSetLevel(HANDLE h, ULONG l) { (void)h;(void)l; return TRUE; }
static BOOL   WINAPI Hook_EwfMgrSetPersistentData(HANDLE h, PVOID d, ULONG n) { (void)h;(void)d;(void)n; return TRUE; }
static LPWSTR WINAPI Hook_EwfMgrVolumeNameEntryPop(PVOID lst) { (void)lst; return NULL; }
static BOOL   WINAPI Hook_EwfMgrVolumeNameListIsEmpty(PVOID lst) { (void)lst; return TRUE; }

/* Suppress unused-static warnings for the extended surface. If we later ship an
 * ewfapi.dll drop-in, these become the DEF exports. Referencing them via a
 * unused-attribute keeps -Wall clean without moving them to the header. */
__attribute__((used)) static void* const k_ewf_extended_used[] = {
    (void*)Hook_EwfMgrCheckpoint,
    (void*)Hook_EwfMgrClearCommand,
    (void*)Hook_EwfMgrClose,
    (void*)Hook_EwfMgrDisable,
    (void*)Hook_EwfMgrEnable,
    (void*)Hook_EwfMgrGetDriveLetterFromVolumeName,
    (void*)Hook_EwfMgrGetOverlayStoreConfig,
    (void*)Hook_EwfMgrGetOverlayStoreVolumeName,
    (void*)Hook_EwfMgrGetPersistentData,
    (void*)Hook_EwfMgrGetProtectedVolumeConfig,
    (void*)Hook_EwfMgrOpenOverlayStore,
    (void*)Hook_EwfMgrRegisterLowSpaceNotification,
    (void*)Hook_EwfMgrRestore,
    (void*)Hook_EwfMgrSetLevel,
    (void*)Hook_EwfMgrSetPersistentData,
    (void*)Hook_EwfMgrVolumeNameEntryPop,
    (void*)Hook_EwfMgrVolumeNameListIsEmpty,
};

void ul_rt_ewf_install(void) {
    if (!ul_rt_env_on("ULTRA_EWF_STUB")) {
        ul_rt_log("ewf: skipped (ULTRA_EWF_STUB!=1)\n");
        return;
    }
    if (g_enabled) return;
    g_enabled = 1;

    /* Patch the 4 sdaemon actually uses. */
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrGetProtectedVolumeList",
                        (void*)Hook_EwfMgrGetProtectedVolumeList);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrOpenProtected",
                        (void*)Hook_EwfMgrOpenProtected);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrCommit",
                        (void*)Hook_EwfMgrCommit);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrVolumeNameListDelete",
                        (void*)Hook_EwfMgrVolumeNameListDelete);

    /* And patch the other 17 defensively -- some Big Buck Hunter / Nitro
     * builds import a longer subset. If the game does not import a given
     * name the walker's per-thunk name-match no-ops silently. */
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrCheckpoint",                    (void*)Hook_EwfMgrCheckpoint);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrClearCommand",                  (void*)Hook_EwfMgrClearCommand);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrClose",                         (void*)Hook_EwfMgrClose);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrDisable",                       (void*)Hook_EwfMgrDisable);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrEnable",                        (void*)Hook_EwfMgrEnable);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrGetDriveLetterFromVolumeName",  (void*)Hook_EwfMgrGetDriveLetterFromVolumeName);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrGetOverlayStoreConfig",         (void*)Hook_EwfMgrGetOverlayStoreConfig);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrGetOverlayStoreVolumeName",     (void*)Hook_EwfMgrGetOverlayStoreVolumeName);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrGetPersistentData",             (void*)Hook_EwfMgrGetPersistentData);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrGetProtectedVolumeConfig",      (void*)Hook_EwfMgrGetProtectedVolumeConfig);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrOpenOverlayStore",              (void*)Hook_EwfMgrOpenOverlayStore);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrRegisterLowSpaceNotification",  (void*)Hook_EwfMgrRegisterLowSpaceNotification);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrRestore",                       (void*)Hook_EwfMgrRestore);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrSetLevel",                      (void*)Hook_EwfMgrSetLevel);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrSetPersistentData",             (void*)Hook_EwfMgrSetPersistentData);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrVolumeNameEntryPop",            (void*)Hook_EwfMgrVolumeNameEntryPop);
    ul_rt_iat_patch_all("ewfapi.dll", "EwfMgrVolumeNameListIsEmpty",         (void*)Hook_EwfMgrVolumeNameListIsEmpty);
    ul_rt_log("ewf: armed (4 primary + 17 defensive EwfMgr* stubs)\n");
}

#endif /* _WIN32 */
