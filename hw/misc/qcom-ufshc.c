/*
 * Qualcomm SM6150 UFS Host Controller (UFSHCI 2.1) & QMP PHY emulation for
 * QEMU.
 *
 * Emulates the UFSHCI 2.1 host controller at 0x1d84000, the QMP UFS PHY at
 * 0x1d87000, and the UFS ICE crypto block at 0x1d90000.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
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

/* Interrupt Status Bits (UFSHCI 2.1 Standard) */
#define INT_UTRCS (1U << 0)  /* UTP Transfer Request Completion (Bit 0) */
#define INT_UDEPRI (1U << 1) /* UIC DME_ENDPOINTRESET Indication (Bit 1) */
#define INT_UE (1U << 2)     /* UIC Error (Bit 2) */
#define INT_UTMS (1U << 3)   /* UIC Test Mode Status (Bit 3) */
#define INT_UPMS (1U << 4)   /* UIC Power Mode Status (Bit 4) */
#define INT_UHXS (1U << 5)   /* UIC Hibernate Exit Status (Bit 5) */
#define INT_UHES (1U << 6)   /* UIC Hibernate Enter Status (Bit 6) */
#define INT_ULLS (1U << 7)   /* UIC Link Lost Status (Bit 7) */
#define INT_ULSS (1U << 8)   /* UIC Link Startup Status (Bit 8) */
#define INT_UTMRCS (1U << 9) /* UTP Task Management Request Completion (Bit 9) */
#define INT_UCCS (1U << 10)  /* UIC Command Completion Status (Bit 10) */
#define INT_DFES (1U << 11)  /* Device Fatal Error Status (Bit 11) */
#define INT_UTPES (1U << 12) /* UTP Error Status (Bit 12) */
#define INT_HCFES (1U << 16) /* Host Controller Fatal Error (Bit 16) */
#define INT_SBFES (1U << 17) /* System Bus Fatal Error (Bit 17) */

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

/* GPT prefix: LBAs 0-2047 (1 MiB) are served from gpt_*_prefix.bin.
 * LBAs >= 2048 are served from the raw ext4 image at offset (lba-2048)*512.
 * This makes the Linux kernel create /dev/block/by-name/system and vendor. */
