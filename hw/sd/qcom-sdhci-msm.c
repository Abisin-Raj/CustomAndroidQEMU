/*
 * Qualcomm SDHCI MSM (eMMC 5.1) Host Controller Emulation
 *
 * Implements qcom,sdhci-msm-v5 controller interface for Xiaomi Violet
 * (Snapdragon 675 / SM6150) eMMC 5.1 host controller at 0x7c4000.
 *
 * Copyright (c) 2026 Antigravity
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/sd/qcom-sdhci-msm.h"
#include "hw/sd/sdhci.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "sdhci-internal.h"
#include "migration/vmstate.h"

/* Qualcomm MSM Vendor Register Offsets (relative to 0x7c4000) */
/* v4 Offsets */
#define CORE_VENDOR_SPEC_V4               0x10c
#define CORE_HC_MODE_V4                   0x114
#define CORE_TESTBUS_CONFIG_V4            0x118
#define CORE_VENDOR_SPEC_CAPABILITIES0_V4 0x11c
#define CORE_PWRCTL_STATUS_V4             0x1dc
#define CORE_PWRCTL_MASK_V4               0x1e0
#define CORE_PWRCTL_CLEAR_V4              0x1e4
#define CORE_PWRCTL_CTL_V4                0x1e8

/* v5 Offsets (SM6150 / Snapdragon 675 / Violet) */
#define CORE_VENDOR_SPEC_V5               0x200
#define CORE_VENDOR_SPEC_CAPABILITIES0_V5 0x208
#define CORE_VENDOR_SPEC_CAPABILITIES1_V5 0x20c
#define CORE_DLL_CONFIG_2_V5              0x210
#define CORE_VENDOR_SPEC3_V5              0x214
#define CORE_DLL_CONFIG_V5                0x218
#define CORE_DLL_STATUS_V5                0x21c
#define CORE_DDR_CONFIG_V5                0x224
#define CORE_PWRCTL_STATUS_V5             0x240
#define CORE_PWRCTL_MASK_V5               0x244
#define CORE_PWRCTL_CLEAR_V5              0x248
#define CORE_PWRCTL_CTL_V5                0x24c
#define CORE_HC_MODE_V5                   0x250
#define CORE_DLL_CONFIG_3_V5              0x254
#define CORE_DDR_CONFIG_2_V5              0x258
#define CORE_DLL_CONFIG_4_V5              0x25c
#define CORE_TESTBUS_CONFIG_V5            0x32c
#define CORE_TESTBUS_STATUS_V5            0x358

static void sdhci_log(const char *fmt, ...)
{
    static FILE *f = NULL;
    if (!f) {
        f = fopen("C:\\qemu_work\\sdhci_msm.log", "w");
    }
    if (f) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fflush(f);
    }
}

static uint64_t qcom_sdhci_hc_read(void *opaque, hwaddr addr, unsigned size)
{
    QComSDHCIState *s = opaque;
    uint64_t ret = 0;

    if (addr < 0x100) {
        /* Standard SDHCI register area */
        ret = s->sdhci.io_ops->read(&s->sdhci, addr, size);
        sdhci_log("[SDHCI_STD] READ  addr=0x%04x size=%d -> 0x%08llx\n",
                  (uint32_t)addr, size, (unsigned long long)ret);
        return ret;
    }

    /* Qualcomm MSM Vendor Registers */
    switch (addr) {
    case CORE_DLL_STATUS_V5:
        /* Bit 31: CORE_DLL_LOCK (1 = DLL locked) */
        ret = s->vendor_regs[(addr - 0x100) / 4] | 0x80000000;
        break;
    case CORE_DDR_CONFIG_V5:
        /* DDR DLL status locked */
        ret = s->vendor_regs[(addr - 0x100) / 4] | 0x80000000;
        break;
    default:
        if ((addr - 0x100) / 4 < ARRAY_SIZE(s->vendor_regs)) {
            ret = s->vendor_regs[(addr - 0x100) / 4];
        }
        break;
    }

    sdhci_log("[SDHCI_MSM] READ  addr=0x%04x size=%d -> 0x%08llx\n",
              (uint32_t)addr, size, (unsigned long long)ret);
    return ret;
}

