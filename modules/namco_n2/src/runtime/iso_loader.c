/*
 * iso_loader.c
 * ISO 9660 / Joliet filesystem reader and virtual mount layer
 *
 * Handles:
 *   - Standard ISO 9660
 *   - Joliet extensions (Unicode filenames)
 *   - Rock Ridge (Linux extended info - used by Lindbergh discs)
 *   - Multi-session (some Lindbergh discs have 2 sessions)
 *
 * ISO 9660 structure:
 *   Sectors are 2048 bytes each.
 *   Sectors 0-15:  System area (unused by us)
 *   Sector 16:     Primary Volume Descriptor (PVD)
 *   Sector 17+:    More Volume Descriptors (Joliet SVD, terminator)
 *   ...            Directory records and file data
 *
 * Once mounted, any open() call to a path inside the ISO is intercepted
 * by the VFS layer and served from the ISO file directly via sector reads.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "loader.h"
#include "iso_loader.h"
#include "vfs.h"


static IsoMount *g_mount = NULL;

/* ------------------------------------------------------------------ */
/* Sector read                                                          */
/* ------------------------------------------------------------------ */
static bool iso_read_sector(IsoMount *m, uint32_t lba,
                             void *buf, uint32_t count)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)lba * ISO_SECTOR_SIZE;
    if (!SetFilePointerEx(m->file, pos, NULL, FILE_BEGIN)) return false;

    DWORD bytes = count * ISO_SECTOR_SIZE;
    DWORD got   = 0;
    return ReadFile(m->file, buf, bytes, &got, NULL) && got == bytes;
}

