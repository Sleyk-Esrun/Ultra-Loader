/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  dinput8_emu_allsux.c -- Sega ALLS UX board's OWN clean-room DirectInput8
 *                          emulation.
 * =============================================================================
 *
 *  Some ALLS titles (Unity/UE engines commonly used on Win10 IoT cabinets) take
 *  part of their input through DirectInput8 rather than (or alongside) the
 *  Sega IO4-USB report this board's sega_io4_allsux.c already services. This
 *  IAT-hooks DirectInput8Create and returns a synthetic IDirectInput8 that
 *  hands out a synthetic IDirectInputDevice8 reporting buttons/axes from this
 *  board's own cabinet-input source (ul_ax_current_input(), fed by
 *  hook_dllmain_allsux.c's XInput poll thread) -- so a title that insists on
 *  finding a DirectInput device completes its input init instead of refusing
 *  to boot.
 *
 *  Clean-room, fully independent: the sega_alls_ux module's own copy over the
 *  public DirectInput8 API. No dependency (source or binary) on
 *  modules/native_x86/src/hook/dinput8_emu.c (read for reference only).
 * =============================================================================
 */
#if defined(_WIN32)

#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <windows.h>
#include <dinput.h>
#include <string.h>
#include <stdlib.h>

#include "allsux_hook.h"

/* ===================== synthetic IDirectInputDevice8A ====================== */
typedef struct {
    const IDirectInputDevice8AVtbl* lpVtbl;
    LONG  ref;
    DWORD fmt_size;
    int   is_keyboard;
} ul_ax_device;

