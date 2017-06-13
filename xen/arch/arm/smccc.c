/*
 * xen/arch/arm/smccc.c
 *
 * Generic handler for SMC and HVC calls according to
 * ARM SMC callling convention
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


#include <xen/config.h>
#include <xen/lib.h>
#include <xen/perfc.h>
/* Need to include xen/sched.h before asm/domain.h or it breaks build*/
#include <xen/sched.h>
#include <xen/stdbool.h>
#include <xen/types.h>
#include <asm/domain.h>
#include <asm/psci.h>
#include <asm/smccc.h>
#include <asm/regs.h>

#define XEN_SMCCC_UID ARM_SMCCC_UID(0xa71812dc, 0xc698, 0x4369, \
                                    0x9a, 0xcf, 0x79, 0xd1, \
                                    0x8d, 0xde, 0xe6, 0x67)

/*
 * We can't use XEN version here:
 * Major revision should change every time SMC/HVC function is removed.
 * Minor revision should change every time SMC/HVC function is added.
 * So, it is SMCCC protocol revision code, not XEN version
 */
#define XEN_SMCCC_MAJOR_REVISION 0
#define XEN_SMCCC_MINOR_REVISION 1
#define XEN_SMCCC_FUNCTION_COUNT 3

/* SMCCC interface for hypervisor. Tell about self */
static bool handle_hypervisor(struct cpu_user_regs *regs, const union hsr hsr)
{
    switch(ARM_SMCCC_FUNC_NUM(get_user_reg(regs, 0)))
    {
    case ARM_SMCCC_FUNC_CALL_COUNT:
        set_user_reg(regs, 0, XEN_SMCCC_FUNCTION_COUNT);
        return true;
    case ARM_SMCCC_FUNC_CALL_UID:
        set_user_reg(regs, 0, XEN_SMCCC_UID.a[0]);
        set_user_reg(regs, 1, XEN_SMCCC_UID.a[1]);
        set_user_reg(regs, 2, XEN_SMCCC_UID.a[2]);
        set_user_reg(regs, 3, XEN_SMCCC_UID.a[3]);
        return true;
    case ARM_SMCCC_FUNC_CALL_REVISION:
        set_user_reg(regs, 0, XEN_SMCCC_MAJOR_REVISION);
        set_user_reg(regs, 1, XEN_SMCCC_MINOR_REVISION);
        return true;
    }
    return false;
}

/**
 * smccc_handle_call() - handle SMC/HVC call according to ARM SMCCC
 */
void smccc_handle_call(struct cpu_user_regs *regs, const union hsr hsr)
{
    bool handled = false;
    switch( ARM_SMCCC_OWNER_NUM(get_user_reg(regs, 0)) )
    {
    case ARM_SMCCC_OWNER_HYPERVISOR:
        handled = handle_hypervisor(regs, hsr);
        break;
    }

    if ( !handled )
    {
        printk("Uhandled SMC/HVC: %08"PRIregister"\n", get_user_reg(regs, 0));
        /* Inform caller that function is not supported */
        set_user_reg(regs, 0, ARM_SMCCC_ERR_UNKNOWN_FUNCTION);
    }
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
