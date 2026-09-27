/*
 * Qualcomm KGSL (Kernel Graphics Support Layer) / Adreno 612 Virtual Device
 *
 * Emulates the Qualcomm KGSL hardware and ABI interface for Adreno 612 (A6xx)
 * on Snapdragon 675 (SM6150).
 *
 * Provides:
 *  - Adreno 6xx MMIO registers at 0x05090000 (size 0x10000)
 *  - GX and GMU interrupt lines (SPI 300, SPI 304)
 *  - KGSL ioctl dispatcher (GETPROPERTY, GPUOBJ_ALLOC, DRAWCTXT, GPU_COMMAND, etc.)
 *  - Shared memstore timestamp shadow page
 *  - GPU memory object manager
 *
 * Copyright (c) 2026 CustomAndroidEmulator project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "exec/address-spaces.h"

#define TYPE_QCOM_KGSL "qcom-kgsl"
OBJECT_DECLARE_SIMPLE_TYPE(QcomKgslState, QCOM_KGSL)

/* Address and Window Definitions */
#define KGSL_MMIO_SIZE          0x10000     /* 64 KB (0x05090000 - 0x050A0000) */
#define KGSL_MEMSTORE_SIZE      0x1000      /* 4 KB (1 page) */
#define KGSL_MAX_CONTEXTS       64
#define KGSL_MAX_GPUOBJS        512

/* Adreno 612 Hardware Identifiers (empirically decoded from libgsl.so / ROM) */
#define ADRENO_CHIPID_A612      0x06010200U /* Core 6, Major 1, Minor 2, Patch 0 */
#define ADRENO_GPUID_A612       612U        /* Adreno 612 */
#define ADRENO_GMEM_SIZE_A612   0x100000U   /* 1 MB GMEM */
#define ADRENO_GMEM_BASE_A612   0x100000ULL /* Base address for GMEM in UCHE */
#define ADRENO_MEMSTORE_GPUVA   0x00010000ULL /* Default GPU VA for memstore shadow */

/* A6XX Register Offsets (Byte offsets = dwords * 4) */
#define A6XX_RBBM_STATUS_BYTE        (0x210 * 4)  /* 0x840 */
#define A6XX_RBBM_CLOCK_CNTL_BYTE    (0x0ae * 4)  /* 0x2b8 */
#define A6XX_CP_HW_FAULT_BYTE        (0x821 * 4)  /* 0x2084 */
#define A6XX_CP_INT_STATUS_BYTE      (0x823 * 4)  /* 0x208c */
#define A6XX_CP_PROTECT_STATUS_BYTE  (0x824 * 4)  /* 0x2090 */
#define A6XX_CP_SQE_STAT_DATA_BYTE   (0x909 * 4)  /* 0x2424 */
#define A6XX_UCHE_CLIENT_PF_BYTE     (0xe19 * 4)  /* 0x3864 */

/* Dispatch Mailbox Registers (Offset 0x4000) */
#define KGSL_REG_MAGIC               0x4000
#define KGSL_REG_CMD                 0x4004
#define KGSL_REG_ARG_GPA_LO          0x4008
#define KGSL_REG_ARG_GPA_HI          0x400c
#define KGSL_REG_ARG_SIZE            0x4010
#define KGSL_REG_EXECUTE             0x4014
#define KGSL_REG_STATUS              0x4018
#define KGSL_REG_MEMSTORE_GPA_LO     0x4020
#define KGSL_REG_MEMSTORE_GPA_HI     0x4024

#define KGSL_MAGIC_VAL               0x4b47534cU /* "KGSL" */

