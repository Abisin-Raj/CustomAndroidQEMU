/*
 * Qualcomm SM6150 DSI 14nm PHY (mdss_dsi_phy0) device for QEMU.
 *
 * This device emulates the Qualcomm DSI 14nm PHY at 0x0ae94400
 * (qcom,mdss_dsi_phy0, compatible "qcom,dsi-phy-v2.0").
 *
 * It models:
 *   - Common PHY controls: CMN_GLBL_TEST_CTRL, CMN_CTRL_1, CMN_LDO_CNTRL
 *   - Per-lane regulator controls: DSIPHY_DLNX_VREG_CNTRL(n) at offsets 0x164, 0x1e4, 0x264, 0x2e4, 0x364
 *   - Per-lane configurations: CFG, TEST_STR, TIMING_CTRL, STRENGTH_CTRL
 *   - PLL / buffer controls: DSIPHY_PLL_CLKBUFLR_EN (0x41c), DSIPHY_PLL_PLL_BANDGAP (0x508)
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

#define TYPE_QCOM_DSI_PHY "qcom-dsi-phy"
OBJECT_DECLARE_SIMPLE_TYPE(QcomDsiPhyState, QCOM_DSI_PHY)

/* DSI PHY register space: 4KB covering 0x588-byte hardware region */
#define QCOM_DSI_PHY_SIZE 0x1000

/* Register offsets */
#define DSIPHY_CMN_REVISION_ID0                   0x0000
#define DSIPHY_CMN_REVISION_ID1                   0x0004
#define DSIPHY_CMN_REVISION_ID2                   0x0008
#define DSIPHY_CMN_REVISION_ID3                   0x000C
#define DSIPHY_CMN_CLK_CFG0                       0x0010
#define DSIPHY_CMN_CLK_CFG1                       0x0014
#define DSIPHY_CMN_GLBL_TEST_CTRL                 0x0018
#define DSIPHY_CMN_CTRL_0                         0x001C
#define DSIPHY_CMN_CTRL_1                         0x0020
#define DSIPHY_CMN_PLL_CNTRL                      0x0048
#define DSIPHY_CMN_LDO_CNTRL                      0x004C
#define DSIPHY_CMN_REGULATOR_CAL_STATUS0          0x0064
#define DSIPHY_CMN_REGULATOR_CAL_STATUS1          0x0068

/* Per-lane VREG control offsets: 0x164 + (n * 0x80) */
#define DSIPHY_DLN0_VREG_CNTRL                    0x0164
#define DSIPHY_DLN1_VREG_CNTRL                    0x01E4
#define DSIPHY_DLN2_VREG_CNTRL                    0x0264
#define DSIPHY_DLN3_VREG_CNTRL                    0x02E4
#define DSIPHY_CKLN_VREG_CNTRL                    0x0364

#define DSIPHY_PLL_CLKBUFLR_EN                    0x041C
#define DSIPHY_PLL_RESETSM_CNTRL5                 0x043C
#define DSIPHY_PLL_PLL_BANDGAP                    0x0508

struct QcomDsiPhyState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /* Backing store for registers */
    uint8_t regs[QCOM_DSI_PHY_SIZE];
};

static void dsi_phy_log(const char *fmt, ...)
{
    static FILE *f = NULL;
    if (!f) {
        f = fopen("C:\\qemu_work\\dsi_phy.log", "w");
    }
    if (f) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fflush(f);
    }
}