#define GPT_PARTITION_START_LBA 2048ULL

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
  uint32_t status = *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS);
  uint32_t enable = *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_ENABLE);
  qemu_set_irq(s->irq, (status & enable) ? 1 : 0);
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
static void handle_query_request(QcomUfsState *s, uint64_t ucd_base, uint8_t *cmd_header,
                                 uint8_t *rsp_header, uint64_t resp_addr) {
  /* Query request transaction-specific fields start at byte 12 of the
   * command UPIU (bytes 0-11 are the UPIU header). */
  uint8_t query_req[20];
  memset(query_req, 0, sizeof(query_req));
  dma_memory_read(&address_space_memory, ucd_base + 12, query_req, 20,
                  MEMTXATTRS_UNSPECIFIED);

  uint8_t opcode = query_req[0];
  uint8_t idn = query_req[1];
  uint8_t index = query_req[2];
  uint8_t selector = query_req[3];

  (void)selector;

  /* Initialize 32-byte Response UPIU Header */
  memcpy(rsp_header, cmd_header, 32);
  rsp_header[0] = UPIU_TRANSACTION_QUERY_RSP; /* 0x36 */
  rsp_header[1] = 0x00;                       /* Flags */
  rsp_header[6] = 0x00;                       /* Query SUCCESS */
  rsp_header[7] = 0x00;                       /* Device SUCCESS */

  /* Initialize 32-byte Query Response Data Segment at resp_addr + 32 */
  uint8_t query_rsp[32];
  memset(query_rsp, 0, sizeof(query_rsp));
  query_rsp[0] = opcode;
  query_rsp[1] = idn;
  query_rsp[2] = index;
  query_rsp[3] = selector;

  uint32_t val = 0;

  ufs_log("[UFS_QUERY] opcode=0x%02x, idn=0x%02x, index=%u, sel=%u\n",
          opcode, idn, index, selector);

  switch (opcode) {
  case 0: /* QUERY NOP – Qualcomm 4.14 ufshcd sends NOP as QUERY_REQ opcode=0.
             ufshcd_dev_cmd_completion checks response trans_code == UPIU_TRANSACTION_NOP_IN (0x20).
             0x36 (QUERY_RSP) and 0x00 are both rejected; 0x20 (NOP_IN) is required. */
    rsp_header[0] = UPIU_TRANSACTION_NOP_IN; /* 0x20 */
    break;
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
      desc_buf[3] = 0x02;  /* bNumberLU = 2 Logical Units (sda=system, sdb=vendor) */
      desc_buf[4] = 0x00;  /* bNumberParams = 0 */
      desc_buf[5] = 0x01;  /* bBootEnable = 1 */
      desc_buf[6] = 0x00;  /* bDescrAccessEn = 0 */
      desc_buf[7] = 0x01;  /* bInitPowerMode = 1 */
      desc_buf[8] = 0x01;  /* bHighPriorityLUN = 1 */
      desc_buf[9] = 0x00;  /* bSecureRemovalType = 0 */
      desc_buf[10] = 0x00; /* bSecurityLU = 0 */
      desc_buf[12] = 0x01; /* bUD0BaseOffset */
      desc_buf[13] = 0x01; /* bUDConfigPLength */
      desc_buf[14] = 0x01; /* bMaxNumOfRTT = 1 */
      desc_buf[15] = 0x00; /* wSpecVersion = 0x0210 */
      desc_buf[16] = 0x02;
      desc_buf[17] = 0x10;
      desc_buf[18] = 0x00; /* wManufacturingDate */
      desc_buf[19] = 0x00;
      desc_buf[20] = 0x01; /* Manufacturer Name String Index */
      desc_buf[21] = 0x02; /* Product Name String Index */
      desc_buf[22] = 0x03; /* Serial Number String Index */
      desc_buf[23] = 0x04; /* OEM ID String Index */
    } else if (idn == QUERY_DESC_IDN_CONFIG) {
      /* Configuration Descriptor (226 bytes) */
      desc_len = 226;
      desc_buf[0] = 226;   /* bLength */
      desc_buf[1] = 0x01;  /* bDescriptorType = CONFIGURATION */
      desc_buf[2] = 0x01;  /* bBootEnable = 1 */
      desc_buf[3] = 0x00;  /* bDescrAccessEn = 0 */
      desc_buf[4] = 0x01;  /* bInitPowerMode = 1 */
      desc_buf[5] = 0x01;  /* bHighPriorityLUN = 1 */
      desc_buf[6] = 0x00;  /* bSecureRemovalType = 0 */
      desc_buf[7] = 0x00;  /* bInitActiveICCLevel = 0 */
      desc_buf[8] = 0x00;  /* wPeriodicRTCUpdate = 0 */
      desc_buf[9] = 0x00;
    } else if (idn == QUERY_DESC_IDN_UNIT) {
      if (index >= 2) {
        rsp_header[6] = 0x01; /* Query result: INVALID_INDEX */
        break;
      }
      desc_len = 35;
      desc_buf[0] = 35;    /* bLength = 35 (0x23) */
      desc_buf[1] = 0x02;  /* bDescriptorType = UNIT */
      desc_buf[2] = index; /* bUnitIndex */
      desc_buf[3] = 0x01;  /* bLUEnable = 1 */
      desc_buf[4] = (index == 0) ? 0x01 : 0x00;  /* bBootLunID: 1 for LUN 0 (sda/system), 0 for LUN 1 (sdb/vendor) */
      desc_buf[5] = 0x00;  /* bLUWriteProtect = 0 */
      desc_buf[6] = 0x20;  /* bLUQueueDepth = 32 */
      desc_buf[7] = 0x00;  /* bReserved */
      desc_buf[8] = 0x00;  /* bMemoryType = 0 (Normal) */
      desc_buf[9] = 0x00;  /* bDataSharedLUN = 0 */
      desc_buf[10] = 0x09; /* bLogicalBlockLength: 2^9 = 512 bytes */

      /* qLogicalBlockCount (8 bytes, big-endian) */
      uint64_t total_blocks = (index == 0)
          ? ((3758096384ULL / 512) + GPT_PARTITION_START_LBA)
          : ((2147483648ULL / 512) + GPT_PARTITION_START_LBA);
      desc_buf[11] = (total_blocks >> 56) & 0xFF;
      desc_buf[12] = (total_blocks >> 48) & 0xFF;
      desc_buf[13] = (total_blocks >> 40) & 0xFF;
      desc_buf[14] = (total_blocks >> 32) & 0xFF;
      desc_buf[15] = (total_blocks >> 24) & 0xFF;
      desc_buf[16] = (total_blocks >> 16) & 0xFF;
      desc_buf[17] = (total_blocks >> 8) & 0xFF;
      desc_buf[18] = total_blocks & 0xFF;

      /* dNumAllocUnits (4 bytes, big-endian) */
      desc_buf[19] = 0x00;
      desc_buf[20] = 0x00;
      desc_buf[21] = 0x08;
      desc_buf[22] = 0x00;
    } else if (idn == QUERY_DESC_IDN_GEOMETRY) {
      /* Geometry Descriptor (68 bytes) */
      desc_len = 68;
      desc_buf[0] = 68;    /* bLength */
      desc_buf[1] = 0x07;  /* bDescriptorType = GEOMETRY */
      desc_buf[2] = 0x01;  /* bMediaTechnology = 1 (eMMC/UFS NAND) */
      desc_buf[4] = 0x08;  /* qTotalRawDeviceCapacity (16 GB) */
      desc_buf[12] = 0x02; /* bMaxNumberLU = 2 */
      desc_buf[13] = 0x04; /* dSegmentSize = 4 MB */
      desc_buf[17] = 0x08; /* bAllocationUnitSize = 8 */
      desc_buf[18] = 0x01; /* bMinAddrBlockSize = 1 (512 bytes) */
      desc_buf[19] = 0x08; /* bOptimalReadBlockSize = 8 (4 KB) */
      desc_buf[20] = 0x08; /* bOptimalWriteBlockSize = 8 (4 KB) */
      desc_buf[21] = 0x01; /* bMaxInBufferSize = 1 */
      desc_buf[22] = 0x01; /* bMaxOutBufferSize = 1 */
      desc_buf[23] = 0x00; /* dRPMB_ReadWriteSize = 0 */
    } else if (idn == 5 /* STRING */) {
      if (index == 0) {
        /* Language ID: 0x0409 (English) */
        desc_len = 4;
        desc_buf[0] = 4;
        desc_buf[1] = 0x05;
        desc_buf[2] = 0x09;
        desc_buf[3] = 0x04;
      } else if (index == 1) {
        desc_len = encode_string_desc("SAMSUNG", desc_buf, sizeof(desc_buf));
      } else if (index == 2) {
        desc_len = encode_string_desc("KLMCG8GEND-B031", desc_buf, sizeof(desc_buf));
      } else if (index == 3) {
        desc_len = encode_string_desc("1234567890", desc_buf, sizeof(desc_buf));
      } else if (index == 4) {
        desc_len = encode_string_desc("QUALCOMM", desc_buf, sizeof(desc_buf));
      } else {
        desc_len = encode_string_desc("GENERIC_UFS", desc_buf, sizeof(desc_buf));
      }
    } else {
      desc_len = 32;
      desc_buf[0] = 32;
      desc_buf[1] = idn;
    }

    if (desc_len > 0) {
      /* Descriptor data goes in the UPIU data segment at resp_addr + 32.
       * (resp_addr is already the base of the response UPIU; the 32-byte
       * UPIU header occupies bytes 0-31, data segment starts at byte 32.) */
      uint64_t desc_addr = resp_addr + 32;
      dma_memory_write(&address_space_memory, desc_addr, desc_buf, desc_len,
                       MEMTXATTRS_UNSPECIFIED);
      rsp_header[10] = (desc_len >> 8) & 0xFF;
      rsp_header[11] = desc_len & 0xFF;
      query_rsp[6] = (desc_len >> 8) & 0xFF;
      query_rsp[7] = desc_len & 0xFF;
      ufs_log("[UFS_QUERY] wrote desc idn=0x%02x, index=%u, addr=0x%" PRIx64
              ", sz=%u\n",
              idn, index, desc_addr, desc_len);
    }
    break;
  }
  case UPIU_QUERY_OPCODE_READ_FLAG: {
    val = 0; /* fDeviceInit is always ready (0) */
    break;
  }
  case UPIU_QUERY_OPCODE_SET_FLAG: {
    val = 0; /* fDeviceInit completed / 0 */
    break;
  }
  case UPIU_QUERY_OPCODE_CLEAR_FLAG: {
    val = 0;
    break;
  }
  case UPIU_QUERY_OPCODE_READ_ATTR: {
    if (idn == 0x00) {
      val = s->attr_bBootLunEn;
    } else if (idn == 0x01) {
      val = s->attr_bCurrentPowerMode;
    }
    break;
  }
  case UPIU_QUERY_OPCODE_WRITE_ATTR: {
    val = ((uint32_t)query_req[8] << 24) | ((uint32_t)query_req[9] << 16) |
          ((uint32_t)query_req[10] << 8) | query_req[11];
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

  /* Pack the query response transaction-specific fields into rsp_header[12..31].
   * The kernel reads them from &response_upiu[12] (struct utp_upiu_query):
   *   [12] opcode, [13] idn, [14] index, [15] selector,
   *   [18..19] length (big-endian), [20..23] value (big-endian). */
  rsp_header[12] = query_rsp[0];           /* opcode */
  rsp_header[13] = query_rsp[1];           /* idn */
  rsp_header[14] = query_rsp[2];           /* index */
  rsp_header[15] = query_rsp[3];           /* selector */
  rsp_header[18] = query_rsp[6];           /* length MSB */
  rsp_header[19] = query_rsp[7];           /* length LSB */
  rsp_header[20] = (val >> 24) & 0xFF;     /* value byte 3 */
  rsp_header[21] = (val >> 16) & 0xFF;     /* value byte 2 */
  rsp_header[22] = (val >> 8) & 0xFF;      /* value byte 1 */
  rsp_header[23] = val & 0xFF;             /* value byte 0 */
  /* (bytes 16-17 = reserved_osf, 24-31 = reserved, already 0 from memset) */
}

