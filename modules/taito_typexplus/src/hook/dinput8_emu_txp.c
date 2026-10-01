/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  dinput8_emu_txp.c -- Ultra Loader clean-room DirectInput8 emulation
 *                       (Taito Type X+ board's own independent copy)
 * =============================================================================
 *
 *  Many PC-based arcade games take their cabinet input through DirectInput8
 *  rather than (or in addition to) JVS-over-serial. This is Taito Type X+'s own
 *  clean-room replacement: we IAT-hook DirectInput8Create and return a
 *  synthetic IDirectInput8 that hands out a synthetic IDirectInputDevice8
 *  reporting buttons/axes from this board's own local input source (keyboard +
 *  XInput controller 0, see hook_dllmain_txp.c). Every method succeeds so the
 *  game's input init completes and it boots (which it otherwise refuses,
 *  ExitProcess(1), when no input device is present).
 *
 *  Read modules/native_x86/src/hook/dinput8_emu.c as a reference only -- this
 *  is Taito Type X+'s own compiled copy; no shared source, no shared binary.
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
#include <stdio.h>

#include "hook_txp.h"

/* diagnostic (ULTRA_JVS_LOG) */
static void di_log(const char* msg) { ultra_txp_log("DINPUT8: %s", msg); }

/* ===================== synthetic IDirectInputDevice8A ====================== */
/* THE AXIS RANGE IS THE CALLER'S TO CHOOSE, AND WE HAVE TO REMEMBER IT.
 *
 * DirectInput lets an application declare what numbers it wants its axes in,
 * with SetProperty(DIPROP_RANGE). Until this struct existed we accepted that
 * call, returned DI_OK, threw the range away, and then reported every axis on
 * a hardcoded 0..65535 scale no matter what had been asked for.
 *
 * On Gundam Spirits of Zeon that is the whole bug. Libigdisplay.dll sets
 * lMin=-1000 / lMax=+1000 on all six axes and then multiplies every reading by
 * 0.001f to land in -1.0 .. +1.0. Handing it our 32767 for "centered" gives it
 * +32.767, about thirty-two times past full deflection, on every one of the
 * 4800+ polls in the log - a stick jammed hard over forever, which presents
 * exactly as "no controls". There was no value we could send that it would
 * read as negative, and none at rest that it would read as neutral.
 *
 * Indexed by DIJOYSTATE axis: 0=lX 1=lY 2=lZ 3=lRx 4=lRy 5=lRz, which is also
 * the byte offset divided by four, so DIPH_BYOFFSET maps straight onto it. */
enum { UL_AXES = 8 };
typedef struct {
    const IDirectInputDevice8AVtbl* lpVtbl;
    LONG  ref;
    DWORD fmt_size;     /* SetDataFormat dwDataSize */
    int   is_keyboard;  /* fmt_size == 256 */
    int   is_mouse;     /* created against GUID_SysMouse */
    POINT last_pt;      /* previous cursor position, for relative deltas */
    int   have_last;
    GUID  guid;         /* the GUID this device was created from */
    int   have_guid;
    LONG  range_lo[UL_AXES], range_hi[UL_AXES];
    int   range_set[UL_AXES];
    DWORD deadzone[UL_AXES];   /* 0..10000, DirectInput's hundredths of a percent */
} ul_txp_device;

/* Map a 0..65535 reading onto whatever range this axis was asked for.
 *
 * Defaults to 0..65535 when the caller never set a range, because that IS the
 * DirectInput default for an absolute axis - so a game that does not call
 * SetProperty sees exactly what it saw before this existed. */
static LONG ul_map_axis(ul_txp_device* d, int i, unsigned v)
{
    LONG lo = 0, hi = 65535;
    if (i >= 0 && i < UL_AXES && d->range_set[i]) { lo = d->range_lo[i]; hi = d->range_hi[i]; }
    if (hi == lo) return lo;
    {
        double span = (double)hi - (double)lo;
        double out  = (double)lo + ((double)v * span) / 65535.0;
        /* Deadzone is expressed against the center, so snap near-center
         * readings to dead center rather than letting stick noise trickle
         * through as a permanent drift. */
        if (i >= 0 && i < UL_AXES && d->deadzone[i] > 0) {
            double mid = (double)lo + span / 2.0;
            double dz  = (span / 2.0) * ((double)d->deadzone[i] / 10000.0);
            if (out > mid - dz && out < mid + dz) out = mid;
        }
        return (LONG)(out >= 0 ? out + 0.5 : out - 0.5);
    }
}