/* KGSL Ioctl Numbers (_IOC_TYPE = 0x09) */
#define IOCTL_KGSL_DEVICE_GETPROPERTY               0xc0180902U
#define IOCTL_KGSL_DEVICE_WAITTIMESTAMP             0x40080906U
#define IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID      0x400c0907U
#define IOCTL_KGSL_RINGBUFFER_ISSUEIBCMDS           0xc0280910U
#define IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_OLD      0xc0080911U
#define IOCTL_KGSL_DRAWCTXT_CREATE                  0xc0080913U
#define IOCTL_KGSL_DRAWCTXT_DESTROY                 0x40040914U
#define IOCTL_KGSL_MAP_USER_MEM                     0xc0300915U
#define IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID   0xc00c0916U
#define IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP_CTXTID 0x40180917U
#define IOCTL_KGSL_SHAREDMEM_FREE                   0x40080921U
#define IOCTL_KGSL_SHAREDMEM_FLUSH_CACHE            0x40080924U
#define IOCTL_KGSL_SETPROPERTY                      0x40080925U
#define IOCTL_KGSL_TIMESTAMP_EVENT                  0xc0200933U
#define IOCTL_KGSL_GPUMEM_ALLOC_ID                  0xc0300934U
#define IOCTL_KGSL_GPUMEM_FREE_ID                   0xc0080935U
#define IOCTL_KGSL_GPUMEM_GET_INFO                  0xc0480936U
#define IOCTL_KGSL_GPUMEM_SYNC_CACHE                0x40200937U
#define IOCTL_KGSL_SUBMIT_COMMANDS                  0xc028093dU
#define IOCTL_KGSL_SYNCSOURCE_CREATE                0xc0100940U
#define IOCTL_KGSL_SYNCSOURCE_DESTROY               0xc0100941U
#define IOCTL_KGSL_SYNCSOURCE_CREATE_FENCE          0xc0180942U
#define IOCTL_KGSL_SYNCSOURCE_SIGNAL_FENCE          0xc0180943U
#define IOCTL_KGSL_GPUOBJ_ALLOC                     0xc0300945U
#define IOCTL_KGSL_GPUOBJ_FREE                      0x40200946U
#define IOCTL_KGSL_GPUOBJ_INFO                      0xc0300947U
#define IOCTL_KGSL_GPUOBJ_IMPORT                    0xc0200948U
#define IOCTL_KGSL_GPUOBJ_SYNC                      0x40100949U
#define IOCTL_KGSL_GPU_COMMAND                      0xc040094aU
#define IOCTL_KGSL_GPUOBJ_SET_INFO                  0x4020094cU

/* KGSL Property Enums */
#define KGSL_PROP_DEVICE_INFO                       0x01
#define KGSL_PROP_DEVICE_SHADOW                     0x02
#define KGSL_PROP_VERSION                           0x08
#define KGSL_PROP_UCHE_GMEM_VADDR                   0x13
#define KGSL_PROP_UCODE_VERSION                     0x15
#define KGSL_PROP_HIGHEST_BANK_BIT                  0x17
#define KGSL_PROP_DEVICE_BITNESS                    0x18
#define KGSL_PROP_DEVICE_QDSS_STM                   0x19
#define KGSL_PROP_MIN_ACCESS_LENGTH                 0x1A
#define KGSL_PROP_UBWC_MODE                         0x1B
#define KGSL_PROP_DEVICE_QTIMER                     0x20
#define KGSL_PROP_SECURE_BUFFER_ALIGNMENT           0x23
#define KGSL_PROP_SECURE_CTXT_SUPPORT               0x24
#define KGSL_PROP_SPEED_BIN                         0x25
#define KGSL_PROP_GAMING_BIN                        0x26

/* Structures matching Linux ARM64 64-bit ABI */
struct QEMU_PACKED kgsl_devmemstore {
    uint32_t soptimestamp;
    uint32_t sbz;
    uint32_t eoptimestamp;
    uint32_t sbz2;
    uint32_t preempted;
    uint32_t sbz3;
};

struct QEMU_PACKED kgsl_device_getproperty {
    uint32_t type;
    uint32_t __pad;
    uint64_t value;
    uint64_t sizebytes;
};

