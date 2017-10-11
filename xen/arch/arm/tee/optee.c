/*
 * xen/arch/arm/tee/optee.c
 *
 * OP-TEE mediator
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

#include <xen/domain_page.h>
#include <xen/types.h>
#include <xen/sched.h>

#include <asm/p2m.h>
#include <asm/tee.h>

#include "optee_msg.h"
#include "optee_smc.h"

/*
 * Global TODO:
 *  1. Create per-domain context, where call and shm will be stored
 *  2. Pin pages shared between OP-TEE and guest
 */
/*
 * OP-TEE violates SMCCC when it defines own UID. So we need
 * to place bytes in correct order.
 */
#define OPTEE_UID  (xen_uuid_t){{                                               \
    (uint8_t)(OPTEE_MSG_UID_0 >>  0), (uint8_t)(OPTEE_MSG_UID_0 >>  8),         \
    (uint8_t)(OPTEE_MSG_UID_0 >> 16), (uint8_t)(OPTEE_MSG_UID_0 >> 24),         \
    (uint8_t)(OPTEE_MSG_UID_1 >>  0), (uint8_t)(OPTEE_MSG_UID_1 >>  8),         \
    (uint8_t)(OPTEE_MSG_UID_1 >> 16), (uint8_t)(OPTEE_MSG_UID_1 >> 24),         \
    (uint8_t)(OPTEE_MSG_UID_2 >>  0), (uint8_t)(OPTEE_MSG_UID_2 >>  8),         \
    (uint8_t)(OPTEE_MSG_UID_2 >> 16), (uint8_t)(OPTEE_MSG_UID_2 >> 24),         \
    (uint8_t)(OPTEE_MSG_UID_3 >>  0), (uint8_t)(OPTEE_MSG_UID_3 >>  8),         \
    (uint8_t)(OPTEE_MSG_UID_3 >> 16), (uint8_t)(OPTEE_MSG_UID_3 >> 24),         \
    }}

#define MAX_NONCONTIG_ENTRIES   8

/*
 * Call context. OP-TEE can issue mulitple RPC returns during one call.
 * We need to preserve context during them.
 */
struct std_call_ctx {
    struct list_head list;
    struct optee_msg_arg *guest_arg;
    struct optee_msg_arg *xen_arg;
    void *non_contig[MAX_NONCONTIG_ENTRIES];
    int non_contig_order[MAX_NONCONTIG_ENTRIES];
    int optee_thread_id;
    int rpc_op;
    domid_t domid;
};
static LIST_HEAD(call_ctx_list);
static DEFINE_SPINLOCK(call_ctx_list_lock);

/*
 * Command buffer shared between OP-TEE and guest.
 * Warning! In the proper implementation this SHM buffer *probably* should
 * by shadowed by XEN.
 * TODO: Reconsider this.
 */
struct shm {
    struct list_head list;
    struct optee_msg_arg *guest_arg;
    struct page *guest_page;
    mfn_t guest_mfn;
    uint64_t cookie;
    domid_t domid;
};

static LIST_HEAD(shm_list);
static DEFINE_SPINLOCK(shm_list_lock);

static int optee_init(void)
{
    printk("OP-TEE mediator init done\n");
    return 0;
}

static void optee_domain_create(struct domain *d)
{
    register_t resp[4];
    call_smccc_smc(OPTEE_SMC_VM_CREATED,
                   d->domain_id + 1, 0, 0, 0, 0, 0, 0, resp);
    if ( resp[0] != OPTEE_SMC_RETURN_OK )
        gprintk(XENLOG_WARNING, "OP-TEE don't want to support domain: %d\n",
                (uint32_t)resp[0]);
    /* TODO: Change function declaration to be able to retun error */
}

static void optee_domain_destroy(struct domain *d)
{
    register_t resp[4];
    call_smccc_smc(OPTEE_SMC_VM_DESTROYED,
                   d->domain_id + 1, 0, 0, 0, 0, 0, 0, resp);
    /* TODO: Clean call contexts and SHMs associated with domain */
}

