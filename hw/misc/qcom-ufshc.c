/*
 * Qualcomm SM6150 UFS Host Controller (UFSHCI 2.1) & QMP PHY emulation for
 * QEMU.
 *
 * Emulates the UFSHCI 2.1 host controller at 0x1d84000, the QMP UFS PHY at
 * 0x1d87000, and the UFS ICE crypto block at 0x1d90000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/osdep.h"
#include "sysemu/dma.h"


static FILE *ufs_log_file = NULL;
static void ufs_log(const char *fmt, ...) {
  if (!ufs_log_file) {
    ufs_log_file = fopen("C:\\qemu_work\\ufs_debug.log", "w");
  }
  if (ufs_log_file) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(ufs_log_file, fmt, ap);
    va_end(ap);
    fflush(ufs_log_file);
  }
}

#define TYPE_QCOM_UFSHC "qcom-ufshc"
OBJECT_DECLARE_SIMPLE_TYPE(QcomUfsState, QCOM_UFSHC)

#define UFSHC_MMIO_SIZE 0x3000
#define UFSPHY_MMIO_SIZE 0x1000
#define UFSICE_MMIO_SIZE 0x8000

/* UFSHCI standard register offsets */
#define REG_CAPABILITIES 0x00
#define REG_UFS_VERSION 0x08
#define REG_CONTROLLER_PID 0x0C
#define REG_CONTROLLER_MID 0x10
#define REG_AHIT 0x14
#define REG_INTERRUPT_STATUS 0x20
#define REG_INTERRUPT_ENABLE 0x24
#define REG_HOST_CONTROLLER_STATUS 0x30
#define REG_HOST_CONTROLLER_ENABLE 0x34
#define REG_UTRIACR 0x38
#define REG_UTRLBA 0x50
#define REG_UTRLBAU 0x54
#define REG_UTRLDBR 0x58
#define REG_UTRLCLR 0x5C
#define REG_UTRLRSR 0x60
#define REG_UTMRLBA 0x70
#define REG_UTMRLBAU 0x74
#define REG_UTMRLDBR 0x78
#define REG_UTMRLCLR 0x7C
#define REG_UTMRLRSR 0x80
#define REG_UICCMD 0x90
#define REG_UICCMDARG1 0x94
#define REG_UICCMDARG2 0x98
#define REG_UICCMDARG3 0x9C

/* Interrupt Status Bits */
#define INT_UTRCS (1U << 0)  /* UTP Transfer Request Completion */
#define INT_UDEPRI (1U << 1) /* UTP Error */
#define INT_UE (1U << 2)     /* Host Controller Fatal Error */
#define INT_UTMRCS (1U << 3) /* Task Management Completion */
#define INT_UPMS (1U << 4)   /* Power Mode Status */
#define INT_UHXS (1U << 5)   /* UIC Hibernate Exit Status */
#define INT_UHES (1U << 6)   /* UIC Hibernate Enter Status */
#define INT_ULLS (1U << 7)   /* UIC Link Lost Status */
#define INT_ULSS (1U << 8)   /* UIC Link Startup Status */
#define INT_UTMRIS (1U << 9) /* Task Management Request Interrupted */
#define INT_UCCS (1U << 10)  /* UIC Command Completion Status */
#define INT_DFES (1U << 11)  /* Device Fatal Error Status */
#define INT_UTPES (1U << 12) /* UTP Error Status */

/* UIC Command codes */
#define UIC_CMD_DME_GET 0x01
#define UIC_CMD_DME_SET 0x02
#define UIC_CMD_DME_PEER_GET 0x03
#define UIC_CMD_DME_PEER_SET 0x04
#define UIC_CMD_DME_POWERON 0x10
#define UIC_CMD_DME_POWEROFF 0x11
#define UIC_CMD_DME_ENABLE 0x12
#define UIC_CMD_DME_RESET 0x14
#define UIC_CMD_DME_ENDPOINT_RESET 0x15
#define UIC_CMD_DME_LINK_STARTUP 0x16
#define UIC_CMD_DME_HIBERN8_ENTER 0x17
#define UIC_CMD_DME_HIBERN8_EXIT 0x18

/* UPIU Transaction Codes */
#define UPIU_TRANSACTION_NOP_OUT 0x00
#define UPIU_TRANSACTION_COMMAND 0x01
#define UPIU_TRANSACTION_QUERY_REQ 0x16
#define UPIU_TRANSACTION_NOP_IN 0x20
#define UPIU_TRANSACTION_RESPONSE 0x21
#define UPIU_TRANSACTION_QUERY_RSP 0x36

/* Query Opcodes */
#define UPIU_QUERY_OPCODE_READ_DESC 0x01
#define UPIU_QUERY_OPCODE_WRITE_DESC 0x02
#define UPIU_QUERY_OPCODE_READ_ATTR 0x03
#define UPIU_QUERY_OPCODE_WRITE_ATTR 0x04
#define UPIU_QUERY_OPCODE_READ_FLAG 0x05
#define UPIU_QUERY_OPCODE_SET_FLAG 0x06
#define UPIU_QUERY_OPCODE_CLEAR_FLAG 0x07
#define UPIU_QUERY_OPCODE_TOGGLE_FLAG 0x08

