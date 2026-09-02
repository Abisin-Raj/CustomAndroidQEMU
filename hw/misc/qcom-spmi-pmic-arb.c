/*
 * Qualcomm SPMI PMIC Arbiter stub device for QEMU.
 *
 * Emulates the Qualcomm SPMI PMIC Arbiter mapped at:
 *   - core:   0x0c440000 (size 0x1100)
 *   - chnls:  0x0c600000 (size 0x2000000)
 *   - obsrvr: 0x0e600000 (size 0x100000)
 *   - intr:   0x0e700000 (size 0xa0000)
 *   - cnfg:   0x0c40a000 (size 0x26000)
 *
 * Copyright (c) 2024 CustomAndroidEmulator project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define TYPE_QCOM_SPMI_PMIC_ARB "qcom-spmi-pmic-arb"
OBJECT_DECLARE_SIMPLE_TYPE(QcomSpmiPmicArbState, QCOM_SPMI_PMIC_ARB)

#define PMIC_ARB_VERSION_V1 0x10000000U
#define PMIC_ARB_STATUS_DONE 0x00000001U

struct QcomSpmiPmicArbState {
    SysBusDevice parent_obj;

    MemoryRegion core_mmio;
    MemoryRegion chnls_mmio;
    MemoryRegion obsrvr_mmio;
    MemoryRegion intr_mmio;
    MemoryRegion cnfg_mmio;

    qemu_irq irq;
};

static uint64_t qcom_spmi_core_read(void *opaque, hwaddr offset, unsigned size)
{
    if (offset == 0x0000) {
        /* PMIC_ARB_VERSION */
        return PMIC_ARB_VERSION_V1;
    }
    if ((offset & 0x0f) == 0x08) {
        /* PMIC_ARB_STATUS */
        return PMIC_ARB_STATUS_DONE;
    }
    return 0;
}

static void qcom_spmi_core_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    /* Writes accepted and acknowledged */
}

static const MemoryRegionOps qcom_spmi_core_ops = {
    .read = qcom_spmi_core_read,
    .write = qcom_spmi_core_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static uint64_t qcom_spmi_chnls_read(void *opaque, hwaddr offset, unsigned size)
{
    if ((offset & 0x0f) == 0x08) {
        return PMIC_ARB_STATUS_DONE;
    }
    return 0;
}

static void qcom_spmi_chnls_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
}

static const MemoryRegionOps qcom_spmi_chnls_ops = {
    .read = qcom_spmi_chnls_read,
    .write = qcom_spmi_chnls_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static uint64_t qcom_spmi_dummy_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static void qcom_spmi_dummy_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
}

static const MemoryRegionOps qcom_spmi_dummy_ops = {
    .read = qcom_spmi_dummy_read,
    .write = qcom_spmi_dummy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void qcom_spmi_pmic_arb_init(Object *obj)
{
    QcomSpmiPmicArbState *s = QCOM_SPMI_PMIC_ARB(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->core_mmio, obj, &qcom_spmi_core_ops, s,
                          "qcom-spmi-core", 0x10000);
    sysbus_init_mmio(sbd, &s->core_mmio);

    memory_region_init_io(&s->chnls_mmio, obj, &qcom_spmi_chnls_ops, s,
                          "qcom-spmi-chnls", 0x2000000);
    sysbus_init_mmio(sbd, &s->chnls_mmio);

    memory_region_init_io(&s->obsrvr_mmio, obj, &qcom_spmi_dummy_ops, s,
                          "qcom-spmi-obsrvr", 0x100000);
    sysbus_init_mmio(sbd, &s->obsrvr_mmio);

    memory_region_init_io(&s->intr_mmio, obj, &qcom_spmi_dummy_ops, s,
                          "qcom-spmi-intr", 0xa0000);
    sysbus_init_mmio(sbd, &s->intr_mmio);

    memory_region_init_io(&s->cnfg_mmio, obj, &qcom_spmi_dummy_ops, s,
                          "qcom-spmi-cnfg", 0x26000);
    sysbus_init_mmio(sbd, &s->cnfg_mmio);

    sysbus_init_irq(sbd, &s->irq);
}

static const TypeInfo qcom_spmi_pmic_arb_info = {
    .name = TYPE_QCOM_SPMI_PMIC_ARB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomSpmiPmicArbState),
    .instance_init = qcom_spmi_pmic_arb_init,
};

static void qcom_spmi_pmic_arb_register_types(void)
{
    type_register_static(&qcom_spmi_pmic_arb_info);
}

type_init(qcom_spmi_pmic_arb_register_types)