/* Helper to write data across PRDT entries or into Response Data Segment */
static void dma_write_prdt(uint64_t ucd_base, uint64_t resp_offset,
                           uint64_t prdt_addr, uint16_t prdt_len,
                           uint8_t *resp_upiu,
                           const void *src, size_t total_sz) {
  ufs_log("[DMA_WR_PRDT] ucd_base=0x%" PRIx64 ", prdt_addr=0x%" PRIx64 ", prdt_len=%u, total_sz=%zu\n",
          ucd_base, prdt_addr, prdt_len, total_sz);
  if (prdt_len == 0 || prdt_addr == 0) {
    /* Write to Response UPIU Data Segment at resp_addr + 32 (within 512-byte response buffer) */
    uint64_t data_addr = ucd_base + resp_offset + 32;
    size_t write_sz = (total_sz > 480) ? 480 : total_sz;
    dma_memory_write(&address_space_memory, data_addr, src, write_sz,
                     MEMTXATTRS_UNSPECIFIED);
    if (resp_upiu) {
      resp_upiu[10] = (write_sz >> 8) & 0xFF;
      resp_upiu[11] = write_sz & 0xFF;
    }
    ufs_log("[DMA_WR_PRDT] (no PRDT) wrote %zu bytes to 0x%" PRIx64 "\n", write_sz, data_addr);
    return;
  }
  const uint8_t *p = (const uint8_t *)src;
  size_t remaining = total_sz;

  for (uint16_t i = 0; i < prdt_len; i++) {
    uint64_t prdt_entry_addr = prdt_addr + i * 16;
    uint32_t prd[4];
    if (dma_memory_read(&address_space_memory, prdt_entry_addr, prd, 16,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
      ufs_log("[DMA_WR_PRDT] Failed to read PRD entry %u at 0x%" PRIx64 "\n", i, prdt_entry_addr);
      break;
    }
    uint64_t dba = (uint64_t)prd[0] | ((uint64_t)prd[1] << 32);
    uint32_t dbc = (prd[3] & 0x3FFFF) + 1;
    size_t chunk = (remaining < dbc) ? remaining : dbc;
    ufs_log("[DMA_WR_PRDT] entry %u: dba=0x%" PRIx64 ", dbc=%u, chunk=%zu, remaining=%zu\n",
            i, dba, dbc, chunk, remaining);
    if (chunk > 0) {
      dma_memory_write(&address_space_memory, dba, p, chunk,
                       MEMTXATTRS_UNSPECIFIED);
      /* Readback check */
      uint8_t rb[16];
      size_t rb_sz = (chunk > sizeof(rb)) ? sizeof(rb) : chunk;
      dma_memory_read(&address_space_memory, dba, rb, rb_sz, MEMTXATTRS_UNSPECIFIED);
      ufs_log("  written chunk to 0x%" PRIx64 ": %02x %02x %02x %02x %02x %02x %02x %02x\n",
              dba, rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7]);
      p += chunk;
      remaining -= chunk;
    }
    if (chunk < dbc) {
      /* Zero-fill any unwritten portion of the PRD buffer */
      static const uint8_t zeroes[512] = {0};
      size_t to_zero = dbc - chunk;
      while (to_zero > 0) {
        size_t zchunk = (to_zero > sizeof(zeroes)) ? sizeof(zeroes) : to_zero;
        dma_memory_write(&address_space_memory, dba + chunk, zeroes, zchunk,
                         MEMTXATTRS_UNSPECIFIED);
        chunk += zchunk;
        to_zero -= zchunk;
      }
    }
  }
}

#ifdef _WIN32
static HANDLE s_sys_file = INVALID_HANDLE_VALUE;
static HANDLE s_vnd_file = INVALID_HANDLE_VALUE;
static HANDLE s_sys_gpt_file = INVALID_HANDLE_VALUE;
static HANDLE s_vnd_gpt_file = INVALID_HANDLE_VALUE;

/* GPT prefix: LBAs 0-2047 (1 MiB) are served from gpt_*_prefix.bin.
 * LBAs >= 2048 are served from the raw ext4 image at offset (lba-2048)*512.
 * This makes the Linux kernel create /dev/block/by-name/system and vendor. */
#define GPT_PARTITION_START_LBA 2048ULL

