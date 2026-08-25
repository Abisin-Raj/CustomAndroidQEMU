/*
 * Qualcomm SM6150 UFS Host Controller (UFSHCI) & QMP PHY emulation for QEMU.
 *
 * Emulates the UFSHCI 2.1 host controller at 0x1d84000, the QMP UFS PHY at
 * 0x1d87000, and the UFS ICE crypto block at 0x1d90000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "exec/address-spaces.h"
#include "sysemu/dma.h"

#define TYPE_QCOM_UFSHC "qcom-ufshc"
OBJECT_DECLARE_SIMPLE_TYPE(QcomUfsState, QCOM_UFSHC)

#define UFSHC_MMIO_SIZE     0x3000
#define UFSPHY_MMIO_SIZE    0x1000
#define UFSICE_MMIO_SIZE    0x8000

/* UFSHCI standard register offsets */
#define REG_CAPABILITIES            0x00
#define REG_UFS_VERSION             0x08
#define REG_CONTROLLER_PID          0x0C
#define REG_CONTROLLER_MID          0x10
#define REG_AHIT                    0x14
#define REG_INTERRUPT_STATUS        0x20
#define REG_INTERRUPT_ENABLE        0x24
#define REG_HOST_CONTROLLER_STATUS  0x30
#define REG_HOST_CONTROLLER_ENABLE  0x34
#define REG_UTRLBA                  0x3C
#define REG_UTRLBAU                 0x40
#define REG_UTRLDBR                 0x44
#define REG_UTRLCLR                 0x48
#define REG_UTRLRSR                 0x4C
#define REG_UTMRLBA                 0x50
#define REG_UTMRLBAU                0x54
#define REG_UTMRLDBR                0x58
#define REG_UTMRLCLR                0x5C
#define REG_UTMRLRSR                0x60
#define REG_UICCMD                  0x90
#define REG_UICCMDARG1              0x94
#define REG_UICCMDARG2              0x98
#define REG_UICCMDARG3              0x9C

/* UIC Command codes */
#define UIC_CMD_DME_GET             0x01
#define UIC_CMD_DME_SET             0x02
#define UIC_CMD_DME_PEER_GET        0x03
#define UIC_CMD_DME_PEER_SET        0x04
#define UIC_CMD_DME_POWERON         0x10
#define UIC_CMD_DME_POWEROFF        0x11
#define UIC_CMD_DME_ENABLE          0x12
#define UIC_CMD_DME_RESET           0x14
#define UIC_CMD_DME_ENDPOINT_RESET   0x15
#define UIC_CMD_DME_LINK_STARTUP    0x16
#define UIC_CMD_DME_HIBERN8_ENTER   0x17
#define UIC_CMD_DME_HIBERN8_EXIT    0x18

/* Interrupt status bits */
#define INT_UTRCS                   (1 << 0)
#define INT_UDEPRI                  (1 << 1)
#define INT_UE                      (1 << 2)
#define INT_UTMS                    (1 << 3)
#define INT_UPMS                    (1 << 4)
#define INT_UHXS                    (1 << 5)
#define INT_UHES                    (1 << 6)
#define INT_ULLS                    (1 << 7)
#define INT_ULSS                    (1 << 8)
#define INT_UTMRCS                  (1 << 9)
#define INT_UCCS                    (1 << 10)

struct QcomUfsState {
    SysBusDevice parent_obj;

    MemoryRegion ufshc_mmio;
    MemoryRegion ufsphy_mmio;
    MemoryRegion ufsice_mmio;
    qemu_irq irq;

    uint8_t ufshc_regs[UFSHC_MMIO_SIZE];
    uint8_t ufsphy_regs[UFSPHY_MMIO_SIZE];
    uint8_t ufsice_regs[UFSICE_MMIO_SIZE];

    /* Internal attributes for DME_GET / DME_SET */
    uint32_t pa_avail_tx_lanes;
    uint32_t pa_avail_rx_lanes;
    uint32_t pa_active_tx_lanes;
    uint32_t pa_active_rx_lanes;
    uint32_t pa_connected_tx_lanes;
    uint32_t pa_connected_rx_lanes;
    uint32_t pa_tx_gear;
    uint32_t pa_rx_gear;
    uint32_t pa_hs_series;
};

static void qcom_ufs_update_irq(QcomUfsState *s)
{
    uint32_t is = *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS);
    uint32_t ie = *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_ENABLE);

    qemu_set_irq(s->irq, (is & ie) != 0);
}