static bool iso_read_bytes(IsoMount *m, uint32_t lba,
                            uint32_t offset, void *buf, uint32_t size)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)lba * ISO_SECTOR_SIZE + offset;
    if (!SetFilePointerEx(m->file, pos, NULL, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(m->file, buf, size, &got, NULL) && got == size;
}

/* ------------------------------------------------------------------ */
/* Entry table management                                              */
/* ------------------------------------------------------------------ */
static void entries_grow(IsoMount *m)
{
    if (m->entry_count >= m->entry_cap) {
        m->entry_cap *= 2;
        m->entries = realloc(m->entries, m->entry_cap * sizeof(IsoEntry));
    }
}

static void add_entry(IsoMount *m, const char *path,
                       uint32_t lba, uint32_t size, bool is_dir)
{
    entries_grow(m);
    IsoEntry *e = &m->entries[m->entry_count++];
    strncpy(e->path, path, ISO_MAX_PATH-1);
    e->lba    = lba;
    e->size   = size;
    e->is_dir = is_dir;
}

/* ------------------------------------------------------------------ */
/* Name extraction from directory record                               */
/* ------------------------------------------------------------------ */
static void iso_get_name(const ISO_DirRecord *dr, bool joliet,
                          char *out, size_t out_sz)
{
    if (dr->name_len == 1 && dr->name[0] == 0) { strncpy(out, ".", out_sz); return; }
    if (dr->name_len == 1 && dr->name[0] == 1) { strncpy(out, "..", out_sz); return; }

    if (joliet) {
        /* Joliet uses UCS-2 big-endian */
        size_t chars = dr->name_len / 2;
        size_t j = 0;
        const uint8_t *src = (const uint8_t*)dr->name;
        for (size_t i = 0; i < chars && j < out_sz-1; i++) {
            uint16_t wc = (src[i*2] << 8) | src[i*2+1];
            /* Simple ASCII-range conversion */
            out[j++] = (wc < 0x80) ? (char)wc : '?';
        }
        out[j] = '\0';
    } else {
        /* Standard ISO 9660: strip version ";1" suffix */
        size_t n = dr->name_len;
        const char *src = dr->name;
        const char *semi = memchr(src, ';', n);
        if (semi) n = semi - src;
        /* Strip trailing dot */
        if (n > 1 && src[n-1] == '.') n--;
        size_t copy = (n < out_sz-1) ? n : out_sz-1;
        memcpy(out, src, copy);
        out[copy] = '\0';
        /* Lowercase for Linux-style paths */
        for (size_t i = 0; i < copy; i++)
            out[i] = (out[i] >= 'A' && out[i] <= 'Z') ?
                      out[i] + 32 : out[i];
    }
}

/* ------------------------------------------------------------------ */
/* Recursive directory scan                                            */
/* ------------------------------------------------------------------ */
static void scan_directory(IsoMount *m, const char *parent_path,
                            uint32_t lba, uint32_t dir_size, bool joliet,
                            int depth)
{
    if (depth > 16) return; /* safety */

    uint8_t *sector = malloc(ISO_SECTOR_SIZE);
    if (!sector) return;

    uint32_t bytes_read = 0;
    uint32_t cur_lba    = lba;

    while (bytes_read < dir_size) {
        if (!iso_read_sector(m, cur_lba, sector, 1)) break;

        size_t off = 0;
        while (off < ISO_SECTOR_SIZE) {
            ISO_DirRecord *dr = (ISO_DirRecord*)(sector + off);
            if (dr->length == 0) break; /* end of this sector's records */

            char name[256] = {0};
            iso_get_name(dr, joliet, name, sizeof(name));

            /* Skip . and .. */
            if (name[0] == '.' && (name[1] == '\0' ||
                (name[1] == '.' && name[2] == '\0'))) {
                off += dr->length;
                continue;
            }

            char full_path[ISO_MAX_PATH];
            if (strcmp(parent_path, "/") == 0)
                snprintf(full_path, ISO_MAX_PATH, "/%s", name);
            else
                snprintf(full_path, ISO_MAX_PATH, "%s/%s", parent_path, name);

            bool is_dir = (dr->flags & ISO_FLAG_DIRECTORY) != 0;
            uint32_t file_lba  = dr->extent_le;
            uint32_t file_size = dr->data_length_le;

            add_entry(m, full_path, file_lba, file_size, is_dir);
            loader_log(LOG_DEBUG, "ISO: %s %s (%u bytes @ LBA %u)",
                       is_dir ? "DIR " : "FILE", full_path, file_size, file_lba);

            if (is_dir && depth < 15) {
                scan_directory(m, full_path, file_lba, file_size,
                               joliet, depth + 1);
            }

            off += dr->length;
        }

        bytes_read += ISO_SECTOR_SIZE;
        cur_lba++;
    }

    free(sector);
}

/* ------------------------------------------------------------------ */
/* Mount an ISO file                                                    */
/* ------------------------------------------------------------------ */
IsoMount *iso_mount(const char *iso_path)
{
    loader_log(LOG_INFO, "=== ISO Mount: %s ===", iso_path);

    HANDLE fh = CreateFileA(iso_path, GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                             NULL);
    if (fh == INVALID_HANDLE_VALUE) {
        loader_log(LOG_ERROR, "Cannot open ISO: %lu", GetLastError());
        return NULL;
    }

    LARGE_INTEGER fsz;
    GetFileSizeEx(fh, &fsz);
    loader_log(LOG_INFO, "ISO size: %.1f MB", fsz.QuadPart / 1048576.0);

    IsoMount *m = calloc(1, sizeof(IsoMount));
    strncpy(m->iso_path, iso_path, MAX_PATH-1);
    m->file      = fh;
    m->entry_cap = 256;
    m->entries   = malloc(m->entry_cap * sizeof(IsoEntry));

    /* Scan for Volume Descriptors - multi-session ISOs may have PVD
     * at different locations. Scan sectors 16-64 plus common session offsets. */
    uint8_t *pvd_buf = malloc(ISO_SECTOR_SIZE);
    uint32_t root_lba  = 0, root_size = 0;
    uint32_t joliet_lba = 0, joliet_size = 0;
    bool found_primary = false;

    /* Build list of candidate sectors to check */
    uint32_t candidate_starts[] = {
        ISO_PVD_SECTOR,   /* standard: sector 16 */
        0,                /* sentinel */
    };

    /* First find any valid CD001 signature in first 256 sectors */
    uint32_t first_pvd_sector = ISO_PVD_SECTOR;
    /* Some ISOs (GD-ROM conversions, multi-track) have CD001 at unusual offsets.
     * Scan more aggressively - check every sector up to 512 */
    for (uint32_t s = 0; s <= 512; s++) {
        if (!iso_read_sector(m, s, pvd_buf, 1)) continue;
        /* Check for CD001 at byte 1 (standard position in VD) */
        uint8_t *raw = pvd_buf;
        if (raw[1] == 'C' && raw[2] == 'D' && raw[3] == '0' &&
            raw[4] == '0' && raw[5] == '1') {
            first_pvd_sector = s;
            loader_log(LOG_INFO, "Found CD001 at sector %u (type=0x%02x)", s, raw[0]);
            break;
        }
    }
    
    /* Also try raw byte scan for CD001 in first 2MB in case of unusual header */
    if (first_pvd_sector == ISO_PVD_SECTOR) {
        loader_log(LOG_INFO, "CD001 not found by sector scan, trying raw byte scan...");
        uint8_t raw_scan[2048];
        for (uint32_t s = 0; s <= 1024; s++) {
            if (!iso_read_sector(m, s, raw_scan, 1)) continue;
            /* Search within sector for CD001 signature */
            for (int off = 0; off < 2043; off++) {
                if (raw_scan[off+1] == 'C' && raw_scan[off+2] == 'D' &&
                    raw_scan[off+3] == '0' && raw_scan[off+4] == '0' &&
                    raw_scan[off+5] == '1') {
                    loader_log(LOG_INFO, "CD001 found in sector %u at byte offset %d", s, off);
                    first_pvd_sector = s;
                    goto found_pvd;
                }
            }
        }
        loader_log(LOG_WARN, "CD001 not found anywhere in first 2MB of ISO");
    /* Dump first bytes of early sectors for diagnosis */
    loader_log(LOG_WARN, "Sector dump for diagnosis:");
    uint8_t diag[2048];
    for (uint32_t s = 0; s <= 20; s++) {
        if (!iso_read_sector(m, s, diag, 1)) continue;
        loader_log(LOG_WARN, "  Sector %2u: %02x %02x %02x %02x %02x %02x %02x %02x | %c%c%c%c%c%c%c%c",
            s,
            diag[0],diag[1],diag[2],diag[3],diag[4],diag[5],diag[6],diag[7],
            isprint(diag[0])?diag[0]:'.', isprint(diag[1])?diag[1]:'.',
            isprint(diag[2])?diag[2]:'.', isprint(diag[3])?diag[3]:'.',
            isprint(diag[4])?diag[4]:'.', isprint(diag[5])?diag[5]:'.',
            isprint(diag[6])?diag[6]:'.', isprint(diag[7])?diag[7]:'.');
    }
    }
    found_pvd:;

    for (uint32_t vd_sector = first_pvd_sector;
         vd_sector < first_pvd_sector + 64; vd_sector++) {
        if (!iso_read_sector(m, vd_sector, pvd_buf, 1)) break;

        ISO_PVD *pvd = (ISO_PVD*)pvd_buf;
        if (memcmp(pvd->id, "CD001", 5) != 0) {
            /* Try next sector - multi-session gap */
            continue;
        }

        if (pvd->type == ISO_VD_TERMINATOR) break;

        if (pvd->type == ISO_VD_PRIMARY && !found_primary) {
            ISO_DirRecord *root = (ISO_DirRecord*)pvd->root_dir_record;
            root_lba   = root->extent_le;
            root_size  = root->data_length_le;
            found_primary = true;
            loader_log(LOG_INFO, "ISO Primary VD: root LBA=%u size=%u",
                       root_lba, root_size);
        }

        if (pvd->type == ISO_VD_SUPPLEMENTARY) {
            /* Check for Joliet: escape sequences %/@, %/C, or %/E */
            if (pvd->escape_sequences[0] == 0x25 &&
                pvd->escape_sequences[1] == 0x2F) {
                ISO_DirRecord *root = (ISO_DirRecord*)pvd->root_dir_record;
                joliet_lba   = root->extent_le;
                joliet_size  = root->data_length_le;
                m->has_joliet = true;
                loader_log(LOG_INFO, "ISO Joliet SVD found: root LBA=%u", joliet_lba);
            }
        }
    }
    free(pvd_buf);

    if (!found_primary) {
        loader_log(LOG_ERROR, "No Primary Volume Descriptor found in ISO 9660 chain");
        CloseHandle(fh);
        free(m->entries);
        free(m);
        return NULL;
    }

    m->root_lba  = root_lba;
    m->root_size = root_size;

    /* Prefer Joliet for proper filenames, fall back to ISO 9660 */
    add_entry(m, "/", root_lba, root_size, true);

    if (m->has_joliet) {
        loader_log(LOG_INFO, "Scanning Joliet directory tree...");
        scan_directory(m, "/", joliet_lba, joliet_size, true, 0);
    } else {
        loader_log(LOG_INFO, "Scanning ISO 9660 directory tree...");
        scan_directory(m, "/", root_lba, root_size, false, 0);
    }

    loader_log(LOG_INFO, "ISO mounted: %d entries", m->entry_count);
    g_mount = m;
    return m;
}

/* ------------------------------------------------------------------ */
/* Find an entry by path                                               */
/* ------------------------------------------------------------------ */
static IsoEntry *iso_find(IsoMount *m, const char *path)
{
    if (!m || !path) return NULL;

    /* Normalize: ensure leading slash, no trailing slash */
    char norm[ISO_MAX_PATH];
    if (path[0] != '/') snprintf(norm, ISO_MAX_PATH, "/%s", path);
    else strncpy(norm, path, ISO_MAX_PATH-1);

    size_t len = strlen(norm);
    if (len > 1 && norm[len-1] == '/') norm[len-1] = '\0';

    /* Case-insensitive search */
    for (int i = 0; i < m->entry_count; i++) {
        if (_stricmp(m->entries[i].path, norm) == 0)
            return &m->entries[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Find the game ELF inside the ISO                                    */
/* ------------------------------------------------------------------ */
static const char *ELF_SEARCH_PATHS[] = {
    "/disk0/abc",
    "/disk0/game",
    "/game/abc",
    "/game/game",
    "/lindbergh/game",
    "/abc",
    NULL
};

const char *iso_find_elf(IsoMount *m)
{
    /* Check known Lindbergh ELF paths */
    for (int i = 0; ELF_SEARCH_PATHS[i]; i++) {
        IsoEntry *e = iso_find(m, ELF_SEARCH_PATHS[i]);
        if (e && !e->is_dir) {
            loader_log(LOG_INFO, "Found game ELF: %s (%u bytes)",
                       e->path, e->size);
            return e->path;
        }
    }

    /* Fallback: scan for any ELF by magic bytes */
    static const uint8_t ELF_MAGIC[] = {0x7F, 'E', 'L', 'F'};
    uint8_t header[4];

    for (int i = 0; i < m->entry_count; i++) {
        IsoEntry *e = &m->entries[i];
        if (e->is_dir || e->size < 64) continue;
        /* Skip obvious non-executables */
        const char *ext = strrchr(e->path, '.');
        if (ext && (_stricmp(ext, ".so") == 0 ||
                    _stricmp(ext, ".dat") == 0 ||
                    _stricmp(ext, ".bin") == 0)) continue;

        if (iso_read_bytes(m, e->lba, 0, header, 4) &&
            memcmp(header, ELF_MAGIC, 4) == 0) {
            loader_log(LOG_INFO, "Found ELF by magic: %s (%u bytes)",
                       e->path, e->size);
            return e->path;
        }
    }

    loader_log(LOG_ERROR, "No ELF found in ISO");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Extract ELF from ISO into heap buffer                              */
/* ------------------------------------------------------------------ */
uint8_t *iso_extract_elf(IsoMount *m, const char *iso_path,
                          size_t *out_size)
{
    IsoEntry *e = iso_find(m, iso_path);
    if (!e || e->is_dir) {
        loader_log(LOG_ERROR, "Cannot extract: %s", iso_path);
        return NULL;
    }

    uint8_t *buf = malloc(e->size);
    if (!buf) {
        loader_log(LOG_ERROR, "Out of memory for ELF (%u bytes)", e->size);
        return NULL;
    }

    loader_log(LOG_INFO, "Extracting %s (%u bytes from LBA %u)...",
               iso_path, e->size, e->lba);

    /* Read in sector-aligned chunks */
    uint32_t sectors  = (e->size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    uint32_t full     = e->size / ISO_SECTOR_SIZE;
    uint32_t leftover = e->size % ISO_SECTOR_SIZE;
    uint8_t *ptr      = buf;

    if (full > 0) {
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)e->lba * ISO_SECTOR_SIZE;
        SetFilePointerEx(m->file, pos, NULL, FILE_BEGIN);
        DWORD got = 0;
        if (!ReadFile(m->file, ptr, full * ISO_SECTOR_SIZE, &got, NULL)) {
            loader_log(LOG_ERROR, "Read failed: %lu", GetLastError());
            free(buf); return NULL;
        }
        ptr += full * ISO_SECTOR_SIZE;
    }

    if (leftover > 0) {
        uint8_t tmp[ISO_SECTOR_SIZE];
        if (iso_read_sector(m, e->lba + full, tmp, 1))
            memcpy(ptr, tmp, leftover);
    }

    loader_log(LOG_INFO, "ELF extracted: %u bytes", e->size);
    *out_size = e->size;
    return buf;
}

/* ------------------------------------------------------------------ */
/* VFS integration: open() calls to ISO paths                         */
/* ------------------------------------------------------------------ */
/*
 * When the game opens a file like "/disk0/data/stage1.dat",
 * we intercept it here and return an ISO-backed virtual fd.
 *
 * We use a simple iso-fd table layered on top of the VFS fd table.
 */
#define ISO_VFD_BASE  0x5000
#define ISO_VFD_MAX   64

typedef struct {
    bool     in_use;
    IsoEntry entry;    /* copy of the entry */
    uint32_t pos;      /* current read position */
} IsoVfd;

static IsoVfd g_iso_vfds[ISO_VFD_MAX];

bool iso_is_isofd(int fd)
{
    int idx = fd - ISO_VFD_BASE;
    return idx >= 0 && idx < ISO_VFD_MAX && g_iso_vfds[idx].in_use;
}

int iso_vfs_open(const char *path)
{
    if (!g_mount) return -1;
    IsoEntry *e = iso_find(g_mount, path);
    if (!e || e->is_dir) return -1;

    for (int i = 0; i < ISO_VFD_MAX; i++) {
        if (!g_iso_vfds[i].in_use) {
            g_iso_vfds[i].in_use = true;
            g_iso_vfds[i].entry  = *e;
            g_iso_vfds[i].pos    = 0;
            loader_log(LOG_DEBUG, "ISO open: %s -> isofd %d", path, ISO_VFD_BASE+i);
            return ISO_VFD_BASE + i;
        }
    }
    return -1;
}

int64_t iso_vfs_read(int fd, void *buf, size_t count)
{
    int idx = fd - ISO_VFD_BASE;
    if (idx < 0 || idx >= ISO_VFD_MAX || !g_iso_vfds[idx].in_use) return -9;

    IsoVfd    *vfd  = &g_iso_vfds[idx];
    IsoEntry  *e    = &vfd->entry;

    if (vfd->pos >= e->size) return 0;

    uint32_t avail = e->size - vfd->pos;
    uint32_t n     = (uint32_t)((count < avail) ? count : avail);

    if (!iso_read_bytes(g_mount, e->lba, vfd->pos, buf, n)) return -5;

    vfd->pos += n;
    return (int64_t)n;
}

int64_t iso_vfs_lseek(int fd, int64_t off, int whence)
{
    int idx = fd - ISO_VFD_BASE;
    if (idx < 0 || idx >= ISO_VFD_MAX || !g_iso_vfds[idx].in_use) return -9;

    IsoVfd *vfd = &g_iso_vfds[idx];
    uint32_t sz = vfd->entry.size;

    uint32_t new_pos;
    switch (whence) {
    case 0: new_pos = (uint32_t)off; break;                        /* SEEK_SET */
    case 1: new_pos = (uint32_t)((int64_t)vfd->pos + off); break;  /* SEEK_CUR */
    case 2: new_pos = (uint32_t)((int64_t)sz + off); break;        /* SEEK_END */
    default: return -22;
    }

    if (new_pos > sz) new_pos = sz;
    vfd->pos = new_pos;
    return (int64_t)new_pos;
}

int iso_vfs_fstat(int fd, Linux_Stat *st)
{
    int idx = fd - ISO_VFD_BASE;
    if (idx < 0 || idx >= ISO_VFD_MAX || !g_iso_vfds[idx].in_use) return -9;

    memset(st, 0, sizeof(*st));
    st->st_size    = g_iso_vfds[idx].entry.size;
    st->st_mode    = 0100444; /* regular, read-only */
    st->st_blksize = ISO_SECTOR_SIZE;
    st->st_blocks  = (g_iso_vfds[idx].entry.size + 511) / 512;
    return 0;
}

int iso_vfs_close(int fd)
{
    int idx = fd - ISO_VFD_BASE;
    if (idx < 0 || idx >= ISO_VFD_MAX) return -9;
    g_iso_vfds[idx].in_use = false;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Check if a path exists in the ISO                                  */
/* ------------------------------------------------------------------ */
bool iso_path_exists(const char *path)
{
    return g_mount && iso_find(g_mount, path) != NULL;
}

/* ------------------------------------------------------------------ */
/* List all ISO paths (for debugging)                                 */
/* ------------------------------------------------------------------ */
void iso_list(IsoMount *m)
{
    printf("\nISO Contents (%d entries):\n", m->entry_count);
    for (int i = 0; i < m->entry_count; i++) {
        IsoEntry *e = &m->entries[i];
        if (e->is_dir)
            printf("  DIR  %s\n", e->path);
        else
            printf("  FILE %s (%u bytes)\n", e->path, e->size);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Unmount                                                             */
/* ------------------------------------------------------------------ */
void iso_unmount(IsoMount *m)
{
    if (!m) return;
    if (m->file != INVALID_HANDLE_VALUE) CloseHandle(m->file);
    free(m->entries);
    free(m);
    if (g_mount == m) g_mount = NULL;
}

/* ------------------------------------------------------------------ */
/* Get global mount (for use by VFS layer)                            */
/* ------------------------------------------------------------------ */
IsoMount *iso_get_mount(void) { return g_mount; }
