/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  game_mkdx_iauth.c -- Mario Kart Arcade GP DX: IN-PROCESS IAuth replacement
 * =============================================================================
 *
 *  WHY THIS FILE EXISTS
 *  ---------------------------------------------------------------------------
 *  allnet_setup.c's own confirmed disassembly of this exact exe documents:
 *      0x004010BB  CoCreateInstance({4603BB03-058D-43D9-B96F-639BE908C1ED},
 *                                    NULL, CLSCTX_LOCAL_SERVER,
 *                                    IID_IAuth{045A5150-D2B3-4590-A38B-
 *                                    C1158678E1AC}, &pIAuth)
 *  That CLSCTX_LOCAL_SERVER means the game expects an OUT-OF-PROCESS COM
 *  server (the real AMAuthd.exe) to answer every call through that IAuth
 *  pointer -- boot self-test rows (local network check, update check),
 *  shop/place info, everything. allnet_setup.c already gets that real,
 *  out-of-process activation WORKING (proxy/stub registered, AMAuthd.exe
 *  launched, COM marshaling succeeds) -- but the real AMAuthd.exe's answers
 *  still depend on the real muchacd/muchadl backend actually functioning,
 *  including an undocumented, proprietary "reservedblock" partition-image
 *  validation this project has not been able to satisfy (see
 *  bananapass-com2-protocol notes and allnet_setup.c's own history).
 *
 *  This file takes the other, complete path: don't make the real backend
 *  happy, stop needing it. It intercepts CoCreateInstance ITSELF (IAT hook,
 *  same ultra_iat_patch_all utility every other per-game file here uses) and,
 *  only for this exact CLSID+IID pair, hands back a plain IN-PROCESS object
 *  instead of ever letting the call reach COM's real out-of-process
 *  activation machinery. The game never talks to AMAuthd.exe, muchacd.exe,
 *  or muchadl at all -- every self-test row this interface answers just
 *  reports "fine" because this object says so directly, in-process, with no
 *  wire protocol on either side.
 *
 *  GROUND TRUTH FOR THE VTABLE SHAPE
 *  ---------------------------------------------------------------------------
 *  A COM interface's vtable layout is permanently fixed by its IID -- that is
 *  the entire point of minting a GUID (change the shape, mint a new IID,
 *  never reuse the old one). gitea.tendokyu.moe/TeamTofuShop/bananatools
 *  (amcus/iauth.h, amcus/iauth.c) independently reverse-engineered THE SAME
 *  IID_IAuth (045A5150-...) for a different Bandai Namco AMCUS-family title,
 *  and its 33-method vtable order/signatures are used here as confirmed,
 *  reused ABI fact, not creative content -- two unrelated reverse-engineering
 *  efforts landing on the identical 128-bit GUID for what both call "IAuth"
 *  is not a coincidence; a shared IID across the AMCUS product line is
 *  exactly what COM's own contract predicts. The method BODIES below are
 *  this file's own, written for this project's conventions (bp_log-style
 *  diagnostics, this file's own gating) -- only the interface SHAPE is
 *  carried over.
 *
 *  Semantics confirmed load-bearing by bananatools (kept exactly):
 *    - Func19(mucha_updater_state*): state MUST be 1 or the caller decides
 *      it isn't authenticated (their comment: "if this isn't 1 [game]
 *      thinks it isn't auth'd" -- generic AMCUS behavior, not game-specific).
 *  Everything else unconfirmed for MKDX specifically is answered with a
 *  plausible, always-successful placeholder (matching this project's own
 *  standing rule against fabricating card/profile data elsewhere, but this
 *  is status/shop metadata the self-test displays, not user data, so a
 *  placeholder that reads as "healthy" is the correct answer by design).
 *
 *  GATING
 *  ---------------------------------------------------------------------------
 *  Same exe-identity check as game_mkdx_bananapass.c (6,819,840 bytes /
 *  MK_AGP3_FINAL.exe / ULTRA_MKDX_ID=1 escape hatch) -- this cannot register
 *  in Pokken Tournament's process (different exe, 64-bit) or any other
 *  namco_es3 title. Unconditional once identity matches: unlike the
 *  Bananapass card emulator, there is no reason to gate "the network/update
 *  self-test rows should report healthy" behind an opt-in flag.
 *
 *  allnet_setup.c's existing AMAuthd.exe spawn + proxy/stub registration is
 *  left completely alone -- it still runs, harmlessly, for every ES3 title;
 *  this file just means MKDX's own CoCreateInstance call never reaches it.
 * =============================================================================
 */
#if defined(_WIN32)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <objbase.h>

extern void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

static void mi_log(const char* fmt, ...) {
    /* GetEnvironmentVariableA, not getenv: this file's constructor can run
     * very early in this DLL's load, before the CRT's own getenv() table is
     * necessarily populated -- and the one-shot cache below means an early
     * miss would stick forever otherwise. */
    static char path[MAX_PATH]; static int checked = 0;
    if (!checked) {
        DWORD n = GetEnvironmentVariableA("ULTRA_JVS_LOG", path, sizeof path);
        if (!(n > 0 && n < sizeof path)) path[0] = 0;
        checked = 1;
    }
    if (!path[0]) return;
    FILE* f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "[MKDX-IAUTH] ");
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* ---------------------------------------------------------------------------
 *  Struct layouts -- confirmed ABI fact (see file header), not creative
 *  content: these are dictated by the compiled game binary's own expectations
 *  for what IAuth's methods write into, independently reverse-engineered
 *  twice (bananatools, for a different AMCUS title; this file, cross-checked
 *  against it for MKDX). Field NAMES are this file's own where the meaning is
 *  actually understood; a few genuinely-unconfirmed reserved fields keep
 *  Member-style names rather than guessing a purpose for them.
 * -------------------------------------------------------------------------- */
struct mi_allnet_state {
    uint32_t state, error;
    int32_t  auth_state, auth_count, last_error;
};
struct mi_mucha_state {
    uint32_t state, error;
    int32_t  auth_state, auth_count;
    int32_t  state_dlexec, state_dlstep, state_dllan, state_dlwan, state_io;
    int16_t  cacfg_ver_major, cacfg_ver_minor, app_ver_major, app_ver_minor;
    int16_t  dl_ver_major, dl_ver_minor;
    int32_t  dl_ver_total, dl_ver_done;
    int64_t  dl_total, dl_done, dl_pc_done, dl_io_total, dl_io_done;
    int32_t  dl_check_complete, token_consumed, token_charged, token_unit;
    int32_t  token_lower, token_upper, token_added;
    int32_t  token_month_lower, token_month_upper, is_forced_boot, reserved0;
};
struct mi_amcus_state {
    struct mi_allnet_state allnet_state;
    struct mi_mucha_state  mucha_state;
    int64_t  clock_status, name_resolution_timeout;
    uint32_t auth_type, cab_mode, state, err;
};
struct mi_version_info {
    char game_rev[4], auth_type[16], game_id[8], game_ver[8];
    char game_cd[8], cacfg_game_ver[8], game_board_type[4], game_board_id[4];
    char auth_url[256];
};
struct mi_network_state {
    char mode[16], pcbid[16], dongle_serial[16], shop_router_ip[16];
    char auth_server_ip[16], local_ip[16], subnet_mask[16], gateway[16], primary_dns[16];
    int hop_count;
    uint32_t line_type, line_status, content_router_status, shop_router_status, hop_status;
};
struct mi_boardauth_resp {
    char url_charge[256], url_file[256], url_url1[256], url_url2[256], url_url3[256];
    char place_id[16], country_cd[16], shop_name[256], shop_nickname[128];
    char area0[64], area1[64], area2[64], area3[64];
    char area_full0[256], area_full1[256], area_full2[256], area_full3[256];
    char shop_name_en[64], shop_nickname_en[32];
    char area0_en[32], area1_en[32], area2_en[32], area3_en[32];
    char area_full0_en[128], area_full1_en[128], area_full2_en[128], area_full3_en[128];
    char prefecture_id[16], expiration_date[16], use_token[16], consume_token[32];
    char dongle_flag[8], force_boot[8];
};
struct mi_updater_state {
    uint64_t state;
    int64_t  enforce_done, enforce_total, distrib_done, distrib_total, exec_total;
    int16_t  dl_ver_major, dl_ver_minor, reserved0, reserved1;
};
struct mi_auth_server_resp {
    char uri[257], host[257], shop_name[256], shop_nickname[256];
    char region0[16], region_name0[256], region_name1[256], region_name2[256], region_name3[256];
    char place_id[16], setting[16], country[16], timezone[32], res_class[64];
};

/* ---------------------------------------------------------------------------
 *  IAuth vtable (33 methods incl. IUnknown) -- order/signatures are the
 *  confirmed ABI (see file header); STDMETHODCALLTYPE = __stdcall on the
 *  32-bit target this actually registers for.
 * -------------------------------------------------------------------------- */
typedef struct IAuthObj IAuthObj;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAuthObj*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IAuthObj*);
    ULONG   (STDMETHODCALLTYPE *Release)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Initialize)(IAuthObj*, int64_t);
    HRESULT (STDMETHODCALLTYPE *Finalize)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func5)(IAuthObj*, int64_t);
    LONG    (STDMETHODCALLTYPE *Func6)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func7)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func8)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *GetUpdaterState)(IAuthObj*, struct mi_amcus_state*);
    HRESULT (STDMETHODCALLTYPE *GetCabinetConfig)(IAuthObj*, struct mi_network_state*);
    HRESULT (STDMETHODCALLTYPE *GetVersionInfo)(IAuthObj*, struct mi_version_info*);
    HRESULT (STDMETHODCALLTYPE *Func12)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func13)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *GetAuthServerResp)(IAuthObj*, struct mi_auth_server_resp*);
    HRESULT (STDMETHODCALLTYPE *Func15)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func16)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func17)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *GetMuchaAuthResponse)(IAuthObj*, struct mi_boardauth_resp*);
    HRESULT (STDMETHODCALLTYPE *Func19)(IAuthObj*, struct mi_updater_state*);
    HRESULT (STDMETHODCALLTYPE *Func20)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func21)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func22)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func23)(IAuthObj*, int64_t);
    HRESULT (STDMETHODCALLTYPE *Func24)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func25)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func26)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func27)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func28)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func29)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func30)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func31)(IAuthObj*);
    HRESULT (STDMETHODCALLTYPE *Func32)(IAuthObj*, int64_t, int64_t*);
    HRESULT (STDMETHODCALLTYPE *Func33)(IAuthObj*, int64_t, int64_t*, int64_t*, int64_t*, int64_t*);
} IAuthVtbl;
struct IAuthObj { const IAuthVtbl *lpVtbl; };

