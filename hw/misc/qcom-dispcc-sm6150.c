/*
 * Qualcomm SM6150 Display Clock Controller (DISPCC) stub device for QEMU.
 *
 * This device emulates the Qualcomm SM6150 DISPCC clock controller at 0x0af00000.
 * It models PLL0 lock, RCG2 command update handshakes, and branch clock status
 * required for the native Linux kernel dispcc-sm6150 and sde-kms drivers.
 *
 * Reference: drivers/clk/qcom/dispcc-sm6150.c, drivers/clk/qcom/clk-rcg2.c
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

#define TYPE_QCOM_DISPCC_SM6150 "qcom-dispcc-sm6150"
OBJECT_DECLARE_SIMPLE_TYPE(QcomDispccSm6150State, QCOM_DISPCC_SM6150)

/* DISPCC register space size: 128 KB */
#define DISPCC_SIZE 0x20000

/*
 * PLL_MODE register value for a firmware-initialized / locked PLL:
 *   Bit 31: LOCK_DET = 1 (PLL is locked)
 *   Bit  0: PLL_OUTCTRL = 1 (output enable)
 */
#define PLL_MODE_LOCKED   0xA0000001U
#define PLL0_DEFAULT_L_VAL 0x0000001EU /* 30 * 19.2MHz = 576 MHz */

/* DISPCC PLL0 register offsets */
#define DISPCC_PLL0_MODE_OFF 0x00000

struct QcomDispccSm6150State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /* Backing RAM for the full DISPCC register space */
    uint8_t regs[DISPCC_SIZE];
};

static inline bool is_pll0_reg(hwaddr offset)
{
    return (offset <= 0x0030);
}

static inline bool is_rcg_cmd(hwaddr offset)
{
    switch (offset) {
    case 0x2060: /* disp_cc_mdss_pclk0_clk_src CMD_RCGR */
    case 0x2078: /* disp_cc_mdss_mdp_clk_src CMD_RCGR */
    case 0x2090: /* disp_cc_mdss_rot_clk_src CMD_RCGR */
    case 0x20A8: /* disp_cc_mdss_vsync_clk_src CMD_RCGR */
    case 0x20C0: /* disp_cc_mdss_byte0_clk_src CMD_RCGR */
    case 0x20DC: /* disp_cc_mdss_esc0_clk_src CMD_RCGR */
    case 0x20F4: /* disp_cc_mdss_dp_link_clk_src CMD_RCGR */
    case 0x2110: /* disp_cc_mdss_dp_pixel_clk_src CMD_RCGR */
    case 0x2120: /* disp_cc_mdss_dp_aux_clk_src CMD_RCGR */
    case 0x2128: /* disp_cc_mdss_dp_vco_div_clk_src CMD_RCGR */
    case 0x2140: /* disp_cc_mdss_dp_crypto_clk_src CMD_RCGR */
    case 0x2158: /* disp_cc_mdss_dp_gtc_clk_src CMD_RCGR */
    case 0x2170: /* disp_cc_mdss_ahb_clk_src CMD_RCGR */
        return true;
    default:
        return false;
    }
}

static inline bool is_branch_cbcr(hwaddr offset)
{
    switch (offset) {
    case 0x2004: /* disp_cc_mdss_pclk0_clk */
    case 0x2008: /* disp_cc_mdss_mdp_clk */
    case 0x2010: /* disp_cc_mdss_rot_clk */
    case 0x2018: /* disp_cc_mdss_mdp_lut_clk */
    case 0x2020: /* disp_cc_mdss_vsync_clk */
    case 0x2024: /* disp_cc_mdss_byte0_clk */
    case 0x2028: /* disp_cc_mdss_byte0_intf_clk */
    case 0x202c: /* disp_cc_mdss_esc0_clk */
    case 0x2030: /* disp_cc_mdss_dp_link_clk */
    case 0x2034: /* disp_cc_mdss_dp_link_intf_clk */
    case 0x2038: /* disp_cc_mdss_dp_crypto_clk */
    case 0x203c: /* disp_cc_mdss_dp_pixel_clk */
    case 0x2040: /* disp_cc_mdss_dp_pixel1_clk */
    case 0x2044: /* disp_cc_mdss_dp_aux_clk */
    case 0x2048: /* disp_cc_mdss_ahb_clk */
    case 0x6054: /* disp_cc_xo_clk */
        return true;
    default:
        /* Match branch registers in 0x2000..0x2070 or 0x6000..0x6060 */
        if ((offset >= 0x2000 && offset < 0x2078) ||
            (offset >= 0x6000 && offset <= 0x6060)) {
            return true;
        }
        return false;
    }
}

