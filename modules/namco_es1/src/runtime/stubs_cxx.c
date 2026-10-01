/*
 * stubs_cxx.c
 * C++ Runtime and Missing libc Stubs
 *
 * After Burner Climax is linked against libstdc++.so.5 (GCC 3.x ABI).
 * We need:
 *   - new/delete operators
 *   - C++ exception runtime (__cxa_*)
 *   - std::string (std::basic_string) methods (_ZNSs*)
 *   - std::ios_base
 *   - __gxx_personality_v0 (C++ exception personality)
 *   - Various libc functions missing from our glibc stubs
 */

#define WIN32_LEAN_AND_MEAN
#include <stddef.h>

/* memmem not available in MinGW */
static void *memmem(const void *h, size_t hl, const void *n, size_t nl) {
    if (!nl) return (void*)h;
    if (hl < nl) return NULL;
    const char *hp = (const char*)h;
    const char *np = (const char*)n;
    for (size_t i = 0; i <= hl - nl; i++)
        if (memcmp(hp + i, np, nl) == 0) return (void*)(hp + i);
    return NULL;
}
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include "loader.h"

/* All allocations in this file are guest C++ runtime (operator new/delete,
 * std::string, __cxa exceptions, SGI STL default_alloc) - route them to the
 * dedicated GUEST heap so Snapshots can isolate the game's memory from the
 * loader + GL driver (which share the process heap). See stubs_glibc.c. */
extern void *guest_malloc(size_t n);
extern void *guest_calloc(size_t a, size_t b);
extern void *guest_realloc(void *p, size_t n);
extern void  guest_free(void *p);
#define malloc  guest_malloc
#define calloc  guest_calloc
#define realloc guest_realloc
#define free    guest_free

/* ------------------------------------------------------------------ */
/* C++ new / delete                                                    */
/* ------------------------------------------------------------------ */
/* _Znwj  = operator new(unsigned int)     [32-bit] */
/* _Znaj  = operator new[](unsigned int)   [32-bit] */
/* _ZdlPv = operator delete(void*)                  */
/* _ZdaPv = operator delete[](void*)               */

static void *stub__Znwj(uint32_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) {
        loader_log(LOG_ERROR, "operator new(%u) failed", size);
        ExitProcess(1);
    }
    return p;
}

static void *stub__Znaj(uint32_t size)   { return stub__Znwj(size); }
static void  stub__ZdlPv(void *p)        { if (p) free(p); }
static void  stub__ZdaPv(void *p)        { if (p) free(p); }

/* ------------------------------------------------------------------ */
/* C++ exception runtime                                               */
/* ------------------------------------------------------------------ */
/* We can't fully implement C++ exceptions without libunwind/libgcc.
 * Strategy: __cxa_throw terminates, begin/end_catch are no-ops,
 * personality function always requests cleanup.
 * Games that actually throw/catch will work for the common case
 * (most Lindbergh games use exceptions only for error paths). */

typedef struct { void *exceptionClass; void *cleanup; } CxaException;

static void stub___cxa_throw(void *obj, void *type, void *dest)
{
    (void)obj; (void)type; (void)dest;
    loader_log(LOG_WARN, "__cxa_throw called, C++ exception (terminating)");
    ExitProcess(1);
}

static void *stub___cxa_begin_catch(void *ex) { return ex; }
static void  stub___cxa_end_catch(void)       {}
static void  stub___cxa_rethrow(void)
{
    loader_log(LOG_WARN, "__cxa_rethrow, terminating");
    ExitProcess(1);
}

/* __cxa_atexit registers a C++ destructor `fn(arg)` to run at exit.
 * The real ABI contract is fn is called WITH arg (fn is void(*)(void*)).
 * Routing through C atexit() is WRONG: it (a) drops arg by casting to
 * void(*)(void), so the destructor runs with a null/garbage `this` and can
 * fault, and (b) depends on MinGW's limited, separately-ordered atexit slot
 * table. Instead we record the triple in our own table. For a game that is
 * terminated wholesale (never a clean C++ teardown), these never need to
 * actually run - recording and returning success is both correct and safe. */
typedef struct { void (*fn)(void*); void *arg; void *dso; } CxaDtor;
static CxaDtor s_cxa_dtors[1024];
static int     s_cxa_dtor_count = 0;
static int stub___cxa_atexit(void (*fn)(void*), void *arg, void *dso)
{
    if (fn && s_cxa_dtor_count < (int)(sizeof(s_cxa_dtors)/sizeof(s_cxa_dtors[0]))) {
        s_cxa_dtors[s_cxa_dtor_count].fn  = fn;
        s_cxa_dtors[s_cxa_dtor_count].arg = arg;
        s_cxa_dtors[s_cxa_dtor_count].dso = dso;
        s_cxa_dtor_count++;
    }
    return 0; /* success; destructors recorded, not invoked (no clean teardown) */
}

