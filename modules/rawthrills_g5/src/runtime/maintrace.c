/*
 * maintrace.c - instrument main()'s init call sequence.
 *
 * Plants an INT3 (0xCC) at each call site inside main(). When the game reaches
 * one, the VEH breakpoint handler logs which init function is about to run,
 * restores the original byte, steps past it, re-arms, and continues. The log
 * thus shows the exact last init call reached before a crash - turning
 * "somewhere in main" into "dies right after init_X". Diagnostic only.
 */

#include "compat32.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "loader.h"

typedef struct { uint32_t site; const char *name; uint8_t orig; int armed; } TracePt;

/* main()'s call sites (addr of the CALL instruction) -> function called.
 * Extracted from the ELF disassembly of main @0x08061b58. */
static TracePt s_pts[] = {
    { 0x08061ba9, "get_cpu_timer",      0, 0 },
    { 0x08061bbe, "init_debug",         0, 0 },
    { 0x08061bc3, "init_prjutil",       0, 0 },
    { 0x08061bdb, "check_option",       0, 0 },
    { 0x08061bec, "init_screen",        0, 0 },
    { 0x08061bf1, "prj_init_matrix",    0, 0 },
    { 0x08061bf6, "init_config",        0, 0 },
    { 0x08061bfb, "init_game_mode_work",0, 0 },
    { 0x08061c0a, "init_glut",          0, 0 },
    { 0x08061c0f, "init_heap_manager",  0, 0 },
    { 0x08061c14, "init_file_manager",  0, 0 },
    { 0x08061c19, "init_system_work",   0, 0 },
    { 0x08061c1e, "init_arcade",        0, 0 },
    { 0x08061c23, "init_graphics",      0, 0 },
    { 0x08061c28, "init_sprite",        0, 0 },
    { 0x08061c2d, "init_io",            0, 0 },
    { 0x08061c32, "init_switch",        0, 0 },
    { 0x08061c37, "init_sound",         0, 0 },
    { 0x08061c3c, "init_pause",         0, 0 },
    { 0x08061c41, "init_objset",        0, 0 },
    { 0x08061c46, "init_motion",        0, 0 },
    { 0x08061c5a, "init_task",          0, 0 },
    { 0x08061c6b, "init_player",        0, 0 },
    { 0x08061c73, "entry_system_task",  0, 0 },
    { 0x08061c7f, "set_vblank_callback",0, 0 },
    { 0x08061c84, "fldInit",            0, 0 },
    { 0x08061c98, "signal",             0, 0 },
    { 0x08061c9d, "glutMainLoop",       0, 0 },
    { 0x08061ca2, "dest_application",   0, 0 },
};
#define NPTS ((int)(sizeof(s_pts)/sizeof(s_pts[0])))

/* init_graphics()'s internal call sites @0x0805f7be. Tracing these tells us
 * exactly which sub-init function is running when the EIP=0 (ret-to-zero) crash
 * happens - the LAST one logged before the crash is the function whose return
 * address got clobbered. GL PLT calls are omitted except the 4
 * glProgramEnvParameter4fARB sites, kept to bracket the GL setup block. */
static TracePt s_pts_ig[] = {
    { 0x0805f7dd, "rndpass_init",                 0, 0 },
    { 0x0805f7e2, "ogl_init_static_render_texture",0, 0 },
    { 0x0805f7ee, "ogl_get_static_render_texture", 0, 0 },
    { 0x0805f7f6, "ogl_push_render_target",        0, 0 },
    { 0x0805f7fb, "init_shader",                   0, 0 },
    { 0x0805f800, "load_all_shaders",              0, 0 },
    { 0x0805f80c, "Matrix4::setIdentity#1",        0, 0 },
    { 0x0805f818, "Matrix4::setIdentity#2",        0, 0 },
    { 0x0805f954, "glProgramEnvParameter4fARB#1",  0, 0 },
    { 0x0805f978, "glProgramEnvParameter4fARB#2",  0, 0 },
    { 0x0805f99c, "glProgramEnvParameter4fARB#3",  0, 0 },
    { 0x0805f9c0, "glProgramEnvParameter4fARB#4",  0, 0 },
    { 0x0805f9c5, "init_scnenv",                   0, 0 },
    { 0x0805f9ca, "set_scene_env",                 0, 0 },
    { 0x0805f9cf, "ogl_pop_render_target",         0, 0 },
    { 0x0805f9d4, "init_texture_manager",          0, 0 },
    { 0x0805f9d9, "init_buffer_object_manager",    0, 0 },
    { 0x0805f9e6, "set_color_operation",           0, 0 },
    { 0x0805f9f2, "set_background_color",          0, 0 },
    { 0x0805f9fa, "set_vistaframe",                0, 0 },
};
#define NPTS_IG ((int)(sizeof(s_pts_ig)/sizeof(s_pts_ig[0])))

