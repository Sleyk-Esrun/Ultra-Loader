/*
 * unwind_stub.c - DWARF2 C++ EH forwarding layer for ELF32 guest.
 *
 * All _Unwind_* symbols are now resolved at LINK TIME from libgcc_eh.a
 * (build_module.sh adds -lgcc_eh).  No runtime DLL loading required.
 *
 * elf_register_eh_frames() is called by elf_loader after mapping the ELF's
 * LOAD segments.  It passes the mapped .eh_frame base to __register_frame()
 * so the DW2 unwinder can locate FDEs inside the ELF and walk the stack to
 * find C++ catch handlers.
 *
 * WHY THIS WORKS:
 *   - The Hummer ELF was compiled with DWARF2 EH (statically links
 *     __gxx_personality_v0 at 0x08412bc0 and has .eh_frame + .gcc_except_table
 *     in its RX LOAD segment).
 *   - MinGW32 libgcc_eh.a also uses DWARF2 (--with-dwarf2,
 *     --disable-sjlj-exceptions), so the FDE binary format is identical.
 *   - We wire each _Unwind_* PLT slot DIRECTLY to the real DW2 function
 *     (no wrapper frame between the ELF's __cxa_throw and the unwinder).
 *   - DW.ref.__gxx_personality_v0 in the ELF data (written by the ELF linker)
 *     points to 0x08412bc0 (the ELF's own personality).  The DW2 unwinder reads
 *     this indirect pointer when processing ELF FDEs, so it calls the ELF's
 *     personality directly - no PLT slot involved for that path.
 *   - All frames between the throw site and the catch handler are inside the
 *     ELF (which has registered .eh_frame), so the DW2 unwinder can process
 *     every frame and locate the landing pad correctly.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include "loader.h"

/* ---- _Unwind ABI types (mirrored; not including system unwind.h) ----------- */
typedef enum {
    _URC_NO_REASON                = 0,
    _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
    _URC_FATAL_PHASE2_ERROR       = 2,
    _URC_FATAL_PHASE1_ERROR       = 3,
    _URC_NORMAL_STOP              = 4,
    _URC_END_OF_STACK             = 5,
    _URC_HANDLER_FOUND            = 6,
    _URC_INSTALL_CONTEXT          = 7,
    _URC_CONTINUE_UNWIND          = 8
} _Unwind_Reason_Code;

struct _Unwind_Exception {
    uint64_t exception_class;
    void    *exception_cleanup;
    uint64_t private_1, private_2;
};
struct _Unwind_Context;   /* opaque */
typedef _Unwind_Reason_Code (*_Unwind_Trace_Fn)(struct _Unwind_Context *, void *);
typedef _Unwind_Reason_Code (*_Unwind_Stop_Fn)(int, int, uint64_t,
    struct _Unwind_Exception *, struct _Unwind_Context *, void *);

/* ---- Real DW2 functions - resolved from -lgcc_eh (libgcc_eh.a) at link time.
 * MinGW prepends '_' to C names, so 'extern _Unwind_Foo' → '__Unwind_Foo' in
 * the COFF object, which matches the symbol in libgcc_eh.a.  Same for
 * '__register_frame' → '___register_frame'. -------------------------------- */
extern _Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *);
extern void                _Unwind_Resume(struct _Unwind_Exception *);
extern _Unwind_Reason_Code _Unwind_Resume_or_Rethrow(struct _Unwind_Exception *);
extern void                _Unwind_DeleteException(struct _Unwind_Exception *);
extern uintptr_t           _Unwind_GetIP(struct _Unwind_Context *);
extern void                _Unwind_SetIP(struct _Unwind_Context *, uintptr_t);
extern uintptr_t           _Unwind_GetIPInfo(struct _Unwind_Context *, int *);
extern uintptr_t           _Unwind_GetGR(struct _Unwind_Context *, int);
extern void                _Unwind_SetGR(struct _Unwind_Context *, int, uintptr_t);
extern uintptr_t           _Unwind_GetLanguageSpecificData(struct _Unwind_Context *);
extern uintptr_t           _Unwind_GetRegionStart(struct _Unwind_Context *);
extern uintptr_t           _Unwind_GetTextRelBase(struct _Unwind_Context *);
extern uintptr_t           _Unwind_GetDataRelBase(struct _Unwind_Context *);
extern uintptr_t           _Unwind_GetCFA(struct _Unwind_Context *);
extern void               *_Unwind_FindEnclosingFunction(void *);
extern _Unwind_Reason_Code _Unwind_Backtrace(_Unwind_Trace_Fn, void *);
extern _Unwind_Reason_Code _Unwind_ForcedUnwind(struct _Unwind_Exception *,
                                                  _Unwind_Stop_Fn, void *);
