/*
 * Qualcomm SCM (Secure Channel Manager) TrustZone Emulation
 * Qualcomm Engine (QE) Architecture
 *
 * Implements:
 * - SCM_SVC_INFO (0x06): IS_CALL_AVAILABLE capability queries
 * - SCM_SVC_DCVS (0x0d): TZ_INIT frequency ladder contract
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "exec/address-spaces.h"
#include "qcom_scm.h"

#define SCM_SVC_INFO            0x06
#define SCM_SVC_DCVS            0x0d

#define SCM_CMD_IS_CALL_AVAIL   0x01
#define SCM_CMD_DCVS_TZ_INIT    0x0b

bool qcom_scm_is_call(uint64_t smc_id)
{
    return ((smc_id & 0x02000000) == 0x02000000 ||
            (smc_id & 0x82000000) == 0x82000000 ||
            (smc_id & 0xc2000000) == 0xc2000000 ||
            (smc_id & 0x32000000) == 0x32000000 ||
            (smc_id & 0x42000000) == 0x42000000);
}

static void set_return_regs(CPUARMState *env, uint64_t r0, uint64_t r1, uint64_t r2, uint64_t r3)
{
    if (is_a64(env)) {
        env->xregs[0] = r0;
        env->xregs[1] = r1;
        env->xregs[2] = r2;
        env->xregs[3] = r3;
    } else {
        env->regs[0] = r0;
        env->regs[1] = r1;
        env->regs[2] = r2;
        env->regs[3] = r3;
    }
}

static bool handle_scm_info(CPUARMState *env, const uint64_t *param)
{
    uint32_t cmd = param[0] & 0xff;

    if (cmd == SCM_CMD_IS_CALL_AVAIL) {
        uint64_t target_fn = param[2];
        uint32_t target_svc = (target_fn >> 8) & 0xff;
        uint32_t target_cmd = target_fn & 0xff;
        bool avail = (target_svc == SCM_SVC_DCVS && (target_cmd == 7 || target_cmd == 8 || target_cmd == 9)) ||
                     (target_svc == SCM_SVC_INFO && target_cmd == SCM_CMD_IS_CALL_AVAIL);

        fprintf(stderr, "[QE-SCM] IS_CALL_AVAIL: query=0x%" PRIx64 " (svc=0x%02x cmd=0x%02x) -> %s\n",
                target_fn, target_svc, target_cmd, avail ? "AVAILABLE (ret=1)" : "UNAVAILABLE (ret=0)");

        set_return_regs(env, 0, avail ? 1 : 0, 0, 0);
        return true;
    }

    return false;
}

static bool handle_scm_dcvs(CPUARMState *env, const uint64_t *param)
{
    uint32_t cmd = param[0] & 0xff;

    if (cmd == SCM_CMD_DCVS_TZ_INIT) {
        uint64_t arginfo = param[1];
        hwaddr dma_addr = param[2];
        uint64_t len = param[3];
        uint8_t buf[44];
        uint32_t num_levels;
        uint32_t freqs[6];
        static const uint32_t expected_freqs[6] = {
            845000000, 745000000, 700000000, 550000000, 435000000, 290000000
        };
        int i;
        bool valid = true;

        fprintf(stderr, "[QE-SCM] SCM_SVC_DCVS: TZ_INIT_ID (0x%" PRIx64 ") received\n", param[0]);
        fprintf(stderr, "[QE-SCM]   arginfo=0x%" PRIx64 " dma_addr=0x%" PRIx64 " len=0x%" PRIx64 "\n",
                arginfo, (uint64_t)dma_addr, len);

        if (arginfo != 0x22) {
            fprintf(stderr, "[QE-SCM]   ERROR: invalid arginfo (0x%" PRIx64 " != 0x22)\n", arginfo);
            valid = false;
        }

        if (len != 0x2c) {
            fprintf(stderr, "[QE-SCM]   ERROR: invalid payload len (0x%" PRIx64 " != 0x2c)\n", len);
            valid = false;
        }

        if (!valid) {
            set_return_regs(env, (uint64_t)-1, 0, 0, 0);
            return true;
        }

        address_space_read(&address_space_memory, dma_addr, MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf));

        num_levels = ldl_le_p(buf);
        if (num_levels != 6) {
            fprintf(stderr, "[QE-SCM]   ERROR: invalid num_levels (%u != 6)\n", num_levels);
            set_return_regs(env, (uint64_t)-1, 0, 0, 0);
            return true;
        }

        fprintf(stderr, "[QE-SCM]   Validated num_levels = 6\n");
        for (i = 0; i < 6; i++) {
            freqs[i] = ldl_le_p(buf + 4 + i * 4);
            fprintf(stderr, "[QE-SCM]     level[%d]: %u Hz (expected %u Hz)%s\n",
                    i, freqs[i], expected_freqs[i],
                    freqs[i] == expected_freqs[i] ? " OK" : " MISMATCH");
            if (freqs[i] != expected_freqs[i]) {
                valid = false;
            }
        }

        if (!valid) {
            fprintf(stderr, "[QE-SCM]   ERROR: frequency table mismatch!\n");
            set_return_regs(env, (uint64_t)-1, 0, 0, 0);
            return true;
        }

        fprintf(stderr, "[QE-SCM] >>> DCVS TZ_INIT contract satisfied! Returning X0=0, X1=0 <<<\n");
        set_return_regs(env, 0, 0, 0, 0);
        return true;
    }

    return false;
}

bool qcom_scm_handle_call(CPUARMState *env, const uint64_t *param)
{
    uint32_t svc = (param[0] >> 8) & 0xff;

    switch (svc) {
    case SCM_SVC_INFO:
        if (handle_scm_info(env, param)) {
            return true;
        }
        break;
    case SCM_SVC_DCVS:
        if (handle_scm_dcvs(env, param)) {
            return true;
        }
        break;
    default:
        fprintf(stderr, "[QE-SCM] Unhandled Qualcomm SMC: 0x%" PRIx64 " (svc=0x%02x cmd=0x%02x)\n",
                param[0], svc, (uint32_t)(param[0] & 0xff));
        break;
    }

    /* Fallback default for known-safe unhandled Qualcomm SMCs */
    set_return_regs(env, 0, 0, 0, 0);
    return true;
}