static void *stub___cxa_allocate_exception(uint32_t size) { return malloc(size); }
static void  stub___cxa_free_exception(void *p)           { free(p); }

/* GCC personality function for C++ exceptions */
static int stub___gxx_personality_v0(void) { return 0; }

/* __gmon_start__ - GCC profiling init, no-op */
static void stub___gmon_start__(void) {}

/* ------------------------------------------------------------------ */
/* std::string (GCC 3.x ABI - _ZNSs* mangled names)                  */
/* ------------------------------------------------------------------ */
/*
 * GCC 3.x std::string uses a reference-counted implementation.
 * Rep layout: [refcount(int), length(int), capacity(int), data...]
 * The "string" pointer points TO THE DATA, not to the Rep header.
 * Rep header is at data - 3*sizeof(int).
 *
 * For our stubs we just use plain malloc'd C strings and ignore
 * the refcount semantics. This leaks memory but avoids crashes.
 */

/* GCC 3.4 COW _Rep header is { _M_length, _M_capacity, _M_refcount } and the
 * string pointer (_M_p) points at the char data immediately after it. So from
 * the data pointer: length is at _M_p-12 (-0xc), capacity at _M_p-8, refcount
 * at _M_p-4. The game has INLINED COW code (e.g. the substr bounds check in
 * load_all_shaders, `cmp -0xc(%ecx),%esi`) that reads _M_length directly at
 * _M_p-0xc, so this field order MUST be length/capacity/refcount, not
 * refcount/length/capacity - otherwise the inlined reads pick up the wrong
 * field (refcount=1) as the length and throw spurious out_of_range. */
typedef struct {
    uint32_t length;    /* _M_length   @ _M_p - 0xc */
    uint32_t capacity;  /* _M_capacity @ _M_p - 0x8 */
    int      refcount;  /* _M_refcount @ _M_p - 0x4 */
    /* char data[] follows */
} StringRep;

static char *make_string(const char *src, uint32_t len)
{
    StringRep *rep = (StringRep*)malloc(sizeof(StringRep) + len + 1);
    rep->refcount = 1;
    rep->length   = len;
    rep->capacity = len;
    char *data = (char*)(rep + 1);
    if (src) memcpy(data, src, len);
    data[len] = '\0';
    return data;
}

static StringRep *get_rep(char *s)
{
    return ((StringRep*)s) - 1;
}

/* _ZNSsC1EPKcRKSaIcE = basic_string(const char*, const allocator&) */
static void stub__ZNSsC1EPKcRKSaIcE(char **self, const char *src, void *alloc)
{
    (void)alloc;
    uint32_t len = src ? (uint32_t)strlen(src) : 0;
    *self = make_string(src, len);
}

/* _ZNSsC1ERKSs = basic_string(const basic_string&) */
static void stub__ZNSsC1ERKSs(char **self, char **other)
{
    if (!other || !*other) { *self = make_string("", 0); return; }
    uint32_t len = get_rep(*other)->length;
    *self = make_string(*other, len);
}

/* _ZNSsC1ERKSsjj = basic_string(const string&, pos, len) */
static void stub__ZNSsC1ERKSsjj(char **self, char **other, uint32_t pos, uint32_t len)
{
    if (!other || !*other) { *self = make_string("", 0); return; }
    uint32_t slen = get_rep(*other)->length;
    if (pos > slen) pos = slen;
    if (len > slen - pos) len = slen - pos;
    *self = make_string(*other + pos, len);
}

/* Destructor stubs */
static void stub__ZNSs4_Rep10_M_disposeERKSaIcE(void *rep, void *alloc)
{
    (void)alloc;
    if (rep) free((char*)rep - sizeof(StringRep));
}
static void stub__ZNSs4_Rep10_M_destroyERKSaIcE(void *rep, void *alloc)
{
    stub__ZNSs4_Rep10_M_disposeERKSaIcE(rep, alloc);
}
static void stub__ZNSs12_M_leak_hardEv(void *self) { (void)self; }

/* assign */
static char **stub__ZNSs6assignERKSs(char **self, char **other)
{
    if (self && other && *other) {
        uint32_t len = get_rep(*other)->length;
        if (*self) free(get_rep(*self));
        *self = make_string(*other, len);
    }
    return self;
}
static char **stub__ZNSs6assignERKSsjj(char **self, char **other, uint32_t pos, uint32_t len)
{
    if (self && other && *other) {
        uint32_t slen = get_rep(*other)->length;
        if (pos > slen) pos = slen;
        if (len > slen - pos) len = slen - pos;
        if (*self) free(get_rep(*self));
        *self = make_string(*other + pos, len);
    }
    return self;
}
static char **stub__ZNSs6assignEPKcj(char **self, const char *src, uint32_t n)
{
    if (self) {
        if (*self) free(get_rep(*self));
        *self = make_string(src, n);
    }
    return self;
}

