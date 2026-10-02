/*
 * Qualcomm SM6150 DSI Controller (mdss_dsi_ctrl0) device for QEMU.
 *
 * This device emulates the Qualcomm DSI host controller at 0x0ae94000
 * (qcom,mdss_dsi_ctrl0, compatible "qcom,dsi-ctrl-hw-v2.3").
 *
 * It models:
 *   - Hardware revision registers: DSI_HW_VERSION (0x0000) and DSI_VERSION (0x01F4)
 *   - Interrupt control / status: DSI_INT_CTRL (0x0110) and DSI_ERR_INT_MASK0 (0x010C)
 *   - Engine control & status: DSI_CTRL (0x0004), DSI_STATUS (0x0008), DSI_FIFO_STATUS (0x000C)
 *   - PHY SW reset: DSI_PHY_SW_RESET (0x012C)
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

#define TYPE_QCOM_DSI_CTRL "qcom-dsi-ctrl"
OBJECT_DECLARE_SIMPLE_TYPE(QcomDsiCtrlState, QCOM_DSI_CTRL)

/* DSI controller register space: 0x400 bytes as specified in Device Tree */
#define QCOM_DSI_CTRL_SIZE 0x400

/* Register offsets */
#define DSI_HW_VERSION                 0x0000
#define DSI_CTRL                       0x0004
#define DSI_STATUS                     0x0008
#define DSI_FIFO_STATUS                0x000C
#define DSI_VIDEO_MODE_CTRL            0x0010
#define DSI_LANE_STATUS                0x00A8
#define DSI_LANE_CTRL                  0x00AC
#define DSI_DLN0_PHY_ERR               0x00B4
#define DSI_TIMEOUT_STATUS             0x00C0
#define DSI_ERR_INT_MASK0              0x010C
#define DSI_INT_CTRL                   0x0110
#define DSI_SOFT_RESET                 0x0118
#define DSI_CLK_CTRL                   0x011C
#define DSI_CLK_STATUS                 0x0120
#define DSI_PHY_SW_RESET               0x012C
#define DSI_VERSION                    0x01F4

/* Qualcomm DSI Controller v2.3.0 hardware version constants */
#define DSI_HW_VERSION_V2_3            0x20030000U
#define DSI_VERSION_V2_3               0x02030000U

struct QcomDsiCtrlState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t hw_version;
    /* Backing store for registers */
    uint8_t regs[QCOM_DSI_CTRL_SIZE];
};

static void dsi_ctrl_log(const char *fmt, ...)
{
    static FILE *f = NULL;
    if (!f) {
        f = fopen("C:\\qemu_work\\dsi_ctrl.log", "w");
    }
    if (f) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fflush(f);
    }
}

static const char *dsi_reg_name(hwaddr offset)
{
    switch (offset) {
    case DSI_HW_VERSION:    return "DSI_HW_VERSION";
    case DSI_CTRL:          return "DSI_CTRL";
    case DSI_STATUS:        return "DSI_STATUS";
    case DSI_FIFO_STATUS:   return "DSI_FIFO_STATUS";
    case DSI_VIDEO_MODE_CTRL: return "DSI_VIDEO_MODE_CTRL";
    case DSI_LANE_STATUS:   return "DSI_LANE_STATUS";
    case DSI_LANE_CTRL:     return "DSI_LANE_CTRL";
    case DSI_DLN0_PHY_ERR:  return "DSI_DLN0_PHY_ERR";
    case DSI_TIMEOUT_STATUS: return "DSI_TIMEOUT_STATUS";
    case DSI_ERR_INT_MASK0: return "DSI_ERR_INT_MASK0";
    case DSI_INT_CTRL:      return "DSI_INT_CTRL";
    case DSI_SOFT_RESET:    return "DSI_SOFT_RESET";
    case DSI_CLK_CTRL:      return "DSI_CLK_CTRL";
    case DSI_CLK_STATUS:    return "DSI_CLK_STATUS";
    case DSI_PHY_SW_RESET:  return "DSI_PHY_SW_RESET";
    case DSI_VERSION:       return "DSI_VERSION";
    default:                return "DSI_REG_OTHER";
    }
}