/* Query Descriptor IDN */
#define QUERY_DESC_IDN_DEVICE 0x00
#define QUERY_DESC_IDN_CONFIG 0x01
#define QUERY_DESC_IDN_UNIT 0x02
#define QUERY_DESC_IDN_INTERCONNECT 0x04
#define QUERY_DESC_IDN_STRING 0x05
#define QUERY_DESC_IDN_GEOMETRY 0x07
#define QUERY_DESC_IDN_POWER 0x08

/* SCSI Commands */
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_INQUIRY 0x12
#define SCSI_MODE_SENSE_6 0x1A
#define SCSI_START_STOP_UNIT 0x1B
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2A
#define SCSI_SYNCHRONIZE_CACHE 0x35
#define SCSI_MODE_SENSE_10 0x5A
#define SCSI_READ_16 0x88
#define SCSI_WRITE_16 0x8A
#define SCSI_READ_CAPACITY_16 0x9E
#define SCSI_REPORT_LUNS 0xA0

struct QcomUfsState {
  SysBusDevice parent_obj;

  MemoryRegion ufshc_mmio;
  MemoryRegion ufsphy_mmio;
  MemoryRegion ufsice_mmio;
  qemu_irq irq;

  QEMUBH *transfer_bh;
  QEMUBH *tm_bh;
  uint32_t pending_utrl_doorbell;
  uint32_t pending_utmrl_doorbell;

  uint8_t ufshc_regs[UFSHC_MMIO_SIZE];
  uint8_t ufsphy_regs[UFSPHY_MMIO_SIZE];
  uint8_t ufsice_regs[UFSICE_MMIO_SIZE];

  /* UniPro / M-PHY Link Attributes */
  uint32_t pa_avail_tx_lanes;
  uint32_t pa_avail_rx_lanes;
  uint32_t pa_active_tx_lanes;
  uint32_t pa_active_rx_lanes;
  uint32_t pa_connected_tx_lanes;
  uint32_t pa_connected_rx_lanes;
  uint32_t pa_tx_gear;
  uint32_t pa_rx_gear;
  uint32_t pa_hs_series;

  /* Device Flags & Attributes */
  uint8_t flag_fDeviceInit;
  uint8_t attr_bBootLunEn;
  uint8_t attr_bCurrentPowerMode;
};

/* Update IRQ status according to (INTERRUPT_STATUS & INTERRUPT_ENABLE) */
static void qcom_ufs_update_irq(QcomUfsState *s) {
  uint32_t is = *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS);
  uint32_t ie = *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_ENABLE);

  bool level = (is & ie) != 0;
  qemu_set_irq(s->irq, level ? 1 : 0);
}