static uint32_t get_uic_attr(QcomUfsState *s, uint32_t attr_sel)
{
    uint32_t attr = (attr_sel >> 16) & 0xFFFF;
    switch (attr) {
    case 0x1560: /* PA_AvailTxDataLanes */
        return s->pa_avail_tx_lanes;
    case 0x1580: /* PA_AvailRxDataLanes */
        return s->pa_avail_rx_lanes;
    case 0x1561: /* PA_ActiveTxDataLanes */
        return s->pa_active_tx_lanes;
    case 0x1581: /* PA_ActiveRxDataLanes */
        return s->pa_active_rx_lanes;
    case 0x1562: /* PA_ConnectedTxDataLanes */
        return s->pa_connected_tx_lanes;
    case 0x1582: /* PA_ConnectedRxDataLanes */
        return s->pa_connected_rx_lanes;
    case 0x1563: /* PA_TxGear */
        return s->pa_tx_gear;
    case 0x1583: /* PA_RxGear */
        return s->pa_rx_gear;
    case 0x156A: /* PA_HSSeries */
        return s->pa_hs_series;
    case 0x1569: /* PA_TxTermination */
    case 0x1589: /* PA_RxTermination */
        return 1;
    case 0x155E: /* PA_Local_TX_LCC_Enable */
    case 0x155F: /* PA_Peer_TX_LCC_Enable */
        return 0;
    case 0xD041: /* DME_FC0ProtectionTimeOutVal */
    case 0xD044: /* DME_FC1ProtectionTimeOutVal */
        return 0x1FFF;
    case 0xD042: /* DME_TC0ReplayTimeOutVal */
    case 0xD045: /* DME_TC1ReplayTimeOutVal */
        return 0xFFFF;
    case 0xD043: /* DME_AFC0ReqTimeOutVal */
    case 0xD046: /* DME_AFC1ReqTimeOutVal */
        return 0x7FFF;
    default:
        return 0;
    }
}

static void set_uic_attr(QcomUfsState *s, uint32_t attr_sel, uint32_t val)
{
    uint32_t attr = (attr_sel >> 16) & 0xFFFF;
    switch (attr) {
    case 0x1561: /* PA_ActiveTxDataLanes */
        s->pa_active_tx_lanes = val;
        break;
    case 0x1581: /* PA_ActiveRxDataLanes */
        s->pa_active_rx_lanes = val;
        break;
    case 0x1563: /* PA_TxGear */
        s->pa_tx_gear = val;
        break;
    case 0x1583: /* PA_RxGear */
        s->pa_rx_gear = val;
        break;
    case 0x156A: /* PA_HSSeries */
        s->pa_hs_series = val;
        break;
    default:
        break;
    }
}

static void handle_uic_command(QcomUfsState *s, uint32_t cmd)
{
    uint32_t cmd_op = cmd & 0xFF;
    uint32_t arg1 = *(uint32_t *)(s->ufshc_regs + REG_UICCMDARG1);
    uint32_t arg3 = *(uint32_t *)(s->ufshc_regs + REG_UICCMDARG3);
    uint32_t is = *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS);

    switch (cmd_op) {
    case UIC_CMD_DME_GET:
    case UIC_CMD_DME_PEER_GET:
        *(uint32_t *)(s->ufshc_regs + REG_UICCMDARG3) = get_uic_attr(s, arg1);
        break;
    case UIC_CMD_DME_SET:
    case UIC_CMD_DME_PEER_SET:
        set_uic_attr(s, arg1, arg3);
        break;
    case UIC_CMD_DME_LINK_STARTUP:
        /* UniPro link startup successful: set status bits */
        is |= INT_ULSS; /* UniPro Link Startup Status */
        break;
    case UIC_CMD_DME_RESET:
    case UIC_CMD_DME_ENABLE:
    case UIC_CMD_DME_POWERON:
    case UIC_CMD_DME_HIBERN8_ENTER:
    case UIC_CMD_DME_HIBERN8_EXIT:
        break;
    default:
        break;
    }

    /* Set UIC Command Completion Status */
    is |= INT_UCCS;
    *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS) = is;
    /* Host controller status: UCRDY=1 (UIC Command Ready) */
    *(uint32_t *)(s->ufshc_regs + REG_HOST_CONTROLLER_STATUS) |= 0x00000008U;

    qcom_ufs_update_irq(s);
}

