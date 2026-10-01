/*
 * thunk32.c - ABI adapter pool for ELF32 running in 64-bit long mode
 *
 * CS=0x23 (32-bit compat mode) is NOT accessible from a pure 64-bit process
 * on Windows x64 (requires WoW64 initialization). We instead run the ELF32
 * binary in CS=0x33 (64-bit long mode).
 *
 * Challenge: ELF32 was compiled for i386 cdecl. Its calls pass args on the
 * stack (32-bit values). Our stubs use Windows x64 ABI (args in registers).
 * In 64-bit mode, PUSH instructions use 8 bytes, so each arg occupies 8 bytes.
 *
 * Each thunk64 in the pool:
 *   - Reads args from the 64-bit stack (8-byte slots for each 32-bit arg)
 *   - Loads them into rcx/rdx/r8/r9 (Windows x64 ABI)
 *   - Calls the 64-bit stub via a direct indirect call through a table
 *   - Returns to ELF32 caller (no mode switch needed)
 *
 * Pool layout:
 *   [+0x0000]  Thunk64 table start (THUNK_START)
 *   [+TABLE_OFFSET] Stub pointer table (uint64_t[MAX_THUNKS])
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "loader.h"
#include "thunk32.h"

/* Pool layout */
#define POOL_SIZE        0x40000     /* 256KB */
#define MAX_THUNKS       512
#define THUNK_SIZE       32          /* bytes per thunk */
#define TABLE_OFFSET     0x100       /* stub pointer table at pool+0x100 */
#define THUNK_START      0x1000      /* thunks at pool+0x1000 */

uint8_t  *g_pool       = NULL;
static uint64_t *g_stub_table = NULL;
static uint32_t  g_thunk_count = 0;

/* ------------------------------------------------------------------ */
/* Thunk64 layout (32 bytes, 64-bit code)                             */
/* ------------------------------------------------------------------ */
/*
 * Called from ELF32 code running in 64-bit long mode.
 * The ELF uses i386 cdecl: args pushed right-to-left as 8-byte values
 * (because PUSH in 64-bit mode is always 8 bytes).
 * CALL pushed an 8-byte return address.
 *
 * Stack on entry:
 *   [rsp+ 0]  return address (8 bytes)
 *   [rsp+ 8]  arg1 (32-bit ELF value in lower 32 bits, upper 32 = 0)
 *   [rsp+16]  arg2
 *   [rsp+24]  arg3
 *   [rsp+32]  arg4
 *
 * Thunk code (31 bytes, fits in 32-byte slot with 1 nop):
 *   [+0]  48 8B 05 [disp32]   mov rax, [RIP+disp32]  7 bytes  <- load stub addr
 *   [+7]  8B 4C 24 08         mov ecx, [rsp+8]        4 bytes
 *   [+11] 8B 54 24 10         mov edx, [rsp+16]       4 bytes
 *   [+15] 44 8B 44 24 18      mov r8d, [rsp+24]       5 bytes
 *   [+20] 48 83 EC 28         sub rsp, 40             4 bytes  (shadow+align)
 *   [+24] FF D0               call rax                2 bytes
 *   [+26] 48 83 C4 28         add rsp, 40             4 bytes
 *   [+30] C3                  ret                     1 byte
 *   [+31] 90                  nop (pad to 32)
 *
 * Note: only 4 args handled. For stubs with >4 args, r9d would also be needed.
 * Most glibc functions need <= 4 args. variadic stubs get the right args via
 * the va_list mechanism.
 */