struct QEMU_PACKED kgsl_devinfo {
    uint32_t device_id;
    uint32_t chip_id;
    uint32_t mmu_enabled;
    uint32_t __pad;
    uint64_t gmem_gpubaseaddr;
    uint32_t gpu_id;
    uint32_t __pad2;
    uint64_t gmem_sizebytes;
};

struct QEMU_PACKED kgsl_shadowprop {
    uint64_t gpuaddr;
    uint64_t size;
    uint32_t flags;
    uint32_t __pad;
};

struct QEMU_PACKED kgsl_version {
    uint32_t drv_major;
    uint32_t drv_minor;
    uint32_t dev_major;
    uint32_t dev_minor;
};

struct QEMU_PACKED kgsl_ucode_version {
    uint32_t pfp;
    uint32_t pm4;
};

struct QEMU_PACKED kgsl_qdss_stm_prop {
    uint64_t gpuaddr;
    uint64_t size;
};

struct QEMU_PACKED kgsl_qtimer_prop {
    uint64_t gpuaddr;
    uint64_t size;
};

struct QEMU_PACKED kgsl_drawctxt_create {
    uint32_t flags;
    uint32_t drawctxt_id;
};

struct QEMU_PACKED kgsl_drawctxt_destroy {
    uint32_t drawctxt_id;
};

struct QEMU_PACKED kgsl_gpuobj_alloc {
    uint64_t size;
    uint64_t flags;
    uint64_t va_len;
    uint64_t mmapsize;
    uint32_t id;
    uint32_t metadata_len;
    uint64_t metadata;
};

struct QEMU_PACKED kgsl_gpuobj_free {
    uint64_t flags;
    uint64_t priv;
    uint32_t id;
    uint32_t type;
    uint32_t len;
};

struct QEMU_PACKED kgsl_gpuobj_info {
    uint64_t gpuaddr;
    uint64_t flags;
    uint64_t size;
    uint64_t va_len;
    uint64_t va_addr;
    uint32_t id;
};

struct QEMU_PACKED kgsl_gpuobj_import {
    uint64_t priv;
    uint64_t priv_len;
    uint64_t flags;
    uint32_t type;
    uint32_t id;
};

struct QEMU_PACKED kgsl_gpu_command {
    uint64_t flags;
    uint64_t cmdlist;
    uint32_t cmdsize;
    uint32_t numcmds;
    uint64_t objlist;
    uint32_t objsize;
    uint32_t numobjs;
    uint64_t synclist;
    uint32_t syncsize;
    uint32_t numsyncs;
    uint32_t context_id;
    uint32_t timestamp;
};

struct QEMU_PACKED kgsl_device_waittimestamp_ctxtid {
    uint32_t context_id;
    uint32_t timestamp;
    uint32_t timeout;
};

struct QEMU_PACKED kgsl_cmdstream_readtimestamp_ctxtid {
    uint32_t context_id;
    uint32_t type;
    uint32_t timestamp;
};

/* Context and GPU Memory Management */
struct QcomKgslContext {
    bool active;
    uint32_t id;
    uint32_t flags;
    uint32_t timestamp;
};

struct QcomKgslGpuObj {
    bool active;
    uint32_t id;
    uint64_t size;
    uint64_t flags;
    uint64_t gpuaddr;
    uint64_t mmapsize;
    void *host_ptr;
    bool is_imported;
};

struct QcomKgslState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    MemoryRegion memstore_mr;

    qemu_irq gx_irq;
    qemu_irq gmu_irq;

    /* MMIO Register array */
    uint32_t regs[KGSL_MMIO_SIZE / 4];

    /* Mailbox registers */
    uint32_t mb_cmd;
    uint64_t mb_arg_gpa;
    uint32_t mb_arg_size;
    int32_t  mb_status;

    /* Contexts & GPU Object tables */
    struct QcomKgslContext contexts[KGSL_MAX_CONTEXTS];
    struct QcomKgslGpuObj gpu_objs[KGSL_MAX_GPUOBJS];

    uint32_t next_context_id;
    uint32_t next_obj_id;
    uint64_t next_gpu_va;
    uint32_t global_timestamp;
};