/* append */
static char **stub__ZNSs6appendERKSs(char **self, char **other)
{
    if (!self || !*self || !other || !*other) return self;
    uint32_t sl = get_rep(*self)->length;
    uint32_t ol = get_rep(*other)->length;
    char *ns = make_string(NULL, sl + ol);
    memcpy(ns, *self, sl);
    memcpy(ns + sl, *other, ol);
    free(get_rep(*self));
    *self = ns;
    return self;
}
static char **stub__ZNSs6appendEPKcj(char **self, const char *s, uint32_t n)
{
    if (!self || !*self || !s) return self;
    uint32_t sl = get_rep(*self)->length;
    char *ns = make_string(NULL, sl + n);
    memcpy(ns, *self, sl);
    memcpy(ns + sl, s, n);
    free(get_rep(*self));
    *self = ns;
    return self;
}
static char **stub__ZNSs6appendERKSsjj(char **self, char **other, uint32_t pos, uint32_t len)
{
    if (!self || !other || !*other) return self;
    uint32_t ol = get_rep(*other)->length;
    if (pos > ol) return self;
    if (len > ol - pos) len = ol - pos;
    return stub__ZNSs6appendEPKcj(self, *other + pos, len);
}
static char **stub__ZNSs6appendEjc(char **self, uint32_t n, char c)
{
    if (!self || !*self) return self;
    uint32_t sl = get_rep(*self)->length;
    char *ns = make_string(NULL, sl + n);
    memcpy(ns, *self, sl);
    memset(ns + sl, c, n);
    free(get_rep(*self));
    *self = ns;
    return self;
}

/* find */
static uint32_t NPOS = 0xFFFFFFFF;
static uint32_t stub__ZNKSs4findEPKcjj(char **self, const char *s, uint32_t pos, uint32_t n)
{
    if (!self || !*self || !s) return NPOS;
    uint32_t len = get_rep(*self)->length;
    if (pos >= len) return NPOS;
    char *found = (char*)memmem(*self + pos, len - pos, s, n);
    return found ? (uint32_t)(found - *self) : NPOS;
}
static uint32_t stub__ZNKSs4findEcj(char **self, char c, uint32_t pos)
{
    if (!self || !*self) return NPOS;
    uint32_t len = get_rep(*self)->length;
    for (uint32_t i = pos; i < len; i++)
        if ((*self)[i] == c) return i;
    return NPOS;
}
static uint32_t stub__ZNKSs5rfindEPKcjj(char **self, const char *s, uint32_t pos, uint32_t n)
{
    if (!self || !*self || !s || n == 0) return NPOS;
    uint32_t len = get_rep(*self)->length;
    if (pos >= len) pos = len - 1;
    for (int32_t i = (int32_t)pos; i >= 0; i--) {
        if ((uint32_t)i + n <= len && memcmp(*self + i, s, n) == 0)
            return (uint32_t)i;
    }
    return NPOS;
}
static uint32_t stub__ZNKSs13find_first_ofEPKcjj(char **self, const char *s, uint32_t pos, uint32_t n)
{
    if (!self || !*self || !s) return NPOS;
    uint32_t len = get_rep(*self)->length;
    for (uint32_t i = pos; i < len; i++)
        if (memchr(s, (*self)[i], n)) return i;
    return NPOS;
}
static uint32_t stub__ZNKSs17find_first_not_ofEPKcjj(char **self, const char *s, uint32_t pos, uint32_t n)
{
    if (!self || !*self || !s) return NPOS;
    uint32_t len = get_rep(*self)->length;
    for (uint32_t i = pos; i < len; i++)
        if (!memchr(s, (*self)[i], n)) return i;
    return NPOS;
}

/* compare */
static int stub__ZNKSs7compareEPKc(char **self, const char *s)
{
    if (!self || !*self) return s ? -1 : 0;
    return strcmp(*self, s);
}

/* replace */
static char **stub__ZNSs7replaceEjjPKcj(char **self, uint32_t pos, uint32_t n,
                                          const char *s, uint32_t sn)
{
    if (!self || !*self) return self;
    uint32_t len = get_rep(*self)->length;
    if (pos > len) pos = len;
    if (n > len - pos) n = len - pos;
    uint32_t newlen = len - n + sn;
    char *ns = make_string(NULL, newlen);
    memcpy(ns, *self, pos);
    if (s && sn) memcpy(ns + pos, s, sn);
    memcpy(ns + pos + sn, *self + pos + n, len - pos - n);
    free(get_rep(*self));
    *self = ns;
    return self;
}

