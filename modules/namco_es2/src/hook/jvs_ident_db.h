/* SPDX-License-Identifier: MIT
 * =============================================================================
 *  jvs_ident_db.h  --  JVS I/O board catalog, capability hints, and in-game
 *                      identity auto-selection
 * =============================================================================
 *
 *  WHAT A JVS IDENTITY IS
 *  ----------------------
 *  A JVS node answers the IOIDENT command (0x10) with one string shaped
 *
 *      <maker>;<board>;<version>[;<comment>]
 *
 *  Plenty of arcade titles do not merely log it -- they MATCH it against a
 *  hard-coded table of the boards that title shipped against, and refuse to
 *  boot on anything else.
 *
 *  Measured 2026-09-10 in Mario Kart Arcade GP DX (MK_AGP3_FINAL.exe), whose
 *  accept-table sits in .data immediately beside its own
 *  "Rx I/O ID command %d: NG %d" log string:
 *
 *      NBGI.;WinArc;Ver2.01;JPN     (held separately, compared whole, in .text)
 *      NeoJV  NA-JV  FCB  MINI-JV  I/O CYBER LEAD  ASCA-3  ASCA-5  TSS-I/O
 *      MIU-I/O  TMIU-I/O  RAYS PCB  Jamma I/O  FCA-1  P-DRIVE PCB  DE-JV PCB
 *      ...and the maker prefix "namco ltd."
 *
 *  Our node used to answer "ULTRA LOADER;JVS-IO;ver1.00;..." -- a string on no
 *  game's list anywhere -- and MKDX stopped with the on-screen error
 *  "E03-02  I/O board type is abnormal".  Branding our own I/O was the bug.
 *
 *  WHY THE CATALOG IS WORTH MORE THAN ONE GAME'S FIX
 *  --------------------------------------------------
 *  Which board a title expects tells us how the cabinet was WIRED, and that is
 *  exactly what the Omni services need in order to map a modern controller
 *  onto it: how many analog channels exist and what they mean, how many
 *  players, whether there is a rotary encoder (a wheel), a gun, or a shifter.
 *  A driving board and a gun board are both "JVS", and a universal control
 *  layer that cannot tell them apart has to guess. So every entry below
 *  carries capability hints alongside the identity string, and the hints are
 *  readable by anything that includes this header -- the board hooks today,
 *  the Omni services and the loader's control mapper next.
 *
 *  RESOLUTION ORDER (ultra_jvs_ident_autoselect)
 *  ---------------------------------------------
 *      1. ULTRA_JVS_ID     -- exact string, verbatim, no interpretation
 *      2. ULTRA_JVS_BOARD  -- a catalog key ("winarc", "fca1", "sega13551")
 *      3. a complete catalog identity found inside the game's own executable
 *      4. any complete "maker;board;Ver..." string found in the game's exe,
 *         even one this catalog has never heard of
 *      5. a catalog board-name token found in the game's exe
 *      6. this module's vendor default
 *      7. the generic identity
 *
 *  Steps 3-5 are the important ones: a title that validates the identity
 *  string necessarily CONTAINS the strings it accepts, so we read the answer
 *  out of the game rather than needing to know it in advance. That is what
 *  makes this work on titles nobody has looked at yet.
 *
 *  PROVENANCE -- READ THIS BEFORE TRUSTING A VERSION FIELD
 *  ------------------------------------------------------
 *    [DUMP-FULL] the entire string was recovered byte-for-byte from a binary
 *                on this machine. Trust it completely.
 *    [DUMP-NAME] the BOARD-NAME field was recovered byte-for-byte; the version
 *                and comment fields are a plausible reconstruction.
 *    [DOC]       a published identity for a real board, not verified against
 *                any binary in this tree.
 *
 *  Games match on the maker and board fields, which is the part [DUMP-NAME]
 *  gets right; a reconstructed version field has not been observed to matter.
 *  It is still a reconstruction, and it is labeled as one. This is precisely
 *  why steps 3-5 outrank the catalog: when the game hands us a complete
 *  string we use it whole and guess nothing.
 * =============================================================================
 */
#ifndef ULTRA_JVS_IDENT_DB_H
#define ULTRA_JVS_IDENT_DB_H

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>   /* getenv */
#include <string.h>

/* Vendor families, so a board module can bias toward its own maker. */
#define UJ_GENERIC  0x00u
#define UJ_NAMCO    0x01u
#define UJ_SEGA     0x02u
#define UJ_TAITO    0x04u
#define UJ_KONAMI   0x08u
#define UJ_CAPCOM   0x10u
#define UJ_OTHER    0x20u