static bool forward_call(struct cpu_user_regs *regs)
{
    register_t resp[4];

    /* TODO: Use separate registers set to prevent leakage to guest */
    call_smccc_smc(get_user_reg(regs, 0),
                   get_user_reg(regs, 1),
                   get_user_reg(regs, 2),
                   get_user_reg(regs, 3),
                   get_user_reg(regs, 4),
                   get_user_reg(regs, 5),
                   get_user_reg(regs, 6),
                   /* VM id 0 is reserved for hypervisor itself */
                   current->domain->domain_id + 1,
                   resp);

    set_user_reg(regs, 0, resp[0]);
    set_user_reg(regs, 1, resp[1]);
    set_user_reg(regs, 2, resp[2]);
    set_user_reg(regs, 3, resp[3]);

    return true;
}

static struct std_call_ctx *allocate_std_call_ctx(void)
{
    struct std_call_ctx *ret;

    ret = xzalloc(struct std_call_ctx);
    if ( !ret )
        return NULL;

    ret->optee_thread_id = -1;
    ret->domid = -1;

    spin_lock(&call_ctx_list_lock);
    list_add_tail(&ret->list, &call_ctx_list);
    spin_unlock(&call_ctx_list_lock);

    return ret;
}

static void free_std_call_ctx(struct std_call_ctx *ctx)
{
    spin_lock(&call_ctx_list_lock);
    list_del(&ctx->list);
    spin_unlock(&call_ctx_list_lock);

    if (ctx->xen_arg)
        free_xenheap_page(ctx->xen_arg);

    if (ctx->guest_arg)
        unmap_domain_page(ctx->guest_arg);

    for (int i = 0; i < MAX_NONCONTIG_ENTRIES; i++) {
        if (ctx->non_contig[i])
            free_xenheap_pages(ctx->non_contig[i], ctx->non_contig_order[i]);
    }

    xfree(ctx);
}

static struct std_call_ctx *find_ctx(int thread_id, domid_t domid)
{
    struct std_call_ctx *ctx;

    spin_lock(&call_ctx_list_lock);
    list_for_each_entry( ctx, &call_ctx_list, list )
    {
        if  (ctx->domid == domid && ctx->optee_thread_id == thread_id )
        {
                spin_unlock(&call_ctx_list_lock);
                return ctx;
        }
    }
    spin_unlock(&call_ctx_list_lock);

    return NULL;
}

#define PAGELIST_ENTRIES_PER_PAGE                       \
    ((OPTEE_MSG_NONCONTIG_PAGE_SIZE / sizeof(u64)) - 1)

static size_t get_pages_list_size(size_t num_entries)
{
    int pages = DIV_ROUND_UP(num_entries, PAGELIST_ENTRIES_PER_PAGE);

    return pages * OPTEE_MSG_NONCONTIG_PAGE_SIZE;
}

static mfn_t lookup_guest_ram_addr(paddr_t gaddr)
{
    mfn_t mfn;
    gfn_t gfn;
    p2m_type_t t;
    gfn = gaddr_to_gfn(gaddr);
    mfn = p2m_lookup(current->domain, gfn, &t);
    if ( t != p2m_ram_rw || mfn_eq(mfn, INVALID_MFN) ) {
        gprintk(XENLOG_INFO, "Domain tries to use invalid gfn\n");
        return INVALID_MFN;
    }
    return mfn;
}

static struct shm *allocate_and_map_shm(paddr_t gaddr, uint64_t cookie)
{
    struct shm *ret;

    ret = xzalloc(struct shm);
    if ( !ret )
        return NULL;

    ret->guest_mfn = lookup_guest_ram_addr(gaddr);

    if ( mfn_eq(ret->guest_mfn, INVALID_MFN) )
    {
        xfree(ret);
        return NULL;
    }

    ret->guest_arg = map_domain_page(ret->guest_mfn);
    if ( !ret->guest_arg )
    {
        gprintk(XENLOG_INFO, "Could not map domain page\n");
        xfree(ret);
        return NULL;
    }
    ret->cookie = cookie;
    ret->domid = current->domain->domain_id;

    spin_lock(&shm_list_lock);
    list_add_tail(&ret->list, &shm_list);
    spin_unlock(&shm_list_lock);
    return ret;
}