static HANDLE get_ufs_disk_handle(uint8_t lun) {
  if (lun == 0) {
    if (s_sys_file == INVALID_HANDLE_VALUE) {
      s_sys_file = CreateFileA(
          "C:\\Users\\ABISIN RAJ\\Desktop\\Projects\\CustomAndroidEmulator\\build\\launcher\\roms\\PixelExperience_violet-13.0-20240127-0711-OFFICIAL\\system.img",
          GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return s_sys_file;
  } else if (lun == 1) {
    if (s_vnd_file == INVALID_HANDLE_VALUE) {
      s_vnd_file = CreateFileA(
          "C:\\Users\\ABISIN RAJ\\Desktop\\Projects\\CustomAndroidEmulator\\build\\launcher\\roms\\PixelExperience_violet-13.0-20240127-0711-OFFICIAL\\vendor.img",
          GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return s_vnd_file;
  }
  return INVALID_HANDLE_VALUE;
}

static HANDLE get_ufs_gpt_handle(uint8_t lun) {
  if (lun == 0) {
    if (s_sys_gpt_file == INVALID_HANDLE_VALUE) {
      s_sys_gpt_file = CreateFileA(
          "C:\\qemu_work\\gpt_system_prefix.bin",
          GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return s_sys_gpt_file;
  } else if (lun == 1) {
    if (s_vnd_gpt_file == INVALID_HANDLE_VALUE) {
      s_vnd_gpt_file = CreateFileA(
          "C:\\qemu_work\\gpt_vendor_prefix.bin",
          GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return s_vnd_gpt_file;
  }
  return INVALID_HANDLE_VALUE;
}
#endif

/* Multi-sector disk reader across PRDT scatter-gather descriptors.
 * LBAs 0..GPT_PARTITION_START_LBA-1 are served from gpt_*_prefix.bin.
 * LBAs >= GPT_PARTITION_START_LBA are served from the raw ext4 image,
 * with file_offset = (lba - GPT_PARTITION_START_LBA) * 512. */
static void dma_read_disk(uint8_t lun, uint64_t start_lba, uint32_t num_blocks,
                          uint64_t prdt_addr, uint16_t prdt_len) {
#ifdef _WIN32
  uint32_t bytes_to_read = num_blocks * 512;
  uint32_t bytes_done = 0;

  ufs_log("[UFS_READ] lun=%u, lba=%" PRIu64 ", blocks=%u, prdt_len=%u\n",
          lun, start_lba, num_blocks, prdt_len);

  for (uint16_t p = 0; p < prdt_len && bytes_done < bytes_to_read; p++) {
    uint32_t prd[4];
    if (dma_memory_read(&address_space_memory, prdt_addr + p * 16, prd, 16,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
      ufs_log("[UFS_READ] PRD DMA read failed at 0x%" PRIx64 "\n", prdt_addr + p * 16);
      break;
    }

    uint64_t entry_addr = (uint64_t)prd[0] | ((uint64_t)prd[1] << 32);
    uint32_t entry_len = (prd[3] & 0x3FFFF) + 1;
    if (entry_addr == 0 || entry_len == 0) {
      break;
    }

    uint32_t chunk = bytes_to_read - bytes_done;
    if (chunk > entry_len) {
      chunk = entry_len;
    }

    uint8_t buf[65536];
    uint32_t chunk_rem = chunk;
    uint32_t chunk_done = 0;

    while (chunk_rem > 0) {
      DWORD to_read = (chunk_rem > sizeof(buf)) ? sizeof(buf) : chunk_rem;
      DWORD actual_read = 0;

      /* Compute which absolute LBA and byte offset within that LBA we are at */
      uint64_t byte_offset_in_transfer = (uint64_t)bytes_done + chunk_done;
      uint64_t curr_lba = start_lba + byte_offset_in_transfer / 512;
      uint64_t intra_lba_offset = byte_offset_in_transfer % 512;

      /* Determine if this chunk crosses the GPT/data boundary */
      uint64_t bytes_available = to_read;
      if (curr_lba < GPT_PARTITION_START_LBA) {
        /* How many bytes remain in GPT prefix region from curr position? */
        uint64_t gpt_end_byte = GPT_PARTITION_START_LBA * 512;
        uint64_t curr_abs_byte = curr_lba * 512 + intra_lba_offset;
        uint64_t gpt_remaining = gpt_end_byte - curr_abs_byte;
        if (bytes_available > gpt_remaining) {
          bytes_available = (uint32_t)gpt_remaining;
        }
      }
      if (bytes_available == 0) bytes_available = to_read;
      DWORD chunk_this = (DWORD)bytes_available;
      if (chunk_this > to_read) chunk_this = to_read;

      uint64_t curr_abs_byte = curr_lba * 512 + intra_lba_offset;

      if (lun < 2 && curr_lba < GPT_PARTITION_START_LBA) {
        /* Serve from GPT prefix file */
        HANDLE hGpt = get_ufs_gpt_handle(lun);
        if (hGpt != INVALID_HANDLE_VALUE) {
          LARGE_INTEGER li;
          li.QuadPart = (LONGLONG)curr_abs_byte;
          SetFilePointerEx(hGpt, li, NULL, FILE_BEGIN);
          if (!ReadFile(hGpt, buf, chunk_this, &actual_read, NULL) || actual_read == 0) {
            ufs_log("[UFS_READ] GPT prefix ReadFile failed (err=%lu)\n", GetLastError());
            memset(buf, 0, chunk_this);
            actual_read = chunk_this;
          } else if (actual_read < chunk_this) {
            memset(buf + actual_read, 0, chunk_this - actual_read);
            actual_read = chunk_this;
          }
        } else {
          memset(buf, 0, chunk_this);
          actual_read = chunk_this;
        }
      } else if (lun < 2 && curr_lba >= GPT_PARTITION_START_LBA) {
        /* Serve from raw ext4 image: file offset = (lba - 2048) * 512 */
        uint64_t file_offset = (curr_lba - GPT_PARTITION_START_LBA) * 512 + intra_lba_offset;
        HANDLE hFile = get_ufs_disk_handle(lun);
        if (hFile != INVALID_HANDLE_VALUE) {
          LARGE_INTEGER li;
          li.QuadPart = (LONGLONG)file_offset;
          SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
          if (!ReadFile(hFile, buf, chunk_this, &actual_read, NULL) || actual_read == 0) {
            ufs_log("[UFS_READ] img ReadFile failed (err=%lu)\n", GetLastError());
            memset(buf, 0, chunk_this);
            actual_read = chunk_this;
          } else if (actual_read < chunk_this) {
            memset(buf + actual_read, 0, chunk_this - actual_read);
            actual_read = chunk_this;
          }
        } else {
          memset(buf, 0, chunk_this);
          actual_read = chunk_this;
        }
      } else {
        memset(buf, 0, chunk_this);
        actual_read = chunk_this;
      }

      dma_memory_write(&address_space_memory, entry_addr + chunk_done, buf, actual_read, MEMTXATTRS_UNSPECIFIED);

      /* For LBA 1 (GPT header), dump the first 64 bytes served so we can verify EFI PART */
      if (curr_lba == 1 && chunk_done == 0) {
        uint32_t dump_len = actual_read < 64 ? actual_read : 64;
        ufs_log("[GPT_HDR_DUMP] lun=%u first %u bytes of LBA 1 served to guest:", lun, dump_len);
        for (uint32_t _di = 0; _di < dump_len; _di++) ufs_log(" %02x", buf[_di]);
        ufs_log("\n");
        /* Also log as ASCII for the EFI PART signature */
        ufs_log("[GPT_HDR_SIG] bytes[0..7] = '%c%c%c%c%c%c%c%c'\n",
                buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
      }

      chunk_done += actual_read;
      chunk_rem -= actual_read;
    }

    bytes_done += chunk;
  }
  ufs_log("[UFS_READ] Complete: done=%u/%u bytes\n", bytes_done, bytes_to_read);
#endif
}


/* Handle SCSI Command */
static void handle_scsi_command(QcomUfsState *s, uint8_t *req, uint8_t *rsp,
                                uint64_t ucd_base, uint64_t resp_offset,
                                uint64_t prdt_addr, uint16_t prdt_len) {
  uint8_t scsi_op = req[16];
  uint8_t lun = req[2];

  /* Decode READ(10)/READ(16)/WRITE(10)/WRITE(16) LBA + transfer-length for the log */
  if (scsi_op == SCSI_READ_10 || scsi_op == SCSI_WRITE_10) {
    uint64_t _lba = ((uint64_t)req[18] << 24) | ((uint64_t)req[19] << 16) |
                    ((uint64_t)req[20] << 8)  |  (uint64_t)req[21];
    uint32_t _nb  = ((uint32_t)req[23] << 8)  |  (uint32_t)req[24];
    ufs_log("[UFS_SCSI] op=0x%02x(%s), lun=%u, lba=%" PRIu64 ", blocks=%u, prdt_len=%u\n",
            scsi_op, (scsi_op == SCSI_READ_10 ? "READ10" : "WRITE10"),
            lun, _lba, _nb, prdt_len);
  } else if (scsi_op == SCSI_READ_16 || scsi_op == SCSI_WRITE_16) {
    uint64_t _lba = ((uint64_t)req[18] << 56) | ((uint64_t)req[19] << 48) |
                    ((uint64_t)req[20] << 40) | ((uint64_t)req[21] << 32) |
                    ((uint64_t)req[22] << 24) | ((uint64_t)req[23] << 16) |
                    ((uint64_t)req[24] << 8)  |  (uint64_t)req[25];
    uint32_t _nb  = ((uint32_t)req[26] << 24) | ((uint32_t)req[27] << 16) |
                    ((uint32_t)req[28] << 8)  |  (uint32_t)req[29];
    ufs_log("[UFS_SCSI] op=0x%02x(%s), lun=%u, lba=%" PRIu64 ", blocks=%u, prdt_len=%u\n",
            scsi_op, (scsi_op == SCSI_READ_16 ? "READ16" : "WRITE16"),
            lun, _lba, _nb, prdt_len);
  } else {
    ufs_log("[UFS_SCSI] op=0x%02x, lun=%u\n", scsi_op, lun);
  }

  memset(rsp, 0, 32);
  rsp[0] = UPIU_TRANSACTION_RESPONSE; /* 0x21 */
  rsp[1] = 0x00;                      /* Flags */
  rsp[2] = lun;                       /* LUN */
  rsp[3] = req[3];                    /* Task Tag */
  rsp[6] = 0x00;                      /* Target SUCCESS */
  rsp[7] = 0x00;                      /* SAM Status: GOOD (0x00) */

  /* Clear sense_data_len at resp_addr + 32 */
  uint32_t zero_sense = 0;
  dma_memory_write(&address_space_memory, ucd_base + resp_offset + 32,
                   &zero_sense, 4, MEMTXATTRS_UNSPECIFIED);

  if (lun >= 2) {
    if (scsi_op == SCSI_INQUIRY) {
      uint8_t inq[36];
      memset(inq, 0, sizeof(inq));
      inq[0] = 0x7F; /* Peripheral Qualifier = 011b (Not supported), Device Type = 1Fh */
      inq[1] = 0x00;
      inq[2] = 0x06; /* SPC-4 / UFS 2.1 standard */
      inq[3] = 0x02; /* Response data format */
      inq[4] = 31;   /* Additional length = 31 bytes */
      rsp[6] = 0x00; /* Target SUCCESS */
      rsp[7] = 0x00; /* SAM Status: GOOD */
      dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, inq, sizeof(inq));
      return;
    }

    /* Return CHECK CONDITION (LOGICAL_UNIT_NOT_SUPPORTED) for any other command */
    rsp[6] = 0x00;
    rsp[7] = 0x02;  /* SAM Status: CHECK CONDITION */
    rsp[16] = 0x00; /* Sense Data Length MSB */
    rsp[17] = 18;   /* Sense Data Length LSB = 18 bytes */

    uint8_t sense[18];
    memset(sense, 0, sizeof(sense));
    sense[0] = 0x70;  /* Response Code: Fixed Current */
    sense[2] = 0x05;  /* Sense Key: ILLEGAL_REQUEST */
    sense[7] = 10;    /* Additional Sense Length = 10 */
    sense[12] = 0x25; /* ASC: LOGICAL_UNIT_NOT_SUPPORTED */
    sense[13] = 0x00; /* ASCQ: 0 */

    dma_memory_write(&address_space_memory, ucd_base + resp_offset + 32,
                     sense, sizeof(sense), MEMTXATTRS_UNSPECIFIED);
    return;
  }

  switch (scsi_op) {
  case SCSI_INQUIRY: {
    uint8_t evpd = req[17] & 0x01;
    uint8_t page_code = req[18];
    if (evpd) {
      if (page_code == 0x00) {
        uint8_t vpd00[7] = {0x00, 0x00, 0x00, 0x03, 0x00, 0x80, 0x83};
        dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, vpd00, sizeof(vpd00));
      } else if (page_code == 0x80) {
        uint8_t vpd80[20];
        memset(vpd80, 0, sizeof(vpd80));
        vpd80[0] = 0x00;
        vpd80[1] = 0x80;
        vpd80[3] = 16;
        memcpy(&vpd80[4], (lun == 0) ? "SYS_SERIAL_0001 " : "VND_SERIAL_0001 ", 16);
        dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, vpd80, sizeof(vpd80));
      } else if (page_code == 0x83) {
        uint8_t vpd83[24];
        memset(vpd83, 0, sizeof(vpd83));
        vpd83[0] = 0x00;
        vpd83[1] = 0x83;
        vpd83[3] = 20;
        vpd83[4] = 0x01;
        vpd83[5] = 0x03;
        vpd83[7] = 16;
        memcpy(&vpd83[8], (lun == 0) ? "QUALCOMM_SYS_LUN" : "QUALCOMM_VND_LUN", 16);
        dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, vpd83, sizeof(vpd83));
      } else {
        uint8_t vpd_empty[4] = {0x00, page_code, 0x00, 0x00};
        dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, vpd_empty, sizeof(vpd_empty));
      }
    } else {
      uint8_t inq[36];
      memset(inq, 0, sizeof(inq));
      inq[0] = 0x00; /* Direct Access Block Device */
      inq[1] = 0x00; /* RMB = 0 */
      inq[2] = 0x06; /* SPC-4 / UFS 2.1 standard */
      inq[3] = 0x02; /* Response data format: SPC-2+ */
      inq[4] = 31;   /* Additional length (36 - 5) */
      memcpy(&inq[8], "QUALCOMM", 8);
      if (lun == 0) {
        memcpy(&inq[16], "UFS_SYSTEM_LUN  ", 16);
      } else {
        memcpy(&inq[16], "UFS_VENDOR_LUN  ", 16);
      }
      memcpy(&inq[32], "0210", 4);
      dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, inq, sizeof(inq));
    }
    break;
  }
  case SCSI_REQUEST_SENSE: {
    uint8_t sense[18];
    memset(sense, 0, sizeof(sense));
    sense[0] = 0x70;
    sense[2] = 0x00; /* NO SENSE */
    sense[7] = 10;
    dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, sense, sizeof(sense));
    break;
  }
  case SCSI_TEST_UNIT_READY:
  case SCSI_START_STOP_UNIT:
  case SCSI_SYNCHRONIZE_CACHE:
    break;
  case SCSI_MODE_SENSE_6: {
    uint8_t mode6[4] = {3, 0, 0, 0};
    dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, mode6, sizeof(mode6));
    break;
  }
  case SCSI_MODE_SENSE_10: {
    uint8_t mode10[8] = {0, 6, 0, 0, 0, 0, 0, 0};
    dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, mode10, sizeof(mode10));
    break;
  }
  case SCSI_READ_CAPACITY_10: {
    uint8_t cap[8];
    memset(cap, 0, sizeof(cap));
    /* Total virtual disk = GPT_PARTITION_START_LBA prefix + raw image sectors.
     * This must match the GPT header's disk_sectors so the kernel accepts
     * the partition table without rejecting it for exceeding capacity. */
    uint32_t last_lba = (lun == 0)
        ? (uint32_t)((3758096384ULL / 512) + GPT_PARTITION_START_LBA - 1)
        : (uint32_t)((2147483648ULL / 512) + GPT_PARTITION_START_LBA - 1);
    cap[0] = (last_lba >> 24) & 0xFF;
    cap[1] = (last_lba >> 16) & 0xFF;
    cap[2] = (last_lba >> 8) & 0xFF;
    cap[3] = last_lba & 0xFF;
    cap[4] = 0x00;
    cap[5] = 0x00;
    cap[6] = 0x02; /* 512 bytes per block */
    cap[7] = 0x00;
    dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, cap, sizeof(cap));
    break;
  }
  case SCSI_READ_CAPACITY_16: {
    uint8_t cap16[32];
    memset(cap16, 0, sizeof(cap16));
    /* Same virtual disk size as READ_CAPACITY_10 but 64-bit */
    uint64_t last_lba = (lun == 0)
        ? ((3758096384ULL / 512) + GPT_PARTITION_START_LBA - 1)
        : ((2147483648ULL / 512) + GPT_PARTITION_START_LBA - 1);
    cap16[0] = (last_lba >> 56) & 0xFF;
    cap16[1] = (last_lba >> 48) & 0xFF;
    cap16[2] = (last_lba >> 40) & 0xFF;
    cap16[3] = (last_lba >> 32) & 0xFF;
    cap16[4] = (last_lba >> 24) & 0xFF;
    cap16[5] = (last_lba >> 16) & 0xFF;
    cap16[6] = (last_lba >> 8) & 0xFF;
    cap16[7] = last_lba & 0xFF;
    cap16[8] = 0x00;
    cap16[9] = 0x00;
    cap16[10] = 0x02; /* 512 */
    cap16[11] = 0x00;
    dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, cap16, sizeof(cap16));
    break;
  }
  case SCSI_REPORT_LUNS: {
    uint8_t luns[24];
    memset(luns, 0, sizeof(luns));
    luns[3] = 16; /* LUN List Length = 16 bytes (2 LUNs: 0, 1) */
    /* SAM Single-Level LUN addressing: Byte 0 = Address method (00b), Byte 1 = LUN */
    luns[8] = 0x00;
    luns[9] = 0x00;  /* LUN 0 (sda: system.img) */
    luns[16] = 0x00;
    luns[17] = 0x01; /* LUN 1 (sdb: vendor.img) */
    dma_write_prdt(ucd_base, resp_offset, prdt_addr, prdt_len, rsp, luns, sizeof(luns));
    break;
  }
  case SCSI_READ_10:
  case SCSI_READ_16: {
    uint64_t lba = 0;
    uint32_t num_blocks = 1;
    if (scsi_op == SCSI_READ_10) {
      lba = ((uint64_t)req[18] << 24) | ((uint64_t)req[19] << 16) |
            ((uint64_t)req[20] << 8) | req[21];
      num_blocks = ((uint32_t)req[23] << 8) | req[24];
    } else {
      lba = ((uint64_t)req[18] << 56) | ((uint64_t)req[19] << 48) |
            ((uint64_t)req[20] << 40) | ((uint64_t)req[21] << 32) |
            ((uint64_t)req[22] << 24) | ((uint64_t)req[23] << 16) |
            ((uint64_t)req[24] << 8) | req[25];
      num_blocks = ((uint32_t)req[26] << 24) | ((uint64_t)req[27] << 16) |
                   ((uint32_t)req[28] << 8) | req[29];
    }
    if (num_blocks == 0) {
      num_blocks = 1;
    }

    dma_read_disk(lun, lba, num_blocks, prdt_addr, prdt_len);
    break;
  }
  case SCSI_WRITE_10:
  case SCSI_WRITE_16:
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

    /* In UFSHCI 2.1 standard (§5.2.1) and Qualcomm 4.14 kernel (ufshcd_init_lrb),
     * RUPIUO and PRDTO in the UTRD descriptor are expressed in DWORDs (4-byte units).
     * Convert DWORD offsets to byte offsets:
     *   rupiuo_raw = 128 (0x80) -> resp_offset = 512 (0x200, matching lrb->ucd_rsp_ptr)
     *   prdto_raw  = 256 (0x100)-> prdt_offset = 1024 (0x400, matching lrb->ucd_prdt_ptr) */
    uint64_t resp_offset = (uint64_t)rupiuo_raw * 4;
    uint64_t prdt_offset = (uint64_t)prdto_raw * 4;
    uint16_t prdt_len    = prdt_len_raw;

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

    ufs_log("[UFS_TX] slot=%d, ucd_base=0x%" PRIx64 ", req_addr=0x%" PRIx64 ", rupiuo_raw=%u, resp_offset=%" PRIu64 ", resp_addr=0x%" PRIx64 ", diff=%" PRIu64 ", prdto_raw=%u, prdt_len=%u\n",
            slot, ucd_base, ucd_base, rupiuo_raw, resp_offset, resp_addr, resp_addr - ucd_base, prdto_raw, prdt_len);
    ufs_log("[UFS_TX] trans_type=0x%02x, cmd_upiu[0]=0x%02x, tag=0x%02x, lun=0x%02x\n",
            trans_type, cmd_upiu[0], cmd_upiu[3], cmd_upiu[2]);

    if (trans_type == UPIU_TRANSACTION_NOP_OUT) {
      resp_upiu[0] = UPIU_TRANSACTION_NOP_IN; /* 0x20 */
      resp_upiu[1] = 0x00;
      resp_upiu[2] = cmd_upiu[2]; /* LUN */
      resp_upiu[3] = cmd_upiu[3]; /* Task Tag */
      resp_upiu[6] = 0x00;        /* Target SUCCESS */
      resp_upiu[7] = 0x00;        /* Device SUCCESS */
      ufs_log("[UFS_NOP_OUT] Handled native NOP_OUT -> NOP_IN (0x20)\n");
    } else if (trans_type == UPIU_TRANSACTION_QUERY_REQ) {
      handle_query_request(s, ucd_base, cmd_upiu, resp_upiu, resp_addr);
    } else if (trans_type == UPIU_TRANSACTION_COMMAND) {
      handle_scsi_command(s, cmd_upiu, resp_upiu, ucd_base, resp_offset, prdt_addr, prdt_len);
    }

    /* Write Response UPIU to guest memory */
    MemTxResult write_res = dma_memory_write(&address_space_memory, resp_addr, resp_upiu, 32,
                                            MEMTXATTRS_UNSPECIFIED);

    /* Read-back verification immediately after DMA write */
    uint8_t readback_rsp[32];
    memset(readback_rsp, 0xAA, sizeof(readback_rsp));
    MemTxResult read_res = dma_memory_read(&address_space_memory, resp_addr, readback_rsp, 32,
                                           MEMTXATTRS_UNSPECIFIED);

    ufs_log("[UFS_WRITE_VERIFY] slot=%d, ucd_base=0x%" PRIx64 ", resp_addr=0x%" PRIx64 " (offset=%" PRIu64 "), write_res=%d, read_res=%d\n",
            slot, ucd_base, resp_addr, resp_addr - ucd_base, (int)write_res, (int)read_res);
    ufs_log("  written [0..31] : ");
    for (int k = 0; k < 32; k++) ufs_log("%02x ", resp_upiu[k]);
    ufs_log("\n  readback[0..31] : ");
    for (int k = 0; k < 32; k++) ufs_log("%02x ", readback_rsp[k]);
    ufs_log("\n");

    /* Dump full 512 bytes of UCD region */
    uint8_t full_ucd[512];
    memset(full_ucd, 0, sizeof(full_ucd));
    dma_memory_read(&address_space_memory, ucd_base, full_ucd, 512, MEMTXATTRS_UNSPECIFIED);
    ufs_log("[UFS_FULL_UCD slot=%d] ucd_base=0x%" PRIx64 "\n", slot, ucd_base);
    for (int blk = 0; blk < 512; blk += 32) {
      ufs_log("  +0x%03x: ", blk);
      for (int k = 0; k < 32; k++) ufs_log("%02x ", full_ucd[blk + k]);
      ufs_log("\n");
    }

    /* Set OCS = 0 (OCS_SUCCESS) in UTRD */
    utrd[2] = 0x00000000U;
    MemTxResult ocs_res = dma_memory_write(&address_space_memory, utrd_addr + 8, &utrd[2], 4,
                                          MEMTXATTRS_UNSPECIFIED);
    uint32_t ocs_readback = 0xFFFFFFFF;
    dma_memory_read(&address_space_memory, utrd_addr + 8, &ocs_readback, 4, MEMTXATTRS_UNSPECIFIED);
    ufs_log("[UFS_OCS] slot=%d, utrd_addr=0x%" PRIx64 ", ocs_res=%d, written=0x0, readback=0x%08x\n",
            slot, utrd_addr + 8, (int)ocs_res, ocs_readback);

    /* Clear this slot's doorbell bit in REG_UTRLDBR */
    *(uint32_t *)(s->ufshc_regs + REG_UTRLDBR) &= ~(1U << slot);
  }

  /* Set Transfer Request Completion Status bit */
  *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS) |= INT_UTRCS;
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
        utmrl_base + slot * 80; /* Task Management descriptor is 80 bytes */
    uint32_t utmrd[20];
    if (dma_memory_read(&address_space_memory, utmrd_addr, utmrd, 80,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
      continue;
    }

    uint8_t tm_req[32];
    dma_memory_read(&address_space_memory, utmrd_addr + 16, tm_req, 32,
                    MEMTXATTRS_UNSPECIFIED);

    /* Write Task Management Response UPIU at offset 48 */
    uint8_t tm_rsp[32];
    memset(tm_rsp, 0, sizeof(tm_rsp));
    tm_rsp[0] = 0x24; /* UPIU_TRANSACTION_TASK_RSP */
    tm_rsp[1] = 0x00;
    tm_rsp[2] = tm_req[2]; /* LUN */
    tm_rsp[3] = tm_req[3]; /* Task Tag */
    tm_rsp[6] = 0x00;      /* UPIU_TASK_MANAGEMENT_FUNCTION_COMPL */
    tm_rsp[7] = 0x00;
    dma_memory_write(&address_space_memory, utmrd_addr + 48, tm_rsp, 32,
                     MEMTXATTRS_UNSPECIFIED);

    /* Write SUCCESS to Task Management Request OCS (Word 2, offset 8) */
    utmrd[2] = 0x00000000U;
    dma_memory_write(&address_space_memory, utmrd_addr + 8, &utmrd[2], 4,
                     MEMTXATTRS_UNSPECIFIED);

    /* Clear this slot's doorbell bit */
    *(uint32_t *)(s->ufshc_regs + REG_UTMRLDBR) &= ~(1U << slot);

    ufs_log("[UFS_TM] slot=%d, lun=%u, tag=%u completed\n", slot, tm_req[2],
            tm_req[3]);
  }

  *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS) |= INT_UTMRCS;
  qcom_ufs_update_irq(s);
}