static HRESULT STDMETHODCALLTYPE d_QueryInterface(IDirectInputDevice8A* T, REFIID riid, void** ppv)
{ (void)riid; if (ppv) { *ppv = T; ((ul_ax_device*)T)->ref++; return S_OK; } return E_POINTER; }
static ULONG STDMETHODCALLTYPE d_AddRef(IDirectInputDevice8A* T) { return (ULONG)++((ul_ax_device*)T)->ref; }
static ULONG STDMETHODCALLTYPE d_Release(IDirectInputDevice8A* T) {
    ul_ax_device* d = (ul_ax_device*)T; LONG r = --d->ref; if (r <= 0) { free(d); return 0; } return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE d_GetCapabilities(IDirectInputDevice8A* T, LPDIDEVCAPS c) {
    (void)T; if (c) { DWORD sz = c->dwSize; memset(c, 0, sz); c->dwSize = sz;
        c->dwFlags = DIDC_ATTACHED | DIDC_POLLEDDEVICE; c->dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        c->dwAxes = 6; c->dwButtons = 16; c->dwPOVs = 1; } return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_EnumObjects(IDirectInputDevice8A* T, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD fl)
{ (void)T;(void)cb;(void)ref;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetProperty(IDirectInputDevice8A* T, REFGUID g, LPDIPROPHEADER ph)
{ (void)T;(void)g;(void)ph; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SetProperty(IDirectInputDevice8A* T, REFGUID g, LPCDIPROPHEADER ph)
{ (void)T;(void)g;(void)ph; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Acquire(IDirectInputDevice8A* T) { (void)T; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_Unacquire(IDirectInputDevice8A* T) { (void)T; return DI_OK; }

static HRESULT STDMETHODCALLTYPE d_GetDeviceState(IDirectInputDevice8A* T, DWORD cb, LPVOID data) {
    if (!data) return E_POINTER;
    memset(data, 0, cb);
    ul_ax_device* d = (ul_ax_device*)T;
    if (d->is_keyboard || cb == 256) {
        BYTE* kb = (BYTE*)data;
        /* DIRECTINPUT INDEXES THIS BUFFER BY DIK SCANCODE, NOT BY VIRTUAL KEY.
         *
         * This loop used to write kb[vk] directly, and the two numbering schemes
         * are unrelated -- so every single key arrived at the game as a DIFFERENT
         * key. Not a subtle skew either:
         *     '9'  VK 0x39 -> kb[0x39], and DIK 0x39 is SPACE
         *     '1'  VK 0x31 -> kb[0x31], and DIK 0x31 is N
         *     '5'  VK 0x35 -> kb[0x35], and DIK 0x35 is SLASH
         *     UP   VK 0x26 -> kb[0x26], and DIK 0x26 is L
         * which is exactly what Contra Evolution HD showed: no control did what it
         * should, while '9' -- SERVICE in our profile -- inserted credits, because
         * the game received SPACE.
         *
         * DIK codes ARE PS/2 set-1 scancodes, so MapVirtualKey answers directly.
         * The wrinkle is the extended keys (arrows, the gray navigation block,
         * right ctrl/alt, numpad divide): their scancode collides with a numpad
         * key and DirectInput separates them with the 0x80 bit -- DIK_UP is 0xC8,
         * which is DIK_NUMPAD8 (0x48) | 0x80. MapVirtualKey does not set that bit,
         * so we add it or every arrow press lands on the numpad.
         *
         * MIGRATED 2026-09-20 from konami_pc, where it was proven on Contra
         * Evolution HD. It sat fixed on that one board and wrong on 22 others: a
         * board-by-board fix is not a fix, it is a fix plus 22 latent copies. */
        for (int vk = 0; vk < 256; ++vk) {
            UINT sc;
            if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
            sc = MapVirtualKeyA((UINT)vk, 0 /* MAPVK_VK_TO_VSC */);
            if (!sc) continue;          /* mouse buttons and unmapped VKs */
            switch (vk) {               /* the extended set */
                case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
                case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
                case VK_PRIOR:  case VK_NEXT:
                case VK_RCONTROL: case VK_RMENU:
                case VK_NUMLOCK: case VK_DIVIDE:
                    sc |= 0x80;
                    break;
                default: break;
            }
            if (sc < cb) kb[sc] = 0x80;
        }
        return DI_OK;
    }
    if (cb < sizeof(DIJOYSTATE)) return DI_OK;
    DIJOYSTATE* js = (DIJOYSTATE*)data;
    js->lX = js->lY = js->lZ = js->lRx = js->lRy = js->lRz = 32767;   /* centered */
    for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = 0xFFFFFFFF;           /* POV centered */

    ultra_jvs_input in;
    ul_ax_current_input(&in);
    uint16_t p = in.player[0];
    uint8_t b1 = (uint8_t)(p >> 8), b2 = (uint8_t)(p & 0xff);
    int up=(b1>>5)&1, down=(b1>>4)&1, left=(b1>>3)&1, right=(b1>>2)&1;
    int start=(b1>>7)&1, service=(b1>>6)&1;
    int push1=(b1>>1)&1, push2=b1&1, push3=(b2>>7)&1, push4=(b2>>6)&1, push5=(b2>>5)&1, push6=(b2>>4)&1;
    js->lX = in.analog[0]; js->lY = in.analog[1];
    if (left) js->lX = 0; else if (right) js->lX = 65535;
    if (up)   js->lY = 0; else if (down)  js->lY = 65535;
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
static HRESULT STDMETHODCALLTYPE d_GetDeviceData(IDirectInputDevice8A* T, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD io, DWORD fl)
{ (void)T;(void)cb;(void)od;(void)fl; if (io) *io = 0; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SetDataFormat(IDirectInputDevice8A* T, LPCDIDATAFORMAT df) {
    ul_ax_device* d = (ul_ax_device*)T; if (df) { d->fmt_size = df->dwDataSize; d->is_keyboard = (df->dwDataSize == 256); } return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_SetEventNotification(IDirectInputDevice8A* T, HANDLE h){ (void)T;(void)h; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SetCooperativeLevel(IDirectInputDevice8A* T, HWND w, DWORD fl){ (void)T;(void)w;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetObjectInfo(IDirectInputDevice8A* T, LPDIDEVICEOBJECTINSTANCEA i, DWORD o, DWORD h){ (void)T;(void)i;(void)o;(void)h; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetDeviceInfo(IDirectInputDevice8A* T, LPDIDEVICEINSTANCEA i){
    (void)T; if (i) { DWORD sz=i->dwSize; memset(i,0,sz); i->dwSize=sz; i->dwDevType=DI8DEVTYPE_JOYSTICK|(DI8DEVTYPEJOYSTICK_STANDARD<<8);
        strncpy(i->tszInstanceName,"Ultra Loader ALLS UX",MAX_PATH-1); strncpy(i->tszProductName,"Ultra Loader ALLS UX",MAX_PATH-1); } return DI_OK;
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
    ul_ax_device* d = (ul_ax_device*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->lpVtbl = &g_dev_vtbl;
    d->ref = 1;
    return (IDirectInputDevice8A*)d;
}

/* ========================= synthetic IDirectInput8A ======================== */
typedef struct { const IDirectInput8AVtbl* lpVtbl; LONG ref; } ul_ax_factory;

static HRESULT STDMETHODCALLTYPE f_QueryInterface(IDirectInput8A* T, REFIID riid, void** ppv)
{ (void)riid; if (ppv) { *ppv = T; ((ul_ax_factory*)T)->ref++; return S_OK; } return E_POINTER; }
static ULONG STDMETHODCALLTYPE f_AddRef(IDirectInput8A* T){ return (ULONG)++((ul_ax_factory*)T)->ref; }
static ULONG STDMETHODCALLTYPE f_Release(IDirectInput8A* T){ ul_ax_factory* f=(ul_ax_factory*)T; LONG r=--f->ref; if(r<=0){ free(f); return 0;} return (ULONG)r; }
static HRESULT STDMETHODCALLTYPE f_CreateDevice(IDirectInput8A* T, REFGUID g, LPDIRECTINPUTDEVICE8A* dev, LPUNKNOWN u){
    (void)T;(void)g;(void)u; if (!dev) return E_POINTER; ul_ax_log("dinput8: CreateDevice -> synthetic device\n"); *dev = make_device(); return *dev ? DI_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE f_EnumDevices(IDirectInput8A* T, DWORD devtype, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD fl) {
    (void)T;(void)devtype;(void)fl;
    if (cb) {
        DIDEVICEINSTANCEA inst; memset(&inst, 0, sizeof inst); inst.dwSize = sizeof inst;
        inst.dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        strncpy(inst.tszInstanceName, "Ultra Loader ALLS UX", MAX_PATH-1);
        strncpy(inst.tszProductName,  "Ultra Loader ALLS UX", MAX_PATH-1);
        cb(&inst, ref);
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
    ul_ax_log("dinput8: DirectInput8Create -> synthetic IDirectInput8\n");
    ul_ax_factory* f = (ul_ax_factory*)calloc(1, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->lpVtbl = &g_fac_vtbl; f->ref = 1;
    *out = f;
    return DI_OK;
}

void ul_ax_install_dinput8(void) {
    if (ul_ax_env_on("ULTRA_NO_DINPUT8")) { ul_ax_log("dinput8: skipped (ULTRA_NO_DINPUT8=1)\n"); return; }
    ul_ax_iat_patch_all("dinput8.dll", "DirectInput8Create", (void*)Hook_DirectInput8Create);
    ul_ax_log("dinput8: synthetic DirectInput8Create armed\n");
}

#endif /* _WIN32 */