static void free_shm(uint64_t cookie, domid_t domid)
{
    struct shm *shm, *found = NULL;
    spin_lock(&shm_list_lock);

    list_for_each_entry( shm, &shm_list, list )
    {
        if  (shm->domid == domid && shm->cookie == cookie )
        {
            found = shm;
            list_del(&found->list);
            break;
        }
    }
    spin_unlock(&shm_list_lock);

    if ( !found ) {
        return;
    }

    if ( found->guest_arg )
        unmap_domain_page(found->guest_arg);

    xfree(found);
}

static struct shm *find_shm(uint64_t cookie, domid_t domid)
{
    struct shm *shm;

    spin_lock(&shm_list_lock);
    list_for_each_entry( shm, &shm_list, list )
    {
        if ( shm->domid == domid && shm->cookie == cookie )
        {
                spin_unlock(&shm_list_lock);
                return shm;
        }
    }
    spin_unlock(&shm_list_lock);

    return NULL;
}

static bool translate_noncontig(struct std_call_ctx *ctx,
                                struct optee_msg_param *param,
                                int idx)
{
    /*
     * Refer to OPTEE_MSG_ATTR_NONCONTIG description in optee_msg.h for details.
     *
     * WARNING: This is test code. It works only with xen page size == 4096
     */
    uint64_t size;
    int page_offset;
    int num_pages;
    int order;
    int entries_on_page = 0;
    paddr_t gaddr;
    mfn_t guest_mfn;
    struct {
        uint64_t pages_list[PAGELIST_ENTRIES_PER_PAGE];
        uint64_t next_page_data;
    } *pages_data_guest, *pages_data_xen, *pages_data_xen_start;

    page_offset = param->u.tmem.buf_ptr & (OPTEE_MSG_NONCONTIG_PAGE_SIZE - 1);

    size = ROUNDUP(param->u.tmem.size + page_offset,
                   OPTEE_MSG_NONCONTIG_PAGE_SIZE);

    num_pages = DIV_ROUND_UP(size, OPTEE_MSG_NONCONTIG_PAGE_SIZE);

    order = get_order_from_bytes(get_pages_list_size(num_pages));

    pages_data_xen_start = alloc_xenheap_pages(order, 0);
    if (!pages_data_xen_start)
        return false;

    gaddr = param->u.tmem.buf_ptr & ~(OPTEE_MSG_NONCONTIG_PAGE_SIZE - 1);
    guest_mfn = lookup_guest_ram_addr(gaddr);
    if ( mfn_eq(guest_mfn, INVALID_MFN) )
        goto err_free;

    pages_data_guest = map_domain_page(guest_mfn);
    if (!pages_data_guest)
        goto err_free;

    pages_data_xen = pages_data_xen_start;
    while ( num_pages ) {
        mfn_t entry_mfn = lookup_guest_ram_addr(
            pages_data_guest->pages_list[entries_on_page]);

        if ( mfn_eq(entry_mfn, INVALID_MFN) )
            goto err_unmap;

        pages_data_xen->pages_list[entries_on_page] = mfn_to_maddr(entry_mfn);
        entries_on_page++;

        if ( entries_on_page == PAGELIST_ENTRIES_PER_PAGE ) {
            pages_data_xen->next_page_data = virt_to_maddr(pages_data_xen + 1);
            pages_data_xen++;
            gaddr = pages_data_guest->next_page_data;
            unmap_domain_page(pages_data_guest);
            guest_mfn = lookup_guest_ram_addr(gaddr);
            if ( mfn_eq(guest_mfn, INVALID_MFN) )
                goto err_free;

            pages_data_guest = map_domain_page(guest_mfn);
            if ( !pages_data_guest )
                goto err_free;
            /* Roll over to the next page */
            entries_on_page = 0;
        }
        num_pages--;
    }

    unmap_domain_page(pages_data_guest);

    param->u.tmem.buf_ptr = virt_to_maddr(pages_data_xen_start) | page_offset;

    ctx->non_contig[idx] = pages_data_xen_start;
    ctx->non_contig_order[idx] = order;

    unmap_domain_page(pages_data_guest);
    return true;

err_unmap:
    unmap_domain_page(pages_data_guest);
err_free:
    free_xenheap_pages(pages_data_xen_start, order);
    return false;
}

