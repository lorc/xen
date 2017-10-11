/*
 * xen/arch/arm/tee/tee.c
 *
 * Generic part of TEE mediator subsystem
 *
 * Volodymyr Babchuk <volodymyr_babchuk@epam.com>
 * Copyright (c) 2017 EPAM Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/types.h>
#include <asm/smccc.h>
#include <asm/tee.h>

/*
 * According to ARM SMCCC (ARM DEN 0028B, page 17), service owner
 * for generic TEE queries is 63.
 */
extern const struct tee_mediator_desc _steemediator[], _eteemediator[];
static const struct tee_mediator_ops *mediator_ops;

/* Helper function to read UID returned by SMC */
static void parse_uid(const register_t regs[4], xen_uuid_t *uid)
{
    uint8_t *bytes = uid->a;
    int n;

    /*
     * UID is returned in registers r0..r3, four bytes per register,
     * first byte is stored in low-order bits of a register.
     * (ARM DEN 0028B page 14)
     */
    for (n = 0; n < 16; n++)
        bytes[n] = (uint8_t)(regs[n/4] >> ((n & 3) * 8));

}

void tee_init(void)
{
    const struct tee_mediator_desc *desc;
    register_t resp[4];
    xen_uuid_t tee_uid;
    int ret;

    /* Read UUID to determine which TEE is running */
    call_smccc_smc(ARM_SMCCC_CALL_UID_FID(TRUSTED_OS_END), 0, 0, 0, 0, 0, 0, 0, resp);
    if ( resp[0] == 0xFFFFFFFF ) {
        printk(XENLOG_INFO "No TEE found\n");
        return;
    }

    parse_uid(resp, &tee_uid);

    printk(XENLOG_INFO "TEE UID: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
           tee_uid.a[0 ], tee_uid.a[1 ], tee_uid.a[2 ], tee_uid.a[3 ],
           tee_uid.a[4 ], tee_uid.a[5 ], tee_uid.a[6 ], tee_uid.a[7 ],
           tee_uid.a[8 ], tee_uid.a[9 ], tee_uid.a[10], tee_uid.a[11],
           tee_uid.a[12], tee_uid.a[13], tee_uid.a[14], tee_uid.a[15]);

    for ( desc = _steemediator; desc != _eteemediator; desc++ )
        if ( memcmp(&desc->uid, &tee_uid, sizeof(xen_uuid_t)) == 0 )
        {
            printk(XENLOG_INFO "Using TEE mediator for %s\n", desc->name);
            mediator_ops = desc->ops;
            break;
        }

    if ( !mediator_ops )
        return;

    ret = mediator_ops->init();
    if ( ret )
    {
        printk(XENLOG_INFO "TEE mediator failed to initialize :%d\n", ret);
        mediator_ops = NULL;
    }
}

bool tee_handle_smc(struct cpu_user_regs *regs)
{
    if ( !mediator_ops )
        return false;

    return mediator_ops->handle_smc(regs);
}

void tee_domain_create(struct domain *d)
{
    if ( !mediator_ops )
        return;

    return mediator_ops->domain_create(d);
}

void tee_domain_destroy(struct domain *d)
{
    if ( !mediator_ops )
        return;

    return mediator_ops->domain_destroy(d);
}

void tee_remove(void)
{
    if ( !mediator_ops )
        return;

    return mediator_ops->remove();
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