/* ------------------------------------------------------------------ */
/* std::ios_base stubs (C++ stream infrastructure)                    */
/* ------------------------------------------------------------------ */
static void stub__ZNSt8ios_base4InitC1Ev(void *self) { (void)self; }
static void stub__ZNSt8ios_base4InitD1Ev(void *self) { (void)self; }
static void stub__ZNSt8ios_baseC2Ev(void *self)      { (void)self; }
static void stub__ZNSt8ios_baseD2Ev(void *self)      { (void)self; }
static void stub__ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate(void *s, int st) { (void)s;(void)st; }
static void stub__ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E(void *s, void *b) { (void)s;(void)b; }
static void stub__ZNSt6localeD1Ev(void *self)        { (void)self; }
static void stub__ZNSt8ios_base4InitC1Ev_(void *self){ (void)self; }

/* filebuf */
static void stub__ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev(void *s) { (void)s; }
static void *stub__ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode(
    void *s, const char *path, int mode) { (void)s;(void)path;(void)mode; return NULL; }
static void *stub__ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv(void *s) { (void)s; return NULL; }
static void stub__ZNSt12__basic_fileIcED1Ev(void *s) { (void)s; }

/* stream operators - return stream */
static void *stub__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(void *os, const char *s)
{
    if (s) fprintf(stdout, "%s", s);
    return os;
}
static void *stub__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c(void *os, char c)
{
    fputc(c, stdout);
    return os;
}
static void *stub__ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E(
    void *os, char **s) {
    if (s && *s) fprintf(stdout, "%s", *s);
    return os;
}
static void *stub__ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_ES4_(
    void *is, char **s, char delim) { (void)is;(void)s;(void)delim; return is; }
static void stub__ZSt20__throw_out_of_rangePKc(const char *s)
{
    loader_log(LOG_WARN, "std::out_of_range: %s", s ? s : "");
    ExitProcess(1);
}

/* Default allocator */
static void *stub__ZNSt24__default_alloc_templateILb1ELi0EE8allocateEj(uint32_t n)
{ return malloc(n); }
static void stub__ZNSt24__default_alloc_templateILb1ELi0EE10deallocateEPvj(void *p, uint32_t n)
{ (void)n; free(p); }

/* ------------------------------------------------------------------ */
/* Missing libc functions                                              */
/* ------------------------------------------------------------------ */
static char *stub_strtok(char *s, const char *d)   { return strtok(s, d); }
static int   stub_isprint(int c)                   { return isprint(c); }
static int   stub_isspace(int c)                   { return isspace(c); }
static int   stub_isdigit(int c)                   { return isdigit(c); }
static int   stub_isxdigit(int c)                  { return isxdigit(c); }
static int   stub_tolower(int c)                   { return tolower(c); }
static int   stub_toupper(int c)                   { return toupper(c); }
static double stub_logf(float x)                   { return logf(x); }
static float  stub_modff(float x, float *i)        { return modff(x, i); }
static float  stub_asinf(float x)                  { return asinf(x); }
static float  stub_acosf(float x)                  { return acosf(x); }
static char  *stub_localtime_r(const time_t *t, struct tm *tm) {
    struct tm *r = localtime(t); if (r && tm) *tm = *r; return r ? tm : NULL;
}
static char  *stub_localtime(const time_t *t)      { return (char*)localtime(t); }
static size_t stub_strftime(char *s, size_t n, const char *fmt, const void *tm) {
    return strftime(s, n, fmt, (const struct tm*)tm);
}
static int    stub_fscanf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfscanf(f, fmt, ap); va_end(ap); return r;
}
static char  *stub_rewind(FILE *f)                 { rewind(f); return NULL; }
static int    stub_creat(const char *p, int m)     { (void)m; return _open(p, 0x101, 0666); }
static int    stub_unlink(const char *p)           { return DeleteFileA(p) ? 0 : -1; }
static int    stub_mkdir(const char *p, int m)     { (void)m; return CreateDirectoryA(p,NULL)?0:-1; }
static int    stub_chdir(const char *p)            { return SetCurrentDirectoryA(p) ? 0 : -1; }
static int    stub_system(const char *cmd)         { return cmd ? _popen(cmd,"r") ? 0:-1 : 1; }
static int    stub_popen_close(FILE *f)            { return fclose(f); }
static FILE  *stub_popen(const char *cmd, const char *m) { return _popen(cmd, m); }
static int    stub_pclose(FILE *f)                 { return _pclose(f); }
static int    stub_iopl(int level)                 { (void)level; return 0; /* stub */ }
static int    stub_fork(void)                      { return -1; /* ENOSYS */ }
static int    stub_execlp(const char *f, const char *a, ...)  { (void)f;(void)a; return -1; }
static int    stub_tcsetattr(int fd, int a, void *t) { (void)fd;(void)a;(void)t; return 0; }
static int    stub_tcflush(int fd, int q)          { (void)fd;(void)q; return 0; }
static int    stub_kill(int pid, int sig)          { (void)pid;(void)sig; return 0; }
static int    stub_select(int n, void *r, void *w, void *e, void *t) {
    (void)n;(void)r;(void)w;(void)e;
    if (t) Sleep(1); return 0;
}
static int    stub_fcntl(int fd, int cmd, ...)     { (void)fd;(void)cmd; return 0; }
static int    stub_mallinfo_fn(void)               { return 0; }
static void   stub_signal(int sig, void *h)        { (void)sig;(void)h; }
/* stub___xstat: fully implemented in stubs_glibc.c with proper linux_stat64
 * buffer layout (st_size_lo at offset 0x2C). Do NOT define a placeholder here
 * - it would overwrite the glibc registration and leave the stat buffer
 * unpopulated, causing smpBinGetFileSizePKc to read garbage as the file size. */