/* UFSHCI Host Controller MMIO */
static uint64_t qcom_ufshc_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomUfsState *s = QCOM_UFSHC(opaque);
    uint64_t val = 0;

    if (offset + size > UFSHC_MMIO_SIZE) {
        return 0;
    }

    if (size == 4) {
        val = *(uint32_t *)(s->ufshc_regs + offset);
    } else if (size == 8) {
        val = *(uint64_t *)(s->ufshc_regs + offset);
    } else {
        memcpy(&val, s->ufshc_regs + offset, size);
    }

    return val;
}

static void qcom_ufshc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    QcomUfsState *s = QCOM_UFSHC(opaque);

    if (offset + size > UFSHC_MMIO_SIZE) {
        return;
    }

    if (size == 4) {
        uint32_t val = (uint32_t)value;

        if (offset == REG_INTERRUPT_STATUS) {
            /* W1C: Write 1 to clear */
            uint32_t is = *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS);
            *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS) = is & ~val;
            qcom_ufs_update_irq(s);
            return;
        } else if (offset == REG_INTERRUPT_ENABLE) {
            *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_ENABLE) = val;
            qcom_ufs_update_irq(s);
            return;
        } else if (offset == REG_HOST_CONTROLLER_ENABLE) {
            if (val & 1) {
                *(uint32_t *)(s->ufshc_regs + REG_HOST_CONTROLLER_ENABLE) = 1;
                *(uint32_t *)(s->ufshc_regs + REG_HOST_CONTROLLER_STATUS) = 0x0000000F;
            } else {
                *(uint32_t *)(s->ufshc_regs + REG_HOST_CONTROLLER_ENABLE) = 0;
            }
            return;
        } else if (offset == REG_UICCMD) {
            *(uint32_t *)(s->ufshc_regs + REG_UICCMD) = val;
            handle_uic_command(s, val);
            return;
        } else if (offset == REG_UTRLDBR) {
            /* Process transfer requests (NOP / Query / SCSI) */
            *(uint32_t *)(s->ufshc_regs + REG_UTRLDBR) = 0;
            *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS) |= INT_UTRCS;
            qcom_ufs_update_irq(s);
            return;
        } else if (offset == REG_UTMRLDBR) {
            /* Task Management Request */
            *(uint32_t *)(s->ufshc_regs + REG_UTMRLDBR) = 0;
            *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS) |= INT_UTMRCS;
            qcom_ufs_update_irq(s);
            return;
        }

        *(uint32_t *)(s->ufshc_regs + offset) = val;
    } else if (size == 8) {
        *(uint64_t *)(s->ufshc_regs + offset) = value;
    } else {
        memcpy(s->ufshc_regs + offset, &value, size);
    }
}

static const MemoryRegionOps qcom_ufshc_ops = {
    .read = qcom_ufshc_read,
    .write = qcom_ufshc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* QMP UFS PHY MMIO */
static uint64_t qcom_ufsphy_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomUfsState *s = QCOM_UFSHC(opaque);
    uint64_t val = 0;

    if (offset + size > UFSPHY_MMIO_SIZE) {
        return 0;
    }

    /*
     * QPHY_PCS_READY_STATUS register:
     * Located at PCS block offset (0x170 / 0x370 / 0x570 depending on PHY version).
     * Returns Bit 0 = 1 (PCS_READY = 1) so phy_power_on() completes instantly.
     */
    if ((offset & 0x1FF) == 0x170 || offset == 0x170 || offset == 0x370 || offset == 0x570) {
        return 0x00000001U;
    }

    if (size == 4) {
        val = *(uint32_t *)(s->ufsphy_regs + offset);
    } else {
        memcpy(&val, s->ufsphy_regs + offset, size);
    }

    return val;
}

static void qcom_ufsphy_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    QcomUfsState *s = QCOM_UFSHC(opaque);

    if (offset + size > UFSPHY_MMIO_SIZE) {
        return;
    }

    if (size == 4) {
        *(uint32_t *)(s->ufsphy_regs + offset) = (uint32_t)value;
    } else {
        memcpy(s->ufsphy_regs + offset, &value, size);
    }
}

