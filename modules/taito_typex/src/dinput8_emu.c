/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  dinput8_emu.c -- Ultra Loader clean-room DirectInput8 emulation
 * =============================================================================
 *
 *  Many PC-based arcade games (incl. early Taito Type X titles like Chaos Breaker
 *  / GigaWing Generations) take their cabinet input through DirectInput8 rather
 *  than (or in addition to) JVS-over-serial. The dumps ship a third-party loader's
 *  dinput8.dll proxy to supply that; this is OUR clean-room replacement.
 *
 *  We IAT-hook DirectInput8Create (in whatever dinput8.dll the game imported -- so
 *  this overrides the bundled proxy too) and return a synthetic IDirectInput8 that
 *  hands out a synthetic IDirectInputDevice8 reporting buttons/axes from Ultra
 *  Loader's shared input block. Every method succeeds so the game's input init
 *  completes and it boots (which it otherwise refuses, ExitProcess(1), when no
 *  input device is present).
 *
 *  Clean-room: Ultra Loader's own code over the public DirectInput8 API.
 * =============================================================================
 */
#if defined(_WIN32)

#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <windows.h>
#include <dinput.h>
#include <string.h>
#include <stdlib.h>

#include "ultra_io_shared.h"
#include "jvs_core.h"
#include <stdio.h>
#include <stdarg.h>

/* from the other hook TUs */
ultra_io_shared* ultra_hook_shared(void);
void ultra_hook_current_input(ultra_jvs_input* out);   /* loader block or gamepad */
void ultra_iat_patch_all(const char* impdll, const char* impfn, void* newfn);

/* diagnostic -- fixed %TEMP% path, not ULTRA_JVS_LOG: earlier live tracing
 * this session found that variable did not reliably resolve to a file that
 * actually got written for this board (getenv() vs GetEnvironmentVariableA()
 * inside an injected DLL is a known trap, see ultra_tracer.c's own header),
 * so this sidesteps that question entirely rather than risk a second silent
 * log. One process-wide lock: many vtable methods can log from different
 * threads. */
static CRITICAL_SECTION g_di_log_cs;
static int g_di_log_cs_ready;
static void di_log(const char* fmt, ...) {
    static char path[MAX_PATH];
    FILE* f;
    if (!path[0]) {
        DWORD n = GetTempPathA(sizeof path - 32, path);
        if (!n || n >= sizeof path - 32) { path[0] = 'x'; path[1] = 0; return; }
        strcat(path, "ultra_dinput_debug.log");
    }
    if (path[0] == 'x' && path[1] == 0) return;
    if (g_di_log_cs_ready) EnterCriticalSection(&g_di_log_cs);
    f = fopen(path, "a");
    if (f) {
        fputs("[dinput8] ", f);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    if (g_di_log_cs_ready) LeaveCriticalSection(&g_di_log_cs);
}
static void di_log_init(void) {
    if (!g_di_log_cs_ready) { InitializeCriticalSection(&g_di_log_cs); g_di_log_cs_ready = 1; }
}

/* Resolve an address to "module.dll+0xOFFSET" -- used to identify exactly
 * which instruction in game.exe calls into our emulation, straight from a
 * live run, instead of guessing vtable offsets in a static disassembly. */
static void modname_at(void *addr, char *out, size_t cap) {
    HMODULE mod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)addr, &mod) && mod) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(mod, full, MAX_PATH)) {
            const char *base = strrchr(full, '\\');
            base = base ? base + 1 : full;
            _snprintf(out, cap, "%s+0x%X", base, (unsigned)((BYTE*)addr - (BYTE*)mod));
            out[cap-1] = 0;
            return;
        }
    }
    _snprintf(out, cap, "(no module owns %p)", addr);
    out[cap-1] = 0;
}

/* ===================== synthetic IDirectInputDevice8A ====================== */
typedef struct {
    const IDirectInputDevice8AVtbl* lpVtbl;
    LONG  ref;
    DWORD fmt_size;     /* SetDataFormat dwDataSize */
    int   is_keyboard;  /* fmt_size == 256 */
    BYTE  kb_prev[256]; /* last snapshot handed out, for GetDeviceData's diff */
    DWORD kb_seq;       /* DIDEVICEOBJECTDATA.dwSequence, monotonic */
} ul_device;