/* init_sprite() internal call sites - loads sprite sets + database from disk,
 * the most likely place to fault on a missing/misread asset. Labels them
 * "init_sprite -> X" so the log shows the last sub-call reached. */
static TracePt s_pts_is[] = {
    { 0x08076359, "load_sprset#1",          0, 0 },
    { 0x08076365, "load_sprset#2",          0, 0 },
    { 0x08076371, "load_sprset#3",          0, 0 },
    { 0x0807637d, "prj_init_database",      0, 0 },
    { 0x08076398, "init_aetctrl",           0, 0 },
    { 0x080763c9, "ogl_push_render_target", 0, 0 },
    { 0x080763fe, "ogl_pop_render_target",  0, 0 },
};
#define NPTS_IS ((int)(sizeof(s_pts_is)/sizeof(s_pts_is[0])))


/* --- hardware-watchpoint probe ---------------------------------------------
 * The EIP=0 crash is a ret-to-zero: something writes 0 over
 * ogl_init_static_render_texture's return-address slot while it runs. To catch
 * the exact culprit, plant an INT3 at ogl_init's ENTRY; when it fires, esp
 * points at the return slot, so we arm a CPU debug-register write-watch (DR0)
 * on that dword. The next write to it raises a single-step exception whose EIP
 * is the offending instruction - which we log, then disarm and let it run on
 * to the (now expected) crash. */
#define OGL_INIT_ENTRY 0x080ea9aeu
static TracePt s_ogl_entry = { OGL_INIT_ENTRY, "ogl_init_entry", 0, 0 };
static uint32_t g_watch_slot  = 0;
static int      g_watch_armed = 0;
static uint32_t g_ogl_ret_slot = 0;   /* address of ogl_init's return slot */

/* ogl_init_static_render_texture's internal call sites. At each, we log the
 * site AND read ogl_init's return slot: the first site that reports the slot
 * already CLOBBERED means the corruption happened in the callee just before
 * it. This pinpoints the culprit with nothing but logging - no debugger. */
static TracePt s_pts_ogl[] = {
    { 0x080eaa08, "FBuffer::init",            0, 0 },
    { 0x080eaa4d, "FBuffer::create",          0, 0 },
    { 0x080eaa76, "glutExtensionSupported",   0, 0 },
    { 0x080eab02, "glGenProgramsARB#1",       0, 0 },
    { 0x080eab47, "glProgramStringARB#1",     0, 0 },
    { 0x080eab7c, "glGenProgramsARB#2",       0, 0 },
    { 0x080eabc3, "glProgramStringARB#2",     0, 0 },
    { 0x080eabf6, "glGenProgramsARB#3",       0, 0 },
    { 0x080eac38, "glProgramStringARB#3",     0, 0 },
    { 0x080eac6b, "glGenProgramsARB#4",       0, 0 },
    { 0x080eacad, "glProgramStringARB#4",     0, 0 },
    { 0x080eacdb, "glGenProgramsARB#5",       0, 0 },
    { 0x080ead22, "glProgramStringARB#5",     0, 0 },
    { 0x080ead55, "glGenProgramsARB#6",       0, 0 },
    { 0x080ead97, "glProgramStringARB#6",     0, 0 },
    { 0x080eadb4, "glGetString(post-ARB)",    0, 0 },
};
#define NPTS_OGL ((int)(sizeof(s_pts_ogl)/sizeof(s_pts_ogl[0])))

/* --- calc_camera_matrix ESP tracer (gated by env ULTRA_TRACE_CALC=1) --------
 * The camera task dies with a deterministic ret-to-0 caused by ESP ending ~0xD8
 * too HIGH inside calc_camera_matrix (0x08056a6a). To find the exact call that
 * desyncs ESP, plant an INT3 at the function entry and at every internal call
 * site, log esp/ebp at each, and RE-ARM after stepping over (calc runs once per
 * camera, so we must re-arm to catch the specific camera whose call desyncs).
 * The site after which esp jumps unexpectedly is the culprit. */
