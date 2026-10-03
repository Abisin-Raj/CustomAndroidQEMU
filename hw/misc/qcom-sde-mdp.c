/*
 * Qualcomm Snapdragon Display Engine (SDE) MDP stub device for QEMU.
 *
 * This device emulates the Qualcomm SDE MDSS top-level register region
 * at 0x0ae00000 (mdp_phys).
 * It models the MDP hardware revision readout register (offset 0x0)
 * required for sde_kms_hw_init() and _sde_hardware_pre_caps().
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
#include "hw/misc/qcom-sde-regdma.h"

#define TYPE_QCOM_SDE_MDP "qcom-sde-mdp"
OBJECT_DECLARE_SIMPLE_TYPE(QcomSdeMdpState, QCOM_SDE_MDP)

/* SDE MDP register space size: 0x85000 (532 KB) */
#define SDE_MDP_SIZE 0x85000

/* Default SDE hardware revision: 0x50000000 (SDE 5.0.0) */
#define SDE_MDP_DEFAULT_HW_REV 0x50000000U

struct QcomSdeMdpState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t hw_rev;
    /* Backing store for registers */
    uint8_t regs[SDE_MDP_SIZE];
};

static void G_GNUC_PRINTF(1, 2) sde_mdp_log(const char *fmt, ...)
{
    static FILE *f = NULL;
    if (!f) {
        f = fopen("C:\\qemu_work\\sde_regdma.log", "a");
    }
    if (f) {
        int64_t now_us = g_get_real_time();
        va_list ap;
        fprintf(f, "[%lld us] ", (long long)now_us);
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fflush(f);
    }
}

static uint64_t qcom_sde_mdp_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomSdeMdpState *s = opaque;
    uint32_t val = 0;

    if (offset + size > SDE_MDP_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-sde-mdp: read out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return 0;
    }

    if (offset == 0x0) {
        val = s->hw_rev;
    } else {
        memcpy(&val, &s->regs[offset], size);
    }

    if (offset == 0x1014) {
        sde_mdp_log("READ  MDP    [0x0ae01014] (INTR_STATUS) -> 0x%08x (INTF_1_VSYNC_bit27=%u)\n",
                    val, (val >> 27) & 1);
    } else if (offset == 0x1010) {
        sde_mdp_log("READ  MDP    [0x0ae01010] (INTR_EN) -> 0x%08x\n", val);
    }

    qemu_log_mask(LOG_UNIMP,
                  "qcom-sde-mdp: read offset=0x%" HWADDR_PRIx " size=%u val=0x%08x\n",
                  offset, size, val);

    return val;
}

static void qcom_sde_mdp_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    QcomSdeMdpState *s = opaque;

    if (offset + size > SDE_MDP_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-sde-mdp: write out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return;
    }

    if (offset >= 0x2000 && offset < 0x2200) {
        if (offset == 0x20d4) {
            sde_mdp_log("WRITE CTL0   [0x0ae020d4] (CTL0_QUEUE_TRIGGER) size=%u <- 0x%08llx\n",
                        size, (unsigned long long)val);
            if (val & 0x1) {
                qcom_sde_regdma_trigger(0, 0);
            }
        } else if (offset == 0x2018) {
            sde_mdp_log("WRITE CTL0   [0x0ae02018] (CTL0_FLUSH) size=%u <- 0x%08llx\n",
                        size, (unsigned long long)val);
        } else if (offset == 0x201c) {
            sde_mdp_log("WRITE CTL0   [0x0ae0201c] (CTL0_START) size=%u <- 0x%08llx\n",
                        size, (unsigned long long)val);
        } else if (offset == 0x20d0) {
            sde_mdp_log("WRITE CTL0   [0x0ae020d0] (CTL0_PREPARE) size=%u <- 0x%08llx\n",
                        size, (unsigned long long)val);
        } else {
            sde_mdp_log("WRITE CTL0   [0x0ae0%04lx] size=%u <- 0x%08llx\n",
                        (unsigned long)offset, size, (unsigned long long)val);
        }
    } else if (offset == 0x1010) {
        sde_mdp_log("WRITE MDP    [0x0ae01010] (INTR_EN) size=%u <- 0x%08llx (INTF_1_VSYNC_bit27=%llu)\n",
                    size, (unsigned long long)val, ((unsigned long long)val >> 27) & 1);
    } else if (offset == 0x1018) {
        sde_mdp_log("WRITE MDP    [0x0ae01018] (INTR_CLEAR) size=%u <- 0x%08llx (INTF_1_VSYNC_bit27=%llu)\n",
                    size, (unsigned long long)val, ((unsigned long long)val >> 27) & 1);
        uint32_t *status = (uint32_t *)&s->regs[0x1014];
        *status &= ~(uint32_t)val;
    } else if (offset == 0x6b800) {
        sde_mdp_log("WRITE INTF_1 [0x0ae6b800] (TIMING_ENGINE_EN) size=%u <- 0x%08llx\n",
                    size, (unsigned long long)val);
    } else if (offset >= 0x6b800 && offset < 0x6b900) {
        sde_mdp_log("WRITE INTF_1 [0x0ae%05lx] (+0x%lx) size=%u <- 0x%08llx\n",
                    (unsigned long)offset, (unsigned long)(offset - 0x6b800), size, (unsigned long long)val);
    }

    qemu_log_mask(LOG_UNIMP,
                  "qcom-sde-mdp: write offset=0x%" HWADDR_PRIx " size=%u val=0x%08" PRIx64 "\n",
                  offset, size, val);

    memcpy(&s->regs[offset], &val, size);
}

static const MemoryRegionOps qcom_sde_mdp_ops = {
    .read = qcom_sde_mdp_read,
    .write = qcom_sde_mdp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void qcom_sde_mdp_reset(DeviceState *dev)
{
    QcomSdeMdpState *s = QCOM_SDE_MDP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    *(uint32_t *)&s->regs[0] = s->hw_rev;
}

static void qcom_sde_mdp_realize(DeviceState *dev, Error **errp)
{
    QcomSdeMdpState *s = QCOM_SDE_MDP(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &qcom_sde_mdp_ops, s,
                          "qcom-sde-mdp", SDE_MDP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    *(uint32_t *)&s->regs[0] = s->hw_rev;
}

static Property qcom_sde_mdp_properties[] = {
    DEFINE_PROP_UINT32("hw-rev", QcomSdeMdpState, hw_rev, SDE_MDP_DEFAULT_HW_REV),
    DEFINE_PROP_END_OF_LIST(),
};

static void qcom_sde_mdp_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_sde_mdp_realize;
    dc->reset = qcom_sde_mdp_reset;
    dc->desc = "Qualcomm Snapdragon SDE MDP controller";
    device_class_set_props(dc, qcom_sde_mdp_properties);
}

static const TypeInfo qcom_sde_mdp_info = {
    .name = TYPE_QCOM_SDE_MDP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomSdeMdpState),
    .class_init = qcom_sde_mdp_class_init,
};

static void qcom_sde_mdp_register_types(void)
{
    type_register_static(&qcom_sde_mdp_info);
}

type_init(qcom_sde_mdp_register_types)
