/*
 * Qualcomm Snapdragon Display Engine (SDE) REGDMA device for QEMU.
 *
 * This device emulates the Qualcomm SDE REGDMA controller at 0x0aeac000.
 * It instruments all queue programming, opmode, and status registers
 * to capture the execution contract for atomic modeset commit.
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

#define TYPE_QCOM_SDE_REGDMA "qcom-sde-regdma"
OBJECT_DECLARE_SIMPLE_TYPE(QcomSdeRegDmaState, QCOM_SDE_REGDMA)

#define SDE_REGDMA_SIZE 0x1000

struct QcomSdeRegDmaState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /* Backing store for registers */
    uint8_t regs[SDE_REGDMA_SIZE];
};

static void regdma_log(const char *fmt, ...)
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

static const char *regdma_reg_name(hwaddr offset)
{
    switch (offset) {
    case 0x004: return "REGDMA_OPMODE";
    case 0x014: return "REGDMA_CTL0_QUEUE0_IOVA";
    case 0x018: return "REGDMA_CTL0_QUEUE0_CMD1";
    case 0x160: return "REGDMA_INTR_STATUS";
    case 0x170: return "REGDMA_INTR_4_STATUS";
    case 0x1a0: return "REGDMA_INTR_CLEAR";
    default:    return "REGDMA_OTHER";
    }
}

static uint64_t qcom_sde_regdma_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomSdeRegDmaState *s = opaque;
    uint32_t val = 0;

    if (offset + size > SDE_REGDMA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-sde-regdma: read out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return 0;
    }

    memcpy(&val, &s->regs[offset], size);

    regdma_log("READ  REGDMA [0x%08llx] (%-24s) size=%u -> 0x%08x\n",
               (unsigned long long)(0x0aeac000 + offset),
               regdma_reg_name(offset), size, val);

    return val;
}

static void qcom_sde_regdma_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    QcomSdeRegDmaState *s = opaque;

    if (offset + size > SDE_REGDMA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-sde-regdma: write out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return;
    }

    regdma_log("WRITE REGDMA [0x%08llx] (%-24s) size=%u <- 0x%08llx\n",
               (unsigned long long)(0x0aeac000 + offset),
               regdma_reg_name(offset), size, (unsigned long long)val);

    if (offset == 0x018) {
        uint32_t dwords = val & 0x3fff;
        uint32_t is_write = (val >> 22) & 1;
        uint32_t last_cmd = (val >> 24) & 1;
        regdma_log("  -> QUEUE0_CMD1 details: dwords=%u, is_write=%u, last_cmd=%u\n",
                   dwords, is_write, last_cmd);
    } else if (offset == 0x1a0) {
        /* W1C clearing: clear corresponding bits in status register 0x160 */
        uint32_t *status = (uint32_t *)&s->regs[0x160];
        *status &= ~(uint32_t)val;
        regdma_log("  -> STATUS_CLEAR: mask=0x%08llx, status 0x160 now=0x%08x\n",
                   (unsigned long long)val, *status);
    }

    memcpy(&s->regs[offset], &val, size);
}

static const MemoryRegionOps qcom_sde_regdma_ops = {
    .read = qcom_sde_regdma_read,
    .write = qcom_sde_regdma_write,
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

static void qcom_sde_regdma_reset(DeviceState *dev)
{
    QcomSdeRegDmaState *s = QCOM_SDE_REGDMA(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static void qcom_sde_regdma_realize(DeviceState *dev, Error **errp)
{
    QcomSdeRegDmaState *s = QCOM_SDE_REGDMA(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &qcom_sde_regdma_ops, s,
                          "qcom-sde-regdma", SDE_REGDMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void qcom_sde_regdma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_sde_regdma_realize;
    dc->reset = qcom_sde_regdma_reset;
    dc->desc = "Qualcomm Snapdragon SDE REGDMA controller";
}

static const TypeInfo qcom_sde_regdma_info = {
    .name = TYPE_QCOM_SDE_REGDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomSdeRegDmaState),
    .class_init = qcom_sde_regdma_class_init,
};

static void qcom_sde_regdma_register_types(void)
{
    type_register_static(&qcom_sde_regdma_info);
}

type_init(qcom_sde_regdma_register_types)