/* Forward declaration */
static int qcom_kgsl_execute_ioctl(QcomKgslState *s);

/* Memstore Management: Updates completion timestamp for a context */
static void qcom_kgsl_update_memstore(QcomKgslState *s, uint32_t context_id, uint32_t timestamp)
{
    if (context_id >= KGSL_MAX_CONTEXTS) {
        return;
    }

    struct kgsl_devmemstore *store = (struct kgsl_devmemstore *)memory_region_get_ram_ptr(&s->memstore_mr);
    if (store) {
        store[context_id].eoptimestamp = timestamp;
    }
}

/* Property Query Handler (IOCTL_KGSL_DEVICE_GETPROPERTY) */
static int qcom_kgsl_getproperty(QcomKgslState *s, uint32_t type, void *out_buf, size_t sizebytes)
{
    if (!out_buf || sizebytes == 0) {
        return -EINVAL;
    }

    switch (type) {
    case KGSL_PROP_DEVICE_INFO: {
        struct kgsl_devinfo info;
        memset(&info, 0, sizeof(info));
        info.device_id = 0;
        info.chip_id = ADRENO_CHIPID_A612;
        info.mmu_enabled = 1;
        info.gmem_gpubaseaddr = ADRENO_GMEM_BASE_A612;
        info.gpu_id = ADRENO_GPUID_A612;
        info.gmem_sizebytes = ADRENO_GMEM_SIZE_A612;
        memcpy(out_buf, &info, MIN(sizebytes, sizeof(info)));
        return 0;
    }
    case KGSL_PROP_DEVICE_SHADOW: {
        struct kgsl_shadowprop shadow;
        memset(&shadow, 0, sizeof(shadow));
        shadow.gpuaddr = ADRENO_MEMSTORE_GPUVA;
        shadow.size = KGSL_MEMSTORE_SIZE;
        shadow.flags = 0x01; /* KGSL_FLAGS_INITIALIZED */
        memcpy(out_buf, &shadow, MIN(sizebytes, sizeof(shadow)));
        return 0;
    }
    case KGSL_PROP_VERSION: {
        struct kgsl_version ver;
        ver.drv_major = 3;
        ver.drv_minor = 1;
        ver.dev_major = 3;
        ver.dev_minor = 1;
        memcpy(out_buf, &ver, MIN(sizebytes, sizeof(ver)));
        return 0;
    }
    case KGSL_PROP_UCHE_GMEM_VADDR: {
        uint64_t gmem_vaddr = ADRENO_GMEM_BASE_A612;
        memcpy(out_buf, &gmem_vaddr, MIN(sizebytes, sizeof(gmem_vaddr)));
        return 0;
    }
    case KGSL_PROP_UCODE_VERSION: {
        struct kgsl_ucode_version ucode;
        ucode.pfp = 1;
        ucode.pm4 = 1;
        memcpy(out_buf, &ucode, MIN(sizebytes, sizeof(ucode)));
        return 0;
    }
    case KGSL_PROP_HIGHEST_BANK_BIT: {
        uint32_t bank_bit = 15; /* LPDDR4x DRAM bank bit */
        memcpy(out_buf, &bank_bit, MIN(sizebytes, sizeof(bank_bit)));
        return 0;
    }
    case KGSL_PROP_DEVICE_BITNESS: {
        uint32_t bitness = 64; /* 64-bit GPU virtual address architecture */
        memcpy(out_buf, &bitness, MIN(sizebytes, sizeof(bitness)));
        return 0;
    }
    case KGSL_PROP_DEVICE_QDSS_STM: {
        struct kgsl_qdss_stm_prop qdss;
        memset(&qdss, 0, sizeof(qdss));
        memcpy(out_buf, &qdss, MIN(sizebytes, sizeof(qdss)));
        return 0;
    }
    case KGSL_PROP_MIN_ACCESS_LENGTH: {
        uint32_t min_len = 32;
        memcpy(out_buf, &min_len, MIN(sizebytes, sizeof(min_len)));
        return 0;
    }
    case KGSL_PROP_UBWC_MODE: {
        uint32_t ubwc = 2; /* UBWC 2.0 */
        memcpy(out_buf, &ubwc, MIN(sizebytes, sizeof(ubwc)));
        return 0;
    }
    case KGSL_PROP_DEVICE_QTIMER: {
        struct kgsl_qtimer_prop qtimer;
        qtimer.gpuaddr = 0x20000;
        qtimer.size = 4096;
        memcpy(out_buf, &qtimer, MIN(sizebytes, sizeof(qtimer)));
        return 0;
    }
    case KGSL_PROP_SECURE_BUFFER_ALIGNMENT: {
        uint32_t align = 0x100000; /* 1 MB */
        memcpy(out_buf, &align, MIN(sizebytes, sizeof(align)));
        return 0;
    }
    case KGSL_PROP_SECURE_CTXT_SUPPORT: {
        uint32_t sec = 1;
        memcpy(out_buf, &sec, MIN(sizebytes, sizeof(sec)));
        return 0;
    }
    case KGSL_PROP_SPEED_BIN: {
        uint32_t bin = 0;
        memcpy(out_buf, &bin, MIN(sizebytes, sizeof(bin)));
        return 0;
    }
    case KGSL_PROP_GAMING_BIN: {
        uint32_t gbin = 0;
        memcpy(out_buf, &gbin, MIN(sizebytes, sizeof(gbin)));
        return 0;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "qcom-kgsl: unhandled property query type=0x%x\n", type);
        return -EINVAL;
    }
}