static const MemoryRegionOps qcom_ufsphy_ops = {
    .read = qcom_ufsphy_read,
    .write = qcom_ufsphy_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* UFS ICE MMIO */
static uint64_t qcom_ufsice_read(void *opaque, hwaddr offset, unsigned size)
{
    QcomUfsState *s = QCOM_UFSHC(opaque);
    uint64_t val = 0;

    if (offset + size > UFSICE_MMIO_SIZE) {
        return 0;
    }

    if (size == 4) {
        val = *(uint32_t *)(s->ufsice_regs + offset);
    } else {
        memcpy(&val, s->ufsice_regs + offset, size);
    }

    return val;
}

static void qcom_ufsice_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    QcomUfsState *s = QCOM_UFSHC(opaque);

    if (offset + size > UFSICE_MMIO_SIZE) {
        return;
    }

    if (size == 4) {
        *(uint32_t *)(s->ufsice_regs + offset) = (uint32_t)value;
    } else {
        memcpy(s->ufsice_regs + offset, &value, size);
    }
}

static const MemoryRegionOps qcom_ufsice_ops = {
    .read = qcom_ufsice_read,
    .write = qcom_ufsice_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void qcom_ufs_reset(DeviceState *dev)
{
    QcomUfsState *s = QCOM_UFSHC(dev);

    memset(s->ufshc_regs, 0, sizeof(s->ufshc_regs));
    memset(s->ufsphy_regs, 0, sizeof(s->ufsphy_regs));
    memset(s->ufsice_regs, 0, sizeof(s->ufsice_regs));

    /*
     * UFSHCI 2.1 Capabilities:
     *   Bit 24: 64-bit addressing (64AS = 1)
     *   Bit 23: Auto-Hibernate support (AUTOH8 = 1)
     *   Bits [19:16]: Number of Task Management Request Slots (NUTMRS = 8 -> 0x07)
     *   Bits [4:0]: Number of UTP Transfer Request Slots (NUTRS = 32 -> 0x1F)
     */
    *(uint32_t *)(s->ufshc_regs + REG_CAPABILITIES) = 0x0187001FU;

    /*
     * UFSHCI 2.1 Version: 0x00000210
     */
    *(uint32_t *)(s->ufshc_regs + REG_UFS_VERSION) = 0x00000210U;

    /*
     * Host Controller Status:
     *   Bit 0: Device Present (DP = 1)
     *   Bit 1: UTP Transfer Request List Ready (UTRLRDY = 1)
     *   Bit 2: UTP Task Management Request List Ready (UTMRDY = 1)
     *   Bit 3: UIC Command Ready (UCRDY = 1)
     */
    *(uint32_t *)(s->ufshc_regs + REG_HOST_CONTROLLER_STATUS) = 0x0000000FU;

    /* Host Controller Enable starts enabled */
    *(uint32_t *)(s->ufshc_regs + REG_HOST_CONTROLLER_ENABLE) = 0x00000001U;

    /* Initialize default UniPro / M-PHY attributes */
    s->pa_avail_tx_lanes = 1;
    s->pa_avail_rx_lanes = 1;
    s->pa_active_tx_lanes = 1;
    s->pa_active_rx_lanes = 1;
    s->pa_connected_tx_lanes = 1;
    s->pa_connected_rx_lanes = 1;
    s->pa_tx_gear = 1;
    s->pa_rx_gear = 1;
    s->pa_hs_series = 2; /* Series B */
}

static void qcom_ufs_realize(DeviceState *dev, Error **errp)
{
    QcomUfsState *s = QCOM_UFSHC(dev);

    /* MMIO Region 0: UFSHCI Host Controller (0x1d84000) */
    memory_region_init_io(&s->ufshc_mmio, OBJECT(s), &qcom_ufshc_ops, s,
                          "qcom-ufshc", UFSHC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->ufshc_mmio);

    /* MMIO Region 1: QMP UFS PHY (0x1d87000) */
    memory_region_init_io(&s->ufsphy_mmio, OBJECT(s), &qcom_ufsphy_ops, s,
                          "qcom-ufsphy", UFSPHY_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->ufsphy_mmio);

    /* MMIO Region 2: UFS ICE Crypto (0x1d90000) */
    memory_region_init_io(&s->ufsice_mmio, OBJECT(s), &qcom_ufsice_ops, s,
                          "qcom-ufsice", UFSICE_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->ufsice_mmio);

    /* Interrupt output */
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void qcom_ufs_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = qcom_ufs_realize;
    dc->reset = qcom_ufs_reset;
    dc->desc = "Qualcomm SM6150 UFS Host Controller and QMP PHY";
}

static const TypeInfo qcom_ufs_info = {
    .name = TYPE_QCOM_UFSHC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(QcomUfsState),
    .class_init = qcom_ufs_class_init,
};

static void qcom_ufs_register_types(void)
{
    type_register_static(&qcom_ufs_info);
}

type_init(qcom_ufs_register_types)
