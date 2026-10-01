/*
 * nvidia_stub.c
 * NVIDIA Linux Driver ioctl Stub Layer
 *
 * Lindbergh games communicate with the GPU through two paths:
 *
 *   1. OpenGL via libGL.so → our glx_bridge (already handled)
 *   2. Direct NVIDIA driver ioctls on /dev/nvidia0, /dev/nvidiactl,
 *      /dev/nvidia-modeset for things like:
 *        - GPU power management
 *        - Memory allocation (NV_ESC_ALLOC_OS_EVENT)
 *        - RM (Resource Manager) control calls
 *        - Display mode setting
 *
 * The NVIDIA Linux driver ioctl protocol is NOT the same as Windows.
 * We can't translate these to Windows NVIDIA API calls because:
 *   a) The Windows NVIDIA driver uses a completely different internal ABI
 *   b) Most of what Lindbergh uses (RM control) is handled transparently
 *      by the Windows OpenGL ICD before games ever see it
 *
 * Strategy: return plausible success values for every NVIDIA ioctl.
 * The game has already gotten a real OpenGL context via our WGL bridge,
 * so the GPU is actually working. These ioctl calls are for housekeeping
 * that the Windows driver handles internally.
 *
 * NVIDIA Linux ioctl numbers (from kernel module source / reverse engineering):
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "loader.h"
#include "nvidia_stub.h"

/* ------------------------------------------------------------------ */
/* NVIDIA ioctl request codes                                          */
/* ------------------------------------------------------------------ */

/* NV control ioctls (nvidiactl device) */
#define NV_ESC_CARD_INFO            0x00
#define NV_ESC_ENV_INFO             0x01
#define NV_ESC_ALLOC_OS_EVENT       0x09
#define NV_ESC_FREE_OS_EVENT        0x0A
#define NV_ESC_STATUS_CODE          0x0C
#define NV_ESC_CHECK_VERSION_STR    0x0D
#define NV_ESC_IOCTL_XFER           0x10
#define NV_ESC_RM_ALLOC_MEMORY      0x27
#define NV_ESC_RM_ALLOC_OBJECT      0x28
#define NV_ESC_RM_FREE              0x29
#define NV_ESC_RM_CONTROL           0x2A
#define NV_ESC_RM_ALLOC             0x2B
#define NV_ESC_RM_DUP_OBJECT        0x2C
#define NV_ESC_RM_SHARE             0x2D
#define NV_ESC_RM_VID_HEAP_CONTROL  0x2E
#define NV_ESC_RM_MAP_MEMORY        0x2F
#define NV_ESC_RM_UNMAP_MEMORY      0x30
#define NV_ESC_RM_UPDATE_DEVICE_MAPPING 0x31
#define NV_ESC_RM_GET_EVENT_DATA    0x32
#define NV_ESC_RM_ALLOC_CONTEXT_DMA2 0x33
#define NV_ESC_RM_MAP_MEMORY2       0x34
#define NV_ESC_AGP_INIT_HEAP        0x37
#define NV_ESC_REGISTER_FD          0x39
#define NV_ESC_ALLOC_UNIX_EV        0x40

/* NV_ESC_CHECK_VERSION_STR response */
#define NV_VERSION_STRING "256.53"  /* Lindbergh era NVIDIA driver version */

/* RM control codes (subset used by Lindbergh era games) */
#define NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS   0x00000301
#define NV0000_CTRL_CMD_GPU_GET_ID_INFO        0x00000303
#define NV2080_CTRL_CMD_FB_GET_INFO            0x20801301
#define NV2080_CTRL_CMD_GPU_GET_INFO           0x20800101
#define NV2080_CTRL_CMD_TIMER_GET_TIME         0x00004701

/* ------------------------------------------------------------------ */
/* NVIDIA ioctl structures (minimal, enough to fake responses)         */
/* ------------------------------------------------------------------ */

/* Generic NVIDIA RM control params header */
typedef struct {
    uint32_t hClient;
    uint32_t hObject;
    uint32_t cmd;
    uint32_t flags;
    uint64_t params;      /* pointer to cmd-specific params */
    uint32_t paramsSize;
    uint32_t status;
} NVOS54_PARAMETERS;

