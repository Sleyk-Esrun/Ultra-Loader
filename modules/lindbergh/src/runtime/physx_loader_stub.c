/*
 * physx_loader_stub.c
 * libPhysXLoader.so.1 stub - NVIDIA PhysX 2.8.1 loader shim.
 *
 * Raw Thrills' Lindbergh port of Cruis'n Blast links against
 *   /usr/lib/PhysX/v2.8.1/libPhysXLoader.so.1
 * which is a thin dispatcher that finds and dlopens libPhysXCore + libPhysX +
 * libPhysXCooking, then re-exports the C factories:
 *
 *   NxCreatePhysicsSDK(version, allocator, output, params)  → NxPhysicsSDK*
 *   NxReleasePhysicsSDK(sdk)
 *   NxGetPhysicsSDK()
 *   NxGetSDKVersion()
 *   NxCreateCookingInterface()                              → NxCookingInterface*
 *   getNxCookingInterface()                                 → NxCookingInterface*
 *
 * The full PhysX 2.8.1 SDK is not shippable. Cruis'n Blast uses PhysX for
 * static-collision queries against the road mesh - the *rendering* of the
 * road, cars, and world doesn't depend on PhysX at all. So a stub that
 * returns valid-but-empty SDK / scene / cooking handles lets the game boot
 * to attract, run the UI, and render menus. Actual collision responses come
 * back as "no hit" (raycasts return false, sweep tests report no contact),
 * which is fine until we plug in a real physics engine (Bullet, planned).
 *
 * The stub is DISPATCHED from stubs_glibc.c::stub_dlopen_v2, which detects
 * "libPhysXLoader" in the filename and hands the handle out here. Symbol
 * lookups then go through physx_loader_dlsym().
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "loader.h"

/* Marker handle returned from stub_dlopen_v2 for the loader lib. */
#define PHYSX_LOADER_HANDLE ((void*)0xDEADFACE)

/* ------------------------------------------------------------------ */
/* Fake SDK / scene / actor objects                                    */
/* ------------------------------------------------------------------ */

/* The game holds these as opaque pointers and calls virtual methods off them.
 * We build a tiny fake vtable of `xor eax,eax; ret 4*N` stubs sized for the
 * SDK's C++ ABI so the game's `sdk->createScene(...)` no-ops instead of
 * crashing. First slot returns non-NULL to satisfy "did the call succeed?"
 * checks; the rest return 0.
 *
 * PhysX SDK vtables have ~64 entries; we pre-fill 128 as headroom. */
static uintptr_t g_fake_vtable_ok  [128];    /* returns 1 (non-NULL) */
static uintptr_t g_fake_vtable_zero[128];    /* returns 0 */

/* Actual objects the game holds. Their first 4 bytes must be a vtable
 * pointer (C++ ABI on Linux x86 sysv). */
typedef struct {
    uintptr_t *vtable;
    uint32_t   pad[62];  /* room for the game to poke object state */
} FakeObj;

static FakeObj g_sdk_obj;
static FakeObj g_scene_obj;
static FakeObj g_cooking_obj;

/* Executable "return 1 / return 0" thunks the vtables point at. Allocated
 * once from an RX arena and reused for every slot. */