/* MMIO Access Handlers */
static uint64_t qcom_kgsl_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomKgslState *s = opaque;
    uint32_t idx = offset / 4;

    if (offset >= KGSL_MMIO_SIZE) {
        return 0;
    }

    /* Mailbox registers */
    switch (offset) {
    case KGSL_REG_MAGIC:
        return KGSL_MAGIC_VAL;
    case KGSL_REG_CMD:
        return s->mb_cmd;
    case KGSL_REG_ARG_GPA_LO:
        return (uint32_t)(s->mb_arg_gpa & 0xffffffff);
    case KGSL_REG_ARG_GPA_HI:
        return (uint32_t)(s->mb_arg_gpa >> 32);
    case KGSL_REG_ARG_SIZE:
        return s->mb_arg_size;
    case KGSL_REG_STATUS:
        return (uint32_t)s->mb_status;
    case KGSL_REG_MEMSTORE_GPA_LO:
        return 0x050a0000U;
    case KGSL_REG_MEMSTORE_GPA_HI:
        return 0;

    /* A6xx GPU status registers */
    case A6XX_RBBM_STATUS_BYTE:
    case A6XX_RBBM_CLOCK_CNTL_BYTE:
    case A6XX_CP_HW_FAULT_BYTE:
    case A6XX_CP_INT_STATUS_BYTE:
    case A6XX_CP_PROTECT_STATUS_BYTE:
    case A6XX_CP_SQE_STAT_DATA_BYTE:
    case A6XX_UCHE_CLIENT_PF_BYTE:
        return 0; /* Hardware status registers: 0 indicates idle and healthy */
    default:
        return s->regs[idx];
    }
}

static void qcom_kgsl_mmio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    QcomKgslState *s = opaque;
    uint32_t idx = offset / 4;

    if (offset >= KGSL_MMIO_SIZE) {
        return;
    }

    switch (offset) {
    case KGSL_REG_CMD:
        s->mb_cmd = (uint32_t)value;
        break;
    case KGSL_REG_ARG_GPA_LO:
        s->mb_arg_gpa = (s->mb_arg_gpa & 0xffffffff00000000ULL) | (uint32_t)value;
        break;
    case KGSL_REG_ARG_GPA_HI:
        s->mb_arg_gpa = (s->mb_arg_gpa & 0x00000000ffffffffULL) | (((uint64_t)value) << 32);
        break;
    case KGSL_REG_ARG_SIZE:
        s->mb_arg_size = (uint32_t)value;
        break;
    case KGSL_REG_EXECUTE:
        if (value == 1) {
            s->mb_status = qcom_kgsl_execute_ioctl(s);
        }
        break;
    default:
        s->regs[idx] = (uint32_t)value;
        break;
    }
}