static bool translate_params(struct std_call_ctx *ctx)
{
    unsigned int i;
    uint32_t attr;

    for ( i = 0; i < ctx->xen_arg->num_params; i++ ) {
        attr = ctx->xen_arg->params[i].attr;

        switch ( attr & OPTEE_MSG_ATTR_TYPE_MASK ) {
        case OPTEE_MSG_ATTR_TYPE_TMEM_INPUT:
        case OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT:
        case OPTEE_MSG_ATTR_TYPE_TMEM_INOUT:
            if ( attr & OPTEE_MSG_ATTR_NONCONTIG ) {
                if ( !translate_noncontig(ctx, ctx->xen_arg->params + i, i) )
                    return false;
            }
            else {
                gprintk(XENLOG_WARNING, "Guest tries to use old tmem arg\n");
                return false;
            }
            break;
        case OPTEE_MSG_ATTR_TYPE_NONE:
        case OPTEE_MSG_ATTR_TYPE_VALUE_INPUT:
        case OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT:
        case OPTEE_MSG_ATTR_TYPE_VALUE_INOUT:
        case OPTEE_MSG_ATTR_TYPE_RMEM_INPUT:
        case OPTEE_MSG_ATTR_TYPE_RMEM_OUTPUT:
        case OPTEE_MSG_ATTR_TYPE_RMEM_INOUT:
            continue;
        }
    }
    return true;
}

/*
 * Copy command buffer into xen memory to:
 * 1) Hide translated addresses from guest
 * 2) Make sure that guest wouldn't change data in command buffer during call
 */
static bool copy_std_request(struct cpu_user_regs *regs,
                             struct std_call_ctx *ctx)
{
    paddr_t cmd_gaddr, xen_addr;
    mfn_t cmd_mfn;

    cmd_gaddr = (paddr_t)get_user_reg(regs, 1) << 32 |
        get_user_reg(regs, 2);

    /*
     * Command buffer should start at page boundary.
     * This is OP-TEE ABI requirement.
     */
    if ( cmd_gaddr & (OPTEE_MSG_NONCONTIG_PAGE_SIZE - 1) )
        return false;

    cmd_mfn = lookup_guest_ram_addr(cmd_gaddr);
    if ( mfn_eq(cmd_mfn, INVALID_MFN) )
        return false;

    ctx->guest_arg = map_domain_page(cmd_mfn);
    if ( !ctx->guest_arg )
        return false;

    ctx->xen_arg = alloc_xenheap_page();
    if ( !ctx->xen_arg )
        return false;

    memcpy(ctx->xen_arg, ctx->guest_arg, OPTEE_MSG_NONCONTIG_PAGE_SIZE);

    xen_addr = virt_to_maddr(ctx->xen_arg);

    set_user_reg(regs, 1, xen_addr >> 32);
    set_user_reg(regs, 2, xen_addr & 0xFFFFFFFF);

    return true;
}

static bool copy_std_request_back(struct cpu_user_regs *regs,
                                  struct std_call_ctx *ctx)
{
    unsigned int i;
    uint32_t attr;

    ctx->guest_arg->ret = ctx->xen_arg->ret;
    ctx->guest_arg->ret_origin = ctx->xen_arg->ret_origin;
    ctx->guest_arg->session = ctx->xen_arg->session;
    for ( i = 0; i < ctx->xen_arg->num_params; i++ ) {
        attr = ctx->xen_arg->params[i].attr;

        switch ( attr & OPTEE_MSG_ATTR_TYPE_MASK ) {
        case OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT:
        case OPTEE_MSG_ATTR_TYPE_TMEM_INOUT:
            ctx->guest_arg->params[i].u.tmem.size =
                ctx->xen_arg->params[i].u.tmem.size;
            continue;
        case OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT:
        case OPTEE_MSG_ATTR_TYPE_VALUE_INOUT:
            ctx->guest_arg->params[i].u.value.a =
                ctx->xen_arg->params[i].u.value.a;
            ctx->guest_arg->params[i].u.value.b =
                ctx->xen_arg->params[i].u.value.b;
            continue;
        case OPTEE_MSG_ATTR_TYPE_RMEM_OUTPUT:
        case OPTEE_MSG_ATTR_TYPE_RMEM_INOUT:
            ctx->guest_arg->params[i].u.rmem.size =
                ctx->xen_arg->params[i].u.rmem.size;
            continue;
        case OPTEE_MSG_ATTR_TYPE_NONE:
        case OPTEE_MSG_ATTR_TYPE_TMEM_INPUT:
        case OPTEE_MSG_ATTR_TYPE_RMEM_INPUT:
        case OPTEE_MSG_ATTR_TYPE_VALUE_INPUT:
            continue;
        }
    }
    return true;
}