/* Capability hints -- what the cabinet behind this board is wired for. These
 * are what a universal control layer needs; the identity string alone does not
 * tell you whether analog channel 0 is a steering wheel or a gun's X axis. */
#define UJ_CAP_WHEEL    0x0001u   /* rotary encoder / steering column        */
#define UJ_CAP_PEDALS   0x0002u   /* accel/brake (and often clutch)          */
#define UJ_CAP_SHIFTER  0x0004u   /* gear positions as switches              */
#define UJ_CAP_GUN      0x0008u   /* screen-position (light gun) channels    */
#define UJ_CAP_STICK    0x0010u   /* flight/analog stick                     */
#define UJ_CAP_ROTARY   0x0020u   /* paddle / spinner                        */
#define UJ_CAP_CARD     0x0040u   /* card reader on the same bus             */
#define UJ_CAP_GENERAL  0x0080u   /* plain digital cabinet (sticks + buttons)*/
/* NOT a JVS serial board at all -- a PC-side SOFTWARE I/O identity. A title can
 * carry one of these for its amdaemon/WinArc path AND still require a real board
 * name on the serial bus, so these must never be auto-selected for a serial
 * node. Measured on Mario Kart GP DX: answering "NBGI.;WinArc;Ver2.01;JPN" on
 * the JVS bus is rejected with E03-02, while "namco ltd.;NA-JV;Ver1.00;JPN"
 * gives "I/O check ... OK  JVI/O connected" on its boot-check screen.
 * Reachable only via an explicit ULTRA_JVS_ID / ULTRA_JVS_BOARD. */
#define UJ_SOFTWARE_IO  0x1000u

typedef struct {
    const char   *key;      /* selector for ULTRA_JVS_BOARD                  */
    const char   *ident;    /* the full IOIDENT (0x10) reply                 */
    const char   *token;    /* substring a game's accept-table would contain */
    unsigned      vendor;
    unsigned      caps;
    unsigned char players;  /* 0 = leave the node default alone              */
    unsigned char analog_ch;/* 0 = leave the node default alone              */
    const char   *note;
} ultra_jvs_board;