/* Card info */
typedef struct {
    uint32_t domain;
    uint16_t bus;
    uint16_t slot;
    uint32_t vendorId;
    uint32_t deviceId;
    uint8_t  uuid[16];
    uint32_t subsystemVendorId;
    uint32_t subsystemId;
    uint32_t gpuId;
    uint32_t isMobile;
    uint32_t pciDeviceId;
} NV_IOCTL_CARD_INFO;

/* Version check */
typedef struct {
    char     versionString[64];
    uint32_t reply; /* 0 = match, 1 = mismatch */
} NV_CHECK_VERSION;

/* OS event alloc */
typedef struct {
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hOsEvent;
    uint32_t fd;
    uint32_t status;
} NV_ALLOC_OS_EVENT;

/* ------------------------------------------------------------------ */
/* Fake resource handle allocator                                      */
/* ------------------------------------------------------------------ */
static uint32_t g_next_handle = 0xDEAD0001;

static uint32_t alloc_rm_handle(void)
{
    return g_next_handle++;
}

/* ------------------------------------------------------------------ */
/* RM control command dispatcher                                       */
/* ------------------------------------------------------------------ */
static int nv_rm_control(NVOS54_PARAMETERS *p)
{
    loader_log(LOG_DEBUG, "  NV_RM_CONTROL cmd=0x%08x hClient=0x%x hObj=0x%x",
               p->cmd, p->hClient, p->hObject);

    switch (p->cmd) {

    case NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS: {
        /* Return one GPU attached */
        if (p->params) {
            uint32_t *ids = (uint32_t*)(uintptr_t)p->params;
            memset(ids, 0xFF, p->paramsSize); /* 0xFFFFFFFF = invalid sentinel */
            ids[0] = 0; /* GPU 0 attached */
        }
        p->status = 0;
        return 0;
    }

    case NV0000_CTRL_CMD_GPU_GET_ID_INFO: {
        if (p->params) {
            uint32_t *info = (uint32_t*)(uintptr_t)p->params;
            info[0] = 0x00000000; /* gpuId */
            info[1] = 0x10DE;     /* vendorId: NVIDIA */
            info[2] = 0x0191;     /* deviceId: GeForce 8800 GTX (Lindbergh GPU) */
        }
        p->status = 0;
        return 0;
    }

    case NV2080_CTRL_CMD_FB_GET_INFO: {
        /* Fake 256MB framebuffer */
        if (p->params && p->paramsSize >= 8) {
            uint32_t *fb = (uint32_t*)(uintptr_t)p->params;
            fb[0] = 256 * 1024; /* total FB in KB */
            fb[1] = 128 * 1024; /* available KB */
        }
        p->status = 0;
        return 0;
    }

    case NV2080_CTRL_CMD_TIMER_GET_TIME: {
        if (p->params && p->paramsSize >= 8) {
            LARGE_INTEGER freq, cnt;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&cnt);
            uint64_t ns = (uint64_t)(cnt.QuadPart * 1000000000LL / freq.QuadPart);
            *(uint64_t*)(uintptr_t)p->params = ns;
        }
        p->status = 0;
        return 0;
    }

    default:
        loader_log(LOG_DEBUG, "  NV_RM_CONTROL: unhandled cmd=0x%08x (returning success)", p->cmd);
        p->status = 0;
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* NVIDIA RM alloc                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    uint64_t pAllocParms;
    uint32_t status;
} NVOS21_PARAMETERS;

