/*
 * libbms_crk.c -- clean-room offline stub for Konami e-amusement BMS library.
 *
 * The real libbms.dll contacts a background service (bmssvc.exe) via named pipe
 * \\.\pipe\bmssvc and then Konami's e-amusement servers.  With no service
 * running, bms_init_wait() blocks ~12 s then the game exits cleanly with code 0.
 *
 * This stub makes every BMS call succeed immediately so the game runs without
 * network.  Only the 55 functions KSHG.exe actually imports are stubbed; the rest
 * are no-ops so the DLL loads cleanly.
 *
 * Return conventions (inferred from BMS string constants):
 *   Request functions:  0 = success/started
 *   _wait variants:     0 = operation COMPLETE (game stops calling), non-zero = still pending
 *   bms_is_active:      1 = active
 *   bms_get_last_error: 0 = no error
 *   output pointers:    zeroed (0 interpreted as "no error / empty result")
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

/* ── lifecycle ─────────────────────────────────────────────────────────── */
/* Zero an OUT buffer only if it really is one.
 *
 * `if (o) memset(o, 0, n)` accepts any non-zero value, including a stale
 * register that a signature mismatch handed us instead of a pointer. Asking
 * the OS whether the page is committed and writable costs one call on a path
 * that runs a handful of times, and turns a silent arbitrary write into a
 * no-op. Defense in depth: the signatures above are now correct, but this
 * file has 120 exports reverse-engineered from call sites and the next wrong
 * one should not be able to corrupt memory. */
static void bms_zero_out(void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if ((size_t)p <= 0x10000u) return;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return;
    if (mbi.State != MEM_COMMIT) return;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return;
    if ((size_t)((char*)mbi.BaseAddress + mbi.RegionSize - (char*)p) < n) return;
    memset(p, 0, n);
}

__declspec(dllexport) int bms_init(void)      { return 0; }
__declspec(dllexport) int bms_init_wait(void) { return 0; }
__declspec(dllexport) int bms_init_error(void)     { return 0; }
__declspec(dllexport) int bms_is_active(void)      { return 1; }
__declspec(dllexport) int bms_get_last_error(void) { return 0; }

