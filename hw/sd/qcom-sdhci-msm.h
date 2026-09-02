/*
 * Qualcomm SDHCI MSM (eMMC 5.1) Host Controller Emulation
 *
 * Copyright (c) 2026 Antigravity
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef HW_SD_QCOM_SDHCI_MSM_H
#define HW_SD_QCOM_SDHCI_MSM_H

#include "hw/sysbus.h"
#include "hw/sd/sdhci.h"
#include "qom/object.h"

#define TYPE_QCOM_SDHCI_MSM "qcom-sdhci-msm"
OBJECT_DECLARE_SIMPLE_TYPE(QComSDHCIState, QCOM_SDHCI_MSM)

#define QCOM_SDHCI_VENDOR_REGS_SIZE 0x400

struct QComSDHCIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    SDHCIState sdhci;
    BusState *bus;

    MemoryRegion hc_mem;
    MemoryRegion cmdq_mem;
    MemoryRegion cmdq_ice;

    qemu_irq hc_irq;
    qemu_irq pwr_irq;

    uint32_t vendor_regs[QCOM_SDHCI_VENDOR_REGS_SIZE / 4];
    uint32_t cmdq_regs[0x100 / 4];
};

#endif /* HW_SD_QCOM_SDHCI_MSM_H */