static TracePt s_pts_calc[] = {
    { 0x08056a6a, "entry",          0, 0 },
    { 0x08056a7c, "setIdentity",    0, 0 },
    { 0x08056a91, "mul_rotate_z",   0, 0 },
    { 0x08056aa6, "mul_rotate_x",   0, 0 },
    { 0x08056abb, "mul_rotate_y",   0, 0 },
    { 0x08056aed, "mul_translate",  0, 0 },
    { 0x08056b5a, "invert",         0, 0 },
    { 0x08056b81, "transform",      0, 0 },
    { 0x08056c33, "glMatrixMode",   0, 0 },
    { 0x08056c38, "glPushMatrix",   0, 0 },
    { 0x08056c3d, "glLoadIdentity", 0, 0 },
    { 0x08056c79, "glFrustum",      0, 0 },
    { 0x08056c87, "ref",            0, 0 },
    { 0x08056c97, "glGetFloatv",    0, 0 },
    { 0x08056c9c, "glPopMatrix",    0, 0 },
    { 0x08056ca4, "gl_to_vm",       0, 0 },
};
#define NPTS_CALC ((int)(sizeof(s_pts_calc)/sizeof(s_pts_calc[0])))
static int       s_calc_trace_on  = 0;
static int       s_calc_invocation = -1;
static TracePt  *s_calc_pending   = 0;   /* call site to re-arm after the TF step */

/* --- screen-size guard ------------------------------------------------------
 * The game's screen dimensions live in two .bss floats:
 *   0x09657950 = screen_width, 0x09657954 = screen_height
 * init_screen() writes them from a clamped, pristine resolution table, so the
 * game itself can only ever store a sane value (320..1600). But init_graphics()
 * -> ogl_init_static_render_texture() reads them back as the render-texture
 * size, and by then they can be garbage (e.g. 34037 x 157350044) because a
 * stray write corrupted them - silently, since our substrate maps .bss/.rodata
 * RWX (on real Linux that write would have faulted at its source).
 *
 * We snapshot the floats at the init_screen call site (before it runs) and at
 * the init_graphics call site (after init_screen, before the texture is built).
 * If they're implausible at the init_graphics site, we repair them to the
 * game's intended window size so the render texture is created correctly and
 * the garbage can't propagate into the FBuffer object. */
#define SCR_W_ADDR 0x09657950u
#define SCR_H_ADDR 0x09657954u
#define SITE_INIT_SCREEN   0x08061becu
#define SITE_INIT_GRAPHICS 0x08061c23u
/* The game's intended screen size (glutInitWindowSize / init_screen mode 1). */
#define SCR_FIX_W 640
#define SCR_FIX_H 480

static void maintrace_screen_guard(uint32_t site)
{
    if (site != SITE_INIT_SCREEN && site != SITE_INIT_GRAPHICS) return;
    if (IsBadReadPtr((void*)(uintptr_t)SCR_W_ADDR, 8)) return;
    volatile float *fw = (volatile float*)(uintptr_t)SCR_W_ADDR;
    volatile float *fh = (volatile float*)(uintptr_t)SCR_H_ADDR;
    float w = *fw, h = *fh;
    loader_log(LOG_INFO, "[MAINTRACE]   screen_size @%s = %.1f x %.1f (raw 0x%08x 0x%08x)",
               (site == SITE_INIT_SCREEN ? "pre-init_screen" : "pre-init_graphics"),
               (double)w, (double)h,
               *(volatile uint32_t*)(uintptr_t)SCR_W_ADDR,
               *(volatile uint32_t*)(uintptr_t)SCR_H_ADDR);
    fflush(stdout);

    if (site != SITE_INIT_GRAPHICS) return;
    /* Pin the game's projection/render resolution at init (before the render
     * texture is built). Default = the game's native 640x480; ULTRA_MAINSCR_W/H
     * override it so we can test letting the game render at a corrected size
     * (the UI is hardcoded to 640-wide, so widening strands it in a corner -
     * documented for the record). */
    {
        int fixw = SCR_FIX_W, fixh = SCR_FIX_H;
        const char *ew = getenv("ULTRA_MAINSCR_W"); if (ew && *ew) fixw = atoi(ew);
        const char *eh = getenv("ULTRA_MAINSCR_H"); if (eh && *eh) fixh = atoi(eh);
        DWORD old;
        if (VirtualProtect((void*)(uintptr_t)SCR_W_ADDR, 8, PAGE_EXECUTE_READWRITE, &old)) {
            *fw = (float)fixw;
            *fh = (float)fixh;
            VirtualProtect((void*)(uintptr_t)SCR_W_ADDR, 8, old, &old);
            loader_log(LOG_WARN, "[MAINTRACE]   screen_size pinned to %d x %d (was %.0fx%.0f from init_screen)",
                       fixw, fixh, (double)w, (double)h);
        }
    }
}

