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
#include <asm/setup.h>
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


static int sa8155p_specific_mapping(struct domain *d)
{
    const struct dt_device_node *node;
    const __be32 *val;
    u32 len;
    bool own_device;
    const struct dt_device_match pdc_dt_int_ctrl_match[] =
    {
        DT_MATCH_COMPATIBLE("qcom,pdc"),
        { /*sentinel*/ },
    };

    /* Map PDC interrupts to Dom0 */
    node = dt_find_interrupt_controller(pdc_dt_int_ctrl_match);
    if ( !node )
        return 0;

    own_device = !dt_device_for_passthrough(node);

    val = dt_get_property(node, "qcom,pdc-ranges", &len);
    if ( !val )
    {
        printk(XENLOG_G_ERR"Can't find 'qcom,pdc-ranges' property for PDC\n");
        return -EINVAL;
    }

    if ( len % (3 * sizeof(u32)) )
    {
        printk(XENLOG_G_ERR"Invalid number of entries for 'qcom,pdc-ranges'\n");
        return -EINVAL;
    }

    for ( ; len > 0; len -= 3 * sizeof(u32) )
    {
        u32 spi, count, i;
        int ret;

        printk("<%d, %d, %d>\n",__be32_to_cpup(val),
               __be32_to_cpup(val+1),
               __be32_to_cpup(val+2));
        /* Skip pin base */
        val++;

        spi = __be32_to_cpup(val++);
        count = __be32_to_cpup(val++);

        for ( i = 0; i < count; i++, spi++)
        {
            /* irq_set_spi_type(spi + 32, IRQ_TYPE_EDGE_FALLING); */
            ret = map_irq_to_domain(d, spi + 32, own_device, "qcom,pdc");
            if ( ret ) {
                printk(XENLOG_G_ERR"failed to map PDC SPI %d to guest\n", spi);
                return ret;
            }
        }
    }

    return 0;
}

PLATFORM_START(sa8155p, "Qualcomm SA8155P")
    .compatible = sa8155p_dt_compat,
    .smc = sa8155p_smc,
    .specific_mapping = sa8155p_specific_mapping
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