static double stub___strtod_internal(const char *s, char **e, int g) {
    (void)g; return strtod(s, e);
}
static long   stub___strtol_internal(const char *s, char **e, int b, int g) {
    (void)g; return strtol(s, e, b);
}
static unsigned long stub___strtoul_internal(const char *s, char **e, int b, int g) {
    (void)g; return strtoul(s, e, b);
}

/* inet functions */
static uint32_t stub_inet_addr(const char *s)
{
    /* Simple parse */
    unsigned int a,b,c,d;
    if (sscanf(s, "%u.%u.%u.%u", &a,&b,&c,&d) == 4)
        return (a)|(b<<8)|(c<<16)|(d<<24);
    return 0;
}
static char *stub_inet_ntoa(uint32_t addr)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             addr&0xFF, (addr>>8)&0xFF, (addr>>16)&0xFF, (addr>>24)&0xFF);
    return buf;
}
static int stub_inet_aton(const char *s, uint32_t *addr)
{
    *addr = stub_inet_addr(s); return *addr ? 1 : 0;
}

/* __ctype_b_loc - returns pointer to ctype table */
static const unsigned short *stub___ctype_b_loc(void)
{
    /* Return a pointer to a static table. Games use this for isspace etc. */
    static unsigned short table[256+128] = {0};
    static bool init = false;
    if (!init) {
        init = true;
        for (int i = 0; i < 256; i++) {
            if (isalpha(i))  table[128+i] |= 0x0400|0x0100;
            if (isdigit(i))  table[128+i] |= 0x0800;
            if (isspace(i))  table[128+i] |= 0x4000;
            if (isupper(i))  table[128+i] |= 0x0100;
            if (islower(i))  table[128+i] |= 0x0200;
            if (isprint(i))  table[128+i] |= 0x8000|0x2000;
            if (ispunct(i))  table[128+i] |= 0x0040;
            if (iscntrl(i))  table[128+i] |= 0x0002;
        }
    }
    return &table[128];
}

/* dl_iterate_phdr - used by C++ exception handling for unwinding */
static int stub_dl_iterate_phdr(void *callback, void *data) {
    (void)callback; (void)data; return 0;
}

/* send/recv - forward to unix_socket layer */
static int stub_send(int fd, const void *buf, uint32_t len, int flags) {
    extern int64_t unix_send(int, const void*, size_t, int);
    return (int)unix_send(fd, buf, len, flags);
}
static int stub_recv(int fd, void *buf, uint32_t len, int flags) {
    extern int64_t unix_recv(int, void*, size_t, int);
    return (int)unix_recv(fd, buf, len, flags);
}
static int stub_bind(int fd, const void *a, uint32_t l) {
    extern int unix_bind(int, const void*, uint32_t);
    return unix_bind(fd, a, l);
}
static int stub_connect(int fd, const void *a, uint32_t l) {
    extern int unix_connect(int, const void*, uint32_t);
    return unix_connect(fd, a, l);
}
static int stub_socket(int d, int t, int p) {
    extern int unix_socket(int, int, int);
    return unix_socket(d, t, p);
}
static int stub_sendto(int fd, const void *b, uint32_t l, int f, const void *a, uint32_t al) {
    extern int64_t unix_sendto(int, const void*, size_t, int, const void*, uint32_t);
    return (int)unix_sendto(fd, b, l, f, a, al);
}
static int stub_recvfrom(int fd, void *b, uint32_t l, int f, void *a, uint32_t *al) {
    extern int64_t unix_recvfrom(int, void*, size_t, int, void*, uint32_t*);
    return (int)unix_recvfrom(fd, b, l, f, a, al);
}
static int stub_setsockopt(int fd, int lv, int opt, const void *v, uint32_t l) {
    extern int unix_setsockopt(int, int, int, const void*, uint32_t);
    return unix_setsockopt(fd, lv, opt, v, l);
}
static int stub_ioctl_fn(int fd, uint32_t req, void *arg) {
    extern int64_t vfs_ioctl(int, uint64_t, void*);
    return (int)vfs_ioctl(fd, req, arg);
}