/* Bottom Half handler for UTRD transfer requests */
static void qcom_ufs_transfer_bh(void *opaque) {
  QcomUfsState *s = QCOM_UFSHC(opaque);
  uint32_t doorbell = *(uint32_t *)(s->ufshc_regs + REG_UTRLDBR);

  if (doorbell) {
    process_utp_transfers(s, doorbell);
  }
}

static void qcom_ufs_tm_bh(void *opaque) {
  QcomUfsState *s = QCOM_UFSHC(opaque);
  uint32_t doorbell = *(uint32_t *)(s->ufshc_regs + REG_UTMRLDBR);

  if (doorbell) {
    process_task_mgmt_transfers(s, doorbell);
  }
}

static uint64_t qcom_ufshc_read(void *opaque, hwaddr offset, unsigned size) {
  QcomUfsState *s = QCOM_UFSHC(opaque);
  if (offset + size <= 0x1000) {
    uint32_t val = *(uint32_t *)(s->ufshc_regs + offset);
    if (offset == REG_INTERRUPT_STATUS || offset == REG_INTERRUPT_ENABLE ||
        offset == REG_UTRLDBR) {
      ufs_log("[UFS_REG_RD] off=0x%" PRIx64 ", val=0x%x\n", offset, val);
    }
    return val;
  }
  return 0;
}