static int nv_rm_alloc(NVOS21_PARAMETERS *p)
{
    loader_log(LOG_DEBUG, "  NV_RM_ALLOC hClass=0x%x", p->hClass);
    p->hObjectNew = p->hObjectNew ? p->hObjectNew : alloc_rm_handle();
    p->status = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main NVIDIA ioctl dispatcher                                        */
/* ------------------------------------------------------------------ */
int64_t nvidia_ioctl(int fd, uint64_t req, void *arg)
{
    uint32_t nr = (uint32_t)(req & 0xFF);

    loader_log(LOG_DEBUG, "nvidia_ioctl fd=%d req=0x%llx nr=0x%02x",
               fd, (unsigned long long)req, nr);

    switch (nr) {

    case NV_ESC_CHECK_VERSION_STR: {
        NV_CHECK_VERSION *v = (NV_CHECK_VERSION*)arg;
        if (v) {
            /* Copy our fake version string into the request */
            strncpy(v->versionString, NV_VERSION_STRING,
                    sizeof(v->versionString) - 1);
            v->reply = 0; /* 0 = version match */
        }
        return 0;
    }

    case NV_ESC_CARD_INFO: {
        NV_IOCTL_CARD_INFO *ci = (NV_IOCTL_CARD_INFO*)arg;
        if (ci) {
            memset(ci, 0, sizeof(*ci));
            ci->vendorId  = 0x10DE; /* NVIDIA */
            ci->deviceId  = 0x0191; /* GeForce 8800 GTX */
            ci->bus       = 1;
            ci->slot      = 0;
            ci->gpuId     = 0;
        }
        return 0;
    }

    case NV_ESC_ENV_INFO: {
        /* pat_supported = 1 */
        if (arg) *(uint32_t*)arg = 1;
        return 0;
    }

    case NV_ESC_ALLOC_OS_EVENT:
    case NV_ESC_ALLOC_UNIX_EV: {
        NV_ALLOC_OS_EVENT *e = (NV_ALLOC_OS_EVENT*)arg;
        if (e) {
            e->hOsEvent = alloc_rm_handle();
            e->status   = 0;
        }
        return 0;
    }

    case NV_ESC_FREE_OS_EVENT:
        return 0;

    case NV_ESC_STATUS_CODE:
        if (arg) *(uint32_t*)arg = 0; /* NV_OK */
        return 0;

    case NV_ESC_RM_CONTROL: {
        NVOS54_PARAMETERS *p = (NVOS54_PARAMETERS*)arg;
        if (p) return nv_rm_control(p);
        return 0;
    }

    case NV_ESC_RM_ALLOC:
    case NV_ESC_RM_ALLOC_OBJECT: {
        NVOS21_PARAMETERS *p = (NVOS21_PARAMETERS*)arg;
        if (p) return nv_rm_alloc(p);
        return 0;
    }

    case NV_ESC_RM_FREE:
        return 0;

    case NV_ESC_RM_MAP_MEMORY:
    case NV_ESC_RM_MAP_MEMORY2: {
        /* Map a fake GPU memory region */
        if (arg) {
            /* The map struct has a pointer field we should fill */
            /* Offset 16: pLinearAddress (pointer to mapped memory) */
            uintptr_t *paddr = (uintptr_t*)((uint8_t*)arg + 16);
            if (*paddr == 0) {
                void *fake = VirtualAlloc(NULL, 64*1024,
                                           MEM_COMMIT|MEM_RESERVE,
                                           PAGE_READWRITE);
                *paddr = (uintptr_t)fake;
            }
            /* status at end of struct */
            uint32_t *status = (uint32_t*)((uint8_t*)arg +
                               ((req >> 16) & 0x3FFF) - 4);
            *status = 0;
        }
        return 0;
    }

    case NV_ESC_RM_UNMAP_MEMORY:
        return 0;

    case NV_ESC_REGISTER_FD:
        return 0;

    case NV_ESC_IOCTL_XFER: {
        /* Wrapped ioctl - recurse with inner request */
        if (arg) {
            uint32_t inner_req = *(uint32_t*)arg;
            void    *inner_arg = *(void**)((uint8_t*)arg + 8);
            return nvidia_ioctl(fd, inner_req, inner_arg);
        }
        return 0;
    }

    default:
        loader_log(LOG_DEBUG, "nvidia_ioctl: unhandled nr=0x%02x (returning 0)", nr);
        return 0; /* Return success for unknown ioctls */
    }
}

/* ------------------------------------------------------------------ */
/* Is this fd an NVIDIA device?                                        */
/* ------------------------------------------------------------------ */
bool nvidia_is_device_path(const char *path)
{
    if (!path) return false;
    return (strncmp(path, "/dev/nvidia",  11) == 0 ||
            strncmp(path, "/dev/nv",       7) == 0 ||
            strcmp(path,  "/dev/dri/card0") == 0 ||
            strcmp(path,  "/dev/dri/renderD128") == 0);
}