static const MemoryRegionOps qcom_kgsl_mmio_ops = {
    .read = qcom_kgsl_mmio_read,
    .write = qcom_kgsl_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* Unified Ioctl Execution Engine */
static int qcom_kgsl_execute_ioctl(QcomKgslState *s)
{
    uint32_t cmd = s->mb_cmd;
    hwaddr gpa = s->mb_arg_gpa;
    uint32_t size = s->mb_arg_size;

    if (!gpa || size == 0) {
        return -EINVAL;
    }

    hwaddr len = size;
    void *buf = cpu_physical_memory_map(gpa, &len, true);
    if (!buf || len < size) {
        if (buf) cpu_physical_memory_unmap(buf, len, true, 0);
        return -EFAULT;
    }

    int ret = 0;

    switch (cmd) {
    case IOCTL_KGSL_DEVICE_GETPROPERTY: {
        struct kgsl_device_getproperty *req = (struct kgsl_device_getproperty *)buf;
        if (req->value && req->sizebytes > 0) {
            hwaddr val_len = req->sizebytes;
            void *val_buf = cpu_physical_memory_map(req->value, &val_len, true);
            if (val_buf) {
                ret = qcom_kgsl_getproperty(s, req->type, val_buf, req->sizebytes);
                cpu_physical_memory_unmap(val_buf, val_len, true, val_len);
            } else {
                ret = -EFAULT;
            }
        } else {
            ret = -EINVAL;
        }
        break;
    }

    case IOCTL_KGSL_DRAWCTXT_CREATE: {
        struct kgsl_drawctxt_create *req = (struct kgsl_drawctxt_create *)buf;
        uint32_t id = s->next_context_id++;
        if (id >= KGSL_MAX_CONTEXTS) {
            ret = -ENOMEM;
            break;
        }
        s->contexts[id].active = true;
        s->contexts[id].id = id;
        s->contexts[id].flags = req->flags;
        s->contexts[id].timestamp = 0;
        req->drawctxt_id = id;
        qcom_kgsl_update_memstore(s, id, 0);
        ret = 0;
        break;
    }

    case IOCTL_KGSL_DRAWCTXT_DESTROY: {
        struct kgsl_drawctxt_destroy *req = (struct kgsl_drawctxt_destroy *)buf;
        uint32_t id = req->drawctxt_id;
        if (id < KGSL_MAX_CONTEXTS) {
            s->contexts[id].active = false;
        }
        ret = 0;
        break;
    }

    case IOCTL_KGSL_GPUOBJ_ALLOC: {
        struct kgsl_gpuobj_alloc *alloc = (struct kgsl_gpuobj_alloc *)buf;
        uint32_t id = s->next_obj_id++;
        if (id >= KGSL_MAX_GPUOBJS) {
            ret = -ENOMEM;
            break;
        }
        uint64_t aligned_sz = QEMU_ALIGN_UP(alloc->size, 4096);
        s->gpu_objs[id].active = true;
        s->gpu_objs[id].id = id;
        s->gpu_objs[id].size = alloc->size;
        s->gpu_objs[id].flags = alloc->flags;
        s->gpu_objs[id].gpuaddr = s->next_gpu_va;
        s->gpu_objs[id].mmapsize = aligned_sz;
        s->gpu_objs[id].host_ptr = g_malloc0(aligned_sz);
        s->gpu_objs[id].is_imported = false;

        s->next_gpu_va += aligned_sz;

        alloc->id = id;
        alloc->mmapsize = aligned_sz;
        ret = 0;
        break;
    }

    case IOCTL_KGSL_GPUOBJ_FREE: {
        struct kgsl_gpuobj_free *free_req = (struct kgsl_gpuobj_free *)buf;
        uint32_t id = free_req->id;
        if (id < KGSL_MAX_GPUOBJS && s->gpu_objs[id].active) {
            s->gpu_objs[id].active = false;
            if (s->gpu_objs[id].host_ptr && !s->gpu_objs[id].is_imported) {
                g_free(s->gpu_objs[id].host_ptr);
                s->gpu_objs[id].host_ptr = NULL;
            }
        }
        ret = 0;
        break;
    }

    case IOCTL_KGSL_GPUOBJ_INFO: {
        struct kgsl_gpuobj_info *info = (struct kgsl_gpuobj_info *)buf;
        uint32_t id = info->id;
        if (id < KGSL_MAX_GPUOBJS && s->gpu_objs[id].active) {
            info->gpuaddr = s->gpu_objs[id].gpuaddr;
            info->size = s->gpu_objs[id].size;
            info->flags = s->gpu_objs[id].flags;
            info->va_len = s->gpu_objs[id].mmapsize;
            ret = 0;
        } else {
            ret = -EINVAL;
        }
        break;
    }

    case IOCTL_KGSL_GPUOBJ_IMPORT: {
        struct kgsl_gpuobj_import *imp = (struct kgsl_gpuobj_import *)buf;
        uint32_t id = s->next_obj_id++;
        if (id >= KGSL_MAX_GPUOBJS) {
            ret = -ENOMEM;
            break;
        }
        s->gpu_objs[id].active = true;
        s->gpu_objs[id].id = id;
        s->gpu_objs[id].size = 4096; /* Default page if not specified */
        s->gpu_objs[id].flags = imp->flags;
        s->gpu_objs[id].gpuaddr = s->next_gpu_va;
        s->gpu_objs[id].is_imported = true;
        s->next_gpu_va += 0x100000; /* Reserve 1MB span for imported buffer */

        imp->id = id;
        ret = 0;
        break;
    }

    case IOCTL_KGSL_GPU_COMMAND: {
        struct kgsl_gpu_command *cmd_req = (struct kgsl_gpu_command *)buf;
        uint32_t ctx_id = cmd_req->context_id;
        uint32_t ts;
        if (ctx_id < KGSL_MAX_CONTEXTS && s->contexts[ctx_id].active) {
            s->contexts[ctx_id].timestamp++;
            ts = s->contexts[ctx_id].timestamp;
        } else {
            s->global_timestamp++;
            ts = s->global_timestamp;
        }
        qcom_kgsl_update_memstore(s, ctx_id, ts);
        cmd_req->timestamp = ts;
        ret = 0;
        break;
    }

    case IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID:
    case IOCTL_KGSL_DEVICE_WAITTIMESTAMP: {
        /* Virtual GPU executes commands synchronously; always retired */
        ret = 0;
        break;
    }

    case IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID: {
        struct kgsl_cmdstream_readtimestamp_ctxtid *rt = (struct kgsl_cmdstream_readtimestamp_ctxtid *)buf;
        uint32_t ctx_id = rt->context_id;
        if (ctx_id < KGSL_MAX_CONTEXTS && s->contexts[ctx_id].active) {
            rt->timestamp = s->contexts[ctx_id].timestamp;
        } else {
            rt->timestamp = s->global_timestamp;
        }
        ret = 0;
        break;
    }

    case IOCTL_KGSL_SYNCSOURCE_CREATE:
    case IOCTL_KGSL_SYNCSOURCE_DESTROY:
    case IOCTL_KGSL_SYNCSOURCE_CREATE_FENCE:
    case IOCTL_KGSL_SYNCSOURCE_SIGNAL_FENCE:
    case IOCTL_KGSL_TIMESTAMP_EVENT:
    case IOCTL_KGSL_SETPROPERTY:
    case IOCTL_KGSL_GPUOBJ_SET_INFO:
    case IOCTL_KGSL_GPUOBJ_SYNC:
    case IOCTL_KGSL_SHAREDMEM_FLUSH_CACHE:
        /* Successfully handled stub operations */
        ret = 0;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "qcom-kgsl: unhandled ioctl cmd=0x%08x\n", cmd);
        ret = 0; /* Return success to allow userspace to proceed */
        break;
    }

    cpu_physical_memory_unmap(buf, len, true, len);
    return ret;
}