static void qcom_sdhci_hc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    QComSDHCIState *s = opaque;

    if (addr < 0x100) {
        sdhci_log("[SDHCI_STD] WRITE addr=0x%04x size=%d val=0x%08llx\n",
                  (uint32_t)addr, size, (unsigned long long)val);
        /* Standard SDHCI register area */
        s->sdhci.io_ops->write(&s->sdhci, addr, val, size);

        /* Qualcomm hardware automatically tracks power/voltage transitions */
        uint32_t req = 0;
        if (addr == 0x3e) { /* SDHCI_HOST_CONTROL2 */
            if (val & (1 << 3)) {
                req = 0x04; /* IO_LOW (1.8V) */
            } else {
                req = 0x08; /* IO_HIGH (3.0V/3.3V) */
            }
        } else if (addr == 0x29) { /* SDHCI_POWER_CONTROL */
            if (val & 0x01) {
                req = 0x01; /* BUS_ON */
            } else {
                req = 0x02; /* BUS_OFF */
            }
        }

        if (req != 0) {
            s->vendor_regs[(CORE_PWRCTL_STATUS_V5 - 0x100) / 4] |= req;
            s->vendor_regs[(CORE_PWRCTL_STATUS_V4 - 0x100) / 4] |= req;
            uint32_t mask = s->vendor_regs[(CORE_PWRCTL_MASK_V5 - 0x100) / 4] |
                            s->vendor_regs[(CORE_PWRCTL_MASK_V4 - 0x100) / 4];
            if (mask & req) {
                qemu_set_irq(s->pwr_irq, 1);
            }
        }
        return;
    }

    sdhci_log("[SDHCI_MSM] WRITE addr=0x%04x size=%d val=0x%08llx\n",
              (uint32_t)addr, size, (unsigned long long)val);

    /* Qualcomm MSM Vendor Registers */
    switch (addr) {
    case CORE_PWRCTL_CTL_V5:
    case CORE_PWRCTL_CTL_V4: {
        s->vendor_regs[(addr - 0x100) / 4] = (uint32_t)val;
        break;
    }

    case CORE_PWRCTL_CLEAR_V5:
    case CORE_PWRCTL_CLEAR_V4: {
        uint32_t clr = (uint32_t)val & 0x0f;
        s->vendor_regs[(CORE_PWRCTL_STATUS_V5 - 0x100) / 4] &= ~clr;
        s->vendor_regs[(CORE_PWRCTL_STATUS_V4 - 0x100) / 4] &= ~clr;
        s->vendor_regs[(addr - 0x100) / 4] = (uint32_t)val;
        
        uint32_t status = s->vendor_regs[(CORE_PWRCTL_STATUS_V5 - 0x100) / 4] |
                          s->vendor_regs[(CORE_PWRCTL_STATUS_V4 - 0x100) / 4];
        uint32_t mask = s->vendor_regs[(CORE_PWRCTL_MASK_V5 - 0x100) / 4] |
                        s->vendor_regs[(CORE_PWRCTL_MASK_V4 - 0x100) / 4];
        if ((status & mask & 0x0f) == 0) {
            qemu_set_irq(s->pwr_irq, 0);
        }
        break;
    }

    case CORE_PWRCTL_MASK_V5:
    case CORE_PWRCTL_MASK_V4: {
        s->vendor_regs[(addr - 0x100) / 4] = (uint32_t)val;
        uint32_t status = s->vendor_regs[(CORE_PWRCTL_STATUS_V5 - 0x100) / 4] |
                          s->vendor_regs[(CORE_PWRCTL_STATUS_V4 - 0x100) / 4];
        if (status & (val & 0x0f)) {
            qemu_set_irq(s->pwr_irq, 1);
        } else {
            qemu_set_irq(s->pwr_irq, 0);
        }
        break;
    }

    default:
        if ((addr - 0x100) / 4 < ARRAY_SIZE(s->vendor_regs)) {
            s->vendor_regs[(addr - 0x100) / 4] = (uint32_t)val;
        }
        break;
    }
}

static const MemoryRegionOps qcom_sdhci_hc_ops = {
    .read = qcom_sdhci_hc_read,
    .write = qcom_sdhci_hc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    }
};

/* CMDQ Engine Stub (0x7c5000) */
static uint64_t qcom_sdhci_cmdq_read(void *opaque, hwaddr addr, unsigned size)
{
    QComSDHCIState *s = opaque;
    uint64_t ret = 0;

    switch (addr) {
    case 0x00: /* CQE_VER */
        ret = 0x00000510; /* eMMC 5.1 Command Queue Engine */
        break;
    case 0x04: /* CQE_CAP */
        ret = 0x00000000;
        break;
    case 0x0c: /* CQE_CTL */
        ret = 0x00000000; /* Not halted */
        break;
    default:
        if (addr / 4 < ARRAY_SIZE(s->cmdq_regs)) {
            ret = s->cmdq_regs[addr / 4];
        }
        break;
    }

    return ret;
}

static void qcom_sdhci_cmdq_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    QComSDHCIState *s = opaque;
    if (addr / 4 < ARRAY_SIZE(s->cmdq_regs)) {
        s->cmdq_regs[addr / 4] = (uint32_t)val;
    }
}

