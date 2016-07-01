/*
 * xen/arch/arm/optee/smc.c
 *
 * OPTEE SMC calls proxy/handler
 *
 * Volodymyr Babchuk <volodymyr.babchuk@globallogic.com>
 * Copyright (c) 2016 GlobalLogic Inc.
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

#include <asm/p2m.h>
#include <asm/system.h>
#include <asm/processor.h>
#include <asm/current.h>

#include <xen/sched.h>
#include <asm/gic.h>
#include <asm/event.h>
#include <public/xen.h>

#include "optee_smc.h"

static void execute_smc(struct cpu_user_regs *regs)
{
	register_t retval[4];

	call_smc_ext(regs->x0,
	             regs->x1,
	             regs->x2,
	             regs->x3,
	             regs->x4,
	             regs->x5,
	             regs->x6,
	             current->domain->domain_id,
	             retval);
	regs->x0 = retval[0];
	regs->x1 = retval[1];
	regs->x2 = retval[2];
	regs->x3 = retval[3];
}

static void do_process_get_shm_config(struct cpu_user_regs *regs)
{
	int ret;
	execute_smc(regs);
	ret =  guest_physmap_add_entry(current->domain, regs->x1 >> PAGE_SHIFT,
	                               regs->x1 >> PAGE_SHIFT,
	                               ffsl(regs->x2) - PAGE_SHIFT, p2m_ram_rw);
}

int optee_handle_smc(struct cpu_user_regs *regs)
{
	uint32_t smc_code = regs->r0;
	switch(smc_code){
	case OPTEE_SMC_GET_SHM_CONFIG:
		do_process_get_shm_config(regs);
		break;
	default:
		/* Just forward request to OPTEE */
		execute_smc(regs);
		break;
	}
	return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