/* Device Realization */
static void qcom_kgsl_realize(DeviceState *dev, Error **errp)
{
    QcomKgslState *s = QCOM_KGSL(dev);

    /* Initialize Primary MMIO Window at 0x05090000 */
    memory_region_init_io(&s->mmio, OBJECT(s), &qcom_kgsl_mmio_ops, s,
                          "qcom-kgsl-mmio", KGSL_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    /* Initialize Memstore Shadow Page */
    memory_region_init_ram(&s->memstore_mr, OBJECT(s),
                           "qcom-kgsl-memstore", KGSL_MEMSTORE_SIZE, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->memstore_mr);

    /* Initialize GX and GMU IRQ lines */
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->gx_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->gmu_irq);

    /* Initialize state counters */
    s->next_context_id = 1;
    s->next_obj_id = 1;
    s->next_gpu_va = 0x10000000ULL; /* Start dynamic GPU VA at 256MB */
    s->global_timestamp = 1;

    /* Global context 0 initialized in memstore */
    qcom_kgsl_update_memstore(s, 0, 0);

    qemu_log_mask(LOG_GUEST_ERROR, "qcom-kgsl: Virtual Qualcomm KGSL (Adreno 612) initialized\n");
}

/* Device Reset */
static void qcom_kgsl_reset(DeviceState *dev)
{
    QcomKgslState *s = QCOM_KGSL(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->contexts, 0, sizeof(s->contexts));
    memset(s->gpu_objs, 0, sizeof(s->gpu_objs));

    s->next_context_id = 1;
    s->next_obj_id = 1;
    s->next_gpu_va = 0x10000000ULL;
    s->global_timestamp = 1;
    s->mb_cmd = 0;
    s->mb_arg_gpa = 0;
    s->mb_arg_size = 0;
    s->mb_status = 0;

    qcom_kgsl_update_memstore(s, 0, 0);
}