static bool execute_std_call(struct cpu_user_regs *regs,
                             struct std_call_ctx *ctx)
{
    register_t optee_ret;
    forward_call(regs);
    optee_ret = get_user_reg(regs, 0);

    if ( OPTEE_SMC_RETURN_IS_RPC(optee_ret) )
    {
        ctx->optee_thread_id = get_user_reg(regs, 3);
        ctx->rpc_op = OPTEE_SMC_RETURN_GET_RPC_FUNC(optee_ret);
        return true;
    }

    copy_std_request_back(regs, ctx);
    free_std_call_ctx(ctx);

    return true;
}

static bool handle_std_call(struct cpu_user_regs *regs)
{
    struct std_call_ctx *ctx;
    bool ret;

    ctx = allocate_std_call_ctx();

    if (!ctx)
        return false;

    ctx->domid = current->domain->domain_id;

    ret = copy_std_request(regs, ctx);
    if ( !ret )
        goto out;

    /* Now we can safely examine contents of command buffer */
    if ( OPTEE_MSG_GET_ARG_SIZE(ctx->xen_arg->num_params) >
         OPTEE_MSG_NONCONTIG_PAGE_SIZE ) {
        ret = false;
        goto out;
    }

    switch ( ctx->xen_arg->cmd )
    {
    case OPTEE_MSG_CMD_OPEN_SESSION:
    case OPTEE_MSG_CMD_CLOSE_SESSION:
    case OPTEE_MSG_CMD_INVOKE_COMMAND:
    case OPTEE_MSG_CMD_CANCEL:
    case OPTEE_MSG_CMD_REGISTER_SHM:
    case OPTEE_MSG_CMD_UNREGISTER_SHM:
        ret = translate_params(ctx);
        break;
    default:
        ret = false;
    }

    if (!ret)
        goto out;

    ret = execute_std_call(regs, ctx);

out:
    if (!ret)
        free_std_call_ctx(ctx);

    return ret;
}

static void handle_rpc_cmd_alloc(struct cpu_user_regs *regs,
                                 struct std_call_ctx *ctx,
                                 struct shm *shm)
{
    if ( shm->guest_arg->params[0].attr != (OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT |
                                            OPTEE_MSG_ATTR_NONCONTIG) )
    {
        gprintk(XENLOG_WARNING, "Invalid attrs for shared mem buffer\n");
        return;
    }

    /* Last entry in non_contig array is used to hold RPC-allocated buffer */
    if ( ctx->non_contig[MAX_NONCONTIG_ENTRIES - 1] )
    {
        free_xenheap_pages(ctx->non_contig[7],
                           ctx->non_contig_order[MAX_NONCONTIG_ENTRIES - 1]);
        ctx->non_contig[7] = NULL;
    }
    translate_noncontig(ctx, shm->guest_arg->params + 0,
                        MAX_NONCONTIG_ENTRIES - 1);
}

static void handle_rpc_cmd(struct cpu_user_regs *regs, struct std_call_ctx *ctx)
{
    struct shm *shm;
    uint64_t cookie;

    cookie = get_user_reg(regs, 1) << 32 | get_user_reg(regs, 2);

    shm = find_shm(cookie, current->domain->domain_id);

    if ( !shm )
    {
        gprintk(XENLOG_ERR, "Can't find SHM with cookie %lx\n", cookie);
        return;
    }

    switch (shm->guest_arg->cmd) {
    case OPTEE_MSG_RPC_CMD_GET_TIME:
        break;
    case OPTEE_MSG_RPC_CMD_WAIT_QUEUE:
        break;
    case OPTEE_MSG_RPC_CMD_SUSPEND:
        break;
    case OPTEE_MSG_RPC_CMD_SHM_ALLOC:
        handle_rpc_cmd_alloc(regs, ctx, shm);
        break;
    case OPTEE_MSG_RPC_CMD_SHM_FREE:
        break;
    default:
        break;
    }
}

