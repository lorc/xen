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

#define SSC_SMCCC_UID ARM_SMCCC_UID(0xf863386f, 0x4b39, 0x4cbd, \
                                    0x92, 0x20, 0xce, 0x16, \
                                    0x41, 0xe5, 0x9f, 0x6f)

#define SSC_SMCCC_MAJOR_REVISION 0
#define SSC_SMCCC_MINOR_REVISION 1
#define SSC_SMCCC_FUNCTION_COUNT 13

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

/* old (arvm7) PSCI interface */
static bool handle_arch(struct cpu_user_regs *regs, const union hsr hsr)
{
    switch( get_user_reg(regs,0) & 0xFFFFFFFF )
    {
    case PSCI_cpu_off:
    {
        uint32_t pstate = get_user_reg(regs, 1);
        perfc_incr(vpsci_cpu_off);
        set_user_reg(regs, 0, do_psci_cpu_off(pstate));
    }
    return true;
    case PSCI_cpu_on:
    {
        uint32_t vcpuid = get_user_reg(regs, 1);
        register_t epoint = get_user_reg(regs, 2);
        perfc_incr(vpsci_cpu_on);
        set_user_reg(regs, 0, do_psci_cpu_on(vcpuid, epoint));
    }
    return true;
    }
    return false;
}

/* helper function for checking arm mode 32/64 bit */
static inline int psci_mode_check(struct domain *d, register_t fid)
{
        return !( is_64bit_domain(d)^( (fid & PSCI_0_2_64BIT) >> 30 ) );
}

/* PSCI 2.0 interface */
static bool handle_ssc(struct cpu_user_regs *regs, const union hsr hsr)
{
    register_t fid = get_user_reg(regs, 0);

    switch( ARM_SMCCC_FUNC_NUM(fid) )
    {
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_PSCI_VERSION):
        perfc_incr(vpsci_version);
        set_user_reg(regs, 0, do_psci_0_2_version());
        return true;
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_CPU_OFF):
        perfc_incr(vpsci_cpu_off);
        set_user_reg(regs, 0, do_psci_0_2_cpu_off());
        return true;
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_MIGRATE_INFO_TYPE):
        perfc_incr(vpsci_migrate_info_type);
        set_user_reg(regs, 0, do_psci_0_2_migrate_info_type());
        return true;
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_MIGRATE_INFO_UP_CPU):
        perfc_incr(vpsci_migrate_info_up_cpu);
        if ( psci_mode_check(current->domain, fid) )
            set_user_reg(regs, 0, do_psci_0_2_migrate_info_up_cpu());
        return true;
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_SYSTEM_OFF):
        perfc_incr(vpsci_system_off);
        do_psci_0_2_system_off();
        set_user_reg(regs, 0, PSCI_INTERNAL_FAILURE);
        return true;
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_SYSTEM_RESET):
        perfc_incr(vpsci_system_reset);
        do_psci_0_2_system_reset();
        set_user_reg(regs, 0, PSCI_INTERNAL_FAILURE);
        return true;
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_CPU_ON):
        perfc_incr(vpsci_cpu_on);
        if ( psci_mode_check(current->domain, fid) )
        {
            register_t vcpuid = get_user_reg(regs,1);
            register_t epoint = get_user_reg(regs,2);
            register_t cid = get_user_reg(regs,3);
            set_user_reg(regs, 0,
                         do_psci_0_2_cpu_on(vcpuid, epoint, cid));
        }
        return true;
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_CPU_SUSPEND):
        perfc_incr(vpsci_cpu_suspend);
        if ( psci_mode_check(current->domain, fid) )
        {
            uint32_t pstate = get_user_reg(regs,1) & 0xFFFFFFFF;
            register_t epoint = get_user_reg(regs,2);
            register_t cid = get_user_reg(regs,3);
            set_user_reg(regs, 0,
                         do_psci_0_2_cpu_suspend(pstate, epoint, cid));
        }
        return true;
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_AFFINITY_INFO):
        perfc_incr(vpsci_cpu_affinity_info);
        if ( psci_mode_check(current->domain, fid) )
        {
            register_t taff = get_user_reg(regs,1);
            uint32_t laff = get_user_reg(regs,2) & 0xFFFFFFFF;
            set_user_reg(regs, 0,
                         do_psci_0_2_affinity_info(taff, laff));
        }
        return true;
    case ARM_SMCCC_FUNC_NUM(PSCI_0_2_FN_MIGRATE):
        perfc_incr(vpsci_cpu_migrate);
        if ( psci_mode_check(current->domain, fid) )
        {
            uint32_t tcpu = get_user_reg(regs,1) & 0xFFFFFFFF;
            set_user_reg(regs, 0, do_psci_0_2_migrate(tcpu));
        }
        return true;
    case ARM_SMCCC_FUNC_CALL_COUNT:
        set_user_reg(regs, 0, SSC_SMCCC_FUNCTION_COUNT);
        return true;
    case ARM_SMCCC_FUNC_CALL_UID:
        set_user_reg(regs, 0, SSC_SMCCC_UID.a[0]);
        set_user_reg(regs, 1, SSC_SMCCC_UID.a[1]);
        set_user_reg(regs, 2, SSC_SMCCC_UID.a[2]);
        set_user_reg(regs, 3, SSC_SMCCC_UID.a[3]);
        return true;
    case ARM_SMCCC_FUNC_CALL_REVISION:
        set_user_reg(regs, 0, SSC_SMCCC_MAJOR_REVISION);
        set_user_reg(regs, 1, SSC_SMCCC_MINOR_REVISION);
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
    case ARM_SMCCC_OWNER_ARCH:
        handled = handle_arch(regs, hsr);
        break;
    case ARM_SMCCC_OWNER_STANDARD:
        handled = handle_ssc(regs, hsr);
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