static HRESULT STDMETHODCALLTYPE d_QueryInterface(IDirectInputDevice8A* T, REFIID riid, void** ppv)
{ (void)riid; if (ppv) { *ppv = T; ((ul_txp_device*)T)->ref++; return S_OK; } return E_POINTER; }
static ULONG STDMETHODCALLTYPE d_AddRef(IDirectInputDevice8A* T) { return (ULONG)++((ul_txp_device*)T)->ref; }
static ULONG STDMETHODCALLTYPE d_Release(IDirectInputDevice8A* T) {
    ul_txp_device* d = (ul_txp_device*)T; LONG r = --d->ref; if (r <= 0) { free(d); return 0; } return (ULONG)r;
}
static HRESULT STDMETHODCALLTYPE d_GetCapabilities(IDirectInputDevice8A* T, LPDIDEVCAPS c) {
    (void)T; if (c) { DWORD sz = c->dwSize; memset(c, 0, sz); c->dwSize = sz;
        c->dwFlags = DIDC_ATTACHED | DIDC_POLLEDDEVICE; c->dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        /* THE BUTTON COUNT IS A FINGERPRINT, NOT A DETAIL.
         *
         * Libigdisplay.dll classifies the pad from this triple and picks how to
         * read it (0x100086E0). It accepts three shapes:
         *
         *   dwPOVs==0 && dwButtons==16   -> profile 1  (also swaps buttons 10/11)
         *   dwPOVs==1 && dwButtons==12   -> profile 2
         *   dwButtons > 16               -> accepted unconditionally, no profile
         *
         * and ANY of them sets the recognized flag at controller+0x74. We used
         * to report 6 / 16 / 1, which is the one combination that fails all
         * three: 1 POV fails the first, 16 buttons fails the second, and 16 is
         * not greater than 16 so the third misses by one. The flag stayed clear
         * and the poller took its generic branch (0x100051FE), which reads six
         * axes with no inversion instead of the two-gun layout at 0x10005110 -
         * P1 on lX/lY and P2 on lZ/lRz, Y and Rz inverted.
         *
         * 20 takes the third door. It is the only one that needs no other
         * change: profile 1 would silently swap two buttons, and profile 2 would
         * mean claiming fewer buttons than we actually report state for. */
        c->dwAxes = 6; c->dwButtons = 20; c->dwPOVs = 1;
        { static int said = 0;
          if (!said) { said = 1;
              ultra_txp_log("DINPUT7: GetCapabilities -> %lu axes / %lu buttons / %lu POV "
                            "(>16 buttons = the shape the engine accepts)",
                            (unsigned long)c->dwAxes, (unsigned long)c->dwButtons,
                            (unsigned long)c->dwPOVs); } }
    } return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_EnumObjects(IDirectInputDevice8A* T, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD fl)
{ (void)T;(void)cb;(void)ref;(void)fl; return DI_OK; }
/* Which axes a property call addresses. DIPH_DEVICE means all of them;
 * DIPH_BYOFFSET names one by its byte offset in the data format, which for
 * DIJOYSTATE is the axis index times four. Returns the count written. */
static int ul_prop_axes(const DIPROPHEADER* ph, int* out)
{
    int n = 0;
    if (!ph) return 0;
    if (ph->dwHow == DIPH_DEVICE) { for (int i = 0; i < UL_AXES; i++) out[n++] = i; return n; }
    if (ph->dwHow == DIPH_BYOFFSET) {
        int i = (int)(ph->dwObj / 4);
        if (i >= 0 && i < UL_AXES) out[n++] = i;
        return n;
    }
    if (ph->dwHow == DIPH_BYID) {
        int i = (int)(DIDFT_GETINSTANCE(ph->dwObj));
        if (i >= 0 && i < UL_AXES) out[n++] = i;
    }
    return n;
}

/* A MAKEDIPROP "GUID" is not a GUID at all - it is a small integer cast to a
 * pointer, which is why these are compared as numbers and not with memcmp. */
#define UL_DIPROP_ID(g)  ((ULONG_PTR)(g))

static HRESULT STDMETHODCALLTYPE d_GetProperty(IDirectInputDevice8A* T, REFGUID g, LPDIPROPHEADER ph)
{
    ul_txp_device* d = (ul_txp_device*)T;
    int ax[UL_AXES], n;
    if (!ph) return E_POINTER;
    /* This used to return DI_OK and fill in NOTHING, which is worse than
     * failing: the caller reads back its own uninitialised stack buffer and
     * believes the numbers it finds there. */
    n = ul_prop_axes(ph, ax);
    if (n < 1) return DIERR_UNSUPPORTED;
    switch (UL_DIPROP_ID(g)) {
    case 4: {   /* DIPROP_RANGE */
        DIPROPRANGE* r = (DIPROPRANGE*)ph;
        int i = ax[0];
        r->lMin = d->range_set[i] ? d->range_lo[i] : 0;
        r->lMax = d->range_set[i] ? d->range_hi[i] : 65535;
        return DI_OK;
    }
    case 5: {   /* DIPROP_DEADZONE */
        DIPROPDWORD* w = (DIPROPDWORD*)ph;
        w->dwData = d->deadzone[ax[0]];
        return DI_OK;
    }
    default:
        return DIERR_UNSUPPORTED;
    }
}

static HRESULT STDMETHODCALLTYPE d_SetProperty(IDirectInputDevice8A* T, REFGUID g, LPCDIPROPHEADER ph)
{
    ul_txp_device* d = (ul_txp_device*)T;
    int ax[UL_AXES], n, k;
    if (!ph) return E_POINTER;
    n = ul_prop_axes(ph, ax);
    switch (UL_DIPROP_ID(g)) {
    case 4: {   /* DIPROP_RANGE -- the one that matters */
        const DIPROPRANGE* r = (const DIPROPRANGE*)ph;
        for (k = 0; k < n; k++) {
            d->range_lo[ax[k]] = r->lMin;
            d->range_hi[ax[k]] = r->lMax;
            d->range_set[ax[k]] = 1;
        }
        if (n > 0) {
            static int said = 0;
            if (!said) { said = 1;
                ultra_txp_log("DINPUT7: axis range set to %ld..%ld (%d axis/axes) "
                              "- reporting on the caller's scale from here",
                              (long)r->lMin, (long)r->lMax, n); }
        }
        return DI_OK;
    }
    case 5: {   /* DIPROP_DEADZONE */
        const DIPROPDWORD* w = (const DIPROPDWORD*)ph;
        for (k = 0; k < n; k++) d->deadzone[ax[k]] = w->dwData;
        return DI_OK;
    }
    /* Everything else is accepted and ignored on purpose: saturation, axis
     * mode, buffer size and autocentre change nothing about what this device
     * can produce, and refusing them makes callers give up on the device. */
    default:
        return DI_OK;
    }
}
/* Whether the game ever ACQUIRES matters: an engine that tracks its own
 * acquired flag will not read a device it believes it never took. Our Poll
 * always succeeds, so the game's recovery-and-Acquire path may simply never
 * be entered. One line each, once, tells us which. */
static HRESULT STDMETHODCALLTYPE d_Acquire(IDirectInputDevice8A* T) {
    static int told = 0;
    (void)T;
    if (!told) { told = 1; ultra_txp_log("DINPUT7: Acquire() called by the game"); }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_Unacquire(IDirectInputDevice8A* T) { (void)T; return DI_OK; }

static HRESULT STDMETHODCALLTYPE d_GetDeviceState(IDirectInputDevice8A* T, DWORD cb, LPVOID data) {
    if (!data) return E_POINTER;
    memset(data, 0, cb);
    ul_txp_device* d = (ul_txp_device*)T;
    /* Is the game actually POLLING us? Everything downstream of here is known
     * good - the device reads JVS and fills DIJOYSTATE, the keyboard path fills
     * from GetAsyncKeyState - so if input is still dead the question is whether
     * this is reached at all, and nothing in this file logged it. First call
     * plus one line every ~10s at 60Hz; silent if the game never polls, which
     * is itself the answer. */
    {
        static unsigned n = 0;
        if (n == 0 || (n % 600u) == 0u)
            ultra_txp_log("DINPUT7: GetDeviceState #%u cb=%lu (%s)",
                          n, (unsigned long)cb,
                          (d->is_keyboard || cb == 256) ? "keyboard" :
                          (d->is_mouse || cb < sizeof(DIJOYSTATE)) ? "mouse" : "joystick");
        n++;
    }
    /* ---- MOUSE ------------------------------------------------------------
     * This branch did not exist, and its absence is not a small gap: a
     * DIMOUSESTATE is 16 bytes and a DIMOUSESTATE2 is 20, both far below
     * sizeof(DIJOYSTATE), so a game asking us for the mouse fell through the
     * joystick size check below and got the all-zero buffer memset above --
     * forever. No motion, no buttons, and no way to tell that apart from a
     * player sitting perfectly still.
     *
     * That is the whole of Gundam Spirits of Zeon's aim and trigger: it runs in
     * its own built-in mouse+keyboard mode (ioemulation), and Libigdisplay.dll
     * takes the mouse through DirectInput 7.
     *
     * The source is the CURSOR, deliberately. DirectInput mouse axes are
     * relative by default, so we report how far the cursor has moved since the
     * last poll -- which means a real mouse, a lightgun pose and a gamepad
     * stick all arrive here identically, because the Omni-Bridge has already
     * put every one of them on the cursor. That is the input contract: Omni
     * decides which devices exist, the bridge says where the player is
     * pointing, and this device is only the translator the game can read. */
    if (d->is_mouse || (!d->is_keyboard && cb != 256 && cb < sizeof(DIJOYSTATE))) {
        LONG dx = 0, dy = 0;
        POINT pt;
        if (cb < sizeof(DIMOUSESTATE)) return DI_OK;
        if (GetCursorPos(&pt)) {
            if (d->have_last) { dx = pt.x - d->last_pt.x; dy = pt.y - d->last_pt.y; }
            d->last_pt = pt;
            d->have_last = 1;
        }
        {
            const char* neutral = getenv("ULTRA_DI8_NEUTRAL");
            DIMOUSESTATE2* ms = (DIMOUSESTATE2*)data;   /* prefix-compatible with DIMOUSESTATE */
            if (neutral && neutral[0] == '1') return DI_OK;
            ms->lX = dx;
            ms->lY = dy;
            ms->lZ = 0;
            /* The bridge fires with real SendInput clicks, so the live button
             * state already carries both a physical mouse and a pad trigger. */
            ms->rgbButtons[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 0x80 : 0;
            ms->rgbButtons[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 0x80 : 0;
            ms->rgbButtons[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 0x80 : 0;
        }
        return DI_OK;
    }

    if (d->is_keyboard || cb == 256) {
        const char* neutral = getenv("ULTRA_DI8_NEUTRAL");
        if (!neutral || neutral[0] != '1') {
            BYTE* kb = (BYTE*)data;
            /* DIRECTINPUT INDEXES THIS BUFFER BY DIK SCANCODE, NOT BY VIRTUAL KEY.
             *
             * This loop used to write kb[vk] directly, and the two numbering
             * schemes are unrelated -- so every single key arrived at the game as
             * a DIFFERENT key. It is not a subtle skew either:
             *     '9'   VK 0x39 -> kb[0x39], and DIK 0x39 is SPACE
             *     '1'   VK 0x31 -> kb[0x31], and DIK 0x31 is N
             *     '5'   VK 0x35 -> kb[0x35], and DIK 0x35 is SLASH
             *     UP    VK 0x26 -> kb[0x26], and DIK 0x26 is L
             * which is exactly what Contra Evolution HD showed: no control did
             * what it should, while '9' -- SERVICE in our profile -- inserted
             * credits, because the game received SPACE.
             *
             * DIK codes ARE PS/2 set-1 scancodes, so MapVirtualKey gives the
             * right answer directly. The only wrinkle is the extended keys
             * (arrows, the gray navigation block, right ctrl/alt, numpad divide):
             * their scancode collides with a numpad key, and DirectInput
             * distinguishes them with the 0x80 bit -- DIK_UP is 0xC8, which is
             * DIK_NUMPAD8 (0x48) | 0x80. MapVirtualKey does not set that bit, so
             * we add it ourselves or every arrow press lands on the numpad.
             *
             * MIGRATED 2026-09-20 from konami_pc, where it was proven on Contra
             * Evolution HD. It was fixed on that one board and left wrong on 22
             * others for weeks -- a board-by-board fix is not a fix, it is a fix
             * plus 22 latent copies of the same bug. */
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
        }
        return DI_OK;
    }
    if (cb < sizeof(DIJOYSTATE)) return DI_OK;
    DIJOYSTATE* js = (DIJOYSTATE*)data;
    /* ULTRA_DI8_NEUTRAL=1: pure silent state -- memset already zeroed all axes.
     * Only POVs need an explicit value (0 means NORTH, not neutral). We do NOT
     * pre-set axes to 32767 before this check: a game using the signed
     * [-32768,32767] DI8 axis range would read 32767 as maximum-positive (full
     * DOWN on Y) and cycle a menu forever. With axes at 0 (signed center) the
     * joystick reads as neutral in both signed and most custom ranges. */
    const char* neutral = getenv("ULTRA_DI8_NEUTRAL");
    if (neutral && neutral[0] == '1') {
        for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = 0xFFFFFFFF;
        return DI_OK;
    }
    /* Center, expressed on whatever scale the caller asked for. This was the
     * literal 32767, which is only "centered" if nobody ever called
     * SetProperty(DIPROP_RANGE) - and the one game on this board always does. */
    js->lX  = ul_map_axis(d, 0, 0x8000);
    js->lY  = ul_map_axis(d, 1, 0x8000);
    js->lZ  = ul_map_axis(d, 2, 0x8000);
    js->lRx = ul_map_axis(d, 3, 0x8000);
    js->lRy = ul_map_axis(d, 4, 0x8000);
    js->lRz = ul_map_axis(d, 5, 0x8000);
    for (int i = 0; i < 4; ++i) js->rgdwPOV[i] = 0xFFFFFFFF;          /* POV centered */
    /* one input source for both JVS and DInput: the local keyboard/XInput read
     * so both transports agree on what the player is doing. */
    ultra_jvs_input_txp in;
    ultra_hook_current_input_txp(&in);
    uint16_t p = in.player[0];
    uint8_t b1 = (uint8_t)(p >> 8), b2 = (uint8_t)(p & 0xff);
    int up=(b1>>5)&1, down=(b1>>4)&1, left=(b1>>3)&1, right=(b1>>2)&1;
    int start=(b1>>7)&1, service=(b1>>6)&1;
    int push1=(b1>>1)&1, push2=b1&1, push3=(b2>>7)&1, push4=(b2>>6)&1, push5=(b2>>5)&1, push6=(b2>>4)&1;
    /* analog axes take precedence; digital dirs otherwise */
    js->lX = ul_map_axis(d, 0, in.analog[0]);
    js->lY = ul_map_axis(d, 1, in.analog[1]);
    if (left) js->lX = ul_map_axis(d, 0, 0); else if (right) js->lX = ul_map_axis(d, 0, 65535);
    if (up)   js->lY = ul_map_axis(d, 1, 0); else if (down)  js->lY = ul_map_axis(d, 1, 65535);
    if (up||down||left||right) {
        int ang = up ? (right?4500:left?31500:0)
                     : down ? (right?13500:left?22500:18000)
                     : (right?9000:27000);
        js->rgdwPOV[0] = (DWORD)ang;
    }
    BYTE* btn = js->rgbButtons;
    btn[0]=push1?0x80:0; btn[1]=push2?0x80:0; btn[2]=push3?0x80:0; btn[3]=push4?0x80:0;
    btn[4]=push5?0x80:0; btn[5]=push6?0x80:0; btn[6]=start?0x80:0; btn[7]=service?0x80:0;

    /* PROVE THE HANDOFF, ONCE.
     *
     * Everything up to here has been reasoned about rather than observed: the
     * range is right, the fingerprint is accepted, the game polls us 60 times a
     * second - and still nothing moves. There are only two possibilities left
     * and they need opposite fixes, so guessing between them is worthless:
     *
     *   the struct leaves here EMPTY  -> the pad never reaches this function,
     *                                    and the fault is upstream in
     *                                    ultra_hook_current_input_txp.
     *   the struct leaves here FULL   -> we are handing the game good data and
     *                                    it is not binding it, which is a
     *                                    problem on the game's side of the
     *                                    handoff (Acquire, or the action map).
     *
     * So say it out loud the first time the player actually does something.
     * Silent = nobody touched the pad, which is itself an answer. */
    {
        static int told = 0;
        int anybtn = 0, k;
        for (k = 0; k < 8; k++) if (btn[k]) anybtn = 1;
        if (!told && (anybtn || left || right || up || down ||
                      in.analog[0] < 0x6000 || in.analog[0] > 0xA000 ||
                      in.analog[1] < 0x6000 || in.analog[1] > 0xA000)) {
            told = 1;
            ultra_txp_log("DINPUT7: FIRST LIVE INPUT -> lX=%ld lY=%ld "
                          "(raw analog %u,%u) buttons=%d%d%d%d%d%d%d%d "
                          "[range %ld..%ld] - the game is being handed this",
                          (long)js->lX, (long)js->lY,
                          (unsigned)in.analog[0], (unsigned)in.analog[1],
                          !!btn[0], !!btn[1], !!btn[2], !!btn[3],
                          !!btn[4], !!btn[5], !!btn[6], !!btn[7],
                          (long)(d->range_set[0] ? d->range_lo[0] : 0),
                          (long)(d->range_set[0] ? d->range_hi[0] : 65535));
        }
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_GetDeviceData(IDirectInputDevice8A* T, DWORD cb, LPDIDEVICEOBJECTDATA od, LPDWORD io, DWORD fl)
{ (void)T;(void)cb;(void)od;(void)fl; if (io) *io = 0; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SetDataFormat(IDirectInputDevice8A* T, LPCDIDATAFORMAT df) {
    ul_txp_device* d = (ul_txp_device*)T;
    if (df) {
        d->fmt_size = df->dwDataSize;
        d->is_keyboard = (df->dwDataSize == 256);
        /* c_dfDIMouse is 16 bytes and c_dfDIMouse2 is 20; a caller that made
         * the device from a generic GUID only reveals which kind it wanted
         * here, so latch it rather than re-deriving it from cb every poll. */
        if (df->dwDataSize == sizeof(DIMOUSESTATE) ||
            df->dwDataSize == sizeof(DIMOUSESTATE2)) d->is_mouse = 1;
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE d_SetEventNotification(IDirectInputDevice8A* T, HANDLE h){ (void)T;(void)h; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_SetCooperativeLevel(IDirectInputDevice8A* T, HWND w, DWORD fl){ (void)T;(void)w;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetObjectInfo(IDirectInputDevice8A* T, LPDIDEVICEOBJECTINSTANCEA i, DWORD o, DWORD h){ (void)T;(void)i;(void)o;(void)h; return DI_OK; }
static HRESULT STDMETHODCALLTYPE d_GetDeviceInfo(IDirectInputDevice8A* T, LPDIDEVICEINSTANCEA i){
    ul_txp_device* d = (ul_txp_device*)T;
    if (i) { DWORD sz=i->dwSize; memset(i,0,sz); i->dwSize=sz; i->dwDevType=DI8DEVTYPE_JOYSTICK|(DI8DEVTYPEJOYSTICK_STANDARD<<8);
        /* Echo the GUID we were created from. Answering GUID_NULL here
         * contradicted what EnumDevices reported a moment earlier. */
        if (d->have_guid) { i->guidInstance = d->guid; i->guidProduct = d->guid; }
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
static HRESULT STDMETHODCALLTYPE d_Poll(IDirectInputDevice8A* T){
    static int told = 0;
    (void)T;
    if (!told) { told = 1; ultra_txp_log("DINPUT7: Poll() called by the game"); }
    return DI_OK;
}
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
    ul_txp_device* d = (ul_txp_device*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->lpVtbl = &g_dev_vtbl;
    d->ref = 1;
    return (IDirectInputDevice8A*)d;
}

/* ========================= synthetic IDirectInput8A ======================== */
typedef struct { const IDirectInput8AVtbl* lpVtbl; LONG ref; } ul_txp_factory;

static HRESULT STDMETHODCALLTYPE f_QueryInterface(IDirectInput8A* T, REFIID riid, void** ppv)
{ (void)riid; if (ppv) { *ppv = T; ((ul_txp_factory*)T)->ref++; return S_OK; } return E_POINTER; }
static ULONG STDMETHODCALLTYPE f_AddRef(IDirectInput8A* T){ return (ULONG)++((ul_txp_factory*)T)->ref; }
static ULONG STDMETHODCALLTYPE f_Release(IDirectInput8A* T){ ul_txp_factory* f=(ul_txp_factory*)T; LONG r=--f->ref; if(r<=0){ free(f); return 0;} return (ULONG)r; }
static HRESULT STDMETHODCALLTYPE f_CreateDevice(IDirectInput8A* T, REFGUID g, LPDIRECTINPUTDEVICE8A* dev, LPUNKNOWN u){
    (void)T;(void)g;(void)u; if (!dev) return E_POINTER; di_log("CreateDevice -> synthetic device"); *dev = make_device(); return *dev ? DI_OK : E_OUTOFMEMORY;
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
    ul_txp_factory* f = (ul_txp_factory*)calloc(1, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->lpVtbl = &g_fac_vtbl; f->ref = 1;
    *out = f;
    return DI_OK;
}

/* ===================== DirectInput *7* (legacy dinput.dll) ==================
 *
 *  Gundam Spirits of Zeon does not use DirectInput 8 at all. Its input lives in
 *  Libigdisplay.dll (the Alchemy/Intrinsic Graphics layer), and that DLL
 *  imports exactly one input symbol:
 *
 *      DINPUT.dll -> DirectInputCreateEx
 *
 *  -- the DirectInput *7* entry point. Hooking dinput8!DirectInput8Create, as
 *  this file did, patches a door the game never opens, which is why the pad,
 *  the keyboard and the mouse all did nothing while the log cheerfully showed
 *  the gamepad being read.
 *
 *  The device side needs no new code. DirectInput's device vtables are strict
 *  prefixes of one another -- IDirectInputDevice2A, then 7 adds two entries,
 *  then 8 adds three more, always appended, never reordered. So a DI8 device
 *  object answers a DI7 caller correctly for every method a DI7 caller can
 *  name. Only the FACTORY differs, and only by one method (CreateDeviceEx).
 *
 *  The interface is declared here by hand rather than by including dinput.h a
 *  second time at a different DIRECTINPUT_VERSION, which does not coexist with
 *  the DI8 declarations above in one translation unit.
 */
typedef struct ul_di7 ul_di7;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ul_di7*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ul_di7*);
    ULONG   (STDMETHODCALLTYPE *Release)(ul_di7*);
    HRESULT (STDMETHODCALLTYPE *CreateDevice)(ul_di7*, REFGUID, void**, LPUNKNOWN);
    HRESULT (STDMETHODCALLTYPE *EnumDevices)(ul_di7*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetDeviceStatus)(ul_di7*, REFGUID);
    HRESULT (STDMETHODCALLTYPE *RunControlPanel)(ul_di7*, HWND, DWORD);
    HRESULT (STDMETHODCALLTYPE *Initialize)(ul_di7*, HINSTANCE, DWORD);
    HRESULT (STDMETHODCALLTYPE *FindDevice)(ul_di7*, REFGUID, LPCSTR, LPGUID);
    HRESULT (STDMETHODCALLTYPE *CreateDeviceEx)(ul_di7*, REFGUID, REFIID, void**, LPUNKNOWN);
} ul_di7_vtbl;
struct ul_di7 { const ul_di7_vtbl* lpVtbl; LONG ref; };

/* GUID_SysKeyboard / GUID_SysMouse, spelled out so the device we hand back is
 * told which kind it is instead of guessing from the buffer size later. */
static const GUID UL_GUID_SysKeyboard =
    {0x6F1D2B61,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID UL_GUID_SysMouse =
    {0x6F1D2B60,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
/* Our synthetic JVS pad. Windows has no fixed GUID for a joystick the way it
 * does for the system keyboard and mouse - a real one is per-device - so we
 * mint a stable private one. It only has to round-trip: whatever EnumDevices
 * reports is what CreateDevice is called with, and a zeroed GUID gives the
 * caller nothing to ask for. */
static const GUID UL_GUID_UltraJoystick =
    {0x554C5452,0x4A56,0x5300,{0x55,0x4C,0x54,0x52,0x41,0x00,0x00,0x01}};

static void* make_device_for(REFGUID g) {
    IDirectInputDevice8A* dev = make_device();
    if (dev && g) {
        ul_txp_device* d = (ul_txp_device*)dev;
        if (!memcmp(g, &UL_GUID_SysKeyboard, sizeof(GUID))) d->is_keyboard = 1;
        if (!memcmp(g, &UL_GUID_SysMouse,    sizeof(GUID))) d->is_mouse    = 1;
        /* Remember what we were created from, so GetDeviceInfo can echo it.
         * It used to answer GUID_NULL, contradicting the guidProduct this same
         * shim had just reported from EnumDevices - and a caller that keys a
         * per-device remap table off guidProduct finds nothing under zero. */
        d->guid = *g; d->have_guid = 1;
        ultra_txp_log("DINPUT7: CreateDevice %s",
                      !memcmp(g, &UL_GUID_SysKeyboard, sizeof(GUID)) ? "SysKeyboard" :
                      !memcmp(g, &UL_GUID_SysMouse,    sizeof(GUID)) ? "SysMouse" :
                                                                      "joystick/other");
    }
    return dev;
}

static HRESULT STDMETHODCALLTYPE i7_QueryInterface(ul_di7* T, REFIID riid, void** ppv)
{ (void)riid; if (!ppv) return E_POINTER; *ppv = T; T->ref++; return S_OK; }
static ULONG STDMETHODCALLTYPE i7_AddRef(ul_di7* T){ return (ULONG)++T->ref; }
static ULONG STDMETHODCALLTYPE i7_Release(ul_di7* T){ LONG r = --T->ref; if (r <= 0) { free(T); return 0; } return (ULONG)r; }
static HRESULT STDMETHODCALLTYPE i7_CreateDevice(ul_di7* T, REFGUID g, void** dev, LPUNKNOWN u)
{ (void)T;(void)u; if (!dev) return E_POINTER; *dev = make_device_for(g); return *dev ? DI_OK : E_OUTOFMEMORY; }
static HRESULT STDMETHODCALLTYPE i7_CreateDeviceEx(ul_di7* T, REFGUID g, REFIID riid, void** dev, LPUNKNOWN u)
{ (void)T;(void)riid;(void)u; if (!dev) return E_POINTER; *dev = make_device_for(g); return *dev ? DI_OK : E_OUTOFMEMORY; }
/* DirectInput *7* device-type constants. Not available here because this TU is
 * compiled at DIRECTINPUT_VERSION 0x0800, and DI8 renumbered every one of them:
 * DI7 MOUSE/KEYBOARD/JOYSTICK are 1/2/4, DI8's are 0x12/0x13/0x15. */
/* Verified against the live game 2026-09-15: it enumerated 2, then 3, then 4.
 * An earlier version of this block had MOUSE=1/KEYBOARD=2, so the request for
 * type 3 (the KEYBOARD) matched nothing and returned "0 device(s)" - visible in
 * the log, which is the only reason it was caught. DIDEVTYPE_DEVICE is 1. */
#define UL_DI7_DEVTYPE_DEVICE    1
#define UL_DI7_DEVTYPE_MOUSE     2
#define UL_DI7_DEVTYPE_KEYBOARD  3
#define UL_DI7_DEVTYPE_JOYSTICK  4

/* Enumerate for a DI7 caller, in DI7's own terms.
 *
 * This used to forward straight to f_EnumDevices(), the DI8 implementation,
 * which reports exactly one device typed DI8DEVTYPE_JOYSTICK (0x15) with a
 * zeroed GUID, and ignores the type filter the caller passed. A DI7 game tests
 * GET_DIDEVICE_TYPE(dwDevType) == DIDEVTYPE_JOYSTICK (4), sees 0x15, decides
 * the device is not what it asked for, and never calls CreateDevice - which is
 * exactly what the Gundam log shows: the factory handed out twice, CreateDevice
 * never once, and no input anywhere in the game.
 *
 * So: answer in DI7 numbering, honor the filter (0 = all), and hand back real
 * instance GUIDs, because CreateDevice is called with the GUID reported here
 * and a caller that gets GUID_NULL back has nothing to ask for. */
static void i7_report(LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD type,
                      const GUID* guid, const char* name, int* n)
{
    DIDEVICEINSTANCEA inst;
    memset(&inst, 0, sizeof inst);
    inst.dwSize = sizeof inst;
    inst.dwDevType = type;
    if (guid) { inst.guidInstance = *guid; inst.guidProduct = *guid; }
    strncpy(inst.tszInstanceName, name, MAX_PATH - 1);
    strncpy(inst.tszProductName,  name, MAX_PATH - 1);
    (*n)++;
    cb(&inst, ref);           /* DIENUM_STOP is honored by the caller, not us */
}

static HRESULT STDMETHODCALLTYPE i7_EnumDevices(ul_di7* T, DWORD devtype,
                                                LPDIENUMDEVICESCALLBACKA cb,
                                                LPVOID ref, DWORD fl)
{
    (void)T; (void)fl;
    if (!cb) return DI_OK;
    {
        DWORD want = devtype & 0xFF;      /* low byte is the device type in DI7 */
        /* 0 = "all", and DIDEVTYPE_DEVICE (1) is the generic ask, not a class of
         * its own - answer both with everything we have. */
        if (want == UL_DI7_DEVTYPE_DEVICE) want = 0;
        int n = 0;
        if (want == 0 || want == UL_DI7_DEVTYPE_KEYBOARD)
            i7_report(cb, ref, UL_DI7_DEVTYPE_KEYBOARD, &UL_GUID_SysKeyboard,
                      "Ultra Loader Keyboard", &n);
        if (want == 0 || want == UL_DI7_DEVTYPE_MOUSE)
            i7_report(cb, ref, UL_DI7_DEVTYPE_MOUSE, &UL_GUID_SysMouse,
                      "Ultra Loader Mouse", &n);
        if (want == 0 || want == UL_DI7_DEVTYPE_JOYSTICK)
            i7_report(cb, ref,
                      UL_DI7_DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8),
                      &UL_GUID_UltraJoystick, "Ultra Loader JVS", &n);
        ultra_txp_log("DINPUT7: EnumDevices(type=0x%08lX) -> reported %d device(s)",
                      (unsigned long)devtype, n);
    }
    return DI_OK;
}
static HRESULT STDMETHODCALLTYPE i7_GetDeviceStatus(ul_di7* T, REFGUID g){ (void)T;(void)g; return DI_OK; }
static HRESULT STDMETHODCALLTYPE i7_RunControlPanel(ul_di7* T, HWND w, DWORD fl){ (void)T;(void)w;(void)fl; return DI_OK; }
static HRESULT STDMETHODCALLTYPE i7_Initialize(ul_di7* T, HINSTANCE h, DWORD v){ (void)T;(void)h;(void)v; return DI_OK; }
static HRESULT STDMETHODCALLTYPE i7_FindDevice(ul_di7* T, REFGUID g, LPCSTR n, LPGUID out){ (void)T;(void)g;(void)n;(void)out; return DI_OK; }

static const ul_di7_vtbl g_di7_vtbl = {
    i7_QueryInterface, i7_AddRef, i7_Release, i7_CreateDevice, i7_EnumDevices,
    i7_GetDeviceStatus, i7_RunControlPanel, i7_Initialize, i7_FindDevice,
    i7_CreateDeviceEx
};

static HRESULT make_di7(void** out, const char* who) {
    if (!out) return E_POINTER;
    ul_di7* f = (ul_di7*)calloc(1, sizeof(*f));
    if (!f) return E_OUTOFMEMORY;
    f->lpVtbl = &g_di7_vtbl; f->ref = 1;
    *out = f;
    ultra_txp_log("DINPUT7: %s -> synthetic IDirectInput7 (our emulation in use)", who);
    return DI_OK;
}

static HRESULT WINAPI Hook_DirectInputCreateEx(HINSTANCE inst, DWORD ver, REFIID riid, void** out, LPUNKNOWN unk)
{ (void)inst;(void)ver;(void)riid;(void)unk; return make_di7(out, "DirectInputCreateEx"); }
static HRESULT WINAPI Hook_DirectInputCreateA(HINSTANCE inst, DWORD ver, void** out, LPUNKNOWN unk)
{ (void)inst;(void)ver;(void)unk; return make_di7(out, "DirectInputCreateA"); }
static HRESULT WINAPI Hook_DirectInputCreateW(HINSTANCE inst, DWORD ver, void** out, LPUNKNOWN unk)
{ (void)inst;(void)ver;(void)unk; return make_di7(out, "DirectInputCreateW"); }

void ultra_dinput8_install_txp(void) {
    const char* skip = getenv("ULTRA_NO_DINPUT8");
    if (skip && skip[0] == '1') return;
    ultra_iat_patch_all_txp("dinput8.dll", "DirectInput8Create", (void*)Hook_DirectInput8Create);
    /* ...and the legacy dinput.dll entry points, which is what this board's
     * titles actually import. Patching all three costs nothing when a game
     * imports none of them. */
    ultra_iat_patch_all_txp("dinput.dll", "DirectInputCreateEx", (void*)Hook_DirectInputCreateEx);
    ultra_iat_patch_all_txp("dinput.dll", "DirectInputCreateA",  (void*)Hook_DirectInputCreateA);
    ultra_iat_patch_all_txp("dinput.dll", "DirectInputCreateW",  (void*)Hook_DirectInputCreateW);
}

#endif /* _WIN32 */
