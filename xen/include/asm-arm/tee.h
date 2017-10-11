/*
 * xen/include/asm-arm/tee.h
 *
 * Generic part of TEE mediator subsystem
 *
 * Volodymyr Babchuk <volodymyr_babchuk@epam.com>
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

#ifndef __ARCH_ARM_TEE_TEE_H__
#define __ARCH_ARM_TEE_TEE_H__

#include <xen/lib.h>
#include <xen/types.h>
#include <asm/regs.h>

#ifdef CONFIG_ARM_TEE

struct tee_mediator_ops {
    int (*init)(void);
    void (*domain_create)(struct domain *d);
    void (*domain_destroy)(struct domain *d);
    bool (*handle_smc)(struct cpu_user_regs *regs);
    void (*remove)(void);
};

struct tee_mediator_desc {
    const char *name;
    const xen_uuid_t uid;
    const struct tee_mediator_ops *ops;
};

void tee_init(void);
bool tee_handle_smc(struct cpu_user_regs *regs);
void tee_domain_create(struct domain *d);
void tee_domain_destroy(struct domain *d);
void tee_remove(void);

#define REGISTER_TEE_MEDIATOR(_name, _namestr, _uid, _ops)          \
static const struct tee_mediator_desc __tee_desc_##_name __used     \
__section(".teemediator.info") = {                                  \
    .name = _namestr,                                               \
    .uid = _uid,                                                    \
    .ops = _ops                                                     \
}

#else

static inline void tee_init(void) {}
static inline bool tee_handle_smc(struct cpu_user_regs *regs)
{
    return false;
}
static inline void tee_domain_create(struct domain *d) {}
static inline tee_domain_destroy(struct domain *d) {}
static inline tee_remove(void) {}

#endif  /* CONFIG_ARM_TEE */

#endif /* __ARCH_ARM_TEE_TEE_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
