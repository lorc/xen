/*
 * xen/arch/arm/platforms/qcom.c
 *
 * Qualcomm SoCs specific code
 *
 * Volodymyr Babchuk <volodymyr_babchuk@epam.com>
 *
 * Copyright (c) 2024 EPAM Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/platform.h>
#include <public/arch-arm/smccc.h>
#include <asm/smccc.h>

#define SCM_SMC_FNID(s, c)	((((s) & 0xFF) << 8) | ((c) & 0xFF))
#define QCOM_SCM_SVC_INFO		0x06
#define QCOM_SCM_INFO_IS_CALL_AVAIL	0x01

#define ARM_SMCCC_SIP_QCOM_SCM_IS_CALL_AVAIL                            \
    ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL,                             \
                       ARM_SMCCC_CONV_64,                               \
                       ARM_SMCCC_OWNER_SIP,                             \
                       SCM_SMC_FNID(QCOM_SCM_SVC_INFO,                  \
                                    QCOM_SCM_INFO_IS_CALL_AVAIL))

static const char * const sa8155p_dt_compat[] __initconst =
{
    "qcom,sa8155p",
    NULL
};

static bool sa8155p_smc(struct cpu_user_regs *regs)
{
    uint32_t funcid = get_user_reg(regs, 0);

    switch ( funcid ) {
    case ARM_SMCCC_SIP_QCOM_SCM_IS_CALL_AVAIL:
        /*
         * We need to implement this specific call only to make Linux
         * counterpart happy: QCOM SCM driver in Linux tries to
         * determine calling convention by issuing this particular
         * SMC. If it receives an error it assumes that platform uses
         * legacy calling convention and tries to issue SMC with
         * funcid = 1. Xen interprets this as PSCI_cpu_off and turns
         * off Linux boot vCPU.
         */
        set_user_reg(regs, 0, ARM_SMCCC_SUCCESS);
        set_user_reg(regs, 1, 1);
        return true;
    default:
        return false;
    }
}

PLATFORM_START(sa8155p, "Qualcomm SA8155P")
    .compatible = sa8155p_dt_compat,
    .smc = sa8155p_smc
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
