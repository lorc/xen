/*
 * xen/arch/vscpi.c
 *
 * Virtual SCPI handler
 *
 * Volodymyr Babchuk <volodymyr_babchuk@epam.com>
 * Copyright (c) 2019 EPAM Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/sched.h>
#include <asm/vscpi.h>
#include <asm/guest_access.h>
#include "cpufreq/scpi_protocol.h"

uint32_t cmd_capabilities(void *arg)
{
    struct scp_capabilities *caps = arg;

    memset(caps, 0, sizeof(*caps));

    caps->protocol_version = (1 << PROTOCOL_REV_MINOR_BITS) | (0);
    caps->event_version = 1 << 16;
    caps->platform_version = ( 1 << FW_REV_MAJOR_BITS) | \
        (0 << FW_REV_MINOR_BITS) | (0);

    caps->commands[0] = BIT(SCPI_CMD_SCPI_CAPABILITIES);

    return PACK_SCPI_CMD(SCPI_SUCCESS, sizeof(*caps));
}

bool vscpi_handle_call(struct cpu_user_regs *regs)
{
    uint64_t cmd;
    uint64_t ret;
    void *arg;
    void *data;
    int res;

    if ( !current->domain->arch.scpi_base_pg )
    {
        printk(XENLOG_ERR "No SCPI mailbox for domain\n");
        return false;
    }

    data = xzalloc_array(char, 256);
    if ( !data )
    {
        printk(XENLOG_ERR "Could not allocate buffer for mbox\n");
        return false;
    }

    flush_page_to_ram(page_to_mfn(current->domain->arch.scpi_base_pg), false);
    res = access_guest_memory_by_ipa(current->domain,
                                     current->domain->arch.scpi_base_ipa + 256,
                                     data, 256, false);
    if ( res )
    {
        printk(XENLOG_ERR "Error reading guest memory %d\n", res);
        goto err;
    }

    cmd = *(uint64_t*)data;
    arg = (uint64_t*)data + 1;

    switch ( CMD_ID(cmd) )
    {
    case SCPI_CMD_SCPI_CAPABILITIES:
        ret = cmd_capabilities(current->domain->arch.scpi_base);
        break;
    default:
        printk(XENLOG_ERR "Unknown SCPI command %x\n",
                 (uint32_t)CMD_ID(cmd));
        ret = SCPI_ERR_SUPPORT;
    }

    *(uint64_t*)data = ret | CMD_XTRACT_UNIQ(cmd);

    res =  access_guest_memory_by_ipa(current->domain,
                                      current->domain->arch.scpi_base_ipa,
                                      data, 256, true);

    flush_page_to_ram(page_to_mfn(current->domain->arch.scpi_base_pg), false);
    if ( res )
    {
        printk(XENLOG_ERR "Error writing guest memory %d\n", res);
        goto err;
    }

    set_user_reg(regs, 0, ret | CMD_XTRACT_UNIQ(cmd));

    xfree(data);
    return true;

err:
    xfree(data);
    return false;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
