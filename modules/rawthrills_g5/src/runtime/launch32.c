/*
 * launch32.c - Native 32-bit ELF launch for WOW64
 *
 * This file is compiled as 32-bit code (i686-w64-mingw32-gcc).
 * Running as a 32-bit PE under WOW64 means:
 *   - All pointers are 32-bit
 *   - [ebp-N] addressing works exactly like 32-bit Linux
 *   - No emulator, no mode switching, native speed
 *   - ELF code runs directly in the same process
 */

#include <windows.h>
#include <stdint.h>
#include "loader.h"

/*
 * Jump to ELF _start with the Linux ABI stack.
 *
 * On entry to 32-bit glibc _start:
 *   [esp+0] = argc
 *   [esp+4] = argv[0]
 *   [esp+8] = argv[1] (or NULL)
 *   ...
 *   followed by envp, auxv
 *
 * We set up the stack exactly this way and jump.
 * The function never returns (game calls exit() or loops forever).
 */
void __attribute__((noreturn, noinline))
elf32_launch(uint32_t entry, uint32_t *abi_stack)
{
    /* Switch stack to our ABI stack and jump to ELF entry point.
     * We load esp = abi_stack (points to argc) and jump to entry.
     * No CALL - we use JMP so no return address is pushed. */
    __asm__ volatile (
        "mov esp, %0\n\t"   /* switch to ABI stack (argc is at [esp]) */
        "xor ebp, ebp\n\t"  /* Linux ABI: ebp=0 at _start */
        "jmp %1\n\t"        /* jump to ELF _start */
        :
        : "r"(abi_stack), "r"(entry)
        : "memory"
    );
    __builtin_unreachable();
}
