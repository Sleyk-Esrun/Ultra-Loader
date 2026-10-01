/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  test_gl_capture.c -- synthetic OpenGL present app that proves the gl_present.c
 *  capture path works end-to-end WITHOUT a real arcade dump.
 * =============================================================================
 *
 *  We can't run a Namco N2 title (no dump). But the capture path -- the
 *  wglSwapBuffers hook -> glReadPixels(GL_BACK, GL_BGRA) -> 24-bit bottom-up BMP --
 *  is API-generic, so a tiny synthetic GL title exercises every line of it:
 *
 *    1. Create a real GL context on a hidden HWND (PIXELFORMATDESCRIPTOR +
 *       wglCreateContext + wglMakeCurrent).
 *    2. glClear the back buffer to a known color.
 *    3. Call the loader's OWN ultra_Hook_wglSwapBuffers(hdc) -- the canonical real
 *       hook the IAT/GetProcAddress route lands on. With ULTRA_PRESENT_DUMP set and
 *       ULTRA_DUMP_EVERY=1 it glReadPixels the back buffer and writes a BMP, then
 *       forwards to the real wglSwapBuffers.
 *    4. Assert the BMP exists, is the right size, and a sampled pixel matches the
 *       clear color -- proving the GL->disk round-trip captured the frame.
 *
 *  Needs a real GL context (a window-station GL is available on a normal desktop
 *  session; a headless CI box without one skips cleanly with a non-fatal message,
 *  exactly like the d3d11 WARP test). Exit 0 = capture verified (or cleanly skipped
 *  when no GL context can be created); non-zero = a real failure.
 * =============================================================================
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* the canonical real hook from gl_present.c (loose-typed extern matches the ABI) */
extern BOOL __stdcall ultra_Hook_wglSwapBuffers(HDC hdc);

/* the logger gl_present.c links against (env-gated; silent unless ULTRA_JVS_LOG) */
void ultra_d3d8_log(const char* fmt, ...) {
    const char* p = getenv("ULTRA_JVS_LOG"); if (!p || !*p) return;
    FILE* f = fopen(p, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
/* the IAT patcher gl_present.c's install path calls -- a no-op for the standalone
 * test (we drive the swap hook directly, no game IAT to walk). */
void ultra_iat_patch_all(const char* a, const char* b, void* c) { (void)a;(void)b;(void)c; }

#define W 256
#define H 128

int main(void) {
    char tmp[MAX_PATH]; GetTempPathA(sizeof tmp, tmp);
    char dir[MAX_PATH + 32]; snprintf(dir, sizeof dir, "%sul_gl_cap", tmp);
    CreateDirectoryA(dir, NULL);
    SetEnvironmentVariableA("ULTRA_PRESENT_DUMP", dir);
    SetEnvironmentVariableA("ULTRA_DUMP_EVERY", "1");
    _putenv_s("ULTRA_PRESENT_DUMP", dir);
    _putenv_s("ULTRA_DUMP_EVERY", "1");

    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ul_gl_test"; wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    /* size the window so its CLIENT area (= the GL drawable the dumper captures via
     * GetClientRect) is exactly W x H; the frame adds title bar + borders. */
    RECT want = { 0, 0, W, H };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&want, style, FALSE);
    HWND hwnd = CreateWindowA("ul_gl_test", "ul", style,
                              0, 0, want.right - want.left, want.bottom - want.top,
                              NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("SKIP: CreateWindow failed (no window station)\n"); return 0; }

    HDC hdc = GetDC(hwnd);
    if (!hdc) { printf("SKIP: GetDC failed\n"); DestroyWindow(hwnd); return 0; }

    PIXELFORMATDESCRIPTOR pfd; memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf || !SetPixelFormat(hdc, pf, &pfd)) {
        printf("SKIP: no usable pixel format (headless GL unavailable)\n");
        ReleaseDC(hwnd, hdc); DestroyWindow(hwnd); return 0;
    }
    HGLRC rc = wglCreateContext(hdc);
    if (!rc || !wglMakeCurrent(hdc, rc)) {
        printf("SKIP: wglCreateContext/MakeCurrent failed (no GL context on this host)\n");
        if (rc) wglDeleteContext(rc);
        ReleaseDC(hwnd, hdc); DestroyWindow(hwnd); return 0;
    }

    /* fix the viewport to our drawable so glReadPixels (and the dumper's GL_VIEWPORT
     * fallback) see the right size, then clear the back buffer to a known color
     * (R=0.20 G=0.40 B=0.80) to match the d3d11 test's tolerance check. */
    glViewport(0, 0, W, H);
    glClearColor(0.20f, 0.40f, 0.80f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    /* drive the loader's OWN swap hook: dump (every=1) then forward the real swap */
    ultra_Hook_wglSwapBuffers(hdc);

    /* the hook names the file "<dir>/gl_0001.bmp" (present count == 1) */
    char bmp[MAX_PATH + 64]; snprintf(bmp, sizeof bmp, "%s/gl_0001.bmp", dir);
    FILE* f = fopen(bmp, "rb");
    if (!f) { printf("FAIL: no BMP at %s (capture did not fire)\n", bmp); goto fail; }
    unsigned char fh[14], ih[40];
    if (fread(fh,1,14,f) != 14 || fread(ih,1,40,f) != 40) { printf("FAIL: BMP header short\n"); fclose(f); goto fail; }
    int bw = *(int*)(ih+4), bh = *(int*)(ih+8);
    unsigned short bpp = *(unsigned short*)(ih+14);
    if (fh[0] != 'B' || fh[1] != 'M' || bw != W || bh != H || bpp != 24) {
        printf("FAIL: BMP geometry %dx%d bpp=%u (want %dx%d 24)\n", bw, bh, bpp, W, H); fclose(f); goto fail;
    }
    /* The dumper writes GL rows in natural order (row 0 = image bottom) into a
     * bottom-up BMP, so file row 0 == GL row 0 == image bottom. A flat clear is
     * uniform, so any row's pixel is the clear color; sample the first data row. */
    int rowsz = W*3, pad = (4-(rowsz&3))&3, stride = rowsz+pad;
    fseek(f, 54, SEEK_SET);
    unsigned char px[3] = {0,0,0};
    if (fread(px,1,3,f) != 3) { printf("FAIL: BMP pixel read\n"); fclose(f); goto fail; }
    fclose(f);
    (void)stride;
    int eb = (int)(0.80f*255+0.5f), eg = (int)(0.40f*255+0.5f), er = (int)(0.20f*255+0.5f);
    int db = abs((int)px[0]-eb), dg = abs((int)px[1]-eg), dr = abs((int)px[2]-er);
    if (db > 12 || dg > 12 || dr > 12) {
        printf("FAIL: pixel BGR=(%u,%u,%u) want ~(%d,%d,%d)\n", px[0],px[1],px[2], eb,eg,er);
        goto fail;
    }

    wglMakeCurrent(NULL, NULL); wglDeleteContext(rc);
    ReleaseDC(hwnd, hdc); DestroyWindow(hwnd);
    printf("PASS: gl capture %dx%d 24bpp, BGR=(%u,%u,%u) == clear color\n",
           bw, bh, px[0], px[1], px[2]);
    return 0;

fail:
    wglMakeCurrent(NULL, NULL); if (rc) wglDeleteContext(rc);
    ReleaseDC(hwnd, hdc); DestroyWindow(hwnd);
    return 1;
}
