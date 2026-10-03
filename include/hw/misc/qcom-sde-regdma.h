/*
 * Qualcomm Snapdragon Display Engine (SDE) REGDMA interface.
 *
 * Copyright (c) 2026 CustomAndroidEmulator project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_QCOM_SDE_REGDMA_H
#define HW_MISC_QCOM_SDE_REGDMA_H

#include <stdint.h>

void qcom_sde_regdma_trigger(uint32_t ctl_idx, uint32_t queue_idx);

#endif /* HW_MISC_QCOM_SDE_REGDMA_H */