static uint64_t qcom_dispcc_sm6150_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomDispccSm6150State *s = QCOM_DISPCC_SM6150(opaque);
    uint64_t val = 0;

    if (offset + size > DISPCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-dispcc-sm6150: read out of range: offset=0x%llx size=%u\n",
                      (unsigned long long)offset, size);
        return 0;
    }

    switch (size) {
    case 4:
        val = *(uint32_t *)(s->regs + offset);
        if (offset == DISPCC_PLL0_MODE_OFF) {
            /* PLL0_MODE: Always locked and output enabled */
            val |= PLL_MODE_LOCKED;
        } else if (is_rcg_cmd(offset)) {
            /*
             * RCG2 CMD_RCGR:
             *   Bit  0: CMD_UPDATE = 0 (Hardware completed update)
             *   Bit 31: ROOT_OFF = 0 (Root clock generator running)
             */
            val &= ~0x80000001U;
        } else if (is_branch_cbcr(offset)) {
            /*
             * Branch CBCR:
             *   Bit  0: CLK_ENABLE
             *   Bit 31: CLK_OFF (0 when clock is running / enabled, 1 when halted / disabled)
             */
            if (val & 0x1) {
                val &= ~0x80000000U;
            } else {
                val |= 0x80000000U;
            }
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

static void qcom_dispcc_sm6150_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    QcomDispccSm6150State *s = QCOM_DISPCC_SM6150(opaque);

    if (offset + size > DISPCC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-dispcc-sm6150: write out of range: offset=0x%llx size=%u\n",
                      (unsigned long long)offset, size);
        return;
    }

    if (size == 4) {
        uint32_t val = (uint32_t)value;

        if (offset == DISPCC_PLL0_MODE_OFF) {
            /* Keep PLL locked and output enabled */
            val |= PLL_MODE_LOCKED;
        } else if (is_rcg_cmd(offset)) {
            /*
             * Hardware completes update immediately:
             * clear CMD_UPDATE (bit 0) and ROOT_OFF (bit 31).
             */
            val &= ~0x80000001U;
        } else if (is_branch_cbcr(offset)) {
            /*
             * Branch CBCR:
             * Reflect bit 31 CLK_OFF based on enable bit 0.
             */
            if (val & 0x1) {
                val &= ~0x80000000U;
            } else {
                val |= 0x80000000U;
            }
        }
        /*
         * Note: CFG_RCGR (e.g. 0x207C, 0x20AC) and all other registers
         * retain their exact written value.
         */
        *(uint32_t *)(s->regs + offset) = val;
    } else if (size == 8) {
        *(uint64_t *)(s->regs + offset) = value;
    } else if (size == 2) {
        *(uint16_t *)(s->regs + offset) = (uint16_t)value;
    } else if (size == 1) {
        s->regs[offset] = (uint8_t)value;
    }
}

static const MemoryRegionOps qcom_dispcc_sm6150_ops = {
    .read = qcom_dispcc_sm6150_read,
    .write = qcom_dispcc_sm6150_write,
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
    /* PLL_L_VAL @ offset + 0x04: Configured multiplier (576 MHz) */
    *(uint32_t *)(regs + offset + 0x04) = PLL0_DEFAULT_L_VAL;
    /* PLL_USER_CTL @ offset + 0x10 */
    *(uint32_t *)(regs + offset + 0x10) = 0x00000001U;
    /* PLL_CONFIG_CTL @ offset + 0x18 */
    *(uint32_t *)(regs + offset + 0x18) = 0x4001055bU;
    /* PLL_STATUS @ offset + 0x24: Status lock */
    *(uint32_t *)(regs + offset + 0x24) = 0x00000001U;
}

static void qcom_dispcc_sm6150_reset(DeviceState *dev)
{
    QcomDispccSm6150State *s = QCOM_DISPCC_SM6150(dev);

    /* Clear all registers to default 0 state */
    memset(s->regs, 0, sizeof(s->regs));

    /* Pre-initialize PLL0 to locked state */
    init_pll_state(s->regs, DISPCC_PLL0_MODE_OFF);
}

static void qcom_dispcc_sm6150_realize(DeviceState *dev, Error **errp)
{
    QcomDispccSm6150State *s = QCOM_DISPCC_SM6150(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &qcom_dispcc_sm6150_ops, s,
                          "qcom-dispcc-sm6150", DISPCC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
}

static void qcom_dispcc_sm6150_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_dispcc_sm6150_realize;
    dc->reset = qcom_dispcc_sm6150_reset;
    dc->desc = "Qualcomm SM6150 DISPCC Clock Controller";
}

static const TypeInfo qcom_dispcc_sm6150_info = {
    .name = TYPE_QCOM_DISPCC_SM6150,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomDispccSm6150State),
    .class_init = qcom_dispcc_sm6150_class_init,
};

static void qcom_dispcc_sm6150_register_types(void)
{
    type_register_static(&qcom_dispcc_sm6150_info);
}

type_init(qcom_dispcc_sm6150_register_types)