static const char *dsi_phy_reg_name(hwaddr offset)
{
    switch (offset) {
    case DSIPHY_CMN_REVISION_ID0:          return "CMN_REVISION_ID0";
    case DSIPHY_CMN_REVISION_ID1:          return "CMN_REVISION_ID1";
    case DSIPHY_CMN_REVISION_ID2:          return "CMN_REVISION_ID2";
    case DSIPHY_CMN_REVISION_ID3:          return "CMN_REVISION_ID3";
    case DSIPHY_CMN_CLK_CFG0:              return "CMN_CLK_CFG0";
    case DSIPHY_CMN_CLK_CFG1:              return "CMN_CLK_CFG1";
    case DSIPHY_CMN_GLBL_TEST_CTRL:        return "CMN_GLBL_TEST_CTRL";
    case DSIPHY_CMN_CTRL_0:                return "CMN_CTRL_0";
    case DSIPHY_CMN_CTRL_1:                return "CMN_CTRL_1";
    case DSIPHY_CMN_PLL_CNTRL:             return "CMN_PLL_CNTRL";
    case DSIPHY_CMN_LDO_CNTRL:             return "CMN_LDO_CNTRL";
    case DSIPHY_CMN_REGULATOR_CAL_STATUS0: return "CMN_REGULATOR_CAL_STATUS0";
    case DSIPHY_CMN_REGULATOR_CAL_STATUS1: return "CMN_REGULATOR_CAL_STATUS1";
    case DSIPHY_DLN0_VREG_CNTRL:           return "DLN0_VREG_CNTRL";
    case DSIPHY_DLN1_VREG_CNTRL:           return "DLN1_VREG_CNTRL";
    case DSIPHY_DLN2_VREG_CNTRL:           return "DLN2_VREG_CNTRL";
    case DSIPHY_DLN3_VREG_CNTRL:           return "DLN3_VREG_CNTRL";
    case DSIPHY_CKLN_VREG_CNTRL:           return "CKLN_VREG_CNTRL";
    case DSIPHY_PLL_CLKBUFLR_EN:           return "PLL_CLKBUFLR_EN";
    case DSIPHY_PLL_RESETSM_CNTRL5:        return "PLL_RESETSM_CNTRL5";
    case DSIPHY_PLL_PLL_BANDGAP:           return "PLL_PLL_BANDGAP";
    default:
        if (offset >= 0x100 && offset < 0x380) {
            unsigned lane = (offset - 0x100) / 0x80;
            unsigned sub = (offset - 0x100) % 0x80;
            if (sub < 0x10) return (lane == 4) ? "CKLN_CFG" : "DLN_CFG";
            if (sub == 0x10) return "DLN_TEST_DATAPATH";
            if (sub == 0x14) return "DLN_TEST_STR";
            if (sub >= 0x18 && sub < 0x38) return "DLN_TIMING_CTRL";
            if (sub >= 0x38 && sub < 0x40) return "DLN_STRENGTH_CTRL";
            if (sub == 0x64) return "DLN_VREG_CNTRL";
        }
        return "DSIPHY_REG_OTHER";
    }
}

static uint64_t qcom_dsi_phy_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomDsiPhyState *s = opaque;
    uint32_t val = 0;

    if (offset + size > QCOM_DSI_PHY_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-dsi-phy: read out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return 0;
    }

    memcpy(&val, &s->regs[offset], size);

    dsi_phy_log("READ  [%-24s (0x%04lx)] size=%u -> 0x%08x\n",
                dsi_phy_reg_name(offset), (unsigned long)offset, size, val);

    qemu_log_mask(LOG_UNIMP,
                  "qcom-dsi-phy: read offset=0x%" HWADDR_PRIx " (%s) size=%u val=0x%08x\n",
                  offset, dsi_phy_reg_name(offset), size, val);

    return val;
}

static void qcom_dsi_phy_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    QcomDsiPhyState *s = opaque;

    if (offset + size > QCOM_DSI_PHY_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-dsi-phy: write out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return;
    }

    dsi_phy_log("WRITE [%-24s (0x%04lx)] size=%u <- 0x%08llx\n",
                dsi_phy_reg_name(offset), (unsigned long)offset, size, (unsigned long long)val);

    qemu_log_mask(LOG_UNIMP,
                  "qcom-dsi-phy: write offset=0x%" HWADDR_PRIx " (%s) size=%u val=0x%08" PRIx64 "\n",
                  offset, dsi_phy_reg_name(offset), size, val);

    memcpy(&s->regs[offset], &val, size);
}

static const MemoryRegionOps qcom_dsi_phy_ops = {
    .read = qcom_dsi_phy_read,
    .write = qcom_dsi_phy_write,
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

static void qcom_dsi_phy_reset(DeviceState *dev)
{
    QcomDsiPhyState *s = QCOM_DSI_PHY(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* Revision ID 2.0.0 */
    s->regs[DSIPHY_CMN_REVISION_ID0] = 0x00;
    s->regs[DSIPHY_CMN_REVISION_ID1] = 0x00;
    s->regs[DSIPHY_CMN_REVISION_ID2] = 0x02;
    s->regs[DSIPHY_CMN_REVISION_ID3] = 0x00;
}

static void qcom_dsi_phy_realize(DeviceState *dev, Error **errp)
{
    QcomDsiPhyState *s = QCOM_DSI_PHY(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &qcom_dsi_phy_ops, s,
                          "qcom-dsi-phy", QCOM_DSI_PHY_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    qcom_dsi_phy_reset(dev);
}

static void qcom_dsi_phy_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_dsi_phy_realize;
    dc->reset = qcom_dsi_phy_reset;
    dc->desc = "Qualcomm Snapdragon SM6150 DSI 14nm PHY (mdss_dsi_phy0)";
}

static const TypeInfo qcom_dsi_phy_info = {
    .name = TYPE_QCOM_DSI_PHY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomDsiPhyState),
    .class_init = qcom_dsi_phy_class_init,
};

static void qcom_dsi_phy_register_types(void)
{
    type_register_static(&qcom_dsi_phy_info);
}

type_init(qcom_dsi_phy_register_types)