static void build_thunk64(uint8_t *p, uint32_t index)
{
    memset(p, 0x90, THUNK_SIZE);

    /* Pointer to this thunk's stub entry in the table */
    uint64_t *stub_slot = g_stub_table + index;

    /* [+0] mov rax, [RIP+disp32]  -- load stub address */
    /* RIP at end of this 7-byte instruction = p+7 */
    p[0] = 0x48; p[1] = 0x8B; p[2] = 0x05;
    int32_t disp = (int32_t)((uint8_t*)stub_slot - (p + 7));
    memcpy(p+3, &disp, 4);

    /* [+7] mov ecx, [rsp+8] */
    p[7] = 0x8B; p[8] = 0x4C; p[9] = 0x24; p[10] = 0x08;

    /* [+11] mov edx, [rsp+16] */
    p[11] = 0x8B; p[12] = 0x54; p[13] = 0x24; p[14] = 0x10;

    /* [+15] mov r8d, [rsp+24] */
    p[15] = 0x44; p[16] = 0x8B; p[17] = 0x44; p[18] = 0x24; p[19] = 0x18;

    /* [+20] sub rsp, 40  (32 bytes shadow + 8 alignment) */
    p[20] = 0x48; p[21] = 0x83; p[22] = 0xEC; p[23] = 0x28;

    /* [+24] call rax */
    p[24] = 0xFF; p[25] = 0xD0;

    /* [+26] add rsp, 40 */
    p[26] = 0x48; p[27] = 0x83; p[28] = 0xC4; p[29] = 0x28;

    /* [+30] ret */
    p[30] = 0xC3;
    /* [+31] nop (already set by memset) */
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

bool thunk32_init(void)
{
    /* Pool can be anywhere - we need it in low memory only for the thunk
     * addresses to fit in 32-bit GOT entries. In 64-bit mode the GOT is read
     * as 8 bytes but we store 64-bit addresses zero-extended from 32-bit,
     * so keeping pool < 4GB keeps the top 4 bytes zero and the address valid. */
    g_pool = NULL;

    /* DO NOT include 0x08000000: that's the Lindbergh ELF's base address.
     * In --cli mode, candidates 0x01/0x02/0x04000000 typically succeed and
     * thunk32 lands far away. But under the menu path, GDI/Win32/font
     * allocations from the settings dialog occupy 0x01/0x02/0x04000000, so
     * thunk32 would fall through to 0x08000000 and collide with ABC's seg 2
     * at 0x08048000 (POOL_SIZE=256KB pushes thunk32 right up to the ELF
     * base; the ELF's exec-range MEM_RESERVE then fails with error 487).
     * Skip 0x08000000 entirely so thunk32 keeps moving to 0x10000000+. */
    static const uintptr_t candidates[] = {
        0x01000000, 0x02000000, 0x04000000,
        /* 0x08000000 reserved for Lindbergh ELF -- never use */
        0x10000000, 0x20000000, 0x30000000, 0x40000000,
        0x50000000, 0x60000000, 0
    };
    for (int i = 0; candidates[i]; i++) {
        g_pool = VirtualAlloc((LPVOID)candidates[i], POOL_SIZE,
                               MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (g_pool) break;
    }
    if (!g_pool)
        g_pool = VirtualAlloc(NULL, POOL_SIZE, MEM_COMMIT|MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (!g_pool) {
        loader_log(LOG_ERROR, "thunk32: VirtualAlloc failed: %lu", GetLastError());
        return false;
    }
    if ((uintptr_t)g_pool > 0xFFFFFFFFULL) {
        loader_log(LOG_ERROR, "thunk32: pool at 0x%llx > 4GB", (unsigned long long)(uintptr_t)g_pool);
        VirtualFree(g_pool, 0, MEM_RELEASE); g_pool = NULL; return false;
    }

    memset(g_pool, 0xCC, POOL_SIZE);   /* int3 trap for debugging */

    g_stub_table  = (uint64_t*)(g_pool + TABLE_OFFSET);
    g_thunk_count = 0;

    loader_log(LOG_INFO, "thunk32: pool at 0x%08x (thunks=0x%08x, table=0x%08x)",
               (uint32_t)(uintptr_t)g_pool,
               (uint32_t)(uintptr_t)(g_pool + THUNK_START),
               (uint32_t)(uintptr_t)g_stub_table);
    return true;
}

/* ------------------------------------------------------------------ */
/* Register a 64-bit stub -> get 32-bit thunk address                */
/* ------------------------------------------------------------------ */
uint32_t thunk32_register(uint64_t stub_addr64, const char *name)
{
    if (!g_pool) return 0;
    if (g_thunk_count >= MAX_THUNKS) {
        loader_log(LOG_ERROR, "thunk32: pool full (%u thunks)", MAX_THUNKS);
        return 0;
    }

    uint32_t index = g_thunk_count++;
    g_stub_table[index] = stub_addr64;

    uint8_t *thunk = g_pool + THUNK_START + index * THUNK_SIZE;
    build_thunk64(thunk, index);

    uint32_t thunk_addr32 = (uint32_t)(uintptr_t)thunk;
    loader_log(LOG_DEBUG, "thunk32[%3u]: %-32s -> 0x%08x (stub=0x%llx)",
               index, name ? name : "?", thunk_addr32,
               (unsigned long long)stub_addr64);

    /* Also register with Unicorn for INT3 dispatch (64-bit mode only) */
#ifdef _WIN64
    extern void uc_register_stub(uint32_t, void*, const char*);
    uc_register_stub(thunk_addr32, (void*)(uintptr_t)stub_addr64, name);
#endif

    return thunk_addr32;
}

uint32_t thunk32_lookup(uint64_t stub_addr64)
{
    if (!g_pool) return 0;
    for (uint32_t i = 0; i < g_thunk_count; i++) {
        if (g_stub_table[i] == stub_addr64)
            return (uint32_t)(uintptr_t)(g_pool + THUNK_START + i * THUNK_SIZE);
    }
    return 0;
}

bool thunk32_initialized(void) { return g_pool != NULL; }
uint8_t *thunk32_get_pool(void) { return g_pool; }
/* No relay in 64-bit mode. Return pool start for compatibility. */
uint32_t thunk32_get_relay_addr(void) { return g_pool ? (uint32_t)(uintptr_t)g_pool : 0; }