static const MemoryRegionOps qcom_sdhci_cmdq_ops = {
    .read = qcom_sdhci_cmdq_read,
    .write = qcom_sdhci_cmdq_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    }
};

/* ICE (Inline Crypto Engine) Stub (0x7c8000) */
static uint64_t qcom_sdhci_ice_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void qcom_sdhci_ice_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps qcom_sdhci_ice_ops = {
    .read = qcom_sdhci_ice_read,
    .write = qcom_sdhci_ice_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    }
};

static void qcom_sdhci_irq_handler(void *opaque, int n, int level)
{
    QComSDHCIState *s = opaque;
    qemu_set_irq(s->hc_irq, level);
}

static void qcom_sdhci_msm_init(Object *obj)
{
    QComSDHCIState *s = QCOM_SDHCI_MSM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    sdhci_log("=== QEMU Qualcomm SDHCI MSM Initialized ===\n");

    /* Initialize inner SDHCI State as a child object */
    object_initialize_child(obj, "generic-sdhci", &s->sdhci, TYPE_SYSBUS_SDHCI);

    /* Memory Region 0: Host Controller + Qualcomm Vendor registers (0x1000) */
    memory_region_init_io(&s->hc_mem, obj, &qcom_sdhci_hc_ops, s,
                          "qcom-sdhci-hc", 0x1000);
    sysbus_init_mmio(sbd, &s->hc_mem);

    /* Memory Region 1: CMDQ Engine (0x1000) */
    memory_region_init_io(&s->cmdq_mem, obj, &qcom_sdhci_cmdq_ops, s,
                          "qcom-sdhci-cmdq", 0x1000);
    sysbus_init_mmio(sbd, &s->cmdq_mem);

    /* Memory Region 2: Storage ICE (0x8000) */
    memory_region_init_io(&s->cmdq_ice, obj, &qcom_sdhci_ice_ops, s,
                          "qcom-sdhci-ice", 0x8000);
    sysbus_init_mmio(sbd, &s->cmdq_ice);

    /* SysBus IRQ 0 (hc_irq) and IRQ 1 (pwr_irq) */
    sysbus_init_irq(sbd, &s->hc_irq);
    sysbus_init_irq(sbd, &s->pwr_irq);
}

static void qcom_sdhci_msm_realize(DeviceState *dev, Error **errp)
{
    QComSDHCIState *s = QCOM_SDHCI_MSM(dev);
    SysBusDevice *sbd_sdhci = SYS_BUS_DEVICE(&s->sdhci);

    /* Configure SDHCI Host Capabilities */
    s->sdhci.sd_spec_version = 3;
    s->sdhci.capareg = 0x0567b4b201e832b2ULL;
    s->sdhci.maxcurr = 0x0000000000000001ULL;

    /* Realize inner generic-sdhci */
    if (!sysbus_realize(sbd_sdhci, errp)) {
        return;
    }

    /* Forward inner SDHCI IRQ to our sysbus hc_irq */
    sysbus_connect_irq(sbd_sdhci, 0, qemu_allocate_irq(qcom_sdhci_irq_handler, s, 0));

    /* Get child sd-bus pointer */
    s->bus = qdev_get_child_bus(DEVICE(sbd_sdhci), "sd-bus");
}

static void qcom_sdhci_msm_reset(DeviceState *dev)
{
    QComSDHCIState *s = QCOM_SDHCI_MSM(dev);

    memset(s->vendor_regs, 0, sizeof(s->vendor_regs));
    memset(s->cmdq_regs, 0, sizeof(s->cmdq_regs));

    device_cold_reset(DEVICE(&s->sdhci));
}

static const VMStateDescription vmstate_qcom_sdhci_msm = {
    .name = "qcom-sdhci-msm",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(vendor_regs, QComSDHCIState, QCOM_SDHCI_VENDOR_REGS_SIZE / 4),
        VMSTATE_UINT32_ARRAY(cmdq_regs, QComSDHCIState, 0x100 / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void qcom_sdhci_msm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_sdhci_msm_realize;
    dc->reset = qcom_sdhci_msm_reset;
    dc->vmsd = &vmstate_qcom_sdhci_msm;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo qcom_sdhci_msm_info = {
    .name = TYPE_QCOM_SDHCI_MSM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QComSDHCIState),
    .instance_init = qcom_sdhci_msm_init,
    .class_init = qcom_sdhci_msm_class_init,
};

static void qcom_sdhci_msm_register_types(void)
{
    type_register_static(&qcom_sdhci_msm_info);
}

type_init(qcom_sdhci_msm_register_types)
