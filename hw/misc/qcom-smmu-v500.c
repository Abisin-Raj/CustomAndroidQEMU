/*
 * Qualcomm Snapdragon SM6150 / QSMMU-v500 stub device for QEMU.
 *
 * This device models the minimal register interface for the Qualcomm
 * apps-smmu at 0x15000000 (size 0x80000).
 * It provides the hardware identification registers (sIDR0, sIDR1, sIDR2)
 * required for the Linux kernel's authentic arm-smmu driver
 * (arm_smmu_device_cfg_probe) to successfully probe the SMMU and register
 * platform_bus_type.iommu_ops via bus_set_iommu().
 *
 * Identification Register Values:
 *   Offset 0x20 (sIDR0): 0x63106d80
 *     - Bit 30: S2TS = 1 (Stage 2 Translation Support)
 *     - Bit 29: S1TS = 1 (Stage 1 Translation Support)
 *     - Bit 27: SMS  = 1 (Stream Match Support)
 *     - Bit 14: CTTW = 1 (Coherent Translation Table Walk)
 *     - Bits 7..0: NUMSMRG = 0x80 (128 Stream Match Register Groups)
 *   Offset 0x24 (sIDR1): 0x00000020
 *     - Bits 7..0: NUMCB = 0x20 (32 Context Banks)
 *     - Bits 23..16: NUMPAGENDXB = 0x0
 *     - Bits 30..28: PAGESIZE = 0x0 (4KB/64KB supported)
 *   Offset 0x28 (sIDR2): 0x00000055
 *     - Bits 3..0: IAS = 0x5 (48-bit Input Address Size)
 *     - Bits 7..4: OAS = 0x5 (48-bit Output Address Size)
 *
 * Reference: drivers/iommu/arm-smmu.c (arm_smmu_device_cfg_probe)
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

#define TYPE_QCOM_SMMU_V500 "qcom-smmu-v500"
OBJECT_DECLARE_SIMPLE_TYPE(QcomSmmuV500State, QCOM_SMMU_V500)

/* apps-smmu register space size: 0x80000 (512 KB) */
#define QCOM_SMMU_V500_SIZE 0x80000

#define SMMU_REG_sIDR0 0x20
#define SMMU_REG_sIDR1 0x24
#define SMMU_REG_sIDR2 0x28

#define SMMU_VAL_sIDR0 0x63106d80U
#define SMMU_VAL_sIDR1 0x00000020U
#define SMMU_VAL_sIDR2 0x00000055U

struct QcomSmmuV500State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /* Backing store for register writes and unmodeled reads */
    uint8_t regs[QCOM_SMMU_V500_SIZE];
};

static uint64_t qcom_smmu_v500_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomSmmuV500State *s = opaque;
    uint32_t val = 0;

    if (offset + size > QCOM_SMMU_V500_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-smmu-v500: read out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return 0;
    }

    switch (offset) {
    case SMMU_REG_sIDR0:
        val = SMMU_VAL_sIDR0;
        break;
    case SMMU_REG_sIDR1:
        val = SMMU_VAL_sIDR1;
        break;
    case SMMU_REG_sIDR2:
        val = SMMU_VAL_sIDR2;
        break;
    default:
        memcpy(&val, &s->regs[offset], size > 4 ? 4 : size);
        break;
    }

    qemu_log_mask(LOG_UNIMP,
                  "qcom-smmu-v500: read offset=0x%" HWADDR_PRIx " size=%u val=0x%08x\n",
                  offset, size, val);

    return val;
}

static void qcom_smmu_v500_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    QcomSmmuV500State *s = opaque;

    if (offset + size > QCOM_SMMU_V500_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-smmu-v500: write out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "qcom-smmu-v500: write offset=0x%" HWADDR_PRIx " size=%u val=0x%08" PRIx64 "\n",
                  offset, size, val);

    memcpy(&s->regs[offset], &val, size > 4 ? 4 : size);
}

static const MemoryRegionOps qcom_smmu_v500_ops = {
    .read = qcom_smmu_v500_read,
    .write = qcom_smmu_v500_write,
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

static void qcom_smmu_v500_reset(DeviceState *dev)
{
    QcomSmmuV500State *s = QCOM_SMMU_V500(dev);

    memset(s->regs, 0, sizeof(s->regs));
    *(uint32_t *)&s->regs[SMMU_REG_sIDR0] = SMMU_VAL_sIDR0;
    *(uint32_t *)&s->regs[SMMU_REG_sIDR1] = SMMU_VAL_sIDR1;
    *(uint32_t *)&s->regs[SMMU_REG_sIDR2] = SMMU_VAL_sIDR2;
}

static void qcom_smmu_v500_realize(DeviceState *dev, Error **errp)
{
    QcomSmmuV500State *s = QCOM_SMMU_V500(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &qcom_smmu_v500_ops, s,
                          "qcom-smmu-v500", QCOM_SMMU_V500_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);

    *(uint32_t *)&s->regs[SMMU_REG_sIDR0] = SMMU_VAL_sIDR0;
    *(uint32_t *)&s->regs[SMMU_REG_sIDR1] = SMMU_VAL_sIDR1;
    *(uint32_t *)&s->regs[SMMU_REG_sIDR2] = SMMU_VAL_sIDR2;
}

static void qcom_smmu_v500_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_smmu_v500_realize;
    dc->reset = qcom_smmu_v500_reset;
    dc->desc = "Qualcomm Snapdragon QSMMU-v500 controller stub";
}

static const TypeInfo qcom_smmu_v500_info = {
    .name = TYPE_QCOM_SMMU_V500,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomSmmuV500State),
    .class_init = qcom_smmu_v500_class_init,
};

static void qcom_smmu_v500_register_types(void)
{
    type_register_static(&qcom_smmu_v500_info);
}

type_init(qcom_smmu_v500_register_types)