/* --- 3D scene render-texture upscale (the "gameplay 480p" fix) --------------
 * The game's 3D scene renders into FBuffer "static render textures" sized from
 * get_screen_size (= 640x480), then composited fullscreen - that 640x480 buffer
 * is the soft 3D even at 4K. ogl_init_static_render_texture (called once, between
 * call sites 0x0805f7e2 and 0x0805f7ee) builds them and stores their W/H in the
 * FBuffer objects; FBuffer::begin_render derives glViewport from that stored W/H
 * and the composite samples by the texture's own size. So if we temporarily scale
 * get_screen_size UP only for the duration of that init, every full-size scene
 * target + its >>N bloom mips are built large and self-consistent (viewport +
 * composite texcoords follow the stored size), while the 640-hardcoded 2D UI -
 * which reads get_screen_size LATER - still sees the native 640 and doesn't corner.
 * ULTRA_SCENE_SCALE (default 1 = off; e.g. 2 -> 1280x960 scene). */
/* Set by settings_apply from the output resolution (ceil(out_h/480), 1..4). A
 * GLOBAL, not the env: settings_apply sets the scale via SetEnvironmentVariableA,
 * which the mingw CRT getenv() snapshot does NOT observe - so reading it back via
 * getenv here always saw "unset" and the scene stayed 640x480. */
int g_ul_scene_scale = 0;
static void maintrace_fbuffer_scale(uint32_t site)
{
    static float saved_w = 0, saved_h = 0; static int active = 0;
    static float scl = -1.0f;
    if (scl < 0.0f) {
        const char *e = getenv("ULTRA_SCENE_SCALE");     /* explicit env override wins */
        if (e && *e) scl = (float)atof(e);
        else scl = (g_ul_scene_scale > 0) ? (float)g_ul_scene_scale : 1.0f;
    }
    if (scl <= 1.0f) return;
    volatile float *fw = (volatile float*)(uintptr_t)SCR_W_ADDR;
    volatile float *fh = (volatile float*)(uintptr_t)SCR_H_ADDR;
    if (IsBadReadPtr((void*)(uintptr_t)SCR_W_ADDR, 8)) return;
    DWORD old;
    if (site == 0x0805f7e2u && !active) {            /* before ogl_init_static_render_texture */
        if (*fw < 64.0f || *fw*scl > 8192.0f) return;
        if (VirtualProtect((void*)(uintptr_t)SCR_W_ADDR, 8, PAGE_EXECUTE_READWRITE, &old)) {
            saved_w = *fw; saved_h = *fh;
            *fw = saved_w * scl; *fh = saved_h * scl;
            VirtualProtect((void*)(uintptr_t)SCR_W_ADDR, 8, old, &old);
            active = 1;
            loader_log(LOG_WARN, "[FBSCALE] scene render-textures x%.2f (%.0fx%.0f -> %.0fx%.0f) for FBuffer init",
                       scl, saved_w, saved_h, saved_w*scl, saved_h*scl);
        }
    } else if (site == 0x0805f7eeu && active) {      /* after ogl_init_static_render_texture */
        if (VirtualProtect((void*)(uintptr_t)SCR_W_ADDR, 8, PAGE_EXECUTE_READWRITE, &old)) {
            *fw = saved_w; *fh = saved_h;
            VirtualProtect((void*)(uintptr_t)SCR_W_ADDR, 8, old, &old);
            active = 0;
            loader_log(LOG_WARN, "[FBSCALE] screen_size restored to %.0fx%.0f (2D UI native)", saved_w, saved_h);
        }
    }
}

static TracePt *s_pending = 0;   /* point we just stepped over, to re-arm   */