/* ------------------------------------------------------------------ */
/* libstdc++ red-black tree primitives (std::map / std::set / std::    */
/* multimap backbone). GSEVO's vsPreCombineShader builds a std::map and */
/* calls these out-of-line tree helpers; ABC never instantiates an     */
/* associative container so its dynsym imports none of them (additive).*/
/* Clean-room implementation of the standard CLRS red-black tree, ABI- */
/* matched to libstdc++'s _Rb_tree_node_base layout:                   */
/*   struct _Rb_tree_node_base {                                       */
/*       int      _M_color;   // 0=red,1=black   (offset 0)            */
/*       _Base*   _M_parent;  // offset 4                              */
/*       _Base*   _M_left;    // offset 8                              */
/*       _Base*   _M_right;   // offset 12                             */
/*   };                                                                */
/* The tree header node's _M_parent points at the root; _M_left/_M_    */
/* right track leftmost/rightmost for begin()/rbegin(). increment/     */
/* decrement walk the in-order successor/predecessor exactly as        */
/* libstdc++ does (header recognized by color==red && parent->parent== */
/* self for decrement's end()-case). */
/* ------------------------------------------------------------------ */
typedef struct RbNode {
    int             color;   /* 0 = red, 1 = black */
    struct RbNode  *parent;
    struct RbNode  *left;
    struct RbNode  *right;
} RbNode;
#define RB_RED   0
#define RB_BLACK 1

static RbNode *rb_minimum(RbNode *x) { while (x->left)  x = x->left;  return x; }
static RbNode *rb_maximum(RbNode *x) { while (x->right) x = x->right; return x; }

/* std::_Rb_tree_increment(_Rb_tree_node_base*) -> in-order successor */
RbNode *stub__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base(RbNode *x)
{
    if (x->right) {
        return rb_minimum(x->right);
    }
    RbNode *y = x->parent;
    while (y && x == y->right) { x = y; y = y->parent; }
    /* libstdc++ quirk: at the root whose right child is the header, the
     * successor IS the header (== end()); detect via x->right!=y. */
    if (x->right != y) x = y;
    return x;
}

/* std::_Rb_tree_decrement(_Rb_tree_node_base*) -> in-order predecessor */
RbNode *stub__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base(RbNode *x)
{
    /* end() (the header): color red and parent->parent == self -> rightmost. */
    if (x->color == RB_RED && x->parent && x->parent->parent == x) {
        return x->right;
    }
    if (x->left) {
        return rb_maximum(x->left);
    }
    RbNode *y = x->parent;
    while (y && x == y->left) { x = y; y = y->parent; }
    return y;
}

static void rb_rotate_left(RbNode *x, RbNode **root)
{
    RbNode *y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (x == *root)            *root = y;
    else if (x == x->parent->left)  x->parent->left  = y;
    else                            x->parent->right = y;
    y->left   = x;
    x->parent = y;
}

static void rb_rotate_right(RbNode *x, RbNode **root)
{
    RbNode *y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    if (x == *root)            *root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else                            x->parent->left  = y;
    y->right  = x;
    x->parent = y;
}

/* std::_Rb_tree_insert_and_rebalance(bool insert_left,
 *      _Rb_tree_node_base* x, _Rb_tree_node_base* p,
 *      _Rb_tree_node_base& header)
 * Links the fresh node x under parent p (on the left iff insert_left),
 * fixes the leftmost/rightmost header pointers, then restores the RB
 * invariants. Exactly libstdc++'s tree.cc algorithm. */
