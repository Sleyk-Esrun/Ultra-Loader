#include <stdint.h>
/*
 * shmem.c
 * Shared Memory Semantic Layer
 *
 * Linux shared memory comes in two flavors:
 *   1. SysV IPC: shmget() / shmat() / shmdt() / shmctl()
 *   2. POSIX:    shm_open() / mmap(MAP_SHARED)
 *
 * Lindbergh uses both. The game process and the system/watchdog process
 * communicate via shared segments. We map both to Win32 named file
 * mappings (CreateFileMapping / MapViewOfFile).
 *
 * SysV keys are translated to Win32 named objects as "LBRGH_shm_%d".
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "shmem.h"

/* ------------------------------------------------------------------ */
/* SysV IPC table                                                      */
/* ------------------------------------------------------------------ */
#define SHMEM_MAX_SEGMENTS  64
#define IPC_PRIVATE         0
#define IPC_CREAT           0001000
#define IPC_EXCL            0002000
#define IPC_RMID            0

typedef struct {
    int     shmid;
    key_t   key;
    size_t  size;
    HANDLE  mapping;
    bool    in_use;
} ShmSegment;

static ShmSegment g_shm_segs[SHMEM_MAX_SEGMENTS];
static int        g_next_shmid = 1;

static ShmSegment *shm_find_by_key(key_t key)
{
    for (int i = 0; i < SHMEM_MAX_SEGMENTS; i++)
        if (g_shm_segs[i].in_use && g_shm_segs[i].key == key)
            return &g_shm_segs[i];
    return NULL;
}

static ShmSegment *shm_find_by_id(int shmid)
{
    for (int i = 0; i < SHMEM_MAX_SEGMENTS; i++)
        if (g_shm_segs[i].in_use && g_shm_segs[i].shmid == shmid)
            return &g_shm_segs[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* shmget                                                               */
/* ------------------------------------------------------------------ */
int shmem_shmget(key_t key, size_t size, int shmflg)
{
    /* Look for existing segment with this key */
    if (key != IPC_PRIVATE) {
        ShmSegment *existing = shm_find_by_key(key);
        if (existing) {
            if ((shmflg & IPC_CREAT) && (shmflg & IPC_EXCL)) return -17; /* EEXIST */
            return existing->shmid;
        }
    }

    if (!(shmflg & IPC_CREAT) && key != IPC_PRIVATE) return -2; /* ENOENT */

    /* Find a free slot */
    ShmSegment *seg = NULL;
    for (int i = 0; i < SHMEM_MAX_SEGMENTS; i++) {
        if (!g_shm_segs[i].in_use) { seg = &g_shm_segs[i]; break; }
    }
    if (!seg) { loader_log(LOG_ERROR, "shmget: segment table full"); return -28; }

    /* Create Win32 named mapping */
    char name[64];
    snprintf(name, sizeof(name), "LBRGH_shm_%d", (int)key);

    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                   PAGE_READWRITE, 0, (DWORD)size, name);
    if (!h) {
        loader_log(LOG_ERROR, "shmget: CreateFileMapping failed: %lu", GetLastError());
        return -12; /* ENOMEM */
    }

    seg->shmid   = g_next_shmid++;
    seg->key     = key;
    seg->size    = size;
    seg->mapping = h;
    seg->in_use  = true;

    loader_log(LOG_DEBUG, "shmget key=%d -> shmid=%d size=%zu", (int)key, seg->shmid, size);
    return seg->shmid;
}

/* ------------------------------------------------------------------ */
/* shmat                                                                */
/* ------------------------------------------------------------------ */
void *shmem_shmat(int shmid, const void *shmaddr, int shmflg)
{
    ShmSegment *seg = shm_find_by_id(shmid);
    if (!seg) { loader_log(LOG_ERROR, "shmat: invalid shmid %d", shmid); return (void*)-1; }

    DWORD access = (shmflg & 0x1000) ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
    void *p = MapViewOfFileEx(seg->mapping, access, 0, 0, seg->size,
                               (LPVOID)shmaddr);
    if (!p) {
        loader_log(LOG_ERROR, "shmat: MapViewOfFile failed: %lu", GetLastError());
        return (void*)-1;
    }

    loader_log(LOG_DEBUG, "shmat shmid=%d -> addr=0x%p", shmid, p);
    return p;
}

/* ------------------------------------------------------------------ */
/* shmdt                                                                */
/* ------------------------------------------------------------------ */
int shmem_shmdt(const void *shmaddr)
{
    if (!UnmapViewOfFile((LPCVOID)shmaddr)) return -22;
    return 0;
}

/* ------------------------------------------------------------------ */
/* shmctl                                                               */
/* ------------------------------------------------------------------ */
int shmem_shmctl(int shmid, int cmd, void *buf)
{
    if (cmd == IPC_RMID) {
        ShmSegment *seg = shm_find_by_id(shmid);
        if (!seg) return -22;
        CloseHandle(seg->mapping);
        memset(seg, 0, sizeof(*seg));
        return 0;
    }
    /* IPC_STAT / IPC_SET - stub */
    return 0;
}

/* ------------------------------------------------------------------ */
/* POSIX shm_open                                                       */
/* ------------------------------------------------------------------ */
int shmem_shm_open(const char *name, int oflag, int mode)
{
    /* Strip leading / from POSIX name */
    if (name && name[0] == '/') name++;
    char wname[256];
    snprintf(wname, sizeof(wname), "LBRGH_posix_%s", name);


    /* We use a 0-size mapping first; ftruncate will resize */
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                   PAGE_READWRITE, 0, 4096, wname);
    if (!h) return -2;

    /* Allocate a fake fd slot in shm table */
    ShmSegment *seg = NULL;
    for (int i = 0; i < SHMEM_MAX_SEGMENTS; i++) {
        if (!g_shm_segs[i].in_use) { seg = &g_shm_segs[i]; break; }
    }
    if (!seg) { CloseHandle(h); return -28; }

    seg->shmid   = 0x8000 + g_next_shmid++;
    seg->key     = 0;
    seg->size    = 4096;
    seg->mapping = h;
    seg->in_use  = true;

    return seg->shmid;
}

/* ------------------------------------------------------------------ */
/* shm_unlink                                                           */
/* ------------------------------------------------------------------ */
int shmem_shm_unlink(const char *name)
{
    /* Win32 named objects are reference-counted; just return success */
    return 0;
}

/* ------------------------------------------------------------------ */
/* mmap MAP_SHARED (file-backed or anonymous)                         */
/* ------------------------------------------------------------------ */
/* Handled in syscall_dispatch.c for the anonymous case.
 * File-backed MAP_SHARED for a real file fd is done there too via
 * CreateFileMapping + MapViewOfFile. This file handles the shm_open fd case. */

void *shmem_map_shared(int shmid, size_t length, int prot, int64_t offset)
{
    ShmSegment *seg = shm_find_by_id(shmid);
    if (!seg) return NULL;

    DWORD access = FILE_MAP_ALL_ACCESS;
    void *p = MapViewOfFile(seg->mapping, access,
                             (DWORD)((uint64_t)offset >> 32), (DWORD)(offset & 0xFFFFFFFF), length);
    return p;
}