static void handle_rpc_func_alloc(struct cpu_user_regs *regs,
                                  struct std_call_ctx *ctx)
{
    paddr_t ptr = get_user_reg(regs, 1) << 32 | get_user_reg(regs, 2);

    if ( ptr & (OPTEE_MSG_NONCONTIG_PAGE_SIZE - 1) )
        gprintk(XENLOG_WARNING, "Domain returned invalid RPC command buffer\n");

    if ( ptr ) {
        uint64_t cookie = get_user_reg(regs, 4) << 32 | get_user_reg(regs, 5);
        struct shm *shm;

        shm = allocate_and_map_shm(ptr, cookie);
        if ( !shm )
        {
            gprintk(XENLOG_WARNING, "Failed to callocate allocate SHM\n");
            ptr = 0;
        }
        else
            ptr = mfn_to_maddr(shm->guest_mfn);

        set_user_reg(regs, 1, ptr >> 32);
        set_user_reg(regs, 2, ptr & 0xFFFFFFFF);
    }
}

static bool handle_rpc(struct cpu_user_regs *regs)
{
    struct std_call_ctx *ctx;

    int optee_thread_id = get_user_reg(regs, 3);

    ctx = find_ctx(optee_thread_id, current->domain->domain_id);

    if (!ctx)
        return false;

    switch ( ctx->rpc_op ) {
    case OPTEE_SMC_RPC_FUNC_ALLOC:
        handle_rpc_func_alloc(regs, ctx);
        break;
    case OPTEE_SMC_RPC_FUNC_FREE:
    {
        uint64_t cookie = get_user_reg(regs, 1) << 32 | get_user_reg(regs, 2);
        free_shm(cookie, current->domain->domain_id);
        break;
    }
    case OPTEE_SMC_RPC_FUNC_FOREIGN_INTR:
        break;
    case OPTEE_SMC_RPC_FUNC_CMD:
        handle_rpc_cmd(regs, ctx);
        break;
    }

    return execute_std_call(regs, ctx);
}

static bool handle_exchange_capabilities(struct cpu_user_regs *regs)
{
        forward_call(regs);

        /* Return error back to the guest */
        if ( get_user_reg(regs, 0) != OPTEE_SMC_RETURN_OK )
            return true;

        /* Don't allow guests to work without dynamic SHM */
        if ( !(get_user_reg(regs, 1) & OPTEE_SMC_SEC_CAP_DYNAMIC_SHM) )
            set_user_reg(regs, 0, OPTEE_SMC_RETURN_ENOTAVAIL);
        return true;
}

static bool optee_handle_smc(struct cpu_user_regs *regs)
{

    switch ( get_user_reg(regs, 0) )
    {
    case OPTEE_SMC_GET_SHM_CONFIG:
        set_user_reg(regs, 0, OPTEE_SMC_RETURN_ENOTAVAIL);
        return true;
    case OPTEE_SMC_EXCHANGE_CAPABILITIES:
        return handle_exchange_capabilities(regs);
    case OPTEE_SMC_CALL_WITH_ARG:
        return handle_std_call(regs);
    case OPTEE_SMC_CALL_RETURN_FROM_RPC:
        return handle_rpc(regs);
    default:
        return forward_call(regs);
    }
    return false;
}

static void optee_remove(void)
{
}

static const struct tee_mediator_ops optee_ops =
{
    .init = optee_init,
    .domain_create = optee_domain_create,
    .domain_destroy = optee_domain_destroy,
    .handle_smc = optee_handle_smc,
    .remove = optee_remove,
};

REGISTER_TEE_MEDIATOR(optee, "OP-TEE", OPTEE_UID, &optee_ops);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