/* ── ea3 status ────────────────────────────────────────────────────────── */
__declspec(dllexport) int bms_ea3_get_status(void)            { return 0; }
__declspec(dllexport) int bms_ea3_get_status_wait(void *o1, void *o2) { if (o1) memset(o1,0,16); if (o2) memset(o2,0,16); return 0; }
__declspec(dllexport) int bms_ea3_get_eapass_valid(void)      { return 0; }
__declspec(dllexport) int bms_ea3_license_agree(void)         { return 0; }
__declspec(dllexport) int bms_ea3_license_agree_wait(void *o) { bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_ea3_download_pause(void)        { return 0; }
__declspec(dllexport) int bms_ea3_download_pause_wait(void *o){ bms_zero_out(o,16); return 0; }

/* ── credit / coin ─────────────────────────────────────────────────────── */
__declspec(dllexport) int bms_credit_reset(void)                    { return 0; }
__declspec(dllexport) int bms_credit_reset_wait(void *o)            { bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_credit_get_status(void)               { return 0; }
__declspec(dllexport) int bms_credit_get_status_wait(void *o)       { bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_credit_update_option(void *a)         { (void)a; return 0; }
__declspec(dllexport) int bms_credit_update_option_wait(void *o)    { bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_credit_coin_throw(int s, int n)       { (void)s;(void)n; return 0; }
__declspec(dllexport) int bms_credit_coin_throw_wait(void *o)       { bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_credit_service(int n)                 { (void)n; return 0; }
__declspec(dllexport) int bms_credit_service_wait(void *o)          { bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_credit_consume(int n)                 { (void)n; return 0; }
__declspec(dllexport) int bms_credit_consume_wait(void *o)          { bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_credit_consume_expired(void)          { return 0; }

/* ── card management (no card) ─────────────────────────────────────────── */
__declspec(dllexport) int bms_cardmng_inquire(int p, const char *c, int f) { (void)p;(void)c;(void)f; return 0; }
__declspec(dllexport) int bms_cardmng_inquire_wait(int p, void *o)          { (void)p; bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_cardmng_cardactivate(int p, const char *c)    { (void)p;(void)c; return 0; }
__declspec(dllexport) int bms_cardmng_cardactivate_wait(int p, void *o)     { (void)p; bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_cardmng_bindmodel(int p, void *m)             { (void)p;(void)m; return 0; }
__declspec(dllexport) int bms_cardmng_bindmodel_wait(int p, void *o)        { (void)p; bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_cardmng_playerlogin(int p, void *d)           { (void)p;(void)d; return 0; }
__declspec(dllexport) int bms_cardmng_playerlogin_wait(int p, void *o)      { (void)p; bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_cardmng_playerlogout(int p)                   { (void)p; return 0; }
__declspec(dllexport) int bms_cardmng_playerlogout_wait(int p, void *o)     { (void)p; bms_zero_out(o,16); return 0; }

/* ── player data ───────────────────────────────────────────────────────── */
__declspec(dllexport) int bms_playerdata_load(int p, void *d)    { (void)p;(void)d; return 0; }
/* ONE argument, and it is the OUT buffer -- read off the call site, not guessed:
 *     4d55fa  lea edx,[esp+0x10]
 *     4d55fe  push edx
 *     4d55ff  call ds:0x680460      ; bms_playerdata_load_wait
 *     4d5605  add esp,4             ; cdecl, exactly 1 arg
 *
 * Declared (int, void*) before, so `p` silently absorbed the real buffer and
 * `o` took whatever stale dword sat above it -- then memset 16 bytes through
 * it. Same class of bug as the shaiolib SYSINFO crash: a stub that reads more
 * arguments than the caller pushes is writing to a register value. */
__declspec(dllexport) int bms_playerdata_load_wait(void *o){ bms_zero_out(o, 16); return 0; }
__declspec(dllexport) int bms_playerdata_save(int p, void *d)    { (void)p;(void)d; return 0; }

__declspec(dllexport) int bms_playerdata_save_wait(void *o){ bms_zero_out(o, 16); return 0; }

/* ── ranking ───────────────────────────────────────────────────────────── */
__declspec(dllexport) int bms_ranking_get(void *q)      { (void)q; return 0; }
__declspec(dllexport) int bms_ranking_get_wait(void *o) { bms_zero_out(o,16); return 0; }

/* ── keepalive / time ──────────────────────────────────────────────────── */
__declspec(dllexport) int bms_keepalive_test_request(void)      { return 0; }
__declspec(dllexport) int bms_keepalive_test_request_wait(void *o){ bms_zero_out(o,16); return 0; }
__declspec(dllexport) int bms_time_get_clock(void)              { return 0; }
__declspec(dllexport) int bms_time_get_clock_wait(void *o)      { bms_zero_out(o,16); return 0; }

/* ── net property ──────────────────────────────────────────────────────── */
__declspec(dllexport) int bms_net_property_get(void)      { return 0; }
__declspec(dllexport) int bms_net_property_get_wait(void *o){ bms_zero_out(o,16); return 0; }

/* ── event log ─────────────────────────────────────────────────────────── */
__declspec(dllexport) int bms_eventlog_request_error      (void *a){ (void)a; return 0; }
__declspec(dllexport) int bms_eventlog_request_eventdata  (void *a){ (void)a; return 0; }
__declspec(dllexport) int bms_eventlog_request_transition (void *a){ (void)a; return 0; }
__declspec(dllexport) int bms_eventlog_request_game_end   (void *a){ (void)a; return 0; }
__declspec(dllexport) int bms_eventlog_request_game_start (void *a){ (void)a; return 0; }
__declspec(dllexport) int bms_eventlog_request_testmode   (void *a){ (void)a; return 0; }
__declspec(dllexport) int bms_eventlog_request_init       (void *a){ (void)a; return 0; }
__declspec(dllexport) int bms_eventlog_request_otdemo     (void *a){ (void)a; return 0; }

/* ── logging (no-op) ──────────────────────────────────────────────────── */
__declspec(dllexport) void bms_log_error(const char *fmt, ...) { (void)fmt; }

/* ── null-call skip VEH ────────────────────────────────────────────────
 * KSHG.exe calls through a NULL function pointer during credit init
 * (bms_credit_update_option call chain, return addr 0x4D22C2).  The real
 * libbms.dll prevents this path; our stub does not.  This VEH catches the
 * specific AV@EIP=0 case, pops the pushed return address from ESP, sets
 * EIP = that return address and EAX = 0, then resumes - making the null
 * call act as a no-op that returns 0.  Any other exception is passed on. */
static LONG WINAPI bms_null_call_skip(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT          *ctx = ep->ContextRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && ctx->Eip == 0) {
        /* [ESP] is the return address the CALL instruction pushed. */
        DWORD ret = *(DWORD *)(ctx->Esp);
        ctx->Esp += 4;
        ctx->Eip  = ret;
        ctx->Eax  = 1;   /* 1 = callback success; 0 causes SECURITY to hang at ".." */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── DllMain ───────────────────────────────────────────────────────────── */
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID v) {
    if (r == DLL_PROCESS_ATTACH)
        AddVectoredExceptionHandler(1, bms_null_call_skip);
    (void)h; (void)v;
    return TRUE;
}