static void qcom_ufshc_write(void *opaque, hwaddr offset, uint64_t val,
                             unsigned size) {
  QcomUfsState *s = QCOM_UFSHC(opaque);
  if (offset + size <= 0x1000) {
    if (offset == REG_INTERRUPT_STATUS || offset == REG_INTERRUPT_ENABLE ||
        offset == REG_UTRLDBR) {
      ufs_log("[UFS_REG_WR] off=0x%" PRIx64 ", val=0x%" PRIx64 "\n", offset, val);
    }
    if (offset == REG_INTERRUPT_STATUS) {
      /* Write 1 to clear (W1C) */
      *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_STATUS) &= ~(uint32_t)val;
      qcom_ufs_update_irq(s);
      return;
    } else if (offset == REG_INTERRUPT_ENABLE) {
      *(uint32_t *)(s->ufshc_regs + REG_INTERRUPT_ENABLE) = (uint32_t)val;
      qcom_ufs_update_irq(s);
      return;
    } else if (offset == REG_UTRLDBR) {
      *(uint32_t *)(s->ufshc_regs + REG_UTRLDBR) |= (uint32_t)val;
      process_utp_transfers(s, (uint32_t)val);
      return;
    } else if (offset == REG_UTMRLDBR) {
      *(uint32_t *)(s->ufshc_regs + REG_UTMRLDBR) |= (uint32_t)val;
      process_task_mgmt_transfers(s, (uint32_t)val);
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
    }

    *(uint32_t *)(s->ufshc_regs + offset) = (uint32_t)val;
  }
}