static const ultra_jvs_board ULTRA_JVS_BOARDS[] = {

/* ---------------------------------------------------------------------------
 *  NAMCO -- every board name below was read out of MK_AGP3_FINAL.exe's own
 *  accept-table, so the names are exact. Version/comment reconstructed except
 *  where marked FULL.
 * ------------------------------------------------------------------------- */
{ "na-jv",     "namco ltd.;NA-JV;Ver1.00;JPN",          "NA-JV",          UJ_NAMCO,
  UJ_CAP_GENERAL|UJ_CAP_WHEEL|UJ_CAP_PEDALS, 2, 8,
  "[DUMP-NAME] MEASURED GOOD on Mario Kart Arcade GP DX 2026-09-10 -- its boot-check screen "
  "reports \"I/O check  OK  JVI/O connected\" with this identity. Listed FIRST so the vendor "
  "default prefers it. P-DRIVE PCB / DE-JV PCB / FCA-1 / RAYS PCB were each rejected with "
  "E03-02 on the same title, so board name is NOT interchangeable." },
{ "winarc",    "NBGI.;WinArc;Ver2.01;JPN",              "WinArc",         UJ_NAMCO,
  UJ_SOFTWARE_IO|UJ_CAP_GENERAL, 2, 8,
  "[DUMP-FULL] Verbatim -- but it is the PC-side SOFTWARE I/O identity, not a serial board. "
  "See UJ_SOFTWARE_IO. MKDX compares it whole in .text for its amdaemon path and REJECTS it "
  "on the JVS bus." },
{ "p-drive",   "namco ltd.;P-DRIVE PCB;Ver1.00;JPN",    "P-DRIVE PCB",    UJ_NAMCO,
  UJ_CAP_WHEEL|UJ_CAP_PEDALS|UJ_CAP_SHIFTER, 1, 8, "[DUMP-NAME] driving cabinets" },
{ "de-jv",     "namco ltd.;DE-JV PCB;Ver1.00;JPN",      "DE-JV PCB",      UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 8, "[DUMP-NAME]" },
{ "fca1",      "namco ltd.;FCA-1;Ver1.00;JPN",          "FCA-1",          UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 8, "[DUMP-NAME]" },
{ "jamma-io",  "namco ltd.;Jamma I/O;Ver1.00;JPN",      "Jamma I/O",      UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 0, "[DUMP-NAME] plain JAMMA harness bridge" },
{ "rays",      "namco ltd.;RAYS PCB;Ver1.00;JPN",       "RAYS PCB",       UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 8, "[DUMP-NAME]" },
{ "tmiu",      "namco ltd.;TMIU-I/O;Ver1.00;JPN",       "TMIU-I/O",       UJ_NAMCO,
  UJ_CAP_GENERAL|UJ_CAP_CARD, 2, 8, "[DUMP-NAME]" },
{ "miu",       "namco ltd.;MIU-I/O;Ver1.00;JPN",        "MIU-I/O",        UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 8, "[DUMP-NAME]" },
{ "tss",       "namco ltd.;TSS-I/O;Ver1.00;JPN",        "TSS-I/O",        UJ_NAMCO,
  UJ_CAP_GUN, 2, 8, "[DUMP-NAME] gun cabinets (Time Crisis lineage)" },
{ "asca5",     "namco ltd.;ASCA-5;Ver1.00;JPN",         "ASCA-5",         UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 8, "[DUMP-NAME]" },
{ "asca3",     "namco ltd.;ASCA-3;Ver1.00;JPN",         "ASCA-3",         UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 8, "[DUMP-NAME]" },
{ "cyberlead", "namco ltd.;I/O CYBER LEAD;Ver1.00;JPN", "I/O CYBER LEAD", UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 8, "[DUMP-NAME]" },
{ "mini-jv",   "namco ltd.;MINI-JV;Ver1.00;JPN",        "MINI-JV",        UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 0, "[DUMP-NAME]" },
{ "neojv",     "namco ltd.;NeoJV;Ver1.00;JPN",          "NeoJV",          UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 8, "[DUMP-NAME]" },
{ "fcb",       "namco ltd.;FCB;Ver1.00;JPN",            NULL,             UJ_NAMCO,
  UJ_CAP_GENERAL, 2, 8,
  "[DUMP-NAME] token deliberately NULL: 'FCB' is three characters and matches inside unrelated "
  "words, so it must never be auto-matched. Reachable only via ULTRA_JVS_BOARD=fcb." },
{ "iopcb4000", "namco ltd.;I/O PCB-4000;ver1.00;JPN,Multipurpose + Rotary Encoder",
  "I/O PCB-4000", UJ_NAMCO, UJ_CAP_GENERAL|UJ_CAP_ROTARY|UJ_CAP_WHEEL, 2, 8,
  "[DOC] the long-running Namco multipurpose board; its comment field declares the rotary encoder" },
{ "jyu-pcb",   "namco ltd.;JYU-PCB;ver1.00;JPN,2Player,Analog-3ch",
  "JYU-PCB",      UJ_NAMCO, UJ_CAP_GENERAL|UJ_CAP_STICK, 2, 3,
  "[DOC] comment field declares 2 players and 3 analog channels" },

/* ---------------------------------------------------------------------------
 *  SEGA -- part numbers are the identifying field. 837-15257 also appears in
 *  this tree's own IO4-USB emulation, which is where that lane's VID/PID work.
 * ------------------------------------------------------------------------- */
{ "sega13551", "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10",
  "837-13551", UJ_SEGA, UJ_CAP_GENERAL, 2, 8, "[DOC] the classic Sega JVS I/O" },
{ "sega13844", "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13844-01 ;Ver1.02;99/07",
  "837-13844", UJ_SEGA, UJ_CAP_GENERAL, 2, 8, "[DOC] NAOMI-era I/O" },
{ "sega14572", "SEGA CORPORATION;I/O BD JVS;837-14572;Ver1.00;05/22/2006",
  "837-14572", UJ_SEGA, UJ_CAP_GENERAL, 2, 8, "[DOC] Lindbergh-era I/O" },
{ "sega15257", "SEGA CORPORATION;I/O BD JVS;837-15257;Ver1.00;",
  "837-15257", UJ_SEGA, UJ_CAP_GENERAL|UJ_CAP_GUN, 2, 12,
  "[DOC/DUMP] IO4-USB. The part number appears in this tree's own io4 emulation; "
  "that lane advertises 12 analog channels." },
{ "sega-drive","SEGA ENTERPRISES,LTD.;I/O 838-13683B ;Ver1.01;00/07",
  "838-13683", UJ_SEGA, UJ_CAP_WHEEL|UJ_CAP_PEDALS|UJ_CAP_SHIFTER, 1, 8,
  "[DOC] Sega driving-cabinet I/O" },

/* ---------------------------------------------------------------------------
 *  OTHER VENDORS -- [DOC] only. Nothing in this tree's dumps confirms these,
 *  and the auto-selector will prefer anything it finds in the game itself.
 * ------------------------------------------------------------------------- */
{ "taito-typex", "TAITO CORP.;Type X JVS I/O;Ver1.00;JPN", "Type X JVS I/O", UJ_TAITO,
  UJ_CAP_GENERAL, 2, 8, "[DOC]" },
{ "konami-white","KONAMI CO.,LTD.;White I/O;Ver1.00;",     "White I/O",      UJ_KONAMI,
  UJ_CAP_GENERAL, 2, 8, "[DOC]" },
{ "capcom-io",   "CAPCOM CO.,LTD.;I/O BOARD;Ver1.00;",     NULL,             UJ_CAPCOM,
  UJ_CAP_GENERAL, 2, 8, "[DOC] token NULL: 'I/O BOARD' is far too generic to auto-match" },

/* ---------------------------------------------------------------------------
 *  LAST RESORT
 * ------------------------------------------------------------------------- */
{ "generic",   "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10",
  NULL, UJ_GENERIC, UJ_CAP_GENERAL, 0, 0,
  "The most widely accepted identity in the JVS world, and deliberately NOT our own branding: "
  "a game that validates this string has never heard of Ultra Loader." },
};