/* NOTE: two leading underscores here - MinGW makes it ___register_frame. */
extern void                __register_frame(void *);

/* ---- Public: called by elf_loader32 after mapping ELF LOAD segments -------- */
void elf_register_eh_frames(void *start)
{
    if (!start || *(uint32_t*)start == 0) {
        loader_log(LOG_WARN, "[EH] elf_register_eh_frames: empty or NULL .eh_frame");
        return;
    }
    loader_log(LOG_INFO, "[EH] Registering ELF32 .eh_frame @ %p (static DW2 / libgcc_eh.a)", start);
    __register_frame(start);
}

/* ---- Personality stub -------------------------------------------------------
 * The DW2 unwinder calls the ELF's own __gxx_personality_v0 via the
 * DW.ref.__gxx_personality_v0 indirect pointer (0x0a55da8c → 0x08412bc0).
 * This stub covers the PLT slot so that any direct ELF call to
 * __gxx_personality_v0 through PLT does not fault. */
static _Unwind_Reason_Code stub_gxx_personality(
    int version, int actions, uint64_t exc_class,
    struct _Unwind_Exception *exc, struct _Unwind_Context *ctx)
{
    (void)version; (void)actions; (void)exc_class; (void)exc; (void)ctx;
    return _URC_CONTINUE_UNWIND;
}

/* ---- SjLj stubs - Hummer ELF is DWARF2, not SjLj; these PLT slots are never
 * called in practice.  Keep stubs so the PLT table entry is valid. ---------- */
static _Unwind_Reason_Code stub_SjLj_Raise(struct _Unwind_Exception *e)
    { (void)e; return _URC_END_OF_STACK; }
static void stub_SjLj_Resume(struct _Unwind_Exception *e)
    { (void)e; loader_log(LOG_ERROR,"[EH] _Unwind_SjLj_Resume (fatal)"); __builtin_trap(); }
static _Unwind_Reason_Code stub_SjLj_ForcedUnwind(struct _Unwind_Exception *e,
    _Unwind_Stop_Fn stop, void *arg)
    { (void)e;(void)stop;(void)arg; return _URC_END_OF_STACK; }
static void stub_SjLj_Register(void *fc)   { (void)fc; }
static void stub_SjLj_Unregister(void *fc) { (void)fc; }

/* ---- Registration: wire ELF PLT slots to real DW2 functions --------------- */
void unwind_stub_register(LoaderCtx *ctx)
{
    loader_log(LOG_INFO, "[EH] Registering _Unwind_* (static DW2 from libgcc_eh.a)");

#define R(sym, fn) sym_register(ctx, #sym, (uintptr_t)(fn))
    R(_Unwind_RaiseException,          _Unwind_RaiseException);
    R(_Unwind_Resume,                  _Unwind_Resume);
    R(_Unwind_Resume_or_Rethrow,       _Unwind_Resume_or_Rethrow);
    R(_Unwind_DeleteException,         _Unwind_DeleteException);
    R(_Unwind_GetIP,                   _Unwind_GetIP);
    R(_Unwind_SetIP,                   _Unwind_SetIP);
    R(_Unwind_GetIPInfo,               _Unwind_GetIPInfo);
    R(_Unwind_GetGR,                   _Unwind_GetGR);
    R(_Unwind_SetGR,                   _Unwind_SetGR);
    R(_Unwind_GetLanguageSpecificData, _Unwind_GetLanguageSpecificData);
    R(_Unwind_GetRegionStart,          _Unwind_GetRegionStart);
    R(_Unwind_GetTextRelBase,          _Unwind_GetTextRelBase);
    R(_Unwind_GetDataRelBase,          _Unwind_GetDataRelBase);
    R(_Unwind_GetCFA,                  _Unwind_GetCFA);
    R(_Unwind_FindEnclosingFunction,   _Unwind_FindEnclosingFunction);
    R(_Unwind_Backtrace,               _Unwind_Backtrace);
    R(_Unwind_ForcedUnwind,            _Unwind_ForcedUnwind);
    R(__gxx_personality_v0,            stub_gxx_personality);
    R(_Unwind_SjLj_RaiseException,     stub_SjLj_Raise);
    R(_Unwind_SjLj_Resume,             stub_SjLj_Resume);
    R(_Unwind_SjLj_ForcedUnwind,       stub_SjLj_ForcedUnwind);
    R(_Unwind_SjLj_Register,           stub_SjLj_Register);
    R(_Unwind_SjLj_Unregister,         stub_SjLj_Unregister);
#undef R
}
