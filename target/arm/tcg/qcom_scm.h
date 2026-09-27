/*
 * Qualcomm SCM (Secure Channel Manager) TrustZone Emulation
 * Qualcomm Engine (QE) Architecture
 */

#ifndef TARGET_ARM_TCG_QCOM_SCM_H
#define TARGET_ARM_TCG_QCOM_SCM_H

#include "qemu/osdep.h"
#include "cpu.h"

bool qcom_scm_is_call(uint64_t smc_id);
bool qcom_scm_handle_call(CPUARMState *env, const uint64_t *param);

#endif /* TARGET_ARM_TCG_QCOM_SCM_H */
