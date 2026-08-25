/*
 * Qualcomm SM6150 Global Clock Controller (GCC) stub device for QEMU.
 *
 * This device emulates the Qualcomm SM6150 GCC clock controller at 0x100000.
 * It models the critical firmware-initialized PLL state that the Linux kernel
 * expects to be pre-configured before boot.
 *
 * On real SM6150 hardware, XBL (bootloader) initializes and locks all PLLs
 * before Linux starts. The kernel's GCC clock driver (gcc-sm6150) expects
 * these PLLs to already be locked (PLL_MODE LOCK_DET bit 31 set) and have
 * a valid L_VAL programmed when it reads the PLL registers.
 *
 * Key pre-initialized PLL registers (relative to GCC base 0x100000):
 *   GPLL0_MODE  @ offset 0x00000 -> 0xA0000001 (LOCK_DET=1, OUTCTRL=1)
 *   GPLL1_MODE  @ offset 0x01000 -> 0xA0000001
 *   GPLL4_MODE  @ offset 0x76000 -> 0xA0000001
 *   GPLL6_MODE  @ offset 0x13000 -> 0xA0000001
 *   GPLL7_MODE  @ offset 0x27000 -> 0xA0000001
 *   GPLL8_MODE  @ offset 0x1e000 -> 0xA0000001
 *
 * Reference: drivers/clk/qcom/gcc-sm6150.c, drivers/clk/qcom/clk-alpha-pll.c
 *
 * Copyright (c) 2024 CustomAndroidEmulator project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"

#define TYPE_QCOM_GCC_SM6150 "qcom-gcc-sm6150"
OBJECT_DECLARE_SIMPLE_TYPE(QcomGccSm6150State, QCOM_GCC_SM6150)

/* GCC register space size */
#define GCC_SIZE    0x1f0000

/*
 * PLL_MODE register value for a firmware-initialized, locked PLL:
 *   Bit 31: LOCK_DET = 1 (PLL is locked)
 *   Bit  0: PLL_OUTCTRL = 1 (output enable)
 *   Value: 0xA0000001 matches reference hardware captured state
 */
#define PLL_MODE_LOCKED   0xA0000001U
#define PLL_DEFAULT_L_VAL 0x0000001FU /* Multiplier: ~600MHz / 19.2MHz */

/* PLL MODE register offsets within GCC MMIO space */
#define GPLL0_MODE_OFF  0x00000
#define GPLL1_MODE_OFF  0x01000
#define GPLL6_MODE_OFF  0x13000
#define GPLL8_MODE_OFF  0x1e000
#define GPLL4_MODE_OFF  0x76000
#define GPLL7_MODE_OFF  0x27000

struct QcomGccSm6150State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /* Backing RAM for the full GCC register space */
    uint8_t regs[GCC_SIZE];
};

static uint64_t qcom_gcc_sm6150_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomGccSm6150State *s = QCOM_GCC_SM6150(opaque);
    uint64_t val = 0;

    if (offset + size > GCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-gcc-sm6150: read out of range: offset=0x%llx size=%u\n",
                      (unsigned long long)offset, size);
        return 0;
    }

    switch (size) {
    case 4:
        val = *(uint32_t *)(s->regs + offset);
        break;
    case 8:
        val = *(uint64_t *)(s->regs + offset);
        break;
    case 2:
        val = *(uint16_t *)(s->regs + offset);
        break;
    case 1:
        val = s->regs[offset];
        break;
    }

    return val;
}

static void qcom_gcc_sm6150_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned size)
{
    QcomGccSm6150State *s = QCOM_GCC_SM6150(opaque);

    if (offset + size > GCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-gcc-sm6150: write out of range: offset=0x%llx size=%u\n",
                      (unsigned long long)offset, size);
        return;
    }

    switch (size) {
    case 4:
        *(uint32_t *)(s->regs + offset) = (uint32_t)value;
        break;
    case 8:
        *(uint64_t *)(s->regs + offset) = value;
        break;
    case 2:
        *(uint16_t *)(s->regs + offset) = (uint16_t)value;
        break;
    case 1:
        s->regs[offset] = (uint8_t)value;
        break;
    }
}

static const MemoryRegionOps qcom_gcc_sm6150_ops = {
    .read = qcom_gcc_sm6150_read,
    .write = qcom_gcc_sm6150_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void init_pll_state(uint8_t *regs, uint32_t offset)
{
    /* PLL_MODE @ offset + 0x00: Locked and Output Enabled */
    *(uint32_t *)(regs + offset + 0x00) = PLL_MODE_LOCKED;
    /* PLL_L_VAL @ offset + 0x04: Configured multiplier */
    *(uint32_t *)(regs + offset + 0x04) = PLL_DEFAULT_L_VAL;
    /* PLL_USER_CTL @ offset + 0x0C: User control enable */
    *(uint32_t *)(regs + offset + 0x0C) = 0x00000001U;
    /* PLL_STATUS @ offset + 0x24: Status lock */
    *(uint32_t *)(regs + offset + 0x24) = 0x00000001U;
}

static void qcom_gcc_sm6150_reset(DeviceState *dev)
{
    QcomGccSm6150State *s = QCOM_GCC_SM6150(dev);

    /* Clear all registers */
    memset(s->regs, 0, sizeof(s->regs));

    /*
     * Pre-initialize all PLLs to firmware-locked state with valid L_VAL.
     * On real hardware, XBL configures these before handing off to Linux.
     * The Linux kernel gcc-sm6150 driver expects LOCK_DET=1 in PLL_MODE
     * and L_VAL != 0 so it does not enter clk_alpha_pll_configure().
     */
    init_pll_state(s->regs, GPLL0_MODE_OFF);
    init_pll_state(s->regs, GPLL1_MODE_OFF);
    init_pll_state(s->regs, GPLL6_MODE_OFF);
    init_pll_state(s->regs, GPLL8_MODE_OFF);
    init_pll_state(s->regs, GPLL4_MODE_OFF);
    init_pll_state(s->regs, GPLL7_MODE_OFF);
}

static void qcom_gcc_sm6150_realize(DeviceState *dev, Error **errp)
{
    QcomGccSm6150State *s = QCOM_GCC_SM6150(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &qcom_gcc_sm6150_ops, s,
                          "qcom-gcc-sm6150", GCC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void qcom_gcc_sm6150_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_gcc_sm6150_realize;
    dc->reset = qcom_gcc_sm6150_reset;
    dc->desc = "Qualcomm SM6150 GCC Clock Controller";
}

static const TypeInfo qcom_gcc_sm6150_info = {
    .name = TYPE_QCOM_GCC_SM6150,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomGccSm6150State),
    .class_init = qcom_gcc_sm6150_class_init,
};

static void qcom_gcc_sm6150_register_types(void)
{
    type_register_static(&qcom_gcc_sm6150_info);
}

type_init(qcom_gcc_sm6150_register_types)