static int arm_one(TracePt *t)
{
    DWORD old;
    if (IsBadReadPtr((void*)(uintptr_t)t->site, 1)) return 0;
    /* Opcode guard: EVERY probe site below is an After Burner Climax (abc)
     * `call rel32` instruction, whose first byte is 0xE8. On a DIFFERENT title
     * the same absolute address lands on unrelated bytes (e.g. Ghost Squad
     * Evolution's init_io_usb spans these addresses), and planting 0xCC there
     * shreds a real instruction mid-stream -> illegal-instruction / wild jump.
     * Only plant when the byte we are about to save is actually a CALL opcode:
     * for abc this is always true (identical behavior), for any other binary it
     * skips, so the tracer can never corrupt a non-abc title's code. */
    uint8_t op = *(volatile uint8_t*)(uintptr_t)t->site;
    if (op != 0xE8) return 0;
    if (!VirtualProtect((void*)(uintptr_t)t->site, 1, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t->orig = op;
    *(volatile uint8_t*)(uintptr_t)t->site = 0xCC;   /* INT3 */
    VirtualProtect((void*)(uintptr_t)t->site, 1, old, &old);
    t->armed = 1;
    return 1;
}

/* Plant breakpoints at all of main's call sites. Call right before main runs. */
void maintrace_arm(void)
{
    /* ALL of the INT3 probe sites below are HARDCODED After Burner Climax (abc)
     * addresses (main()/init_graphics()/init_sprite()/ogl_init()/calc_camera).
     * They are a pure diagnostic tracer (they only LOG which init function the
     * game reaches) and play no functional role. On a DIFFERENT title those same
     * absolute addresses point at completely unrelated code: e.g. Ghost Squad
     * Evolution's init_io_usb() spans 0x080ea950..0x080eab.., so the abc probes
     * 0x080ea9ae (ogl_init_entry) and 0x080eaa08 (FBuffer::init) land MID-
     * INSTRUCTION inside init_io_usb and overwrite a byte with 0xCC - corrupting
     * `movl $0x800,0x8(%esp)` into an illegal `c7 cc ..` and turning the device
     * loop into a wild jump to the zero page. So this tracer must NEVER arm for a
     * non-abc binary. Default OFF; opt in with ULTRA_MAINTRACE=1 (only meaningful
     * when running abc). This makes the loader safe for every other title while
     * leaving the abc diagnostic available on demand. */
    { const char *e = getenv("ULTRA_MAINTRACE");
      if (!e || !atoi(e)) {
          loader_log(LOG_INFO, "[MAINTRACE] disabled (abc-only diagnostic; set ULTRA_MAINTRACE=1 to arm)");
          return;
      } }

    /* Software INT3 breakpoints fight an external debugger (x32dbg sees our
     * 0xCC bytes as its own breakpoints and stops on every one). When a
     * debugger is attached, stand down entirely and let it drive - you'll set
     * real breakpoints there instead. Run once WITHOUT a debugger to get the
     * init_graphics->X trace that names the culprit, then attach. */
    if (IsDebuggerPresent()) {
        loader_log(LOG_INFO, "[MAINTRACE] debugger present, INT3 tracing disabled (use the debugger)");
        return;
    }
    int n = 0;
    for (int i = 0; i < NPTS; i++) if (arm_one(&s_pts[i])) n++;
    loader_log(LOG_INFO, "[MAINTRACE] armed %d/%d call-site breakpoints in main()", n, NPTS);
    int m = 0;
    for (int i = 0; i < NPTS_IG; i++) if (arm_one(&s_pts_ig[i])) m++;
    loader_log(LOG_INFO, "[MAINTRACE] armed %d/%d call-site breakpoints in init_graphics()", m, NPTS_IG);
    int is = 0;
    for (int i = 0; i < NPTS_IS; i++) if (arm_one(&s_pts_is[i])) is++;
    loader_log(LOG_INFO, "[MAINTRACE] armed %d/%d call-site breakpoints in init_sprite()", is, NPTS_IS);
    if (arm_one(&s_ogl_entry))
        loader_log(LOG_INFO, "[MAINTRACE] armed ogl_init entry watch-probe @0x%08x", OGL_INIT_ENTRY);
    int k = 0;
    for (int i = 0; i < NPTS_OGL; i++) if (arm_one(&s_pts_ogl[i])) k++;
    loader_log(LOG_INFO, "[MAINTRACE] armed %d/%d slot-check points in ogl_init()", k, NPTS_OGL);
    if (getenv("ULTRA_TRACE_CALC")) {
        s_calc_trace_on = 1;
        int c = 0;
        for (int i = 0; i < NPTS_CALC; i++) if (arm_one(&s_pts_calc[i])) c++;
        loader_log(LOG_INFO, "[MAINTRACE] armed %d/%d calc_camera_matrix esp-trace points (ULTRA_TRACE_CALC)", c, NPTS_CALC);
    }
}

/* Called from the VEH handler on EXCEPTION_BREAKPOINT.
 * Returns 1 if it handled the breakpoint and set ctx to resume. */
int maintrace_on_breakpoint(CONTEXT *ctx)
{
    uint32_t eip = (uint32_t)ctx->Rip;
    /* INT3 may leave EIP at the site or site+1 depending on the handler. */
    for (int i = 0; i < NPTS; i++) {
        if ((eip - 1 == s_pts[i].site || eip == s_pts[i].site) && s_pts[i].armed) {
            loader_log(LOG_INFO, "[MAINTRACE] main -> %s (0x%08x)", s_pts[i].name, s_pts[i].site);
            /* Bisect probe: track the first tag_SpriteBuffer list (0x0956bc30).
             * At load it is .bss zero; whichever main-> step first shows a
             * non-self _M_next is where the lists got populated/corrupted. */
            {
                uint32_t lp = 0, mn = 0;
                if (!IsBadReadPtr((void*)0x0956bc30, 4)) {
                    lp = *(uint32_t*)0x0956bc30;
                    if (lp >= 0x10000 && lp < 0xc0000000 && !IsBadReadPtr((void*)(uintptr_t)lp, 4))
                        mn = *(uint32_t*)(uintptr_t)lp;
                }
                loader_log(LOG_INFO, "    [SPRPROBE] list@0x0956bc30=0x%08x sentinel._M_next=0x%08x %s",
                           lp, mn, (lp && mn == lp) ? "(empty)" : (lp ? "(NON-EMPTY)" : "(zero)"));
            }
            fflush(stdout);
            /* Snapshot/repair the screen-size floats at the init_screen and
             * init_graphics call sites (see maintrace_screen_guard above). */
            maintrace_screen_guard(s_pts[i].site);
            /* Restore the original byte and DO NOT re-arm. Each call site only
             * needs logging once (main's init sequence runs once), so we avoid
             * the fragile single-step/re-arm dance entirely: just put the real
             * instruction back, point EIP at it, and resume. */
            DWORD old;
            VirtualProtect((void*)(uintptr_t)s_pts[i].site, 1, PAGE_EXECUTE_READWRITE, &old);
            *(volatile uint8_t*)(uintptr_t)s_pts[i].site = s_pts[i].orig;
            VirtualProtect((void*)(uintptr_t)s_pts[i].site, 1, old, &old);
            s_pts[i].armed = 0;
            ctx->Rip = s_pts[i].site;   /* resume at the real instruction */
            return 1;
        }
    }
    /* init_graphics() internal call sites - label them "init_graphics -> X". */
    for (int i = 0; i < NPTS_IG; i++) {
        if ((eip - 1 == s_pts_ig[i].site || eip == s_pts_ig[i].site) && s_pts_ig[i].armed) {
            loader_log(LOG_INFO, "[MAINTRACE] init_graphics -> %s (0x%08x)", s_pts_ig[i].name, s_pts_ig[i].site);
            /* Scale the scene render-textures up around ogl_init_static_render_texture. */
            maintrace_fbuffer_scale(s_pts_ig[i].site);
            fflush(stdout);
            DWORD old;
            VirtualProtect((void*)(uintptr_t)s_pts_ig[i].site, 1, PAGE_EXECUTE_READWRITE, &old);
            *(volatile uint8_t*)(uintptr_t)s_pts_ig[i].site = s_pts_ig[i].orig;
            VirtualProtect((void*)(uintptr_t)s_pts_ig[i].site, 1, old, &old);
            s_pts_ig[i].armed = 0;
            ctx->Rip = s_pts_ig[i].site;
            return 1;
        }
    }
    /* init_sprite() internal call sites - label them "init_sprite -> X". */
    for (int i = 0; i < NPTS_IS; i++) {
        if ((eip - 1 == s_pts_is[i].site || eip == s_pts_is[i].site) && s_pts_is[i].armed) {
            loader_log(LOG_INFO, "[MAINTRACE] init_sprite -> %s (0x%08x)", s_pts_is[i].name, s_pts_is[i].site);
            fflush(stdout);
            DWORD old;
            VirtualProtect((void*)(uintptr_t)s_pts_is[i].site, 1, PAGE_EXECUTE_READWRITE, &old);
            *(volatile uint8_t*)(uintptr_t)s_pts_is[i].site = s_pts_is[i].orig;
            VirtualProtect((void*)(uintptr_t)s_pts_is[i].site, 1, old, &old);
            s_pts_is[i].armed = 0;
            ctx->Rip = s_pts_is[i].site;
            return 1;
        }
    }
    /* ogl_init entry. NOTE: self-set debug registers (DR0/DR7) do not work from
     * an in-process VEH under WOW64 - they wedge the thread without delivering
     * the trap (confirmed twice). So we DON'T arm a hardware watch here; use an
     * external debugger (x32dbg: bp 080EA9AE; bphws esp,w,4) to catch the
     * write. We still log entry for confirmation. */
    if ((eip - 1 == OGL_INIT_ENTRY || eip == OGL_INIT_ENTRY) && s_ogl_entry.armed) {
        uint32_t slot = (uint32_t)ctx->Rsp;
        uint32_t ret  = 0;
        if (!IsBadReadPtr((void*)(uintptr_t)slot, 4)) ret = *(uint32_t*)(uintptr_t)slot;
        DWORD old;
        VirtualProtect((void*)(uintptr_t)OGL_INIT_ENTRY, 1, PAGE_EXECUTE_READWRITE, &old);
        *(volatile uint8_t*)(uintptr_t)OGL_INIT_ENTRY = s_ogl_entry.orig;
        VirtualProtect((void*)(uintptr_t)OGL_INIT_ENTRY, 1, old, &old);
        s_ogl_entry.armed = 0;
        ctx->Rip = OGL_INIT_ENTRY;
        g_ogl_ret_slot = slot;
        loader_log(LOG_INFO, "[OGL] ogl_init entered; return slot 0x%08x holds 0x%08x", slot, ret);
        fflush(stdout);
        return 1;
    }
    /* ogl_init internal checkpoints: log the slot value at each. The first one
     * showing the slot clobbered => the previous callee did it. */
    for (int i = 0; i < NPTS_OGL; i++) {
        if ((eip - 1 == s_pts_ogl[i].site || eip == s_pts_ogl[i].site) && s_pts_ogl[i].armed) {
            uint32_t v = 0xFFFFFFFFu;
            if (g_ogl_ret_slot && !IsBadReadPtr((void*)(uintptr_t)g_ogl_ret_slot, 4))
                v = *(uint32_t*)(uintptr_t)g_ogl_ret_slot;
            const char *state = (v == 0x0805f7e7u) ? "intact" : "*** CLOBBERED ***";
            loader_log(LOG_INFO, "[OGL] before %-24s slot[0x%08x]=0x%08x %s",
                       s_pts_ogl[i].name, g_ogl_ret_slot, v, state);
            fflush(stdout);
            DWORD old;
            VirtualProtect((void*)(uintptr_t)s_pts_ogl[i].site, 1, PAGE_EXECUTE_READWRITE, &old);
            *(volatile uint8_t*)(uintptr_t)s_pts_ogl[i].site = s_pts_ogl[i].orig;
            VirtualProtect((void*)(uintptr_t)s_pts_ogl[i].site, 1, old, &old);
            s_pts_ogl[i].armed = 0;
            ctx->Rip = s_pts_ogl[i].site;
            return 1;
        }
    }
    /* calc_camera_matrix esp trace: log esp/ebp at the entry and each internal
     * call site, then single-step over and RE-ARM so every per-camera call is
     * traced. The site after which esp jumps by the un-cleaned amount is the
     * desync source. */
    if (s_calc_trace_on) {
        for (int i = 0; i < NPTS_CALC; i++) {
            if ((eip - 1 == s_pts_calc[i].site || eip == s_pts_calc[i].site) && s_pts_calc[i].armed) {
                if (i == 0) s_calc_invocation++;   /* entry site bumps the invocation # */
                loader_log(LOG_ERROR, "[CALCTRACE] calc#%d %-14s esp=0x%08x ebp=0x%08x",
                           s_calc_invocation, s_pts_calc[i].name,
                           (uint32_t)ctx->Rsp, (uint32_t)ctx->Rbp);
                fflush(stdout);
                DWORD old;
                VirtualProtect((void*)(uintptr_t)s_pts_calc[i].site, 1, PAGE_EXECUTE_READWRITE, &old);
                *(volatile uint8_t*)(uintptr_t)s_pts_calc[i].site = s_pts_calc[i].orig;
                VirtualProtect((void*)(uintptr_t)s_pts_calc[i].site, 1, old, &old);
                s_pts_calc[i].armed = 0;
                ctx->Rip = s_pts_calc[i].site;     /* re-execute the real instruction */
                ctx->EFlags |= 0x100u;             /* trap flag: single-step it */
                s_calc_pending = &s_pts_calc[i];   /* re-arm after the step */
                return 1;
            }
        }
    }
    return 0;
}

/* Fires on EXCEPTION_SINGLE_STEP. If our DR0 write-watch tripped, the slot was
 * just written - ctx->Rip is at (or one instruction past) the culprit. Log it
 * with surrounding bytes so we can identify the instruction, then disarm. */
int maintrace_on_singlestep(CONTEXT *ctx)
{
    /* Re-arm the calc_camera_matrix trace point we just stepped over. */
    if (s_calc_pending) {
        ctx->EFlags &= ~0x100u;          /* clear trap flag */
        arm_one(s_calc_pending);          /* re-plant the INT3 */
        s_calc_pending = 0;
        return 1;
    }
    if (g_watch_armed) {
        /* First single-step after arming the watch == the write that tripped
         * DR0 (no trap flag is set otherwise). Log unconditionally and disarm,
         * since WOW64 may not surface the Dr6 status bits in this context. */
        uint32_t eip = (uint32_t)ctx->Rip;
        uint32_t now = 0;
        if (!IsBadReadPtr((void*)(uintptr_t)g_watch_slot, 4)) now = *(uint32_t*)(uintptr_t)g_watch_slot;
        loader_log(LOG_ERROR, "[WATCH] *** single-step (Dr6=0x%08x) slot 0x%08x now=0x%08x ***",
                   (uint32_t)ctx->Dr6, g_watch_slot, now);
        loader_log(LOG_ERROR, "[WATCH]   culprit at/just-after EIP=0x%08x", eip);
        if (eip >= 16 && !IsBadReadPtr((void*)(uintptr_t)(eip - 16), 32)) {
            uint8_t *p = (uint8_t*)(uintptr_t)(eip - 16);
            char line[160]; int n = 0;
            for (int i = 0; i < 32 && n < (int)sizeof(line) - 4; i++)
                n += snprintf(line + n, sizeof(line) - n, "%02x ", p[i]);
            loader_log(LOG_ERROR, "[WATCH]   bytes[EIP-16..+15]: %s", line);
        }
        loader_log(LOG_ERROR, "[WATCH]   eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x esp=%08x ebp=%08x",
                   (uint32_t)ctx->Rax, (uint32_t)ctx->Rcx, (uint32_t)ctx->Rdx, (uint32_t)ctx->Rbx,
                   (uint32_t)ctx->Rsi, (uint32_t)ctx->Rdi, (uint32_t)ctx->Rsp, (uint32_t)ctx->Rbp);
        fflush(stdout);
        ctx->Dr0 = 0; ctx->Dr7 = 0; ctx->Dr6 = 0;
        ctx->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        g_watch_armed = 0;
        return 1;
    }
    return 0;
}
#if 0
int maintrace_on_breakpoint_OLD(CONTEXT *ctx)
{
        /* clear trap flag */
        ctx->EFlags &= ~0x100u;
        arm_one(s_pending);
        s_pending = 0;
        /* fall through: this same exception might be a real BP too, but the
         * single-step trap is delivered as EXCEPTION_SINGLE_STEP, handled
         * below - so here we just return to continue. */
    }

    uint32_t eip = (uint32_t)ctx->Rip;
    /* INT3 normally leaves EIP at site+1, but some handlers observe EIP at the
     * site itself. Match either. */
    for (int i = 0; i < NPTS; i++) {
        if ((eip - 1 == s_pts[i].site || eip == s_pts[i].site) && s_pts[i].armed) {
            loader_log(LOG_INFO, "[MAINTRACE] main -> %s (0x%08x)", s_pts[i].name, s_pts[i].site);
            fflush(stdout);
            /* restore original byte, back EIP up to it, single-step over it */
            DWORD old;
            VirtualProtect((void*)(uintptr_t)s_pts[i].site, 1, PAGE_EXECUTE_READWRITE, &old);
            *(volatile uint8_t*)(uintptr_t)s_pts[i].site = s_pts[i].orig;
            VirtualProtect((void*)(uintptr_t)s_pts[i].site, 1, old, &old);
            s_pts[i].armed = 0;
            ctx->Rip      = s_pts[i].site;   /* re-execute the real instruction */
            ctx->EFlags  |= 0x100u;          /* set trap flag: single-step it    */
            s_pending     = &s_pts[i];       /* re-arm after the step            */
            return 1;
        }
    }
    return 0;
}

/* Called from the VEH handler on EXCEPTION_SINGLE_STEP: just re-arm pending. */
int maintrace_on_singlestep_OLD(CONTEXT *ctx)
{
    if (s_pending) {
        ctx->EFlags &= ~0x100u;
        arm_one(s_pending);
        s_pending = 0;
        return 1;
    }
    return 0;
}
#endif