/* VMState for Checkpointing & Snapshotting */
static const VMStateDescription vmstate_qcom_kgsl = {
    .name = "qcom-kgsl",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, QcomKgslState, KGSL_MMIO_SIZE / 4),
        VMSTATE_UINT32(mb_cmd, QcomKgslState),
        VMSTATE_UINT64(mb_arg_gpa, QcomKgslState),
        VMSTATE_UINT32(mb_arg_size, QcomKgslState),
        VMSTATE_INT32(mb_status, QcomKgslState),
        VMSTATE_UINT32(next_context_id, QcomKgslState),
        VMSTATE_UINT32(next_obj_id, QcomKgslState),
        VMSTATE_UINT64(next_gpu_va, QcomKgslState),
        VMSTATE_UINT32(global_timestamp, QcomKgslState),
        VMSTATE_END_OF_LIST()
    }
};

static void qcom_kgsl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_kgsl_realize;
    dc->reset = qcom_kgsl_reset;
    dc->vmsd = &vmstate_qcom_kgsl;
    dc->desc = "Qualcomm KGSL / Adreno 612 Virtual GPU Device";
}

static const TypeInfo qcom_kgsl_info = {
    .name          = TYPE_QCOM_KGSL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomKgslState),
    .class_init    = qcom_kgsl_class_init,
};

static void qcom_kgsl_register_types(void)
{
    type_register_static(&qcom_kgsl_info);
}

type_init(qcom_kgsl_register_types)