/* {045A5150-D2B3-4590-A38B-C1158678E1AC} -- confirmed twice: allnet_setup.c's
 * own disassembly of THIS exe, and bananatools' independent reverse-engineer
 * of the same interface on a different AMCUS title. */
static const IID MI_IID_IAuth =
    { 0x045A5150, 0xD2B3, 0x4590, { 0xA3,0x8B,0xC1,0x15,0x86,0x78,0xE1,0xAC } };
/* {4603BB03-058D-43D9-B96F-639BE908C1ED} -- AMAuthd's CLSID, from
 * allnet_setup.c's own confirmed disassembly of this exact exe. */
static const CLSID MI_CLSID_AMAuthd =
    { 0x4603BB03, 0x058D, 0x43D9, { 0xB9,0x6F,0x63,0x9B,0xE9,0x08,0xC1,0xED } };

static int mi_is_init = 1;

static HRESULT STDMETHODCALLTYPE mi_QueryInterface(IAuthObj* t, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &MI_IID_IAuth) || IsEqualIID(riid, &IID_IUnknown)) {
        *out = t; return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE mi_AddRef(IAuthObj* t)  { (void)t; return 1; }
static ULONG STDMETHODCALLTYPE mi_Release(IAuthObj* t) { (void)t; return 1; }

static HRESULT STDMETHODCALLTYPE mi_Initialize(IAuthObj* t, int64_t p0) {
    (void)t; mi_log("Initialize(%lld) -> OK", (long long)p0); mi_is_init = 1; return S_OK;
}
static HRESULT STDMETHODCALLTYPE mi_Finalize(IAuthObj* t) { (void)t; mi_is_init = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func5(IAuthObj* t, int64_t p0) { (void)t; (void)p0; return S_OK; }
static LONG    STDMETHODCALLTYPE mi_Func6(IAuthObj* t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE mi_Func7(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func8(IAuthObj* t) { (void)t; return S_OK; }

/* Self-test's "local network check" / "update check" rows read THIS: every
 * field reporting the healthiest/idle terminal state. dl_check_complete=1 +
 * state_dlwan=DLWAN_COMPLETE(2) + state_dllan=DLLAN_DISABLE(2, "no LAN master,
 * not an error") mirror bananatools' own confirmed-working values exactly. */
static HRESULT STDMETHODCALLTYPE mi_GetUpdaterState(IAuthObj* t, struct mi_amcus_state* a) {
    (void)t;
    memset(a, 0, sizeof *a);
    a->allnet_state.state      = 15; /* DAEMON_IDLE */
    a->allnet_state.auth_state = 2;
    a->allnet_state.auth_count = 1;
    a->mucha_state.state        = 9;  /* DAEMON_DL */
    a->mucha_state.auth_state   = 2;
    a->mucha_state.auth_count   = 1;
    a->mucha_state.state_dlexec = 27; /* DLEXEC_PROC */
    a->mucha_state.state_dlstep = 32; /* DLSTEP_IDLE */
    a->mucha_state.state_dllan  = 43; /* DLLAN_DISABLE */
    a->mucha_state.state_dlwan  = 52; /* DLWAN_COMPLETE */
    a->mucha_state.state_io     = 59; /* DAEMON_IO_NONE */
    a->mucha_state.dl_check_complete = 1;
    a->clock_status = 1;
    a->auth_type    = 1; /* AUTH_TYPE_ALLNET */
    a->cab_mode     = 3; /* DAEMON_MODE_STANDALONE */
    a->state        = 9; /* DAEMON_DL */
    mi_log("GetUpdaterState -> idle/complete (network+update checks read this)");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mi_GetCabinetConfig(IAuthObj* t, struct mi_network_state* a) {
    (void)t;
    memset(a, 0, sizeof *a);
    strcpy(a->mode, "STANDALONE");
    strcpy(a->pcbid, "ULTRALOADER1");
    strcpy(a->dongle_serial, "0000000000");
    strcpy(a->shop_router_ip, "192.168.1.1");
    strcpy(a->auth_server_ip, "192.168.1.1");
    strcpy(a->local_ip, "192.168.1.10");
    strcpy(a->subnet_mask, "255.255.255.0");
    strcpy(a->gateway, "192.168.1.1");
    strcpy(a->primary_dns, "192.168.1.1");
    a->hop_count = 1;
    a->line_type = 1;             /* TYPE_FFT */
    a->line_status = 1;           /* STATE_OK */
    a->content_router_status = 1; /* STATE_OK */
    a->shop_router_status = 1;    /* STATE_OK */
    a->hop_status = 1;            /* STATE_OK */
    mi_log("GetCabinetConfig -> fake-but-healthy LAN topology");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mi_GetVersionInfo(IAuthObj* t, struct mi_version_info* a) {
    (void)t;
    memset(a, 0, sizeof *a);
    strcpy(a->game_rev, "1");
    strcpy(a->auth_type, "ALL.NET");
    strcpy(a->game_id, "SBZB");
    strcpy(a->game_ver, "1.10");
    strcpy(a->game_cd, "MK31");
    strcpy(a->cacfg_game_ver, "1.10");
    strcpy(a->game_board_type, "0");
    strcpy(a->game_board_id, "PCB");
    strcpy(a->auth_url, "http://127.0.0.1/mucha_front/");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mi_Func12(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func13(IAuthObj* t) { (void)t; return S_OK; }

static HRESULT STDMETHODCALLTYPE mi_GetAuthServerResp(IAuthObj* t, struct mi_auth_server_resp* a) {
    (void)t;
    memset(a, 0, sizeof *a);
    strcpy(a->uri, "http://127.0.0.1/mucha_front/");
    strcpy(a->host, "127.0.0.1");
    strcpy(a->shop_name, "Ultra Loader Cabinet");
    strcpy(a->shop_nickname, "Ultra Loader");
    strcpy(a->region0, "1");
    strcpy(a->region_name0, "Home");
    strcpy(a->place_id, "0128");
    strcpy(a->setting, "1");
    strcpy(a->country, "JPN");
    strcpy(a->timezone, "+0900");
    strcpy(a->res_class, "PowerOnResponseVer2");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mi_Func15(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func16(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func17(IAuthObj* t) { (void)t; return S_OK; }

static HRESULT STDMETHODCALLTYPE mi_GetMuchaAuthResponse(IAuthObj* t, struct mi_boardauth_resp* a) {
    (void)t;
    memset(a, 0, sizeof *a);
    strcpy(a->shop_name, "Ultra Loader Cabinet");
    strcpy(a->shop_name_en, "Ultra Loader Cabinet");
    strcpy(a->shop_nickname, "Ultra Loader");
    strcpy(a->shop_nickname_en, "Ultra Loader");
    strcpy(a->place_id, "JPN0128");
    strcpy(a->country_cd, "JPN");
    strcpy(a->area0, "008"); strcpy(a->area0_en, "008");
    strcpy(a->area1, "009"); strcpy(a->area1_en, "009");
    strcpy(a->prefecture_id, "1");
    strcpy(a->expiration_date, "null");
    strcpy(a->use_token, "0");
    strcpy(a->consume_token, "0");
    strcpy(a->dongle_flag, "1");
    strcpy(a->force_boot, "0");
    strcpy(a->url_charge, "http://127.0.0.1/charge/");
    strcpy(a->url_file, "http://127.0.0.1/file/");
    return S_OK;
}

/* CONFIRMED LOAD-BEARING (bananatools' own comment): state must be 1 or the
 * caller decides it isn't authenticated. Not MKDX-specific -- generic AMCUS
 * behavior on this same interface. */
static HRESULT STDMETHODCALLTYPE mi_Func19(IAuthObj* t, struct mi_updater_state* a) {
    (void)t;
    memset(a, 0, sizeof *a);
    a->state = 1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE mi_Func20(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func21(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func22(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func23(IAuthObj* t, int64_t p0) { (void)t; (void)p0; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func24(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func25(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func26(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func27(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func28(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func29(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func30(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func31(IAuthObj* t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func32(IAuthObj* t, int64_t p0, int64_t* p1) { (void)t; (void)p0; (void)p1; return S_OK; }
static HRESULT STDMETHODCALLTYPE mi_Func33(IAuthObj* t, int64_t p0, int64_t* p1, int64_t* p2, int64_t* p3, int64_t* p4) {
    (void)t; (void)p0; (void)p1; (void)p2; (void)p3; (void)p4; return S_OK;
}

static const IAuthVtbl mi_vtbl = {
    mi_QueryInterface, mi_AddRef, mi_Release,
    mi_Initialize, mi_Finalize, mi_Func5, mi_Func6, mi_Func7, mi_Func8,
    mi_GetUpdaterState, mi_GetCabinetConfig, mi_GetVersionInfo,
    mi_Func12, mi_Func13, mi_GetAuthServerResp, mi_Func15, mi_Func16, mi_Func17,
    mi_GetMuchaAuthResponse, mi_Func19, mi_Func20, mi_Func21, mi_Func22, mi_Func23,
    mi_Func24, mi_Func25, mi_Func26, mi_Func27, mi_Func28, mi_Func29, mi_Func30,
    mi_Func31, mi_Func32, mi_Func33,
};
static IAuthObj mi_singleton = { &mi_vtbl };

/* ---------------------------------------------------------------------------
 *  CoCreateInstance interception
 * -------------------------------------------------------------------------- */
typedef HRESULT (WINAPI *PFN_CoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static PFN_CoCreateInstance mi_real_CoCreateInstance = NULL;

static HRESULT WINAPI mi_Hook_CoCreateInstance(REFCLSID rclsid, LPUNKNOWN outer,
                                                DWORD ctx, REFIID riid, LPVOID *ppv) {
    if (rclsid && riid && IsEqualCLSID(rclsid, &MI_CLSID_AMAuthd) && IsEqualIID(riid, &MI_IID_IAuth)) {
        mi_log("CoCreateInstance(AMAuthd, IAuth) intercepted -> in-process stub "
               "(never reaches AMAuthd.exe/muchacd/muchadl)");
        *ppv = &mi_singleton;
        return S_OK;
    }
    if (mi_real_CoCreateInstance)
        return mi_real_CoCreateInstance(rclsid, outer, ctx, riid, ppv);
    return CO_E_NOTINITIALIZED;
}

/* ---------------------------------------------------------------------------
 *  Auto-registration -- same identity check as game_mkdx_bananapass.c.
 * -------------------------------------------------------------------------- */
static void __attribute__((constructor)) game_mkdx_iauth_autoregister(void) {
    int is_mkdx = 0;
    const char* e = getenv("ULTRA_MKDX_ID");
    if (e && e[0] == '1') is_mkdx = 1;
    if (!is_mkdx) {
        char p[MAX_PATH];
        GetModuleFileNameA(NULL, p, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) &&
            fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 6819840)
            is_mkdx = 1;
        if (!is_mkdx) {
            const char* base = strrchr(p, '\\');
            base = base ? base + 1 : p;
            if (_stricmp(base, "MK_AGP3_FINAL.exe") == 0) is_mkdx = 1;
        }
    }
    if (!is_mkdx) return;

    HMODULE ole32 = GetModuleHandleA("ole32.dll");
    if (ole32) mi_real_CoCreateInstance = (PFN_CoCreateInstance)GetProcAddress(ole32, "CoCreateInstance");
    ultra_iat_patch_all("ole32.dll", "CoCreateInstance", (void*)mi_Hook_CoCreateInstance);
    mi_log("IAuth in-process stub armed for MK_AGP3_FINAL.exe");

    /* Force the game-server boot row ONLINE with two surgical patches that
     * touch ONLY this one row (0x5EC380). No dependency on the AllNet HTTP
     * server, the connection object's state, IAuth, or the +0xDAC status byte.
     *
     * The row's state-1 handler is:
     *     5EC461  mov eax, ds:0xE416D0          ; connection object ptr
     *     5EC466  cmp [eax], 0xa                ; GATE: obj state == 10 ?
     *     5EC469  je  0x5EC482                  ;   yes -> do the check
     *             ... (no) fall through, stay in state 1, poll again ...
     *     5EC482  call 0x6E23E0                 ; check -> al (0=ok,1=fail)
     *     5EC489  test al, al
     *     5EC48B  setne cl                      ; cl = (al != 0)
     *     5EC49E  add ecx, 2                    ; -> 2 (ONLINE) or 3 (OFFLINE)
     *     5EC4A1  mov [esi+0x68], ecx           ; write row state
     *
     * The connection object never reaches state 10 (the AllNet server is not
     * up), so the gate never opens on its own. And 0x6E23E0 is shared by six
     * call sites, so it cannot be blanket-forced. Instead:
     *
     *   Patch 1 @ 0x5EC466: cmp/je  ->  EB 1A  jmp 0x5EC482   (+3 nop)
     *       Always fall through to the row's own check call site.
     *   Patch 2 @ 0x5EC482: call 0x6E23E0  ->  31 C0  xor eax,eax  (+3 nop)
     *       al = 0 at the call site  ->  row computes state = 2 (ONLINE).
     *
     * Both are 5-byte, in-place, ASLR-independent (offset from module base). */
    {
        HMODULE exe = GetModuleHandle(NULL);
        if (exe) {
            BYTE *base = (BYTE*)exe;
            DWORD old;
            BYTE *g = base + (0x5EC466 - 0x400000);  /* gate */
            BYTE *c = base + (0x5EC482 - 0x400000);  /* check call site */
            int ok = 1;

            if (VirtualProtect(g, 5, PAGE_EXECUTE_READWRITE, &old)) {
                g[0] = 0xEB; g[1] = 0x1A;              /* jmp short +0x1a -> 0x5EC482 */
                g[2] = 0x90; g[3] = 0x90; g[4] = 0x90; /* nop nop nop */
                VirtualProtect(g, 5, old, &old);
            } else { ok = 0; }

            if (VirtualProtect(c, 5, PAGE_EXECUTE_READWRITE, &old)) {
                c[0] = 0x31; c[1] = 0xC0;              /* xor eax,eax (al=0 -> ONLINE) */
                c[2] = 0x90; c[3] = 0x90; c[4] = 0x90; /* nop nop nop */
                VirtualProtect(c, 5, old, &old);
            } else { ok = 0; }

            if (ok) mi_log("Game server row forced ONLINE (gate+call patched)");
            else    mi_log("Game server row patch FAILED: VirtualProtect denied");
        }
    }
}

#endif /* _WIN32 */
