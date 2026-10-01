/*
 * dinput8_crk.c -- standalone neutral DirectInput8 proxy for Konami PC arcade.
 *
 * Placed in the game directory as dinput8.dll, this is loaded by Windows before
 * the system dinput8.dll (app directory wins in DLL search order).  It provides
 * a completely neutral/silent synthetic IDirectInput8A: every EnumDevices call
 * reports one joystick, every GetDeviceState returns zeroed DIJOYSTATE so no
 * axis or button is seen as pressed.
 *
 * Why: KSHG.exe imports DirectInput8Create and needs a valid device to initialize,
 * but feeds ALL real input through shaiolib.dll (gun + buttons).  The system DI8
 * would hand out a real joystick (your Xbox pad through the XInput compat layer)
 * whose left-stick Y has drift -> "down permanently pressed" in the service menu.
 *
 * Gun calibration note: the calibration loop may poll GetDeviceState for the
 * trigger button.  We map real LMB (from SendInput or actual click) to button 0
 * so the auto-click thread's SendInput pulses drive calibration to completion.
 * Mouse cursor position is mapped to the joystick axes for gun position.
 */

#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── synthetic IDirectInputDevice8A ──────────────────────────────────────── */
typedef struct {
    const IDirectInputDevice8AVtbl* lpVtbl;
    LONG  ref;
    int   is_keyboard;   /* SetDataFormat saw dwDataSize == 256 */
} ul_dev;

static HRESULT STDMETHODCALLTYPE
d_QI(IDirectInputDevice8A* T, REFIID r, void** p)
{ (void)r; if (p) { *p = T; ++((ul_dev*)T)->ref; return S_OK; } return E_POINTER; }

static ULONG STDMETHODCALLTYPE d_AddRef(IDirectInputDevice8A* T) { return (ULONG)++((ul_dev*)T)->ref; }