void stub__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_(
        int insert_left, RbNode *x, RbNode *p, RbNode *header)
{
    RbNode **root = &header->parent;   /* header._M_parent == root */

    x->parent = p;
    x->left   = 0;
    x->right  = 0;
    x->color  = RB_RED;

    if (insert_left) {
        p->left = x;                       /* also makes leftmost = x when p==header */
        if (p == header)      { header->parent = x; header->right = x; }
        else if (p == header->left) header->left = x;   /* maintain leftmost */
    } else {
        p->right = x;
        if (p == header->right) header->right = x;       /* maintain rightmost */
    }

    /* Rebalance. */
    while (x != *root && x->parent->color == RB_RED) {
        RbNode *xpp = x->parent->parent;
        if (x->parent == xpp->left) {
            RbNode *y = xpp->right;            /* uncle */
            if (y && y->color == RB_RED) {
                x->parent->color = RB_BLACK;
                y->color         = RB_BLACK;
                xpp->color       = RB_RED;
                x = xpp;
            } else {
                if (x == x->parent->right) { x = x->parent; rb_rotate_left(x, root); }
                x->parent->color      = RB_BLACK;
                x->parent->parent->color = RB_RED;
                rb_rotate_right(x->parent->parent, root);
            }
        } else {
            RbNode *y = xpp->left;             /* uncle */
            if (y && y->color == RB_RED) {
                x->parent->color = RB_BLACK;
                y->color         = RB_BLACK;
                xpp->color       = RB_RED;
                x = xpp;
            } else {
                if (x == x->parent->left) { x = x->parent; rb_rotate_right(x, root); }
                x->parent->color      = RB_BLACK;
                x->parent->parent->color = RB_RED;
                rb_rotate_left(x->parent->parent, root);
            }
        }
    }
    (*root)->color = RB_BLACK;
}

/* C++ ABI type_info vtables - the bases an RTTI class_type_info vtable
 * points at. The game's typeinfo objects (for dynamic_cast / catch
 * matching) take a R_386_COPY of these vtable symbols. We provide a
 * small zero-filled vtable so the COPY has a valid source; the loader's
 * exceptions are stubbed (no real unwinding), so these are only ever
 * read as opaque identity pointers. Additive - additive RTTI data only. */
static void *s_ti_class_vtable[4]    = {0,0,0,0};
static void *s_ti_si_class_vtable[4] = {0,0,0,0};
static void *s_ti_vmi_class_vtable[4]= {0,0,0,0};