#define ULTRA_JVS_NBOARDS ((int)(sizeof ULTRA_JVS_BOARDS / sizeof ULTRA_JVS_BOARDS[0]))

/* Catalog accessors, so the Omni services / control mapper can enumerate the
 * same table the hooks answer with instead of keeping a second copy. */
static int  ultra_jvs_board_count(void) { return ULTRA_JVS_NBOARDS; }
static const ultra_jvs_board *ultra_jvs_board_at(int i)
{ return (i >= 0 && i < ULTRA_JVS_NBOARDS) ? &ULTRA_JVS_BOARDS[i] : NULL; }

/* -------------------------------------------------------------------------- */
/*  Logging -- self-contained on purpose                                       */
/* -------------------------------------------------------------------------- */
/* Only a handful of board hooks define ultra_d3d8_log(), so this header must
 * not depend on it or it would fail to link on most boards. Append to the same
 * ULTRA_JVS_LOG file the serial hook uses, so the identity decision shows up in
 * the same trace as the JVS conversation it explains. */
static void uj_log(const char *fmt, ...)
{
    const char *path = getenv("ULTRA_JVS_LOG");
    FILE *f;
    va_list ap;
    if (!path || !*path) return;
    f = fopen(path, "a");
    if (!f) return;
    fputs("[JVS-ID] ", f);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* -------------------------------------------------------------------------- */
/*  Reading the game's own executable                                          */
/* -------------------------------------------------------------------------- */
/* The largest game image in this tree is under 8MB; 64MB is generous and still
 * bounds a pathological mapping. */
#define UJ_MAX_IMAGE (64u * 1024u * 1024u)

typedef struct { const unsigned char *p; size_t n; HANDLE f, m; } uj_image;

static int uj_map_own_exe(uj_image *im)
{
    char path[MAX_PATH];
    DWORD hi = 0, lo;

    memset(im, 0, sizeof *im);
    if (!GetModuleFileNameA(NULL, path, MAX_PATH)) return 0;

    /* FILE_SHARE_WRITE as well: the image is already running, and some packers
     * keep a writable handle open on their own file. */
    im->f = CreateFileA(path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (im->f == INVALID_HANDLE_VALUE) { im->f = NULL; return 0; }

    lo = GetFileSize(im->f, &hi);
    if (lo == INVALID_FILE_SIZE || hi != 0 || lo == 0 || lo > UJ_MAX_IMAGE) {
        CloseHandle(im->f); im->f = NULL; return 0;
    }
    im->m = CreateFileMappingA(im->f, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!im->m) { CloseHandle(im->f); im->f = NULL; return 0; }

    im->p = (const unsigned char *)MapViewOfFile(im->m, FILE_MAP_READ, 0, 0, 0);
    if (!im->p) { CloseHandle(im->m); CloseHandle(im->f); im->m = im->f = NULL; return 0; }
    im->n = (size_t)lo;
    return 1;
}

static void uj_unmap(uj_image *im)
{
    if (im->p) UnmapViewOfFile((LPCVOID)im->p);
    if (im->m) CloseHandle(im->m);
    if (im->f) CloseHandle(im->f);
    memset(im, 0, sizeof *im);
}

static const unsigned char *uj_find(const uj_image *im, const char *needle)
{
    size_t nl = needle ? strlen(needle) : 0;
    size_t i;
    if (!nl || im->n < nl) return NULL;
    for (i = 0; i + nl <= im->n; ++i)
        if (im->p[i] == (unsigned char)needle[0] && memcmp(im->p + i, needle, nl) == 0)
            return im->p + i;
    return NULL;
}

static int uj_body_char(unsigned char c) { return c >= 0x20 && c <= 0x7e; }

/* Pull the complete printable run containing `at` and decide whether it looks
 * like a JVS identity: at least two semicolons, and a third field starting
 * "Ver"/"ver". That version test is what separates a real identity from an
 * arbitrary semicolon-bearing blob such as a shader define or a path list. */
static int uj_extract_ident(const uj_image *im, size_t at, char *out, size_t cap)
{
    size_t start = at, end = at, len, i;
    size_t semis = 0;
    const char *s, *f2 = NULL;
    int n = 0;

    while (start > 0 && uj_body_char(im->p[start - 1])) start--;
    while (end < im->n && uj_body_char(im->p[end])) end++;

    len = end - start;
    if (len < 8 || len > 96 || len >= cap) return 0;

    for (i = start; i < end; ++i) if (im->p[i] == ';') semis++;
    if (semis < 2) return 0;

    memcpy(out, im->p + start, len);
    out[len] = 0;

    for (s = out; *s; ++s) if (*s == ';' && ++n == 2) { f2 = s + 1; break; }
    if (!f2) return 0;
    while (*f2 == ' ') f2++;
    return (f2[0] == 'V' || f2[0] == 'v') &&
           (f2[1] == 'E' || f2[1] == 'e') &&
           (f2[2] == 'R' || f2[2] == 'r');
}

/* -------------------------------------------------------------------------- */
/*  Selection                                                                  */
/* -------------------------------------------------------------------------- */
static const ultra_jvs_board *uj_by_key(const char *key)
{
    int i;
    if (!key || !*key) return NULL;
    for (i = 0; i < ULTRA_JVS_NBOARDS; ++i)
        if (_stricmp(ULTRA_JVS_BOARDS[i].key, key) == 0) return &ULTRA_JVS_BOARDS[i];
    return NULL;
}

static unsigned uj_vendor_of_board(const char *board_id)
{
    if (!board_id) return UJ_GENERIC;
    if (strstr(board_id, "namco"))  return UJ_NAMCO;
    if (strstr(board_id, "sega"))   return UJ_SEGA;
    if (strstr(board_id, "taito"))  return UJ_TAITO;
    if (strstr(board_id, "konami")) return UJ_KONAMI;
    return UJ_GENERIC;
}

/* Returns the identity to answer with, or NULL to keep the caller's default.
 * `board_id` is this module's board name ("namco_es3", "taito_typex", ...) and
 * only breaks ties -- it never overrides evidence found inside the game. */
static const char *ultra_jvs_ident_autoselect(const char *board_id)
{
    static char chosen[128];
    const char *env;
    const ultra_jvs_board *b;
    unsigned want;
    uj_image im;
    int i;

    env = getenv("ULTRA_JVS_ID");
    if (env && *env) { uj_log("using ULTRA_JVS_ID verbatim: \"%s\"", env); return env; }

    env = getenv("ULTRA_JVS_BOARD");
    if (env && *env) {
        b = uj_by_key(env);
        if (b) { uj_log("ULTRA_JVS_BOARD=%s -> \"%s\"", env, b->ident); return b->ident; }
        uj_log("ULTRA_JVS_BOARD=%s is not a catalog key -- ignoring", env);
    }

    want = uj_vendor_of_board(board_id);

    if (uj_map_own_exe(&im)) {
        int best = -1;

        /* 3. A catalog entry whose COMPLETE identity is present in the game
         *    is the strongest evidence there is: those are the exact bytes the
         *    game will compare against. */
        for (i = 0; i < ULTRA_JVS_NBOARDS; ++i) {
            if (!ULTRA_JVS_BOARDS[i].ident) continue;
            /* A software-I/O identity is not a valid answer for a serial node,
               even when the game plainly contains the string. */
            if (ULTRA_JVS_BOARDS[i].caps & UJ_SOFTWARE_IO) continue;
            if (uj_find(&im, ULTRA_JVS_BOARDS[i].ident)) {
                uj_log("game carries the complete identity \"%s\" -> using it",
                       ULTRA_JVS_BOARDS[i].ident);
                strncpy(chosen, ULTRA_JVS_BOARDS[i].ident, sizeof chosen - 1);
                chosen[sizeof chosen - 1] = 0;
                uj_unmap(&im);
                return chosen;
            }
        }

        /* 4. Any complete identity string at all, including boards this
         *    catalog has never seen. Prefer one whose maker matches. */
        {
            char cand[128], keep[128];
            size_t at;
            int keep_score = -1;

            keep[0] = 0;
            for (at = 0; at + 8 < im.n; ++at) {
                if (im.p[at] != ';') continue;
                if (uj_extract_ident(&im, at, cand, sizeof cand)) {
                    /* Never adopt our own historic branding, which may be lying
                     * around inside a stale hook DLL shipped next to the game. */
                    if (!strstr(cand, "ULTRA LOADER") && !strstr(cand, "WinArc")) {
                        int score = 1;
                        if (want == UJ_NAMCO  && (strstr(cand, "namco") || strstr(cand, "NBGI"))) score = 3;
                        if (want == UJ_SEGA   &&  strstr(cand, "SEGA"))   score = 3;
                        if (want == UJ_TAITO  &&  strstr(cand, "TAITO"))  score = 3;
                        if (want == UJ_KONAMI &&  strstr(cand, "KONAMI")) score = 3;
                        if (score > keep_score) {
                            keep_score = score;
                            strncpy(keep, cand, sizeof keep - 1);
                            keep[sizeof keep - 1] = 0;
                        }
                    }
                }
                /* Step past this whole printable run rather than re-extracting
                 * it once per semicolon inside it. */
                while (at < im.n && uj_body_char(im.p[at])) at++;
            }
            if (keep[0]) {
                uj_log("game carries identity \"%s\" (score %d) -> using it", keep, keep_score);
                strncpy(chosen, keep, sizeof chosen - 1);
                chosen[sizeof chosen - 1] = 0;
                uj_unmap(&im);
                return chosen;
            }
        }

        /* 5. A bare board-name token from the catalog. */
        for (i = 0; i < ULTRA_JVS_NBOARDS; ++i) {
            const ultra_jvs_board *e = &ULTRA_JVS_BOARDS[i];
            if (e->caps & UJ_SOFTWARE_IO) continue;
            if (!e->token || strlen(e->token) < 5) continue;
            if (!uj_find(&im, e->token)) continue;
            if (best < 0 || (e->vendor == want && ULTRA_JVS_BOARDS[best].vendor != want))
                best = i;
        }
        if (best >= 0) {
            uj_log("game accept-table names board \"%s\" -> answering \"%s\"",
                   ULTRA_JVS_BOARDS[best].token, ULTRA_JVS_BOARDS[best].ident);
            strncpy(chosen, ULTRA_JVS_BOARDS[best].ident, sizeof chosen - 1);
            chosen[sizeof chosen - 1] = 0;
            uj_unmap(&im);
            return chosen;
        }
        uj_unmap(&im);
        uj_log("no identity evidence found inside the game image");
    } else {
        uj_log("could not map the game image -- falling back to a vendor default");
    }

    for (i = 0; i < ULTRA_JVS_NBOARDS; ++i)
        if (want != UJ_GENERIC && ULTRA_JVS_BOARDS[i].vendor == want
            && !(ULTRA_JVS_BOARDS[i].caps & UJ_SOFTWARE_IO)) {
            uj_log("using the %s vendor default \"%s\"",
                   board_id ? board_id : "?", ULTRA_JVS_BOARDS[i].ident);
            return ULTRA_JVS_BOARDS[i].ident;
        }

    b = uj_by_key("generic");
    if (b) { uj_log("using the generic identity \"%s\"", b->ident); return b->ident; }
    return NULL;
}

/* Silence -Wunused-function on boards that only use part of this header. */
static void uj_keep_referenced(void)
{
    (void)ultra_jvs_board_count; (void)ultra_jvs_board_at;
    (void)ultra_jvs_ident_autoselect; (void)uj_keep_referenced;
}

#endif /* ULTRA_JVS_IDENT_DB_H */
