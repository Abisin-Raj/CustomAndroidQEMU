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
#include "hw/irq.h"
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
#define DSI_VIDEO_MODE_DATA_CTRL       0x0020
#define DSI_VIDEO_MODE_ACTIVE_H        0x0024
#define DSI_VIDEO_MODE_ACTIVE_V        0x0028
#define DSI_VIDEO_MODE_TOTAL           0x002C
#define DSI_VIDEO_MODE_HSYNC           0x0030
#define DSI_VIDEO_MODE_VSYNC           0x0034
#define DSI_VIDEO_MODE_VSYNC_VPOS      0x0038
#define DSI_COMMAND_MODE_DMA_CTRL      0x003C
#define DSI_COMMAND_MODE_MDP_CTRL      0x0040
#define DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL 0x0044
#define DSI_DMA_CMD_OFFSET             0x0048
#define DSI_DMA_CMD_LENGTH             0x004C
#define DSI_DMA_FIFO_CTRL              0x0050
#define DSI_TRIG_CTRL                  0x0084
#define DSI_CMD_MODE_DMA_SW_TRIGGER    0x0090
#define DSI_CMD_MODE_MDP_SW_TRIGGER    0x0094
#define DSI_CMD_MODE_BTA_SW_TRIGGER    0x0098
#define DSI_RESET_SW_TRIGGER           0x009C
#define DSI_MISR_VIDEO_CTRL            0x00A4
#define DSI_LANE_STATUS                0x00A8
#define DSI_LANE_CTRL                  0x00AC
#define DSI_LANE_SWAP_CTRL             0x00B0
#define DSI_DLN0_PHY_ERR               0x00B4
#define DSI_HS_TIMER_CTRL              0x00BC
#define DSI_TIMEOUT_STATUS             0x00C0
#define DSI_CLKOUT_TIMING_CTRL         0x00C4
#define DSI_EOT_PACKET_CTRL            0x00CC
#define DSI_DMA_SCHEDULE_CTRL          0x0100
#define DSI_ERR_INT_MASK0              0x010C
#define DSI_INT_CTRL                   0x0110
#define DSI_SOFT_RESET                 0x0118
#define DSI_CLK_CTRL                   0x011C
#define DSI_CLK_STATUS                 0x0120
#define DSI_PHY_SW_RESET               0x012C
#define DSI_TEST_PATTERN_GEN_CTRL      0x015C
#define DSI_TEST_PATTERN_GEN_VIDEO_ENABLE 0x0180
#define DSI_DSI_TIMING_FLUSH           0x01E4
#define DSI_DSI_TIMING_DB_MODE         0x01E8
#define DSI_TPG_DMA_FIFO_RESET         0x01EC
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
    case DSI_HW_VERSION:                 return "DSI_HW_VERSION";
    case DSI_CTRL:                       return "DSI_CTRL";
    case DSI_STATUS:                     return "DSI_STATUS";
    case DSI_FIFO_STATUS:                return "DSI_FIFO_STATUS";
    case DSI_VIDEO_MODE_CTRL:            return "DSI_VIDEO_MODE_CTRL";
    case DSI_VIDEO_MODE_DATA_CTRL:       return "DSI_VIDEO_MODE_DATA_CTRL";
    case DSI_VIDEO_MODE_ACTIVE_H:        return "DSI_VIDEO_MODE_ACTIVE_H";
    case DSI_VIDEO_MODE_ACTIVE_V:        return "DSI_VIDEO_MODE_ACTIVE_V";
    case DSI_VIDEO_MODE_TOTAL:           return "DSI_VIDEO_MODE_TOTAL";
    case DSI_VIDEO_MODE_HSYNC:           return "DSI_VIDEO_MODE_HSYNC";
    case DSI_VIDEO_MODE_VSYNC:           return "DSI_VIDEO_MODE_VSYNC";
    case DSI_VIDEO_MODE_VSYNC_VPOS:      return "DSI_VIDEO_MODE_VSYNC_VPOS";
    case DSI_COMMAND_MODE_DMA_CTRL:      return "DSI_COMMAND_MODE_DMA_CTRL";
    case DSI_COMMAND_MODE_MDP_CTRL:      return "DSI_COMMAND_MODE_MDP_CTRL";
    case DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL: return "DSI_COMMAND_MODE_MDP_DCS_CMD_CTRL";
    case DSI_DMA_CMD_OFFSET:             return "DSI_DMA_CMD_OFFSET";
    case DSI_DMA_CMD_LENGTH:             return "DSI_DMA_CMD_LENGTH";
    case DSI_DMA_FIFO_CTRL:              return "DSI_DMA_FIFO_CTRL";
    case DSI_TRIG_CTRL:                  return "DSI_TRIG_CTRL";
    case DSI_CMD_MODE_DMA_SW_TRIGGER:     return "DSI_CMD_MODE_DMA_SW_TRIGGER";
    case DSI_CMD_MODE_MDP_SW_TRIGGER:     return "DSI_CMD_MODE_MDP_SW_TRIGGER";
    case DSI_CMD_MODE_BTA_SW_TRIGGER:     return "DSI_CMD_MODE_BTA_SW_TRIGGER";
    case DSI_RESET_SW_TRIGGER:            return "DSI_RESET_SW_TRIGGER";
    case DSI_MISR_VIDEO_CTRL:            return "DSI_MISR_VIDEO_CTRL";
    case DSI_LANE_STATUS:                return "DSI_LANE_STATUS";
    case DSI_LANE_CTRL:                  return "DSI_LANE_CTRL";
    case DSI_LANE_SWAP_CTRL:             return "DSI_LANE_SWAP_CTRL";
    case DSI_DLN0_PHY_ERR:               return "DSI_DLN0_PHY_ERR";
    case DSI_HS_TIMER_CTRL:              return "DSI_HS_TIMER_CTRL";
    case DSI_TIMEOUT_STATUS:             return "DSI_TIMEOUT_STATUS";
    case DSI_CLKOUT_TIMING_CTRL:         return "DSI_CLKOUT_TIMING_CTRL";
    case DSI_EOT_PACKET_CTRL:            return "DSI_EOT_PACKET_CTRL";
    case DSI_DMA_SCHEDULE_CTRL:          return "DSI_DMA_SCHEDULE_CTRL";
    case DSI_ERR_INT_MASK0:              return "DSI_ERR_INT_MASK0";
    case DSI_INT_CTRL:                   return "DSI_INT_CTRL";
    case DSI_SOFT_RESET:                 return "DSI_SOFT_RESET";
    case DSI_CLK_CTRL:                   return "DSI_CLK_CTRL";
    case DSI_CLK_STATUS:                 return "DSI_CLK_STATUS";
    case DSI_PHY_SW_RESET:               return "DSI_PHY_SW_RESET";
    case DSI_TEST_PATTERN_GEN_CTRL:      return "DSI_TEST_PATTERN_GEN_CTRL";
    case DSI_TEST_PATTERN_GEN_VIDEO_ENABLE: return "DSI_TEST_PATTERN_GEN_VIDEO_ENABLE";
    case DSI_DSI_TIMING_FLUSH:           return "DSI_DSI_TIMING_FLUSH";
    case DSI_DSI_TIMING_DB_MODE:         return "DSI_DSI_TIMING_DB_MODE";
    case DSI_TPG_DMA_FIFO_RESET:         return "DSI_TPG_DMA_FIFO_RESET";
    case DSI_VERSION:                    return "DSI_VERSION";
    default:                             return "DSI_REG_OTHER";
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

    dsi_ctrl_log("READ  [%-34s (0x%04lx)] size=%u -> 0x%08x\n",
                 dsi_reg_name(offset), (unsigned long)offset, size, val);

    if (offset == DSI_INT_CTRL) {
        dsi_ctrl_log("  -> READ DSI_INT_CTRL: 0x%08x [cmd_dma_done_status(bit0)=%u, cmd_dma_done_mask(bit1)=%u, cmd_frame_done_status(bit8)=%u, vid_frame_done_status(bit16)=%u, bta_done(bit20)=%u, err_status(bit24)=%u, err_mask(bit25)=%u]\n",
                     val,
                     val & 1, (val >> 1) & 1,
                     (val >> 8) & 1,
                     (val >> 16) & 1,
                     (val >> 20) & 1,
                     (val >> 24) & 1, (val >> 25) & 1);
    } else if (offset == DSI_FIFO_STATUS) {
        dsi_ctrl_log("  -> READ DSI_FIFO_STATUS: 0x%08x\n", val);
    } else if (offset == DSI_STATUS) {
        dsi_ctrl_log("  -> READ DSI_STATUS: 0x%08x\n", val);
    }

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

    dsi_ctrl_log("WRITE [%-34s (0x%04lx)] size=%u <- 0x%08llx\n",
                 dsi_reg_name(offset), (unsigned long)offset, size, (unsigned long long)val);

    qemu_log_mask(LOG_UNIMP,
                  "qcom-dsi-ctrl: write offset=0x%" HWADDR_PRIx " (%s) size=%u val=0x%08" PRIx64 "\n",
                  offset, dsi_reg_name(offset), size, val);

    memcpy(&s->regs[offset], &val, size);

    /* Special register effects */
    if (offset == DSI_CMD_MODE_DMA_SW_TRIGGER) {
        uint32_t dma_offset = *(uint32_t *)&s->regs[DSI_DMA_CMD_OFFSET];
        uint32_t dma_len = *(uint32_t *)&s->regs[DSI_DMA_CMD_LENGTH];
        uint32_t dma_ctrl = *(uint32_t *)&s->regs[DSI_COMMAND_MODE_DMA_CTRL];
        uint32_t fifo_ctrl = *(uint32_t *)&s->regs[DSI_DMA_FIFO_CTRL];
        uint32_t int_ctrl = *(uint32_t *)&s->regs[DSI_INT_CTRL];
        dsi_ctrl_log("  -> DSI_CMD_MODE_DMA_SW_TRIGGER written: val=0x%08llx\n"
                     "     DMA Configuration Snapshot:\n"
                     "       OFFSET:    0x%08x\n"
                     "       LENGTH:    %u bytes (0x%x)\n"
                     "       DMA_CTRL:  0x%08x (bcast=%u, master=%u, lpm=%u, embedded=%u)\n"
                     "       FIFO_CTRL: 0x%08x\n"
                     "       INT_CTRL:  0x%08x (cmd_dma_done_mask(bit1)=%u, cmd_dma_done_status(bit0)=%u)\n",
                     (unsigned long long)val,
                     dma_offset, dma_len, dma_len,
                     dma_ctrl, (dma_ctrl >> 31) & 1, (dma_ctrl >> 30) & 1, (dma_ctrl >> 26) & 1, (dma_ctrl >> 28) & 1,
                     fifo_ctrl,
                     int_ctrl, (int_ctrl >> 1) & 1, int_ctrl & 1);

        if (val & 0x1) {
            uint32_t *int_ctrl_p = (uint32_t *)&s->regs[DSI_INT_CTRL];
            *int_ctrl_p |= (1U << 0); /* BIT(0) = DSI_CMD_MODE_DMA_DONE */
            dsi_ctrl_log("  -> DSI_CMD_MODE_DMA_SW_TRIGGER: asserted DSI_INT_CTRL bit 0 (val=0x%08x)\n",
                         *int_ctrl_p);

            if (*int_ctrl_p & (1U << 1)) { /* BIT(1) = DSI_CMD_MODE_DMA_DONE_MASK */
                dsi_ctrl_log("  -> DSI_CMD_MODE_DMA_SW_TRIGGER: pulsing s->irq\n");
                qemu_irq_pulse(s->irq);
            }
        }
    } else if (offset == DSI_INT_CTRL) {
        uint32_t int_ctrl = (uint32_t)val;
        uint32_t *int_ctrl_p = (uint32_t *)&s->regs[DSI_INT_CTRL];
        /* Preserve W1C clearing: writing 1 to bit 0 clears bit 0 */
        if (int_ctrl & 1U) {
            *int_ctrl_p &= ~1U;
        }
        dsi_ctrl_log("  -> WRITE DSI_INT_CTRL: 0x%08x [clr_status_bit0=%u, cmd_dma_done_mask_bit1=%u, err_mask_bit25=%u, result=0x%08x]\n",
                     int_ctrl, int_ctrl & 1, (int_ctrl >> 1) & 1, (int_ctrl >> 25) & 1, *int_ctrl_p);
    } else if (offset == DSI_FIFO_STATUS) {
        dsi_ctrl_log("  -> WRITE DSI_FIFO_STATUS: 0x%08llx\n", (unsigned long long)val);
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