/* ------------------------------------------------------------------ */
/* R_386_COPY (reloc type 5) handler - already done in segment map    */
/* The warnings in the log are from data symbols. We handle these by  */
/* registering them as addresses in the stub tables.                  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Register all C++ and missing libc stubs                            */
/* ------------------------------------------------------------------ */
void cxx_register_stubs(LoaderCtx *ctx)
{
    loader_log(LOG_INFO, "Registering C++ runtime and libc stubs");
    #define R(name) sym_register(ctx, #name, (uintptr_t)stub_##name)
    #define RS(mangled, fn) sym_register(ctx, mangled, (uintptr_t)fn)

    /* new/delete */
    RS("_Znwj",  stub__Znwj);
    RS("_Znaj",  stub__Znaj);
    RS("_ZdlPv", stub__ZdlPv);
    RS("_ZdaPv", stub__ZdaPv);

    /* exception */
    RS("__cxa_throw",              stub___cxa_throw);
    RS("__cxa_begin_catch",        stub___cxa_begin_catch);
    RS("__cxa_end_catch",          stub___cxa_end_catch);
    RS("__cxa_rethrow",            stub___cxa_rethrow);
    RS("__cxa_atexit",             stub___cxa_atexit);
    RS("__cxa_allocate_exception", stub___cxa_allocate_exception);
    RS("__cxa_free_exception",     stub___cxa_free_exception);
    RS("__gxx_personality_v0",     stub___gxx_personality_v0);
    RS("__gmon_start__",           stub___gmon_start__);

    /* std::string */
    RS("_ZNSsC1EPKcRKSaIcE",       stub__ZNSsC1EPKcRKSaIcE);
    RS("_ZNSsC1ERKSs",             stub__ZNSsC1ERKSs);
    RS("_ZNSsC1ERKSsjj",           stub__ZNSsC1ERKSsjj);
    RS("_ZNSs4_Rep10_M_disposeERKSaIcE",  stub__ZNSs4_Rep10_M_disposeERKSaIcE);
    RS("_ZNSs4_Rep10_M_destroyERKSaIcE",  stub__ZNSs4_Rep10_M_destroyERKSaIcE);
    RS("_ZNSs12_M_leak_hardEv",    stub__ZNSs12_M_leak_hardEv);
    RS("_ZNSs6assignERKSs",        stub__ZNSs6assignERKSs);
    RS("_ZNSs6assignERKSsjj",      stub__ZNSs6assignERKSsjj);
    RS("_ZNSs6assignEPKcj",        stub__ZNSs6assignEPKcj);
    RS("_ZNSs6appendERKSs",        stub__ZNSs6appendERKSs);
    RS("_ZNSs6appendEPKcj",        stub__ZNSs6appendEPKcj);
    RS("_ZNSs6appendERKSsjj",      stub__ZNSs6appendERKSsjj);
    RS("_ZNSs6appendEjc",          stub__ZNSs6appendEjc);
    RS("_ZNKSs4findEPKcjj",        stub__ZNKSs4findEPKcjj);
    RS("_ZNKSs4findEcj",           stub__ZNKSs4findEcj);
    RS("_ZNKSs5rfindEPKcjj",       stub__ZNKSs5rfindEPKcjj);
    RS("_ZNKSs13find_first_ofEPKcjj", stub__ZNKSs13find_first_ofEPKcjj);
    RS("_ZNKSs17find_first_not_ofEPKcjj", stub__ZNKSs17find_first_not_ofEPKcjj);
    RS("_ZNKSs7compareEPKc",       stub__ZNKSs7compareEPKc);
    RS("_ZNSs7replaceEjjPKcj",     stub__ZNSs7replaceEjjPKcj);

    /* ios */
    RS("_ZNSt8ios_base4InitC1Ev",  stub__ZNSt8ios_base4InitC1Ev);
    RS("_ZNSt8ios_base4InitD1Ev",  stub__ZNSt8ios_base4InitD1Ev);
    RS("_ZNSt8ios_baseC2Ev",       stub__ZNSt8ios_baseC2Ev);
    RS("_ZNSt8ios_baseD2Ev",       stub__ZNSt8ios_baseD2Ev);
    RS("_ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate",
        stub__ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate);
    RS("_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E",
        stub__ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_E);
    RS("_ZNSt6localeD1Ev",         stub__ZNSt6localeD1Ev);
    RS("_ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev",
        stub__ZNSt13basic_filebufIcSt11char_traitsIcEEC1Ev);
    RS("_ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode",
        stub__ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode);
    RS("_ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv",
        stub__ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv);
    RS("_ZNSt12__basic_fileIcED1Ev",stub__ZNSt12__basic_fileIcED1Ev);
    RS("_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc",
        stub__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc);
    RS("_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c",
        stub__ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c);
    RS("_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E",
        stub__ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKSbIS4_S5_T1_E);
    RS("_ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_ES4_",
        stub__ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RSbIS4_S5_T1_ES4_);
    RS("_ZSt20__throw_out_of_rangePKc", stub__ZSt20__throw_out_of_rangePKc);
    RS("_ZNSt24__default_alloc_templateILb1ELi0EE8allocateEj",
        stub__ZNSt24__default_alloc_templateILb1ELi0EE8allocateEj);
    RS("_ZNSt24__default_alloc_templateILb1ELi0EE10deallocateEPvj",
        stub__ZNSt24__default_alloc_templateILb1ELi0EE10deallocateEPvj);

    /* libstdc++ red-black tree helpers (std::map/std::set) - GSEVO shader map */
    RS("_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base",
        stub__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base);
    RS("_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base",
        stub__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base);
    RS("_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_",
        stub__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_);

    /* C++ ABI RTTI vtables (COPY-reloc sources for the game's type_info) */
    RS("_ZTVN10__cxxabiv117__class_type_infoE",    s_ti_class_vtable);
    RS("_ZTVN10__cxxabiv120__si_class_type_infoE", s_ti_si_class_vtable);
    RS("_ZTVN10__cxxabiv121__vmi_class_type_infoE",s_ti_vmi_class_vtable);

    /* libc */
    R(strtok); R(isprint); R(isspace); R(isdigit); R(isxdigit);
    R(tolower); R(toupper); R(logf); R(modff); R(asinf); R(acosf);
    R(localtime); R(strftime); R(fscanf); R(rewind);
    R(creat); R(unlink); R(mkdir); R(chdir);
    R(system); R(popen); R(pclose);
    R(iopl); R(fork); R(execlp);
    R(tcsetattr); R(tcflush); R(kill); R(select); R(fcntl);
    R(signal); R(inet_addr); R(inet_ntoa); R(inet_aton);
    RS("mallinfo", stub_mallinfo_fn);
    /* __xstat: registered by stubs_glibc.c with the proper linux_stat64 stub */
    RS("__strtod_internal",  stub___strtod_internal);
    RS("__strtol_internal",  stub___strtol_internal);
    RS("__strtoul_internal", stub___strtoul_internal);
    RS("__ctype_b_loc",      stub___ctype_b_loc);
    RS("dl_iterate_phdr",    stub_dl_iterate_phdr);

    /* network */
    R(send); R(recv); R(bind); R(connect); R(socket);
    R(sendto); R(recvfrom); R(setsockopt);
    RS("ioctl", stub_ioctl_fn);

    #undef R
    #undef RS
}