static uint64_t qcom_dsi_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomDsiCtrlState *s = opaque;
    uint32_t val = 0;

    if (offset + size > QCOM_DSI_CTRL_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-dsi-ctrl: read out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return 0;
    }

    if (offset == DSI_HW_VERSION) {
        val = s->hw_version;
    } else if (offset == DSI_VERSION) {
        val = DSI_VERSION_V2_3;
    } else {
        memcpy(&val, &s->regs[offset], size);
    }

    dsi_ctrl_log("READ  [%-20s (0x%04lx)] size=%u -> 0x%08x\n",
                 dsi_reg_name(offset), (unsigned long)offset, size, val);

    qemu_log_mask(LOG_UNIMP,
                  "qcom-dsi-ctrl: read offset=0x%" HWADDR_PRIx " (%s) size=%u val=0x%08x\n",
                  offset, dsi_reg_name(offset), size, val);

    return val;
}

static void qcom_dsi_ctrl_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    QcomDsiCtrlState *s = opaque;

    if (offset + size > QCOM_DSI_CTRL_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "qcom-dsi-ctrl: write out of range: offset=0x%" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return;
    }

    dsi_ctrl_log("WRITE [%-20s (0x%04lx)] size=%u <- 0x%08llx\n",
                 dsi_reg_name(offset), (unsigned long)offset, size, (unsigned long long)val);

    qemu_log_mask(LOG_UNIMP,
                  "qcom-dsi-ctrl: write offset=0x%" HWADDR_PRIx " (%s) size=%u val=0x%08" PRIx64 "\n",
                  offset, dsi_reg_name(offset), size, val);

    memcpy(&s->regs[offset], &val, size);

    /* Special register effects */
    if (offset == DSI_INT_CTRL) {
        uint32_t int_ctrl = (uint32_t)val;
        dsi_ctrl_log("  -> DSI_INT_CTRL updated: err_mask_bit25=%u status_en=0x%08x\n",
                     (int_ctrl >> 25) & 1, int_ctrl & 0xAAAAAA02U);
    } else if (offset == DSI_PHY_SW_RESET) {
        dsi_ctrl_log("  -> DSI_PHY_SW_RESET: val=0x%08llx\n", (unsigned long long)val);
    }
}

static const MemoryRegionOps qcom_dsi_ctrl_ops = {
    .read = qcom_dsi_ctrl_read,
    .write = qcom_dsi_ctrl_write,
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

static void qcom_dsi_ctrl_reset(DeviceState *dev)
{
    QcomDsiCtrlState *s = QCOM_DSI_CTRL(dev);

    memset(s->regs, 0, sizeof(s->regs));
    *(uint32_t *)&s->regs[DSI_HW_VERSION] = s->hw_version;
    *(uint32_t *)&s->regs[DSI_VERSION] = DSI_VERSION_V2_3;
}

static void qcom_dsi_ctrl_realize(DeviceState *dev, Error **errp)
{
    QcomDsiCtrlState *s = QCOM_DSI_CTRL(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &qcom_dsi_ctrl_ops, s,
                          "qcom-dsi-ctrl", QCOM_DSI_CTRL_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    qcom_dsi_ctrl_reset(dev);
}

static Property qcom_dsi_ctrl_properties[] = {
    DEFINE_PROP_UINT32("hw-version", QcomDsiCtrlState, hw_version, DSI_HW_VERSION_V2_3),
    DEFINE_PROP_END_OF_LIST(),
};

static void qcom_dsi_ctrl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_dsi_ctrl_realize;
    dc->reset = qcom_dsi_ctrl_reset;
    dc->desc = "Qualcomm Snapdragon SM6150 DSI Controller (mdss_dsi_ctrl0)";
    device_class_set_props(dc, qcom_dsi_ctrl_properties);
}

static const TypeInfo qcom_dsi_ctrl_info = {
    .name = TYPE_QCOM_DSI_CTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomDsiCtrlState),
    .class_init = qcom_dsi_ctrl_class_init,
};

static void qcom_dsi_ctrl_register_types(void)
{
    type_register_static(&qcom_dsi_ctrl_info);
}

type_init(qcom_dsi_ctrl_register_types)