static const MemoryRegionOps qcom_ufshc_ops = {
    .read = qcom_ufshc_read,
    .write = qcom_ufshc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void qcom_ufs_reset(DeviceState *dev) {
  QcomUfsState *s = QCOM_UFSHC(dev);
  memset(s->ufshc_regs, 0, sizeof(s->ufshc_regs));
  memset(s->ufsphy_regs, 0, sizeof(s->ufsphy_regs));
  memset(s->ufsice_regs, 0, sizeof(s->ufsice_regs));

  *(uint32_t *)(s->ufshc_regs + REG_CAPABILITIES) = 0x0187001FU;
  *(uint32_t *)(s->ufshc_regs + REG_UFS_VERSION) = 0x00000210U;
  *(uint32_t *)(s->ufshc_regs + REG_HOST_CONTROLLER_STATUS) = 0x0000000FU;
  *(uint32_t *)(s->ufshc_regs + REG_HOST_CONTROLLER_ENABLE) = 0x00000001U;

  s->pa_avail_tx_lanes = 1;
  s->pa_avail_rx_lanes = 1;
  s->pa_active_tx_lanes = 1;
  s->pa_active_rx_lanes = 1;
  s->pa_connected_tx_lanes = 1;
  s->pa_connected_rx_lanes = 1;
  s->pa_tx_gear = 1;
  s->pa_rx_gear = 1;
  s->pa_hs_series = 2;

  s->flag_fDeviceInit = 0;
  s->attr_bBootLunEn = 1;
  s->attr_bCurrentPowerMode = 0x11;
}

static uint64_t qcom_dummy_read(void *opaque, hwaddr offset, unsigned size) {
  return 0;
}

static void qcom_dummy_write(void *opaque, hwaddr offset, uint64_t value, unsigned size) {
}

static const MemoryRegionOps qcom_ufsphy_ops = {
    .read = qcom_dummy_read,
    .write = qcom_dummy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static const MemoryRegionOps qcom_ufsice_ops = {
    .read = qcom_dummy_read,
    .write = qcom_dummy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

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