static HRESULT STDMETHODCALLTYPE d_QueryInterface(IDirectInputDevice8A* T, REFIID riid, void** ppv)
{ (void)riid; if (ppv) { *ppv = T; ((ul_device*)T)->ref++; return S_OK; } return E_POINTER; }
static ULONG STDMETHODCALLTYPE d_AddRef(IDirectInputDevice8A* T) { return (ULONG)++((ul_device*)T)->ref; }
static ULONG STDMETHODCALLTYPE d_Release(IDirectInputDevice8A* T) {
    ul_device* d = (ul_device*)T; LONG r = --d->ref; if (r <= 0) { free(d); return 0; } return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE d_GetCapabilities(IDirectInputDevice8A* T, LPDIDEVCAPS c) {
    (void)T; if (c) { DWORD sz = c->dwSize; memset(c, 0, sz); c->dwSize = sz;
        c->dwFlags = DIDC_ATTACHED | DIDC_POLLEDDEVICE; c->dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        c->dwAxes = 6; c->dwButtons = 16; c->dwPOVs = 1; } return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_EnumObjects(IDirectInputDevice8A* T, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD fl)
{ (void)T;(void)cb;(void)ref; di_log("EnumObjects(fl=0x%lX)", (unsigned long)fl); return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetProperty(IDirectInputDevice8A* T, REFGUID g, LPDIPROPHEADER ph)
{ (void)T;(void)g;(void)ph; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SetProperty(IDirectInputDevice8A* T, REFGUID g, LPCDIPROPHEADER ph)
{ (void)T;
  const char *pname = "(other)";
  if (g == DIPROP_BUFFERSIZE) pname = "DIPROP_BUFFERSIZE";
  else if (g == DIPROP_AXISMODE) pname = "DIPROP_AXISMODE";
  else if (g == DIPROP_RANGE) pname = "DIPROP_RANGE";
  di_log("SetProperty(%s, headerSize=%lu)", pname, ph ? (unsigned long)ph->dwHeaderSize : 0);
  return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Acquire(IDirectInputDevice8A* T) { (void)T; di_log("Acquire()"); return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Unacquire(IDirectInputDevice8A* T) { (void)T; di_log("Unacquire()"); return DI_OK; }

/* Real DirectInput keyboard format: 256 bytes indexed by DIK_* scan code,
 * high bit set = down. GetAsyncKeyState reports BOTH real hardware keys and
 * SendInput-injected ones (same low-level input queue), so mapping each
 * scancode back to its virtual key and reading it here gives this device the
 * same answer a real DirectInput keyboard would -- this used to
 * unconditionally report "nothing pressed", confirmed live to be why Block
 * King Ball Shooter's touch-calibration screen ("[ENTER]"/"[TEST]") never
 * responded to anything, including a gamepad->SendInput bridge independently
 * verified to reach the right window. Shared by GetDeviceState (polled) and
 * GetDeviceData (buffered) below -- a menu reading press/release EVENTS
 * instead of raw state needs the same real answer from both. */
static void fill_kb_snapshot(BYTE kb[256]) {
    for (DWORD sc = 0; sc < 256; ++sc) {
        UINT vk = MapVirtualKeyA(sc, MAPVK_VSC_TO_VK_EX);
        kb[sc] = (vk && (GetAsyncKeyState((int)vk) & 0x8000)) ? 0x80 : 0x00;
    }
}

static HRESULT STDMETHODCALLTYPE d_GetDeviceState(IDirectInputDevice8A* T, DWORD cb, LPVOID data) {
    if (!data) return E_POINTER;
    memset(data, 0, cb);
    ul_device* d = (ul_device*)T;
    if (d->is_keyboard || cb == 256) {
        static int logged_kb;
        /* Every DISTINCT call site (up to 16) that ever polls the keyboard
         * gets logged once, with the exact game.exe+offset that called us --
         * this pinpoints the real code without guessing vtable slots in a
         * static disassembly. Logged separately for "polled while nothing
         * down" vs "polled while something down", since the calibration
         * screen's own poll is the one active at the moment a real key shows
         * up as pressed. */
        static void* seen_callers[16]; static int n_seen_callers;
        void* caller = __builtin_return_address(0);
        int already = 0;
        for (int i = 0; i < n_seen_callers; ++i) if (seen_callers[i] == caller) { already = 1; break; }
        BYTE kb[256];
        fill_kb_snapshot(kb);
        memcpy(data, kb, cb < 256 ? cb : 256);
        if (!logged_kb) { di_log("GetDeviceState: first KEYBOARD read (cb=%lu)", (unsigned long)cb); logged_kb = 1; }
        if (!already && n_seen_callers < 16) {
            char cn[128]; modname_at(caller, cn, sizeof cn);
            int down_sc = -1;
            for (DWORD sc = 0; sc < 256 && sc < cb; ++sc) if (kb[sc]) { down_sc = (int)sc; break; }
            di_log("GetDeviceState(KEYBOARD) called from %s (return addr %p) -- %s",
                   cn, caller, down_sc >= 0 ? "a key IS down right now" : "nothing down at this call");
            seen_callers[n_seen_callers++] = caller;
        }
        return DI_OK;
    }
    {
        static int logged_joy;
        static void* joy_seen[16]; static int n_joy_seen;
        void* caller = __builtin_return_address(0);
        int already = 0;
        for (int i = 0; i < n_joy_seen; ++i) if (joy_seen[i] == caller) { already = 1; break; }
        if (!logged_joy) { di_log("GetDeviceState: first JOYSTICK read (cb=%lu, need>=%lu)",
            (unsigned long)cb, (unsigned long)sizeof(DIJOYSTATE)); logged_joy = 1; }
        if (!already && n_joy_seen < 16) {
            char cn[128]; modname_at(caller, cn, sizeof cn);
            di_log("GetDeviceState(JOYSTICK) called from %s (return addr %p)", cn, caller);
            joy_seen[n_joy_seen++] = caller;
        }
    }
    if (cb < sizeof(DIJOYSTATE)) return DI_OK;
    DIJOYSTATE* js = (DIJOYSTATE*)data;
    js->lX = js->lY = js->lZ = js->lRx = js->lRy = js->lRz = 32767;  /* centered (0..65535) */
    for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = 0xFFFFFFFF;          /* POV centered */
    /* one input source for both JVS and DInput: loader block when present,
     * otherwise the local gamepad (XInput) so standalone runs are playable. */
    ultra_jvs_input in;
    ultra_hook_current_input(&in);
    uint16_t p = in.player[0];
    uint8_t b1 = (uint8_t)(p >> 8), b2 = (uint8_t)(p & 0xff);
    int up=(b1>>5)&1, down=(b1>>4)&1, left=(b1>>3)&1, right=(b1>>2)&1;
    int start=(b1>>7)&1, service=(b1>>6)&1;
    int push1=(b1>>1)&1, push2=b1&1, push3=(b2>>7)&1, push4=(b2>>6)&1, push5=(b2>>5)&1, push6=(b2>>4)&1;
    /* analog axes take precedence; digital dirs otherwise */
    js->lX = in.analog[0]; js->lY = in.analog[1];
    if (left) js->lX = 0; else if (right) js->lX = 65535;
    if (up)   js->lY = 0; else if (down)  js->lY = 65535;
    /* POV hat from the d-pad so menus that read the hat also navigate */
    if (up||down||left||right) {
        int ang = up ? (right?4500:left?31500:0)
                     : down ? (right?13500:left?22500:18000)
                     : (right?9000:27000);
        js->rgdwPOV[0] = (DWORD)ang;
    }
    BYTE* btn = js->rgbButtons;
    btn[0]=push1?0x80:0; btn[1]=push2?0x80:0; btn[2]=push3?0x80:0; btn[3]=push4?0x80:0;
    btn[4]=push5?0x80:0; btn[5]=push6?0x80:0; btn[6]=start?0x80:0; btn[7]=service?0x80:0;
    return DI_OK;
}
/* Buffered mode: many menu/UI screens read PRESS/RELEASE EVENTS here instead
 * of polling GetDeviceState, specifically so one physical press registers as
 * exactly one action. This used to always report "0 events" for every
 * device, keyboard included -- for a menu built on buffered reads, that is
 * indistinguishable from a keyboard that is never touched, regardless of
 * what GetDeviceState (now real, see fill_kb_snapshot) says. Only the
 * keyboard case is implemented (confirmed need); a buffered joystick read
 * still reports empty, unchanged, since nothing has shown a need for it. */
static HRESULT STDMETHODCALLTYPE d_GetDeviceData(IDirectInputDevice8A* T, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD io, DWORD fl)
{
    (void)cb;
    ul_device* d = (ul_device*)T;
    static int logged_call;
    if (!logged_call) { di_log("GetDeviceData: first call (is_keyboard=%d, cap=%lu, fl=0x%lX)",
        d->is_keyboard, io ? (unsigned long)*io : 0, (unsigned long)fl); logged_call = 1; }
    if (!io) return E_POINTER;
    if (!d->is_keyboard) { *io = 0; return DI_OK; }
    int peek = (fl & DIGDD_PEEK) != 0;
    DWORD cap = od ? *io : 0xFFFFFFFF;   /* od==NULL: caller wants a count only */
    BYTE kb[256];
    fill_kb_snapshot(kb);
    DWORD now = GetTickCount();
    DWORD n = 0;
    for (DWORD sc = 0; sc < 256; ++sc) {
        if (kb[sc] == d->kb_prev[sc]) continue;
        if (od && n < cap) {
            od[n].dwOfs = sc;
            od[n].dwData = kb[sc];
            od[n].dwTimeStamp = now;
            od[n].dwSequence = ++d->kb_seq;
        }
        n++;
        if (!peek) d->kb_prev[sc] = kb[sc];
    }
    if (n) di_log("GetDeviceData: %lu event(s), first dwOfs=0x%02X dwData=0x%02X",
                   (unsigned long)n, od ? od[0].dwOfs : 0, od ? (unsigned)od[0].dwData : 0);
    *io = n;
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_SetDataFormat(IDirectInputDevice8A* T, LPCDIDATAFORMAT df) {
    ul_device* d = (ul_device*)T; if (df) { d->fmt_size = df->dwDataSize; d->is_keyboard = (df->dwDataSize == 256); }
    di_log("SetDataFormat(dwDataSize=%lu, dwNumObjs=%lu) -> is_keyboard=%d",
           df ? (unsigned long)df->dwDataSize : 0, df ? (unsigned long)df->dwNumObjs : 0,
           df ? (df->dwDataSize == 256) : -1);
    /* A non-standard size (not 256 keyboard, not sizeof(DIJOYSTATE)) means a
     * CUSTOM DIDATAFORMAT -- casting the buffer to DIJOYSTATE and writing at
     * ITS offsets, as GetDeviceState currently does, lands data at whatever
     * offset happens to be there, not at whatever offset the game's own
     * rgodf[] actually declared. Dumping every object's real offset/type
     * here is the only way to build a response this game will actually
     * parse correctly, instead of guessing again. */
    if (df && df->dwDataSize != 256 && df->dwDataSize != sizeof(DIJOYSTATE) && df->rgodf) {
        di_log("  CUSTOM format dump (%lu objects):", (unsigned long)df->dwNumObjs);
        for (DWORD i = 0; i < df->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT *o = &df->rgodf[i];
            BYTE tbyte = (BYTE)(o->dwType & 0xFF);
            const char *tname = "?";
            if (tbyte & DIDFT_BUTTON) tname = "BUTTON";
            else if (tbyte & DIDFT_POV) tname = "POV";
            else if (tbyte & DIDFT_AXIS) tname = "AXIS";
            const char *gname = "(null)";
            if (o->pguid) {
                if (memcmp(o->pguid, &GUID_XAxis, sizeof(GUID)) == 0) gname = "GUID_XAxis";
                else if (memcmp(o->pguid, &GUID_YAxis, sizeof(GUID)) == 0) gname = "GUID_YAxis";
                else if (memcmp(o->pguid, &GUID_ZAxis, sizeof(GUID)) == 0) gname = "GUID_ZAxis";
                else if (memcmp(o->pguid, &GUID_Button, sizeof(GUID)) == 0) gname = "GUID_Button";
                else if (memcmp(o->pguid, &GUID_POV, sizeof(GUID)) == 0) gname = "GUID_POV";
                else if (memcmp(o->pguid, &GUID_Key, sizeof(GUID)) == 0) gname = "GUID_Key";
                else gname = "(other)";
            }
            di_log("    [%3lu] ofs=%3lu type=0x%08lX(%s) flags=0x%08lX guid=%s",
                   (unsigned long)i, (unsigned long)o->dwOfs, (unsigned long)o->dwType, tname,
                   (unsigned long)o->dwFlags, gname);
        }
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_SetEventNotification(IDirectInputDevice8A* T, HANDLE h){ (void)T; di_log("SetEventNotification(h=%p)", (void*)h); return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SetCooperativeLevel(IDirectInputDevice8A* T, HWND w, DWORD fl){
    (void)T; di_log("SetCooperativeLevel(hwnd=%p, fl=0x%lX%s%s%s%s)", (void*)w, (unsigned long)fl,
        (fl & DISCL_EXCLUSIVE) ? " EXCLUSIVE" : "", (fl & DISCL_NONEXCLUSIVE) ? " NONEXCLUSIVE" : "",
        (fl & DISCL_FOREGROUND) ? " FOREGROUND" : "", (fl & DISCL_BACKGROUND) ? " BACKGROUND" : "");
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_GetObjectInfo(IDirectInputDevice8A* T, LPDIDEVICEOBJECTINSTANCEA i, DWORD o, DWORD h){ (void)T;(void)i;(void)o;(void)h; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetDeviceInfo(IDirectInputDevice8A* T, LPDIDEVICEINSTANCEA i){
    (void)T; if (i) { DWORD sz=i->dwSize; memset(i,0,sz); i->dwSize=sz; i->dwDevType=DI8DEVTYPE_JOYSTICK|(DI8DEVTYPEJOYSTICK_STANDARD<<8);
        strncpy(i->tszInstanceName,"Ultra Loader JVS",MAX_PATH-1); strncpy(i->tszProductName,"Ultra Loader JVS",MAX_PATH-1); } return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_RunControlPanel(IDirectInputDevice8A* T, HWND w, DWORD fl){ (void)T;(void)w;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Initialize(IDirectInputDevice8A* T, HINSTANCE h, DWORD v, REFGUID g){ (void)T;(void)h;(void)v;(void)g; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_CreateEffect(IDirectInputDevice8A* T, REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* eff, LPUNKNOWN u){ (void)T;(void)g;(void)e;(void)u; if(eff)*eff=NULL; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE d_EnumEffects(IDirectInputDevice8A* T, LPDIENUMEFFECTSCALLBACKA cb, LPVOID r, DWORD t){ (void)T;(void)cb;(void)r;(void)t; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetEffectInfo(IDirectInputDevice8A* T, LPDIEFFECTINFOA i, REFGUID g){ (void)T;(void)i;(void)g; return DIERR_UNSUPPORTED; }
static HRESULT STDMETHODCALLTYPE d_GetForceFeedbackState(IDirectInputDevice8A* T, LPDWORD o){ (void)T; if(o)*o=0; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SendForceFeedbackCommand(IDirectInputDevice8A* T, DWORD c){ (void)T;(void)c; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_EnumCreatedEffectObjects(IDirectInputDevice8A* T, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID r, DWORD fl){ (void)T;(void)cb;(void)r;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Escape(IDirectInputDevice8A* T, LPDIEFFESCAPE e){ (void)T;(void)e; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Poll(IDirectInputDevice8A* T){ (void)T; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SendDeviceData(IDirectInputDevice8A* T, DWORD cb, LPCDIDEVICEOBJECTDATA od, LPDWORD io, DWORD fl){ (void)T;(void)cb;(void)od;(void)io;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_EnumEffectsInFile(IDirectInputDevice8A* T, LPCSTR f, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID r, DWORD fl){ (void)T;(void)f;(void)cb;(void)r;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_WriteEffectToFile(IDirectInputDevice8A* T, LPCSTR f, DWORD n, LPDIFILEEFFECT e, DWORD fl){ (void)T;(void)f;(void)n;(void)e;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_BuildActionMap(IDirectInputDevice8A* T, LPDIACTIONFORMATA a, LPCSTR u, DWORD fl){ (void)T;(void)a;(void)u;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SetActionMap(IDirectInputDevice8A* T, LPDIACTIONFORMATA a, LPCSTR u, DWORD fl){ (void)T;(void)a;(void)u;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetImageInfo(IDirectInputDevice8A* T, LPDIDEVICEIMAGEINFOHEADERA h){ (void)T;(void)h; return DI_OK; }

static const IDirectInputDevice8AVtbl g_dev_vtbl = {
    d_QueryInterface, d_AddRef, d_Release, d_GetCapabilities, d_EnumObjects,
    d_GetProperty, d_SetProperty, d_Acquire, d_Unacquire, d_GetDeviceState,
    d_GetDeviceData, d_SetDataFormat, d_SetEventNotification, d_SetCooperativeLevel,
    d_GetObjectInfo, d_GetDeviceInfo, d_RunControlPanel, d_Initialize, d_CreateEffect,
    d_EnumEffects, d_GetEffectInfo, d_GetForceFeedbackState, d_SendForceFeedbackCommand,
    d_EnumCreatedEffectObjects, d_Escape, d_Poll, d_SendDeviceData, d_EnumEffectsInFile,
    d_WriteEffectToFile, d_BuildActionMap, d_SetActionMap, d_GetImageInfo
};

static IDirectInputDevice8A* make_device(void) {
    ul_device* d = (ul_device*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->lpVtbl = &g_dev_vtbl;
    d->ref = 1;
    return (IDirectInputDevice8A*)d;
}

/* ========================= synthetic IDirectInput8A ======================== */
typedef struct { const IDirectInput8AVtbl* lpVtbl; LONG ref; } ul_factory;

static HRESULT STDMETHODCALLTYPE f_QueryInterface(IDirectInput8A* T, REFIID riid, void** ppv)
{ (void)riid; if (ppv) { *ppv = T; ((ul_factory*)T)->ref++; return S_OK; } return E_POINTER; }
static ULONG STDMETHODCALLTYPE f_AddRef(IDirectInput8A* T){ return (ULONG)++((ul_factory*)T)->ref; }
static ULONG STDMETHODCALLTYPE f_Release(IDirectInput8A* T){ ul_factory* f=(ul_factory*)T; LONG r=--f->ref; if(r<=0){ free(f); return 0;} return (ULONG)r; }
static HRESULT STDMETHODCALLTYPE f_CreateDevice(IDirectInput8A* T, REFGUID g, LPDIRECTINPUTDEVICE8A* dev, LPUNKNOWN u){
    (void)T;(void)u; if (!dev) return E_POINTER;
    const char *gname = "(other GUID)";
    if (g) {
        if (memcmp(g, &GUID_SysKeyboard, sizeof(GUID)) == 0) gname = "GUID_SysKeyboard";
        else if (memcmp(g, &GUID_SysMouse, sizeof(GUID)) == 0) gname = "GUID_SysMouse";
        else if (memcmp(g, &GUID_Joystick, sizeof(GUID)) == 0) gname = "GUID_Joystick";
    } else gname = "(NULL guid)";
    di_log("CreateDevice(%s) -> synthetic device (SAME object/behavior regardless of which GUID was asked for)", gname);
    *dev = make_device(); return *dev ? DI_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE f_EnumDevices(IDirectInput8A* T, DWORD devtype, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD fl) {
    (void)T;(void)devtype;(void)fl;
    if (cb) {
        DIDEVICEINSTANCEA inst; memset(&inst, 0, sizeof inst); inst.dwSize = sizeof inst;
        inst.dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        strncpy(inst.tszInstanceName, "Ultra Loader JVS", MAX_PATH-1);
        strncpy(inst.tszProductName,  "Ultra Loader JVS", MAX_PATH-1);
        cb(&inst, ref);   /* report exactly one joystick */
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetDeviceStatus(IDirectInput8A* T, REFGUID g){ (void)T;(void)g; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_RunControlPanel(IDirectInput8A* T, HWND w, DWORD fl){ (void)T;(void)w;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_Initialize(IDirectInput8A* T, HINSTANCE h, DWORD v){ (void)T;(void)h;(void)v; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_FindDevice(IDirectInput8A* T, REFGUID g, LPCSTR n, LPGUID out){ (void)T;(void)g;(void)n;(void)out; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_EnumDevicesBySemantics(IDirectInput8A* T, LPCSTR u, LPDIACTIONFORMATA a, LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID r, DWORD fl){ (void)T;(void)u;(void)a;(void)cb;(void)r;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE f_ConfigureDevices(IDirectInput8A* T, LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSA p, DWORD fl, LPVOID r){ (void)T;(void)cb;(void)p;(void)fl;(void)r; return DI_OK; }

static const IDirectInput8AVtbl g_fac_vtbl = {
    f_QueryInterface, f_AddRef, f_Release, f_CreateDevice, f_EnumDevices,
    f_GetDeviceStatus, f_RunControlPanel, f_Initialize, f_FindDevice,
    f_EnumDevicesBySemantics, f_ConfigureDevices
};

/* ===================== DirectInput8Create hook + install =================== */
static HRESULT WINAPI Hook_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN unk) {
    (void)inst; (void)ver; (void)riid; (void)unk;
    if (!out) return E_POINTER;
    di_log("DirectInput8Create -> synthetic IDirectInput8 (our emulation in use)");
    ul_factory* f = (ul_factory*)calloc(1, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->lpVtbl = &g_fac_vtbl; f->ref = 1;
    *out = f;
    return DI_OK;
}

void ultra_dinput8_install(void) {
    const char* skip = getenv("ULTRA_NO_DINPUT8");
    if (skip && skip[0] == '1') return;
    di_log_init();
    ultra_iat_patch_all("dinput8.dll", "DirectInput8Create", (void*)Hook_DirectInput8Create);
}

#endif /* _WIN32 */
