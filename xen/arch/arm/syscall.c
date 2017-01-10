/*
 * xen/arch/arm/syscall.c
 *
 * XEN App syscall handlers
 *
 * Copyright (c) 2017 EPAM Systems.
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
#include <xen/init.h>
#include <xen/string.h>
#include <xen/version.h>
#include <xen/smp.h>
#include <xen/symbols.h>
#include <xen/irq.h>
#include <xen/lib.h>
#include <xen/livepatch.h>
#include <xen/mm.h>
#include <xen/errno.h>
#include <xen/syscall.h>
#include <xen/softirq.h>
#include <xen/domain_page.h>
#include <xen/perfc.h>
#include <xen/virtual_region.h>
#include <xen/guest_access.h>
#include <public/sched.h>
#include <public/xen.h>
#include <asm/debugger.h>
#include <asm/event.h>
#include <asm/regs.h>
#include <asm/cpregs.h>
#include <asm/psci.h>
#include <asm/mmio.h>
#include <asm/cpufeature.h>
#include <asm/flushtlb.h>
#include <asm/monitor.h>
#include <asm/gic.h>
#include <asm/vgic.h>
#include <asm/cpuerrata.h>


void return_from_el0_app(struct vcpu *v, unsigned long code);

long do_app_exit(uint32_t exit_code)
{
    return_from_el0_app(current, exit_code);
    return 0;
}

long do_app_console(XEN_GUEST_HANDLE_PARAM(char) ptr, size_t size)
{
    char buf[128] = {0};
    if (size > sizeof(buf) - 1)
        size = sizeof(buf) - 1;
    copy_from_guest(buf, ptr, size);
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
