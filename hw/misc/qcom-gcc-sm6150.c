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

static inline bool is_pll_reg(hwaddr offset)
{
    return ((offset <= GPLL0_MODE_OFF + 0x30) ||
            (offset >= GPLL1_MODE_OFF && offset <= GPLL1_MODE_OFF + 0x30) ||
            (offset >= GPLL6_MODE_OFF && offset <= GPLL6_MODE_OFF + 0x30) ||
            (offset >= GPLL8_MODE_OFF && offset <= GPLL8_MODE_OFF + 0x30) ||
            (offset >= GPLL4_MODE_OFF && offset <= GPLL4_MODE_OFF + 0x30) ||
            (offset >= GPLL7_MODE_OFF && offset <= GPLL7_MODE_OFF + 0x30));
}

static inline bool is_vote_reg(hwaddr offset)
{
    /* APCS voting registers for shared clocks and PLLs (0x38000 - 0x38030) */
    return (offset >= 0x38000 && offset <= 0x38030);
}

static inline bool is_ufs_rcg_cmd(hwaddr offset)
{
    switch (offset) {
    case 0x77014: /* gcc_ufs_phy_axi_clk_src CMD_RCGR */
    case 0x77044: /* gcc_ufs_phy_unipro_core_clk_src CMD_RCGR */
    case 0x77060: /* gcc_ufs_phy_ice_core_clk_src CMD_RCGR */
    case 0x77094: /* gcc_ufs_phy_phy_aux_clk_src CMD_RCGR */
        return true;
    default:
        return false;
    }
}

static inline bool is_ufs_cbcr(hwaddr offset)
{
    /* All UFS clock branches in 0x77000..0x770ff and the UFS reference clock 0x8c000 */
    return ((offset >= 0x77000 && offset <= 0x770ff && !is_ufs_rcg_cmd(offset)) ||
            offset == 0x8c000);
}

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
        if (is_ufs_cbcr(offset)) {
            /*
             * Qualcomm UFS Branch Control Register (CBCR):
             * Bit 0: CLK_ENABLE / HWCG mode active.
             * Bit 31: CLK_OFF = 0 (running).
             * Guarantees all kernel clock enable/disable/hwcg checks succeed instantly.
             */
            return 0x00000001U;
        } else if (is_ufs_rcg_cmd(offset)) {
            /*
             * Qualcomm RCG2 Command Register (CMD_RCGR):
             * Bit  0: UPDATE = 0 (Hardware completed update)
             * Bit 31: ROOT_OFF = 0 (Root generator is active)
             */
            val &= ~0x80000001U;
        }
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

    if (size == 4) {
        uint32_t val = (uint32_t)value;

        if (is_pll_reg(offset)) {
            /* Keep PLL locked and output enabled */
            val |= PLL_MODE_LOCKED;
        } else if (is_vote_reg(offset)) {
            /* Voting registers (APCS_*_ENA_VOTE): store exact bitmask */
        } else if (is_ufs_cbcr(offset)) {
            /*
             * Model Qualcomm Branch Control Register (CBCR) hardware behavior:
             *   Bit  0: CLK_ENABLE (1 = enable, 0 = disable)
             *   Bit 31: CLK_OFF    (0 = running, 1 = gated/off)
             *   Bits [30:28]: NOC FSM status (0 = ON, 2 = OFF)
             */
            if (val & 1) {
                val &= ~0xF0000000U;
            } else {
                val = (val & ~0xF0000000U) | 0x80000000U;
            }
        } else if (is_ufs_rcg_cmd(offset)) {
            /* Auto-clear UPDATE bit on write and ensure ROOT_OFF = 0 */
            val &= ~0x80000001U;
        }

        *(uint32_t *)(s->regs + offset) = val;
    } else if (size == 8) {
        *(uint64_t *)(s->regs + offset) = value;
    } else if (size == 2) {
        *(uint16_t *)(s->regs + offset) = (uint16_t)value;
    } else if (size == 1) {
        s->regs[offset] = (uint8_t)value;
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

static const uint32_t ufs_hwcg_cbcr_offsets[] = {
    0x77010, /* gcc_ufs_phy_axi_clk */
    0x77038, /* gcc_ufs_phy_ahb_clk */
    0x77040, /* gcc_ufs_phy_unipro_core_clk */
    0x77058, /* gcc_ufs_phy_ice_core_clk */
    0x7705c, /* gcc_ufs_phy_tx_symbol_0_clk */
    0x77078, /* gcc_ufs_phy_rx_symbol_0_clk */
    0x7708c, /* gcc_ufs_phy_rx_symbol_1_clk */
    0x77090, /* gcc_ufs_phy_phy_aux_clk */
    0x770c0, /* gcc_ufs_phy_unipro_core_clk */
};

static void qcom_gcc_sm6150_reset(DeviceState *dev)
{
    QcomGccSm6150State *s = QCOM_GCC_SM6150(dev);

    /* Clear all registers to default 0 state */
    memset(s->regs, 0, sizeof(s->regs));

    /*
     * Pre-initialize all UFS branch clocks to active / running state (CLK_OFF = 0).
     */
    for (size_t i = 0; i < ARRAY_SIZE(ufs_hwcg_cbcr_offsets); i++) {
        uint32_t off = ufs_hwcg_cbcr_offsets[i];
        if (off + 4 <= GCC_SIZE) {
            *(uint32_t *)(s->regs + off) = 0x00000000U;
        }
    }

    /*
     * Pre-initialize gcc_ufs_mem_clkref_clk (0x8c000) to disabled/gated state (CLK_OFF = 1).
     * The Linux UFS platform driver disables this reference clock on initial setup.
     */
    *(uint32_t *)(s->regs + 0x8c000) = 0x80000000U;

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