static uintptr_t alloc_ret_thunk(uint32_t retval)
{
    static uint8_t *arena = NULL;
    static size_t   off   = 0;
    if (!arena) {
        arena = (uint8_t*)VirtualAlloc(NULL, 4096,
                                       MEM_COMMIT|MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
    }
    if (!arena || off + 16 > 4096) return 0;
    uint8_t *p = arena + off;
    off += 16;
    /* mov eax, imm32 ; ret */
    p[0] = 0xB8;
    p[1] = (uint8_t)(retval      );
    p[2] = (uint8_t)(retval >>  8);
    p[3] = (uint8_t)(retval >> 16);
    p[4] = (uint8_t)(retval >> 24);
    p[5] = 0xC3;
    return (uintptr_t)p;
}

static void physx_init_fakes(void)
{
    static int done = 0;
    if (done) return;
    done = 1;

    uintptr_t ok_thunk   = alloc_ret_thunk(1);
    uintptr_t zero_thunk = alloc_ret_thunk(0);

    for (int i = 0; i < 128; i++) {
        g_fake_vtable_ok[i]   = ok_thunk;
        g_fake_vtable_zero[i] = zero_thunk;
    }

    /* SDK object: createScene → non-NULL; releaseScene → OK; queries → 0. */
    g_sdk_obj.vtable     = g_fake_vtable_ok;
    /* Scene: raycasts/sweep queries return 0 (no hit). Update / setGravity
     * return 1 so the game's "sim step ok" check passes. */
    g_scene_obj.vtable   = g_fake_vtable_ok;
    /* Cooking: the game may cook a triangle mesh once at load. Return non-NULL
     * for the cook, then noop. */
    g_cooking_obj.vtable = g_fake_vtable_ok;
}

/* ------------------------------------------------------------------ */
/* PhysX C-level factories                                             */
/* ------------------------------------------------------------------ */

/* PhysX SDK version constant Raw Thrills' build was compiled against */
#define NX_PHYSICS_SDK_VERSION 0x02080100  /* 2.8.1 */

static void *physx_NxCreatePhysicsSDK(uint32_t version, void *allocator,
                                      void *output, void *params)
{
    (void)allocator; (void)output; (void)params;
    physx_init_fakes();
    loader_log(LOG_INFO, "[PhysX] NxCreatePhysicsSDK(0x%08x) -> stub SDK",
               version);
    return &g_sdk_obj;
}

static void physx_NxReleasePhysicsSDK(void *sdk)
{
    (void)sdk;
    loader_log(LOG_INFO, "[PhysX] NxReleasePhysicsSDK");
}

static void *physx_NxGetPhysicsSDK(void)
{
    physx_init_fakes();
    return &g_sdk_obj;
}

static uint32_t physx_NxGetSDKVersion(void)
{
    return NX_PHYSICS_SDK_VERSION;
}

static void *physx_NxCreateCookingInterface(void)
{
    physx_init_fakes();
    loader_log(LOG_INFO, "[PhysX] NxCreateCookingInterface -> stub");
    return &g_cooking_obj;
}

static void *physx_getNxCookingInterface(void)
{
    physx_init_fakes();
    return &g_cooking_obj;
}

/* Some PhysX 2.8.1 builds also export the utility singletons. */
static void *physx_NxGetFoundationSDK(void)     { physx_init_fakes(); return &g_sdk_obj; }
static void  physx_NxReleaseFoundationSDK(void) { }
static uint32_t physx_NxGetFoundationSDKVersion(void) { return NX_PHYSICS_SDK_VERSION; }

/* ------------------------------------------------------------------ */
/* Public dispatch - used by stubs_glibc.c::stub_dlopen_v2/dlsym_v2    */
/* ------------------------------------------------------------------ */

bool physx_loader_is_lib(const char *filename)
{
    if (!filename) return false;
    return strstr(filename, "libPhysXLoader") != NULL
        || strstr(filename, "libPhysX")       != NULL
        || strstr(filename, "libNxCharacter") != NULL
        || strstr(filename, "libNxCooking")   != NULL;
}

void *physx_loader_dlopen(const char *filename)
{
    (void)filename;
    physx_init_fakes();
    loader_log(LOG_INFO, "[PhysX] dlopen(%s) -> stub handle", filename);
    return PHYSX_LOADER_HANDLE;
}

void *physx_loader_dlsym(void *handle, const char *symbol)
{
    (void)handle;
    if (!symbol) return NULL;

    if (!strcmp(symbol, "NxCreatePhysicsSDK"))       return (void*)physx_NxCreatePhysicsSDK;
    if (!strcmp(symbol, "NxReleasePhysicsSDK"))      return (void*)physx_NxReleasePhysicsSDK;
    if (!strcmp(symbol, "NxGetPhysicsSDK"))          return (void*)physx_NxGetPhysicsSDK;
    if (!strcmp(symbol, "NxGetSDKVersion"))          return (void*)physx_NxGetSDKVersion;
    if (!strcmp(symbol, "NxCreateCookingInterface")) return (void*)physx_NxCreateCookingInterface;
    if (!strcmp(symbol, "getNxCookingInterface"))    return (void*)physx_getNxCookingInterface;
    if (!strcmp(symbol, "NxGetFoundationSDK"))       return (void*)physx_NxGetFoundationSDK;
    if (!strcmp(symbol, "NxReleaseFoundationSDK"))   return (void*)physx_NxReleaseFoundationSDK;
    if (!strcmp(symbol, "NxGetFoundationSDKVersion"))return (void*)physx_NxGetFoundationSDKVersion;

    /* Any other PhysX symbol resolves to the shared 0-returning thunk. */
    static uintptr_t generic_zero = 0;
    if (!generic_zero) generic_zero = alloc_ret_thunk(0);
    loader_log(LOG_DEBUG, "[PhysX] dlsym(%s) -> generic no-op", symbol);
    return (void*)generic_zero;
}