static ULONG STDMETHODCALLTYPE d_Release(IDirectInputDevice8A* T) {
    ul_dev* d = (ul_dev*)T;
    LONG r = --d->ref;
    if (r <= 0) { free(d); return 0; }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
d_GetCaps(IDirectInputDevice8A* T, LPDIDEVCAPS c) {
    (void)T;
    if (c) {
        DWORD sz = c->dwSize; memset(c, 0, sz); c->dwSize = sz;
        c->dwFlags   = DIDC_ATTACHED | DIDC_POLLEDDEVICE;
        c->dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        c->dwAxes = 6; c->dwButtons = 16; c->dwPOVs = 1;
    }
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE
d_EnumObjects(IDirectInputDevice8A* T, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD fl)
{ (void)T;(void)cb;(void)ref;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_GetProperty(IDirectInputDevice8A* T, REFGUID g, LPDIPROPHEADER h)
{ (void)T;(void)g;(void)h; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_SetProperty(IDirectInputDevice8A* T, REFGUID g, LPCDIPROPHEADER h)
{ (void)T;(void)g;(void)h; return DI_OK; }

static HRESULT STDMETHODCALLTYPE d_Acquire   (IDirectInputDevice8A* T) { (void)T; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Unacquire (IDirectInputDevice8A* T) { (void)T; return DI_OK; }

/* Shared log: appends to the same file shaiolib_crk writes when
 * ULTRA_SHAIOLIB_LOG=1.  Zero-overhead when the env var is absent. */
static void di8_log(const char *msg) {
    const char *lp = getenv("ULTRA_SHAIOLIB_LOG");
    if (!lp || !lp[0]) return;
    char path[MAX_PATH];
    if (lp[0] == '1' && lp[1] == '\0') {
        GetTempPathA(MAX_PATH, path);
        strncat(path, "shaiolib_debug.log", MAX_PATH - strlen(path) - 1);
    } else {
        strncpy(path, lp, MAX_PATH - 1); path[MAX_PATH - 1] = '\0';
    }
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "[DI8] %s\n", msg); fclose(f); }
}

static HRESULT STDMETHODCALLTYPE
d_GetDeviceState(IDirectInputDevice8A* T, DWORD cb, LPVOID data) {
    (void)T;
    if (!data) return E_POINTER;
    memset(data, 0, cb);
    if (!((ul_dev*)T)->is_keyboard && cb >= sizeof(DIJOYSTATE)) {
        DIJOYSTATE* js = (DIJOYSTATE*)data;
        /* POV hats: 0xFFFFFFFF = no hat engaged (0 = NORTH, wrong default). */
        for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = 0xFFFFFFFF;
        /* Map real LMB (physical or SendInput) to button 0 - the GHL trigger.
         * The gun calibration loop polls GetDeviceState for this bit.
         * GetAsyncKeyState reads kernel state updated by SendInput even when the
         * message pump is frozen, so the auto-click thread's pulses are seen. */
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
            js->rgbButtons[0] = 0x80;
            di8_log("GetDeviceState: btn0 (trigger) SET from LMB");
        } else {
            static int s_once = 0;
            if (!s_once) { s_once = 1; di8_log("GetDeviceState: first call (joystick, btn0 clear)"); }
        }
        /* Map cursor position to joystick axes (0-65535 range). */
        POINT pt; GetCursorPos(&pt);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        js->lX = (sw > 1) ? (int)((long)pt.x * 65535 / (sw - 1)) : 32768;
        js->lY = (sh > 1) ? (int)((long)pt.y * 65535 / (sh - 1)) : 32768;
    }
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE
d_GetDeviceData(IDirectInputDevice8A* T, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD io, DWORD fl)
{ (void)T;(void)cb;(void)od;(void)fl; if (io) *io = 0; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_SetDataFormat(IDirectInputDevice8A* T, LPCDIDATAFORMAT df) {
    ul_dev* d = (ul_dev*)T;
    if (df) d->is_keyboard = (df->dwDataSize == 256);
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE
d_SetEventNotification(IDirectInputDevice8A* T, HANDLE h) { (void)T;(void)h; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_SetCoopLevel(IDirectInputDevice8A* T, HWND w, DWORD fl) { (void)T;(void)w;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_GetObjectInfo(IDirectInputDevice8A* T, LPDIDEVICEOBJECTINSTANCEA i, DWORD o, DWORD h)
{ (void)T;(void)i;(void)o;(void)h; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_GetDeviceInfo(IDirectInputDevice8A* T, LPDIDEVICEINSTANCEA i) {
    (void)T;
    if (i) {
        DWORD sz = i->dwSize; memset(i, 0, sz); i->dwSize = sz;
        i->dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        strncpy(i->tszInstanceName, "Ultra Loader JVS", MAX_PATH - 1);
        strncpy(i->tszProductName,  "Ultra Loader JVS", MAX_PATH - 1);
    }
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE
d_RunControlPanel(IDirectInputDevice8A* T, HWND w, DWORD fl) { (void)T;(void)w;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_Initialize(IDirectInputDevice8A* T, HINSTANCE h, DWORD v, REFGUID g)
{ (void)T;(void)h;(void)v;(void)g; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_CreateEffect(IDirectInputDevice8A* T, REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* ef, LPUNKNOWN u)
{ (void)T;(void)g;(void)e;(void)u; if(ef)*ef=NULL; return DIERR_UNSUPPORTED; }

static HRESULT STDMETHODCALLTYPE
d_EnumEffects(IDirectInputDevice8A* T, LPDIENUMEFFECTSCALLBACKA cb, LPVOID r, DWORD t)
{ (void)T;(void)cb;(void)r;(void)t; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_GetEffectInfo(IDirectInputDevice8A* T, LPDIEFFECTINFOA i, REFGUID g)
{ (void)T;(void)i;(void)g; return DIERR_UNSUPPORTED; }

static HRESULT STDMETHODCALLTYPE
d_GetFFState(IDirectInputDevice8A* T, LPDWORD o) { (void)T; if(o)*o=0; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_SendFFCmd(IDirectInputDevice8A* T, DWORD c) { (void)T;(void)c; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_EnumCreated(IDirectInputDevice8A* T, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID r, DWORD fl)
{ (void)T;(void)cb;(void)r;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_Escape(IDirectInputDevice8A* T, LPDIEFFESCAPE e) { (void)T;(void)e; return DI_OK; }

static HRESULT STDMETHODCALLTYPE d_Poll(IDirectInputDevice8A* T) { (void)T; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_SendDeviceData(IDirectInputDevice8A* T, DWORD cb, LPCDIDEVICEOBJECTDATA od, LPDWORD io, DWORD fl)
{ (void)T;(void)cb;(void)od;(void)io;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_EnumEffectsInFile(IDirectInputDevice8A* T, LPCSTR f, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID r, DWORD fl)
{ (void)T;(void)f;(void)cb;(void)r;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_WriteEffectToFile(IDirectInputDevice8A* T, LPCSTR f, DWORD n, LPDIFILEEFFECT e, DWORD fl)
{ (void)T;(void)f;(void)n;(void)e;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_BuildActionMap(IDirectInputDevice8A* T, LPDIACTIONFORMATA a, LPCSTR u, DWORD fl)
{ (void)T;(void)a;(void)u;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_SetActionMap(IDirectInputDevice8A* T, LPDIACTIONFORMATA a, LPCSTR u, DWORD fl)
{ (void)T;(void)a;(void)u;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
d_GetImageInfo(IDirectInputDevice8A* T, LPDIDEVICEIMAGEINFOHEADERA h)
{ (void)T;(void)h; return DI_OK; }

static const IDirectInputDevice8AVtbl g_dev_vtbl = {
    d_QI, d_AddRef, d_Release,
    d_GetCaps, d_EnumObjects, d_GetProperty, d_SetProperty,
    d_Acquire, d_Unacquire, d_GetDeviceState, d_GetDeviceData,
    d_SetDataFormat, d_SetEventNotification, d_SetCoopLevel,
    d_GetObjectInfo, d_GetDeviceInfo, d_RunControlPanel, d_Initialize,
    d_CreateEffect, d_EnumEffects, d_GetEffectInfo, d_GetFFState,
    d_SendFFCmd, d_EnumCreated, d_Escape, d_Poll, d_SendDeviceData,
    d_EnumEffectsInFile, d_WriteEffectToFile,
    d_BuildActionMap, d_SetActionMap, d_GetImageInfo
};

static IDirectInputDevice8A* make_device(void) {
    ul_dev* d = (ul_dev*)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->lpVtbl = &g_dev_vtbl;
    d->ref    = 1;
    return (IDirectInputDevice8A*)d;
}

/* ── synthetic IDirectInput8A ─────────────────────────────────────────────── */
typedef struct { const IDirectInput8AVtbl* lpVtbl; LONG ref; } ul_fac;

static HRESULT STDMETHODCALLTYPE
f_QI(IDirectInput8A* T, REFIID r, void** p)
{ (void)r; if (p) { *p = T; ++((ul_fac*)T)->ref; return S_OK; } return E_POINTER; }

static ULONG STDMETHODCALLTYPE f_AddRef(IDirectInput8A* T) { return (ULONG)++((ul_fac*)T)->ref; }

static ULONG STDMETHODCALLTYPE f_Release(IDirectInput8A* T) {
    ul_fac* f = (ul_fac*)T;
    LONG r = --f->ref;
    if (r <= 0) { free(f); return 0; }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
f_CreateDevice(IDirectInput8A* T, REFGUID g, LPDIRECTINPUTDEVICE8A* dev, LPUNKNOWN u) {
    (void)T;(void)g;(void)u;
    if (!dev) return E_POINTER;
    *dev = make_device();
    return *dev ? DI_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE
f_EnumDevices(IDirectInput8A* T, DWORD dtype, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD fl) {
    (void)T;(void)dtype;(void)fl;
    if (cb) {
        DIDEVICEINSTANCEA inst; memset(&inst, 0, sizeof inst); inst.dwSize = sizeof inst;
        inst.dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        strncpy(inst.tszInstanceName, "Ultra Loader JVS", MAX_PATH - 1);
        strncpy(inst.tszProductName,  "Ultra Loader JVS", MAX_PATH - 1);
        cb(&inst, ref);
    }
    return DI_OK;
}

static HRESULT STDMETHODCALLTYPE
f_GetDeviceStatus(IDirectInput8A* T, REFGUID g) { (void)T;(void)g; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
f_RunControlPanel(IDirectInput8A* T, HWND w, DWORD fl) { (void)T;(void)w;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
f_Initialize(IDirectInput8A* T, HINSTANCE h, DWORD v) { (void)T;(void)h;(void)v; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
f_FindDevice(IDirectInput8A* T, REFGUID g, LPCSTR n, LPGUID out)
{ (void)T;(void)g;(void)n;(void)out; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
f_EnumBySemantics(IDirectInput8A* T, LPCSTR u, LPDIACTIONFORMATA a,
                  LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID r, DWORD fl)
{ (void)T;(void)u;(void)a;(void)cb;(void)r;(void)fl; return DI_OK; }

static HRESULT STDMETHODCALLTYPE
f_ConfigureDevices(IDirectInput8A* T, LPDICONFIGUREDEVICESCALLBACK cb,
                   LPDICONFIGUREDEVICESPARAMSA p, DWORD fl, LPVOID r)
{ (void)T;(void)cb;(void)p;(void)fl;(void)r; return DI_OK; }

static const IDirectInput8AVtbl g_fac_vtbl = {
    f_QI, f_AddRef, f_Release,
    f_CreateDevice, f_EnumDevices,
    f_GetDeviceStatus, f_RunControlPanel, f_Initialize,
    f_FindDevice, f_EnumBySemantics, f_ConfigureDevices
};

/* ── exported entry-point ─────────────────────────────────────────────────── */
__declspec(dllexport)
HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid,
                                  LPVOID* out, LPUNKNOWN unk) {
    (void)inst;(void)ver;(void)riid;(void)unk;
    if (!out) return E_POINTER;
    ul_fac* f = (ul_fac*)calloc(1, sizeof *f);
    if (!f) return E_OUTOFMEMORY;
    f->lpVtbl = &g_fac_vtbl;
    f->ref    = 1;
    *out = f;
    return DI_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID v) { (void)h;(void)r;(void)v; return TRUE; }