/* UniPro DME Attributes */
static uint32_t get_uic_attr(QcomUfsState *s, uint32_t attr_sel) {
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

static void set_uic_attr(QcomUfsState *s, uint32_t attr_sel, uint32_t val) {
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

static void handle_uic_command(QcomUfsState *s, uint32_t cmd) {
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

/* Encode UTF-16LE string for UFS string descriptor */
static int encode_string_desc(const char *str, uint8_t *buf, int max_len) {
  int len = strlen(str);
  int desc_len = 2 + len * 2;
  if (desc_len > max_len)
    desc_len = max_len;

  buf[0] = (uint8_t)desc_len;
  buf[1] = 0x05; /* STRING DESC */

  for (int i = 0; i < len && (2 + i * 2 + 1) < desc_len; i++) {
    buf[2 + i * 2] = (uint8_t)str[i];
    buf[2 + i * 2 + 1] = 0x00;
  }
  return desc_len;
}

/* Handle Query Request UPIU */
static void handle_query_request(QcomUfsState *s, uint8_t *req, uint8_t *rsp,
                                 uint64_t data_dma_addr, uint32_t data_len) {
  uint8_t opcode = req[16];
  uint8_t idn = req[17];
  uint8_t index = req[18];
  uint8_t selector = req[19];

  (void)selector;

  memcpy(rsp, req, 32);
  rsp[0] = UPIU_TRANSACTION_QUERY_RSP; /* 0x36 */
  rsp[6] = 0x00;                       /* SUCCESS */
  rsp[7] = 0x00;

  switch (opcode) {
  case UPIU_QUERY_OPCODE_READ_DESC: {
    uint8_t desc_buf[256];
    memset(desc_buf, 0, sizeof(desc_buf));
    int desc_len = 0;

    if (idn == QUERY_DESC_IDN_DEVICE) {
      /* Device Descriptor (64 bytes) */
      desc_len = 64;
      desc_buf[0] = 64;    /* bLength */
      desc_buf[1] = 0x00;  /* bDescriptorType = DEVICE */
      desc_buf[2] = 0x01;  /* bDeviceSubClass = UFS */
      desc_buf[3] = 0x08;  /* bNumberLU = 8 Logical Units */
      desc_buf[4] = 0x01;  /* bBootEnable = 1 */
      desc_buf[5] = 0x01;  /* bDescrAccessEn = 1 */
      desc_buf[6] = 0x01;  /* bInitPowerMode = 1 */
      desc_buf[7] = 0x00;  /* bHighPriorityLUN */
      desc_buf[8] = 0x00;  /* bSecureRemovalType */
      desc_buf[9] = 0x00;  /* bSecurityLU */
      desc_buf[10] = 0x00; /* bInitActiveICCLevel */
      desc_buf[11] = 0x02; /* wSpecVersion MSB (0x0210) */
      desc_buf[12] = 0x10; /* wSpecVersion LSB */
      desc_buf[13] = 0x20; /* wManufacturerDate MSB */
      desc_buf[14] = 0x24; /* wManufacturerDate LSB */
      desc_buf[15] = 0x01; /* iManufacturerName (String 1) */
      desc_buf[16] = 0x02; /* iProductName (String 2) */
      desc_buf[17] = 0x03; /* iSerialNumber (String 3) */
      desc_buf[18] = 0x04; /* iOemID (String 4) */
      desc_buf[19] = 0x01; /* wManufacturerID MSB (0x01CE) */
      desc_buf[20] = 0xCE; /* wManufacturerID LSB */
      desc_buf[21] = 0x16; /* bUD0BaseOffset */
      desc_buf[22] = 0x1A; /* bUDConfigPLength */
      desc_buf[23] = 0x02; /* bDeviceRTTCap */
      desc_buf[24] = 0x00; /* wPeriodicRTCUpdate */
      desc_buf[25] = 0x00;
    } else if (idn == QUERY_DESC_IDN_GEOMETRY) {
      /* Geometry Descriptor (84 bytes) */
      desc_len = 84;
      desc_buf[0] = 84;   /* bLength */
      desc_buf[1] = 0x07; /* bDescriptorType = GEOMETRY */
      desc_buf[2] = 0x00; /* bMediaTechnology = Normal */
      /* qTotalRawDeviceCapacity = 32 GB in 512B sectors (0x04000000) */
      desc_buf[4] = 0x00;
      desc_buf[5] = 0x00;
      desc_buf[6] = 0x00;
      desc_buf[7] = 0x00;
      desc_buf[8] = 0x04;
      desc_buf[9] = 0x00;
      desc_buf[10] = 0x00;
      desc_buf[11] = 0x00;
      desc_buf[12] = 0x08; /* bMaxNumberLU = 8 */
      desc_buf[13] = 0x00; /* dSegmentSize = 128KB */
      desc_buf[14] = 0x02;
      desc_buf[15] = 0x00;
      desc_buf[16] = 0x00;
      desc_buf[17] = 0x08; /* bAllocationUnitSize = 4MB */
      desc_buf[18] = 0x08; /* bMinAddrBlockSize = 4KB */
      desc_buf[19] = 0x08; /* bOptimalReadBlockSize = 4KB */
      desc_buf[20] = 0x08; /* bOptimalWriteBlockSize = 4KB */
      desc_buf[21] = 0x08; /* bMaxInBufferSize = 4KB */
      desc_buf[22] = 0x08; /* bMaxOutBufferSize = 4KB */
      desc_buf[23] = 0x20; /* bRPMB_ReadWriteSize */
      desc_buf[24] = 0x00; /* bDynamicCapacityResourcePolicy */
      desc_buf[25] = 0x00; /* bDataOrdering */
      desc_buf[26] = 0x08; /* bMaxConLogicalUnitNum = 8 */
      desc_buf[27] = 0x00; /* bSupportedMemoryTypes */
    } else if (idn == QUERY_DESC_IDN_UNIT) {
      /* Unit Descriptor (45 bytes) */
      desc_len = 45;
      desc_buf[0] = 45;    /* bLength */
      desc_buf[1] = 0x02;  /* bDescriptorType = UNIT */
      desc_buf[2] = index; /* bUnitIndex */
      desc_buf[3] =
          (index == 0 ? 0x01
                      : 0x00); /* bLUEnable = 1 for LUN 0, 0 for LUN > 0 */
      desc_buf[4] = (index == 0 ? 0x01 : 0x00); /* bBootLunID */
      desc_buf[5] = 0x00;                       /* bLUWriteProtect */
      desc_buf[6] = 0x00;                       /* bMemoryType */
      desc_buf[7] = 0x00; /* dNumAllocUnits (4096 * 4MB = 16 GB for LUN 0) */
      desc_buf[8] = 0x00;
      desc_buf[9] = 0x10;
      desc_buf[10] = 0x00;
      desc_buf[11] = 0x00; /* bDataReliability */
      desc_buf[12] = 0x09; /* bLogicalBlockSize = 9 (512 bytes: 2^9 = 512) */
      /* qLogicalBlockCount = 33,554,432 blocks (16 GB) = 0x0000000002000000 */
      desc_buf[13] = 0x00;
      desc_buf[14] = 0x00;
      desc_buf[15] = 0x00;
      desc_buf[16] = 0x00;
      desc_buf[17] = 0x02;
      desc_buf[18] = 0x00;
      desc_buf[19] = 0x00;
      desc_buf[20] = 0x00;
    } else if (idn == QUERY_DESC_IDN_STRING) {
      if (index == 0) {
        desc_len = 4;
        desc_buf[0] = 4;
        desc_buf[1] = 0x05;
        desc_buf[2] = 0x09; /* 0x0409 English */
        desc_buf[3] = 0x04;
      } else if (index == 1) {
        desc_len =
            encode_string_desc("Qualcomm Inc.", desc_buf, sizeof(desc_buf));
      } else if (index == 2) {
        desc_len = encode_string_desc("UFS 2.1 Android Storage", desc_buf,
                                      sizeof(desc_buf));
      } else if (index == 3) {
        desc_len =
            encode_string_desc("QC-UFS-00000001", desc_buf, sizeof(desc_buf));
      } else {
        desc_len = encode_string_desc("QCOM-EMU", desc_buf, sizeof(desc_buf));
      }
    }

    if (desc_len > 0 && data_dma_addr != 0) {
      uint32_t write_sz =
          (data_len < (uint32_t)desc_len) ? data_len : (uint32_t)desc_len;
      dma_memory_write(&address_space_memory, data_dma_addr, desc_buf, write_sz,
                       MEMTXATTRS_UNSPECIFIED);
      rsp[10] = (write_sz >> 8) & 0xFF;
      rsp[11] = write_sz & 0xFF;
      rsp[14] = (write_sz >> 8) & 0xFF;
      rsp[15] = write_sz & 0xFF;
      ufs_log("[UFS_QUERY] wrote desc idn=0x%02x, index=%u, addr=0x%" PRIx64
              ", sz=%u\n",
              idn, index, data_dma_addr, write_sz);
    }
    break;
  }
  case UPIU_QUERY_OPCODE_READ_FLAG: {
    uint8_t flag_val = 0;
    if (idn == 0x01) {
      flag_val = 0; /* fDeviceInit is always ready (0) */
    }
    rsp[23] = flag_val;
    break;
  }
  case UPIU_QUERY_OPCODE_SET_FLAG: {
    rsp[23] = 0; /* fDeviceInit transitions to 0 */
    break;
  }
  case UPIU_QUERY_OPCODE_CLEAR_FLAG: {
    rsp[23] = 0;
    break;
  }
  case UPIU_QUERY_OPCODE_READ_ATTR: {
    uint32_t val = 0;
    if (idn == 0x00) {
      val = s->attr_bBootLunEn;
    } else if (idn == 0x01) {
      val = s->attr_bCurrentPowerMode;
    }
    rsp[20] = (val >> 24) & 0xFF;
    rsp[21] = (val >> 16) & 0xFF;
    rsp[22] = (val >> 8) & 0xFF;
    rsp[23] = val & 0xFF;
    break;
  }
  case UPIU_QUERY_OPCODE_WRITE_ATTR: {
    uint32_t val = ((uint32_t)req[20] << 24) | ((uint32_t)req[21] << 16) |
                   ((uint32_t)req[22] << 8) | req[23];
    if (idn == 0x00) {
      s->attr_bBootLunEn = val & 0xFF;
    } else if (idn == 0x01) {
      s->attr_bCurrentPowerMode = val & 0xFF;
    }
    break;
  }
  default:
    break;
  }
}

/* Helper to write data across PRDT entries */
static void dma_write_prdt(uint64_t prdt_addr, uint16_t prdt_len,
                           const void *src, size_t total_sz) {
  const uint8_t *p = (const uint8_t *)src;
  size_t rem = total_sz;

  ufs_log("[UFS_DMA] write_prdt: addr=0x%" PRIx64 ", len=%u, sz=%zu\n",
          prdt_addr, prdt_len, total_sz);

  for (uint16_t i = 0; i < prdt_len && rem > 0; i++) {
    uint32_t prd[4];
    if (dma_memory_read(&address_space_memory, prdt_addr + i * 16, prd, 16,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
      ufs_log("[UFS_DMA] prd read fail at 0x%" PRIx64 "\n", prdt_addr + i * 16);
      break;
    }

    uint64_t entry_addr = (uint64_t)prd[0] | ((uint64_t)prd[1] << 32);
    uint32_t entry_len = (prd[3] & 0x3FFFF) + 1;
    ufs_log("[UFS_DMA] prd[%u]: entry_addr=0x%" PRIx64
            ", entry_len=%u (prd3=0x%x)\n",
            i, entry_addr, entry_len, prd[3]);
    if (entry_addr == 0 || entry_len == 0) {
      break;
    }
    uint32_t chunk = (rem < entry_len) ? (uint32_t)rem : entry_len;

    dma_memory_write(&address_space_memory, entry_addr, p, chunk,
                     MEMTXATTRS_UNSPECIFIED);

    p += chunk;
    rem -= chunk;
  }
}

/* Multi-sector GPT disk reader across PRDT scatter-gather descriptors */
static void dma_read_disk(uint64_t start_lba, uint32_t num_blocks,
                          uint64_t prdt_addr, uint16_t prdt_len) {
  uint8_t sec[512];
  uint32_t blk_idx = 0;
  uint32_t rem_blocks = num_blocks;

  ufs_log("[UFS_DMA] read_disk: start_lba=%" PRIu64
          ", num_blocks=%u, prdt_addr=0x%" PRIx64 ", prdt_len=%u\n",
          start_lba, num_blocks, prdt_addr, prdt_len);

  for (uint16_t p = 0; p < prdt_len && rem_blocks > 0; p++) {
    uint32_t prd[4];
    if (dma_memory_read(&address_space_memory, prdt_addr + p * 16, prd, 16,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
      break;
    }

    uint64_t entry_addr = (uint64_t)prd[0] | ((uint64_t)prd[1] << 32);
    uint32_t entry_len = (prd[3] & 0x3FFFF) + 1;
    if (entry_addr == 0 || entry_len == 0) {
      break;
    }
    uint32_t entry_written = 0;

    while (entry_written < entry_len && rem_blocks > 0) {
      uint64_t lba = start_lba + blk_idx;
      memset(sec, 0, sizeof(sec));

      if (lba == 0) {
        /* Protective MBR */
        sec[446 + 4] = 0xEE; /* GPT Protective */
        sec[446 + 8] = 0x01; /* Starting LBA 1 */
        sec[446 + 12] = 0xFF;
        sec[446 + 13] = 0xFF;
        sec[446 + 14] = 0xFF;
        sec[446 + 15] = 0x01;
        sec[510] = 0x55;
        sec[511] = 0xAA;
      } else if (lba == 1) {
        /* GPT Header */
        static const uint8_t gpt_hdr[92] = {
            0x45, 0x46, 0x49, 0x20, 0x50, 0x41, 0x52, 0x54, 0x00, 0x00, 0x01,
            0x00, 0x5c, 0x00, 0x00, 0x00, 0x99, 0xa1, 0xfa, 0xf9, 0x00, 0x00,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
            0xff, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0xde, 0xff, 0xff, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x01, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
            0x2d, 0xd6, 0x4c, 0xd0};
        memcpy(sec, gpt_hdr, sizeof(gpt_hdr));
      } else if (lba == 2) {
        /* Partition Table Entry 0 ("system") and Entry 1 ("userdata") */
        sec[0] = 0x0f;
        sec[1] = 0xc6;
        sec[2] = 0x3d;
        sec[3] = 0xaf;
        sec[4] = 0xb7;
        sec[5] = 0x83;
        sec[6] = 0x4c;
        sec[7] = 0x4d;
        sec[8] = 0x8e;
        sec[9] = 0x99;
        sec[10] = 0x44;
        sec[11] = 0xa0;
        sec[12] = 0x54;
        sec[13] = 0x60;
        sec[14] = 0x97;
        sec[15] = 0xc7;
        sec[16] = 0x01;
        sec[17] = 0x00;
        sec[18] = 0x01;
        sec[19] = 0x7c;
        /* Starting LBA: 2048 (0x00000800) */
        sec[32] = 0x00;
        sec[33] = 0x08;
        /* Ending LBA: 4194303 (0x003FFFFF) */
        sec[40] = 0xff;
        sec[41] = 0xff;
        sec[42] = 0x3f;
        /* Name: "system" UTF-16LE */
        sec[56] = 's';
        sec[58] = 'y';
        sec[60] = 's';
        sec[62] = 't';
        sec[64] = 'e';
        sec[66] = 'm';

        /* Entry 1: "userdata" */
        sec[128 + 0] = 0x0f;
        sec[128 + 1] = 0xc6;
        sec[128 + 2] = 0x3d;
        sec[128 + 3] = 0xaf;
        sec[128 + 4] = 0xb7;
        sec[128 + 5] = 0x83;
        sec[128 + 6] = 0x4c;
        sec[128 + 7] = 0x4d;
        sec[128 + 8] = 0x8e;
        sec[128 + 9] = 0x99;
        sec[128 + 10] = 0x44;
        sec[128 + 11] = 0xa0;
        sec[128 + 12] = 0x54;
        sec[128 + 13] = 0x60;
        sec[128 + 14] = 0x97;
        sec[128 + 15] = 0xc7;
        sec[128 + 16] = 0x02;
        sec[128 + 17] = 0x00;
        sec[128 + 18] = 0x01;
        sec[128 + 19] = 0x7c;
        /* Starting LBA: 4194304 (0x00400000) */
        sec[128 + 34] = 0x40;
        /* Ending LBA: 33554398 (0x01FFFFDE) */
        sec[128 + 40] = 0xde;
        sec[128 + 41] = 0xff;
        sec[128 + 42] = 0xff;
        sec[128 + 43] = 0x01;
        /* Name: "userdata" UTF-16LE */
        sec[128 + 56] = 'u';
        sec[128 + 58] = 's';
        sec[128 + 60] = 'e';
        sec[128 + 62] = 'r';
        sec[128 + 64] = 'd';
        sec[128 + 66] = 'a';
        sec[128 + 68] = 't';
        sec[128 + 70] = 'a';
      }

      uint32_t chunk = entry_len - entry_written;
      if (chunk > sizeof(sec)) {
        chunk = sizeof(sec);
      }

      dma_memory_write(&address_space_memory, entry_addr + entry_written, sec,
                       chunk, MEMTXATTRS_UNSPECIFIED);
      entry_written += chunk;
      blk_idx++;
      rem_blocks--;
    }
  }
}

/* Handle SCSI Command UPIU */
static void handle_scsi_command(QcomUfsState *s, uint8_t *req, uint8_t *rsp,
                                uint64_t prdt_addr, uint16_t prdt_len) {
  (void)s;
  uint8_t cdb_op = req[16];
  uint8_t lun = req[2];

  memset(rsp, 0, 32);
  rsp[0] = UPIU_TRANSACTION_RESPONSE; /* 0x21 */
  rsp[1] = 0x00;                      /* Flags */
  rsp[2] = lun;                       /* LUN */
  rsp[3] = req[3];                    /* Task Tag */
  rsp[6] = 0x00;                      /* Target Status: SUCCESS (0x00) */
  rsp[7] = 0x00;                      /* SAM Status: GOOD (0x00) */

  ufs_log("[UFS_SCSI] op=0x%02x, lun=%u, tag=%u, prdt_addr=0x%" PRIx64
          ", prdt_len=%u\n",
          cdb_op, lun, req[3], prdt_addr, prdt_len);

  switch (cdb_op) {
  case SCSI_INQUIRY: {
    uint8_t evpd = req[17] & 0x01;
    uint8_t page_code = req[18];

    if (evpd) {
      if (page_code == 0x00) {
        /* Supported VPD Pages */
        uint8_t vpd0[7] = {0x00, 0x00, 0x00, 0x03, 0x00, 0x80, 0x83};
        dma_write_prdt(prdt_addr, prdt_len, vpd0, sizeof(vpd0));
      } else if (page_code == 0x80) {
        /* Unit Serial Number */
        uint8_t vpd80[12] = {0x00, 0x80, 0x00, 0x08, '1', '2',
                             '3',  '4',  '5',  '6',  '7', '8'};
        dma_write_prdt(prdt_addr, prdt_len, vpd80, sizeof(vpd80));
      } else if (page_code == 0x83) {
        /* Device Identification (NAA format) */
        uint8_t vpd83[16];
        memset(vpd83, 0, sizeof(vpd83));
        vpd83[1] = 0x83;
        vpd83[3] = 12;   /* Length of designators */
        vpd83[4] = 0x01; /* Code set: Binary */
        vpd83[5] = 0x03; /* Designator type: NAA */
        vpd83[7] = 8;    /* Designator length */
        vpd83[8] = 0x51;
        vpd83[9] = 0x23;
        vpd83[10] = 0x45;
        vpd83[11] = 0x67;
        vpd83[12] = 0x89;
        vpd83[13] = 0xAB;
        vpd83[14] = 0xCD;
        vpd83[15] = lun;
        dma_write_prdt(prdt_addr, prdt_len, vpd83, sizeof(vpd83));
      } else {
        uint8_t vpd_empty[4] = {0x00, page_code, 0x00, 0x00};
        dma_write_prdt(prdt_addr, prdt_len, vpd_empty, sizeof(vpd_empty));
      }
    } else {
      /* Standard INQUIRY */
      uint8_t inq[36];
      memset(inq, 0, sizeof(inq));
      if (lun == 0) {
        inq[0] = 0x00; /* Direct Access Block Device */
        inq[1] = 0x00; /* RMB = 0 */
        inq[2] = 0x06; /* SPC-4 */
        inq[3] = 0x02; /* Response format */
        inq[4] = 31;   /* Additional length */
        inq[7] = 0x02; /* CmdQue = 1 */
        memcpy(&inq[8], "QCOM    ", 8);
        memcpy(&inq[16], "UFS 2.1 DISK    ", 16);
        memcpy(&inq[32], "0001", 4);
      } else {
        inq[0] = 0x7F; /* No physical device attached on LUN > 0 */
      }

      dma_write_prdt(prdt_addr, prdt_len, inq, sizeof(inq));
    }
    break;
  }
  case SCSI_REQUEST_SENSE: {
    uint8_t sense[18];
    memset(sense, 0, sizeof(sense));
    sense[0] = 0x70; /* Response Code: Current Fixed Format */
    sense[2] = 0x00; /* Sense Key: NO SENSE */
    sense[7] = 10;   /* Additional Sense Length */
    dma_write_prdt(prdt_addr, prdt_len, sense, sizeof(sense));
    break;
  }
  case SCSI_TEST_UNIT_READY:
  case SCSI_START_STOP_UNIT:
  case SCSI_SYNCHRONIZE_CACHE:
    /* Already SUCCESS / GOOD */
    break;
  case SCSI_MODE_SENSE_6: {
    uint8_t mode6[4] = {3, 0, 0, 0};
    dma_write_prdt(prdt_addr, prdt_len, mode6, sizeof(mode6));
    break;
  }
  case SCSI_MODE_SENSE_10: {
    uint8_t mode10[8] = {0, 6, 0, 0, 0, 0, 0, 0};
    dma_write_prdt(prdt_addr, prdt_len, mode10, sizeof(mode10));
    break;
  }
  case SCSI_READ_CAPACITY_10: {
    uint8_t cap[8];
    memset(cap, 0, sizeof(cap));
    if (lun == 0) {
      /* 33,554,431 blocks (16GB) = 0x01FFFFFF */
      cap[0] = 0x01;
      cap[1] = 0xFF;
      cap[2] = 0xFF;
      cap[3] = 0xFF;
      /* Block size = 512 bytes = 0x00000200 */
      cap[4] = 0x00;
      cap[5] = 0x00;
      cap[6] = 0x02;
      cap[7] = 0x00;
    }

    dma_write_prdt(prdt_addr, prdt_len, cap, sizeof(cap));
    break;
  }
  case SCSI_READ_CAPACITY_16: {
    uint8_t cap16[32];
    memset(cap16, 0, sizeof(cap16));
    if (lun == 0) {
      /* Returned Logical Block Address: 0x0000000001FFFFFF */
      cap16[4] = 0x01;
      cap16[5] = 0xFF;
      cap16[6] = 0xFF;
      cap16[7] = 0xFF;
      /* Block Length: 512 = 0x00000200 */
      cap16[8] = 0x00;
      cap16[9] = 0x00;
      cap16[10] = 0x02;
      cap16[11] = 0x00;
    }

    dma_write_prdt(prdt_addr, prdt_len, cap16, sizeof(cap16));
    break;
  }
  case SCSI_REPORT_LUNS: {
    uint8_t luns[16];
    memset(luns, 0, sizeof(luns));
    luns[3] = 8; /* LUN List Length = 8 bytes (1 LUN) */
    /* LUN 0 = 0x0000000000000000 */
    dma_write_prdt(prdt_addr, prdt_len, luns, sizeof(luns));
    break;
  }
  case SCSI_READ_10:
  case SCSI_READ_16: {
    uint64_t lba = 0;
    uint32_t num_blocks = 1;
    if (cdb_op == SCSI_READ_10) {
      lba = ((uint64_t)req[18] << 24) | ((uint64_t)req[19] << 16) |
            ((uint64_t)req[20] << 8) | req[21];
      num_blocks = ((uint32_t)req[23] << 8) | req[24];
    } else {
      lba = ((uint64_t)req[18] << 56) | ((uint64_t)req[19] << 48) |
            ((uint64_t)req[20] << 40) | ((uint64_t)req[21] << 32) |
            ((uint64_t)req[22] << 24) | ((uint64_t)req[23] << 16) |
            ((uint64_t)req[24] << 8) | req[25];
      num_blocks = ((uint32_t)req[26] << 24) | ((uint32_t)req[27] << 16) |
                   ((uint32_t)req[28] << 8) | req[29];
    }
    if (num_blocks == 0) {
      num_blocks = 1;
    }

    dma_read_disk(lba, num_blocks, prdt_addr, prdt_len);
    break;
  }
  case SCSI_WRITE_10:
  case SCSI_WRITE_16:
    /* Successfully absorbed writes */
    break;
  default:
    break;
  }
}

/* Process UTP Transfer Requests on REG_UTRLDBR write */
static void process_utp_transfers(QcomUfsState *s, uint32_t doorbell) {
  uint32_t utrdl_ba_l = *(uint32_t *)(s->ufshc_regs + REG_UTRLBA);
  uint32_t utrdl_ba_u = *(uint32_t *)(s->ufshc_regs + REG_UTRLBAU);
  uint64_t utrdl_base = (uint64_t)utrdl_ba_l | ((uint64_t)utrdl_ba_u << 32);

  for (int slot = 0; slot < 32; slot++) {
    if (!(doorbell & (1U << slot))) {
      continue;
    }

    uint64_t utrd_addr = utrdl_base + slot * 32;
    uint32_t utrd[8];
    if (dma_memory_read(&address_space_memory, utrd_addr, utrd, 32,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
      continue;
    }

    uint32_t ucd_ba_l = utrd[4];
    uint32_t ucd_ba_u = utrd[5];
    uint64_t ucd_base = (uint64_t)ucd_ba_l | ((uint64_t)ucd_ba_u << 32);

    uint32_t rupiuo_raw = (utrd[6] >> 16) & 0xFFFF;
    uint32_t prdto_raw = (utrd[7] >> 16) & 0xFFFF;
    uint16_t prdt_len_raw = utrd[7] & 0xFFFF;

    /* Qualcomm sets UFSHCD_QUIRK_PRDT_BYTE_GRAN where offsets and lengths are
     * in bytes */
    uint64_t resp_offset =
        (rupiuo_raw >= 512) ? rupiuo_raw : ((uint64_t)rupiuo_raw << 2);
    uint64_t prdt_offset =
        (prdto_raw >= 1024) ? prdto_raw : ((uint64_t)prdto_raw << 2);
    uint16_t prdt_len = (prdt_len_raw >= 16 && (prdt_len_raw % 16 == 0))
                            ? (prdt_len_raw / 16)
                            : prdt_len_raw;

    if (resp_offset == 0) {
      resp_offset = 512;
    }
    if (prdt_offset == 0) {
      prdt_offset = 1024;
    }

    uint64_t prdt_addr = ucd_base + prdt_offset;
    uint64_t resp_addr = ucd_base + resp_offset;

    /* Read Command UPIU (32 bytes) */
    uint8_t cmd_upiu[32];
    uint8_t resp_upiu[32];
    memset(resp_upiu, 0, sizeof(resp_upiu));

    if (dma_memory_read(&address_space_memory, ucd_base, cmd_upiu, 32,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
      continue;
    }

    uint8_t trans_type = cmd_upiu[0] & 0x3F;

    ufs_log("[UFS_TR] slot=%d, ucd_base=0x%" PRIx64
            ", trans_type=0x%02x, op=0x%02x, lun=%u, prdt_addr=0x%" PRIx64
            ", prdt_len=%u\n",
            slot, ucd_base, trans_type, cmd_upiu[16], cmd_upiu[2], prdt_addr,
            prdt_len);

    if (trans_type == UPIU_TRANSACTION_NOP_OUT) {
      resp_upiu[0] = UPIU_TRANSACTION_NOP_IN; /* 0x20 */
      resp_upiu[1] = 0x00;
      resp_upiu[2] = cmd_upiu[2]; /* LUN */
      resp_upiu[3] = cmd_upiu[3]; /* Task Tag */
      resp_upiu[6] = 0x00;        /* Target SUCCESS */
      resp_upiu[7] = 0x00;        /* Device SUCCESS */
    } else if (trans_type == UPIU_TRANSACTION_QUERY_REQ) {
      uint64_t desc_data_addr = ucd_base + resp_offset + 32;
      handle_query_request(s, cmd_upiu, resp_upiu, desc_data_addr, 256);
    } else if (trans_type == UPIU_TRANSACTION_COMMAND) {
      handle_scsi_command(s, cmd_upiu, resp_upiu, prdt_addr, prdt_len);
    }

    /* Write Response UPIU to guest memory */
    dma_memory_write(&address_space_memory, resp_addr, resp_upiu, 32,
                     MEMTXATTRS_UNSPECIFIED);

    /* Set OCS = 0 (OCS_SUCCESS) in UTRD */
    utrd[2] = 0x00000000U;
    dma_memory_write(&address_space_memory, utrd_addr + 8, &utrd[2], 4,
                     MEMTXATTRS_UNSPECIFIED);

    /* Clear this slot's doorbell bit in REG_UTRLDBR */
    *(uint32_t *)(s->ufshc_regs + REG_UTRLDBR) &= ~(1U << slot);
  }

  /* Set Transfer Request Completion Status bit */
  *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS) |= INT_UTRCS;
  qemu_set_irq(s->irq, 0);
  qcom_ufs_update_irq(s);
}

/* Process Task Management Requests */
static void process_task_mgmt_transfers(QcomUfsState *s, uint32_t doorbell) {
  uint32_t utmrl_ba_l = *(uint32_t *)(s->ufshc_regs + REG_UTMRLBA);
  uint32_t utmrl_ba_u = *(uint32_t *)(s->ufshc_regs + REG_UTMRLBAU);
  uint64_t utmrl_base = (uint64_t)utmrl_ba_l | ((uint64_t)utmrl_ba_u << 32);

  for (int slot = 0; slot < 8; slot++) {
    if (!(doorbell & (1U << slot))) {
      continue;
    }

    uint64_t utmrd_addr =
        utmrl_base + slot * 64; /* Task Management descriptor is 64 bytes */
    uint32_t utmrd[16];
    if (dma_memory_read(&address_space_memory, utmrd_addr, utmrd, 64,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
      continue;
    }

    /* Write Task Management Response UPIU at offset 32 */
    uint8_t tm_rsp[32];
    memset(tm_rsp, 0, sizeof(tm_rsp));
    tm_rsp[0] = 0x24; /* UPIU_TRANSACTION_TASK_RSP */
    tm_rsp[6] = 0x00; /* UPIU_TASK_MANAGEMENT_FUNCTION_COMPL */
    dma_memory_write(&address_space_memory, utmrd_addr + 32, tm_rsp, 32,
                     MEMTXATTRS_UNSPECIFIED);

    /* Write SUCCESS to Task Management Request OCS (Word 2, offset 8) */
    utmrd[2] = 0x00000000U;
    dma_memory_write(&address_space_memory, utmrd_addr + 8, &utmrd[2], 4,
                     MEMTXATTRS_UNSPECIFIED);

    /* Clear this slot's doorbell bit */
    *(uint32_t *)(s->ufshc_regs + REG_UTMRLDBR) &= ~(1U << slot);
  }

  *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS) |= INT_UTMRCS;
  qemu_set_irq(s->irq, 0);
  qcom_ufs_update_irq(s);
}

/* Bottom Half handler for UTRD transfer requests */
static void qcom_ufs_transfer_bh(void *opaque) {
  QcomUfsState *s = QCOM_UFSHC(opaque);
  uint32_t doorbell = s->pending_utrl_doorbell;
  s->pending_utrl_doorbell = 0;

  if (doorbell) {
    process_utp_transfers(s, doorbell);
  }
}

/* Bottom Half handler for Task Management requests */
static void qcom_ufs_tm_bh(void *opaque) {
  QcomUfsState *s = QCOM_UFSHC(opaque);
  uint32_t doorbell = s->pending_utmrl_doorbell;
  s->pending_utmrl_doorbell = 0;

  if (doorbell) {
    process_task_mgmt_transfers(s, doorbell);
  }
}

/* UFSHCI Host Controller MMIO */
static uint64_t qcom_ufshc_read(void *opaque, hwaddr offset, unsigned size) {
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

static void qcom_ufshc_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size) {
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
      *(uint32_t *)(s->ufshc_regs + REG_UTRLDBR) |= val;
      s->pending_utrl_doorbell |= val;
      qemu_bh_schedule(s->transfer_bh);
      return;
    } else if (offset == REG_UTMRLDBR) {
      *(uint32_t *)(s->ufshc_regs + REG_UTMRLDBR) |= val;
      s->pending_utmrl_doorbell |= val;
      qemu_bh_schedule(s->tm_bh);
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
    stop memset(s->ufshc_regs, 0, sizeof(s->ufshc_regs));
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

/* Device Attributes and Flags */
s->flag_fDeviceInit = 0;
s->attr_bBootLunEn = 1;
s->attr_bCurrentPowerMode = 0x11;
}

static void qcom_ufs_realize(DeviceState *dev, Error **errp) {
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

  /* Bottom Halves for asynchronous transfer execution */
  s->transfer_bh =
      qemu_bh_new_guarded(qcom_ufs_transfer_bh, s, &dev->mem_reentrancy_guard);
  s->tm_bh = qemu_bh_new_guarded(qcom_ufs_tm_bh, s, &dev->mem_reentrancy_guard);

  if (!ufs_log_file) {
    ufs_log_file = fopen("C:\\qemu_work\\ufs_debug.log", "w");
    ufs_log("=== QEMU Qualcomm UFS Controller Realized ===\n");
  }
}

static void qcom_ufs_class_init(ObjectClass *klass, void *data) {
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

static void qcom_ufs_register_types(void) {
  type_register_static(&qcom_ufs_info);
}

type_init(qcom_ufs_register_types)
