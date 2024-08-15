/*
 * xen/arch/arm/sci/scmi_smc.c
 *
 * SCMI mediator driver, using SCP as transport.
 *
 * Oleksii Moisieiev <oleksii_moisieiev@epam.com>
 * Copyright (C) 2021, EPAM Systems.
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

#include <asm/sci/sci.h>
#include <asm/smccc.h>
#include <asm/io.h>
#include <xen/bitops.h>
#include <xen/config.h>
#include <xen/sched.h>
#include <xen/device_tree.h>
#include <xen/iocap.h>
#include <xen/init.h>
#include <xen/err.h>
#include <xen/lib.h>
#include <xen/list.h>
#include <xen/mm.h>
#include <xen/string.h>
#include <xen/time.h>
#include <xen/vmap.h>

#define SCMI_BASE_PROTOCOL                  0x10
#define SCMI_BASE_PROTOCOL_ATTIBUTES        0x1
#define SCMI_BASE_SET_DEVICE_PERMISSIONS    0x9
#define SCMI_BASE_RESET_AGENT_CONFIGURATION 0xB
#define SCMI_BASE_DISCOVER_AGENT            0x7

/* SCMI return codes. See section 4.1.4 of SCMI spec (DEN0056C) */
#define SCMI_SUCCESS              0
#define SCMI_NOT_SUPPORTED      (-1)
#define SCMI_INVALID_PARAMETERS (-2)
#define SCMI_DENIED             (-3)
#define SCMI_NOT_FOUND          (-4)
#define SCMI_OUT_OF_RANGE       (-5)
#define SCMI_BUSY               (-6)
#define SCMI_COMMS_ERROR        (-7)
#define SCMI_GENERIC_ERROR      (-8)
#define SCMI_HARDWARE_ERROR     (-9)
#define SCMI_PROTOCOL_ERROR     (-10)

#define DT_MATCH_SCMI_SMC DT_MATCH_COMPATIBLE("arm,scmi-smc")

#define SCMI_SMC_ID                        "arm,smc-id"
#define SCMI_SHARED_MEMORY                 "arm,scmi-shmem"
#define SCMI_SHMEM                         "shmem"
#define SCMI_SHMEM_MAPPED_SIZE             PAGE_SIZE

#define HYP_CHANNEL                          0x0

#define HDR_ID                             GENMASK(7,0)
#define HDR_TYPE                           GENMASK(9, 8)
#define HDR_PROTO                          GENMASK(17, 10)

/* SCMI protocol, refer to section 4.2.2.2 (DEN0056C) */
#define MSG_N_AGENTS_MASK                  GENMASK(15, 8)

#define FIELD_GET(_mask, _reg)\
    ((typeof(_mask))(((_reg) & (_mask)) >> (ffs64(_mask) - 1)))
#define FIELD_PREP(_mask, _val)\
    (((typeof(_mask))(_val) << (ffs64(_mask) - 1)) & (_mask))

typedef struct scmi_msg_header {
    uint8_t id;
    uint8_t type;
    uint8_t protocol;
} scmi_msg_header_t;

#define SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE   BIT(0, UL)
#define SCMI_SHMEM_CHAN_STAT_CHANNEL_ERROR  BIT(1, UL)

#define SCMI_ALLOW_ACCESS                   BIT(0, UL)

struct scmi_shared_mem {
    uint32_t reserved;
    uint32_t channel_status;
    uint32_t reserved1[2];
    uint32_t flags;
    uint32_t length;
    uint32_t msg_header;
    uint8_t msg_payload[];
};

struct dt_channel_addr {
    u64 addr;
    u64 size;
    struct list_head list;
};

struct scmi_channel {
    int chan_id;
    int agent_id;
    uint32_t func_id;
    domid_t domain_id;
    uint64_t paddr;
    uint64_t len;
    struct scmi_shared_mem *shmem;
    spinlock_t lock;
    struct list_head list;
};

struct scmi_data {
    struct list_head channel_list;
    spinlock_t channel_list_lock;
    bool initialized;
};

static struct scmi_data scmi_data;


/*
 * pack_scmi_header() - packs and returns 32-bit header
 *
 * @hdr: pointer to header containing all the information on message id,
 *    protocol id and type id.
 *
 * Return: 32-bit packed message header to be sent to the platform.
 */
static inline uint32_t pack_scmi_header(scmi_msg_header_t *hdr)
{
    return FIELD_PREP(HDR_ID, hdr->id) |
        FIELD_PREP(HDR_TYPE, hdr->type) |
        FIELD_PREP(HDR_PROTO, hdr->protocol);
}

/*
 * unpack_scmi_header() - unpacks and records message and protocol id
 *
 * @msg_hdr: 32-bit packed message header sent from the platform
 * @hdr: pointer to header to fetch message and protocol id.
 */
static inline void unpack_scmi_header(uint32_t msg_hdr, scmi_msg_header_t *hdr)
{
    hdr->id = FIELD_GET(HDR_ID, msg_hdr);
    hdr->type = FIELD_GET(HDR_TYPE, msg_hdr);
    hdr->protocol = FIELD_GET(HDR_PROTO, msg_hdr);
}

static inline int channel_is_free(struct scmi_channel *chan_info)
{
    return ( chan_info->shmem->channel_status
            & SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE ) ? 0 : -EBUSY;
}

/*
 * Copy data from IO memory space to "real" memory space.
 */
void __memcpy_fromio(void *to, const volatile void __iomem *from, size_t count)
{
    while (count && !IS_ALIGNED((unsigned long)from, 4)) {
        *(u8 *)to = __raw_readb(from);
        from++;
        to++;
        count--;
    }

    while (count >= 4) {
        *(u32 *)to = __raw_readl(from);
        from += 4;
        to += 4;
        count -= 4;
    }

    while (count) {
        *(u8 *)to = __raw_readb(from);
        from++;
        to++;
        count--;
    }
}

/*
 * Copy data from "real" memory space to IO memory space.
 */
void __memcpy_toio(volatile void __iomem *to, const void *from, size_t count)
{
    while (count && !IS_ALIGNED((unsigned long)to, 4)) {
        __raw_writeb(*(u8 *)from, to);
        from++;
        to++;
        count--;
    }

    while (count >= 4) {
        __raw_writel(*(u32 *)from, to);
        from += 4;
        to += 4;
        count -= 4;
    }

    while (count) {
        __raw_writeb(*(u8 *)from, to);
        from++;
        to++;
        count--;
    }
}

static int send_smc_message(struct scmi_channel *chan_info,
                            scmi_msg_header_t *hdr, void *data, int len)
{
    struct arm_smccc_res resp;
    int ret;

    if ( (len + sizeof(chan_info->shmem->msg_header)) >
                         SCMI_SHMEM_MAPPED_SIZE )
    {
        printk(XENLOG_ERR
               "scmi: Wrong size of smc message. Data is invalid\n");
        return -EINVAL;
    }

    printk(XENLOG_DEBUG "scmi: status =%d len=%d\n",
           chan_info->shmem->channel_status, len);
    printk(XENLOG_DEBUG "scmi: header id = %d type = %d, proto = %d\n",
           hdr->id, hdr->type, hdr->protocol);

    ret = channel_is_free(chan_info);
    if ( IS_ERR_VALUE(ret) )
        return ret;

    chan_info->shmem->channel_status = 0x0;
    /* Writing 0x0 right now, but SCMI_SHMEM_FLAG_INTR_ENABLED can be set */
    chan_info->shmem->flags = 0x0;
    chan_info->shmem->length = sizeof(chan_info->shmem->msg_header) + len;
    chan_info->shmem->msg_header = pack_scmi_header(hdr);

    printk(XENLOG_DEBUG "scmi: Writing to shmem address %p\n",
           chan_info->shmem);
    if ( len > 0 && data )
        __memcpy_toio((void *)(chan_info->shmem->msg_payload), data, len);

    arm_smccc_smc(chan_info->func_id, 0, 0, 0, 0, 0, 0, chan_info->chan_id,
                  &resp);

    printk(XENLOG_DEBUG "scmi: scmccc_smc response %d\n", (int)(resp.a0));

    if ( resp.a0 )
        return -EOPNOTSUPP;

    return 0;
}

static int check_scmi_status(int scmi_status)
{
    if ( scmi_status == SCMI_SUCCESS )
        return 0;

    printk(XENLOG_DEBUG "scmi: Error received: %d\n", scmi_status);

    switch ( scmi_status )
    {
    case SCMI_NOT_SUPPORTED:
        return -EOPNOTSUPP;
    case SCMI_INVALID_PARAMETERS:
        return -EINVAL;
    case SCMI_DENIED:
        return -EACCES;
    case SCMI_NOT_FOUND:
        return -ENOENT;
    case SCMI_OUT_OF_RANGE:
        return -ERANGE;
    case SCMI_BUSY:
        return -EBUSY;
    case SCMI_COMMS_ERROR:
        return -ENOTCONN;
    case SCMI_GENERIC_ERROR:
        return -EIO;
    case SCMI_HARDWARE_ERROR:
        return -ENXIO;
    case SCMI_PROTOCOL_ERROR:
        return -EBADMSG;
    default:
        return -EINVAL;
    }
}

static int get_smc_response(struct scmi_channel *chan_info,
                            scmi_msg_header_t *hdr, void *data, int len)
{
    int recv_len;
    int ret;

    printk(XENLOG_DEBUG "scmi: get smc responce msgid %d\n", hdr->id);

    if ( len >= SCMI_SHMEM_MAPPED_SIZE - sizeof(chan_info->shmem) )
    {
        printk(XENLOG_ERR
               "scmi: Wrong size of input smc message. Data may be invalid\n");
        return -EINVAL;
    }

    ret = channel_is_free(chan_info);
    if ( IS_ERR_VALUE(ret) )
        return ret;

    recv_len = chan_info->shmem->length - sizeof(chan_info->shmem->msg_header);

    if ( recv_len < 0 )
    {
        printk(XENLOG_ERR
               "scmi: Wrong size of smc message. Data may be invalid\n");
        return -EINVAL;
    }

    if ( recv_len > len )
    {
        printk(XENLOG_ERR
               "scmi: Not enough buffer for message %d, expecting %d\n",
               recv_len, len);
        return -EINVAL;
    }

    unpack_scmi_header(chan_info->shmem->msg_header, hdr);

    if ( recv_len > 0 )
    {
        __memcpy_fromio(data, chan_info->shmem->msg_payload, recv_len);
    }

    return 0;
}

static int do_smc_xfer(struct scmi_channel *channel, scmi_msg_header_t *hdr, void *tx_data, int tx_size,
                       void *rx_data, int rx_size)
{
    int ret = 0;

    ASSERT( channel && channel->shmem);

    if ( !hdr )
        return -EINVAL;

    spin_lock(&channel->lock);

    ret = send_smc_message(channel, hdr, tx_data, tx_size);
    if ( ret )
        goto clean;

    ret = get_smc_response(channel, hdr, rx_data, rx_size);
clean:
    spin_unlock(&channel->lock);

    return ret;
}

static struct scmi_channel *get_channel_by_id(uint8_t chan_id)
{
    struct scmi_channel *curr;
    bool found = false;

    spin_lock(&scmi_data.channel_list_lock);
    list_for_each_entry(curr, &scmi_data.channel_list, list)
    {
        if ( curr->chan_id == chan_id )
        {
            found = true;
            break;
        }
    }

    spin_unlock(&scmi_data.channel_list_lock);
    if ( found )
        return curr;

    return NULL;
}

static struct scmi_channel *aquire_scmi_channel(domid_t domain_id)
{
    struct scmi_channel *curr;
    bool found = false;

    ASSERT(domain_id != DOMID_INVALID && domain_id >= 0);

    spin_lock(&scmi_data.channel_list_lock);
    list_for_each_entry(curr, &scmi_data.channel_list, list)
    {
        if ( curr->domain_id == DOMID_INVALID )
        {
            curr->domain_id = domain_id;
            found = true;
            break;
        }
    }

    spin_unlock(&scmi_data.channel_list_lock);
    if ( found )
        return curr;

    return NULL;
}

static void relinquish_scmi_channel(struct scmi_channel *channel)
{
    ASSERT(channel != NULL);

    spin_lock(&scmi_data.channel_list_lock);
    channel->domain_id = DOMID_INVALID;
    spin_unlock(&scmi_data.channel_list_lock);
}

static int map_channel_memory(struct scmi_channel *channel)
{
    ASSERT( channel && channel->paddr );
    channel->shmem = ioremap_cache(channel->paddr, SCMI_SHMEM_MAPPED_SIZE);
    if ( !channel->shmem )
        return -ENOMEM;

    channel->shmem->channel_status = SCMI_SHMEM_CHAN_STAT_CHANNEL_FREE;
    printk(XENLOG_DEBUG "scmi: Got shmem after vmap %p\n", channel->shmem);
    return 0;
}

static void unmap_channel_memory(struct scmi_channel *channel)
{
    ASSERT( channel && channel->shmem );
    iounmap(channel->shmem);
    channel->shmem = NULL;
}

static struct scmi_channel *smc_create_channel(uint8_t chan_id,
                                               uint32_t func_id, uint64_t addr)
{
    struct scmi_channel *channel;

    channel = get_channel_by_id(chan_id);
    if ( channel )
        return ERR_PTR(EEXIST);

    channel = xmalloc(struct scmi_channel);
    if ( !channel )
        return ERR_PTR(ENOMEM);

    channel->chan_id = chan_id;
    channel->func_id = func_id;
    channel->domain_id = DOMID_INVALID;
    channel->shmem = NULL;
    channel->paddr = addr;
    spin_lock_init(&channel->lock);
    spin_lock(&scmi_data.channel_list_lock);
    list_add(&channel->list, &scmi_data.channel_list);
    spin_unlock(&scmi_data.channel_list_lock);
    return channel;
}

static int mem_permit_access(struct domain *d, uint64_t addr, uint64_t len)
{
    return iomem_permit_access(d, paddr_to_pfn(addr),
                paddr_to_pfn(PAGE_ALIGN(addr + len -1)));
}

static int mem_deny_access(struct domain *d, uint64_t addr,
                                     uint64_t len)
{
    return iomem_deny_access(d, paddr_to_pfn(addr),
                paddr_to_pfn(PAGE_ALIGN(addr + len -1)));
}

static int dt_update_domain_range(uint64_t addr, uint64_t size)
{
    struct dt_device_node *shmem_node;
    __be32 *hw_reg;
    const struct dt_property *pp;
    uint32_t len;

    shmem_node = dt_find_compatible_node(NULL, NULL, SCMI_SHARED_MEMORY);
    if ( !shmem_node )
    {
        printk(XENLOG_ERR "scmi: Unable to find %s node in DT\n", SCMI_SHMEM);
        return -EINVAL;
    }

    pp = dt_find_property(shmem_node, "reg", &len);
    if ( !pp )
    {
        printk(XENLOG_ERR "scmi: Unable to find regs entry in shmem node\n");
        return -ENOENT;
    }

    hw_reg = pp->value;
    dt_set_range(&hw_reg, shmem_node, addr, size);

    return 0;
}

static void free_channel_list(void)
{
    struct scmi_channel *curr, *_curr;

    spin_lock(&scmi_data.channel_list_lock);
    list_for_each_entry_safe (curr, _curr, &scmi_data.channel_list, list)
    {
        list_del(&curr->list);
        xfree(curr);
    }

    spin_unlock(&scmi_data.channel_list_lock);
}

static struct dt_device_node *get_dt_node_from_property(
                struct dt_device_node *node, const char * p_name)
{
    const __be32 *prop;

    ASSERT( node );

    prop = dt_get_property(node, p_name, NULL);
    if ( !prop )
        return ERR_PTR(-EINVAL);

    return dt_find_node_by_phandle(be32_to_cpup(prop));
}

static int get_shmem_regions(struct list_head *head, u64 hyp_addr)
{
    struct dt_device_node *node;
    int ret;
    struct dt_channel_addr *lchan;
    u64 laddr, lsize;

    node = dt_find_compatible_node(NULL, NULL, SCMI_SHARED_MEMORY);
    if ( !node )
        return -ENOENT;

    while ( node )
    {
        ret = dt_device_get_address(node, 0, &laddr, &lsize);
        if ( ret )
            return ret;

        if ( laddr != hyp_addr )
        {
            lchan = xmalloc(struct dt_channel_addr);
            if ( !lchan )
                return -ENOMEM;
            lchan->addr = laddr;
            lchan->size = lsize;

            list_add_tail(&lchan->list, head);
        }

        node = dt_find_compatible_node(node, NULL, SCMI_SHARED_MEMORY);
    }

    return 0;
}

static int read_hyp_channel_addr(struct dt_device_node *scmi_node,
                                 u64 *addr, u64 *size)
{
    struct dt_device_node *shmem_node;
    shmem_node = get_dt_node_from_property(scmi_node, "shmem");
    if ( IS_ERR_OR_NULL(shmem_node) )
    {
        printk(XENLOG_ERR
               "scmi: Device tree error, can't parse reserved memory %ld\n",
               PTR_ERR(shmem_node));
        return PTR_ERR(shmem_node);
    }

    return dt_device_get_address(shmem_node, 0, addr, size);
}

static void free_shmem_regions(struct list_head *addr_list)
{
    struct dt_channel_addr *curr, *_curr;

    list_for_each_entry_safe (curr, _curr, addr_list, list)
    {
        list_del(&curr->list);
        xfree(curr);
    }
}

static __init bool scmi_probe(struct dt_device_node *scmi_node)
{
    u64 addr, size;
    int ret, i;
    struct scmi_channel *channel, *agent_channel;
    int n_agents;
    scmi_msg_header_t hdr;
    struct rx_t {
        int32_t status;
        uint32_t attributes;
    } rx;
    struct dt_channel_addr *entry;
    struct list_head addr_list;

    uint32_t func_id;

    ASSERT(scmi_node != NULL);

    INIT_LIST_HEAD(&scmi_data.channel_list);
    spin_lock_init(&scmi_data.channel_list_lock);

    if ( !dt_property_read_u32(scmi_node, SCMI_SMC_ID, &func_id) )
    {
        printk(XENLOG_ERR "scmi: Unable to read smc-id from DT\n");
        return false;
    }

    ret = read_hyp_channel_addr(scmi_node, &addr, &size);
    if ( IS_ERR_VALUE(ret) )
        return false;

    if ( !IS_ALIGNED(size, SCMI_SHMEM_MAPPED_SIZE) )
    {
        printk(XENLOG_ERR "scmi: Reserved memory is not aligned\n");
        return false;
    }

    INIT_LIST_HEAD(&addr_list);

    ret = get_shmem_regions(&addr_list, addr);
    if ( IS_ERR_VALUE(ret) )
        goto out;

    channel = smc_create_channel(HYP_CHANNEL, func_id, addr);
    if ( IS_ERR(channel) )
        goto out;

    ret = map_channel_memory(channel);
    if ( ret )
        goto out;

    spin_lock(&scmi_data.channel_list_lock);
    channel->domain_id = DOMID_XEN;
    spin_unlock(&scmi_data.channel_list_lock);

    hdr.id = SCMI_BASE_PROTOCOL_ATTIBUTES;
    hdr.type = 0;
    hdr.protocol = SCMI_BASE_PROTOCOL;

    ret = do_smc_xfer(channel, &hdr, NULL, 0, &rx, sizeof(rx));
    if ( ret )
        goto error;

    ret = check_scmi_status(rx.status);
    if ( ret )
        goto error;

    n_agents = FIELD_GET(MSG_N_AGENTS_MASK, rx.attributes);
    printk(XENLOG_DEBUG "scmi: Got agent count %d\n", n_agents);

    i = 1;
    list_for_each_entry(entry, &addr_list, list)
    {
        uint32_t tx_agent_id = 0xFFFFFFFF;
        struct {
            int32_t status;
            uint32_t agent_id;
            char name[16];
        } da_rx;

        agent_channel = smc_create_channel(i, func_id,
                                           entry->addr);
        if ( IS_ERR(agent_channel) )
        {
            ret = PTR_ERR(agent_channel);
            goto error;
        }

        ret = map_channel_memory(agent_channel);
        if ( ret )
            goto error;

        hdr.id = SCMI_BASE_DISCOVER_AGENT;
        hdr.type = 0;
        hdr.protocol = SCMI_BASE_PROTOCOL;

        ret = do_smc_xfer(agent_channel, &hdr, &tx_agent_id,
                          sizeof(tx_agent_id), &da_rx, sizeof(da_rx));
        if ( ret )
        {
            unmap_channel_memory(agent_channel);
            goto error;
        }

        unmap_channel_memory(agent_channel);

        ret = check_scmi_status(da_rx.status);
        if ( ret )
            goto error;

        printk(XENLOG_DEBUG "scmi: status=0x%x id=0x%x name=%s\n",
                da_rx.status, da_rx.agent_id, da_rx.name);

        agent_channel->agent_id = da_rx.agent_id;

        if ( i == n_agents )
            break;

        i++;
    }

    scmi_data.initialized = true;
    goto out;

error:
    unmap_channel_memory(channel);
    free_channel_list();
out:
    free_shmem_regions(&addr_list);
    return ret == 0;
}

static int scmi_domain_init(struct domain *d,
                           struct xen_arch_domainconfig *config)
{
    struct scmi_channel *channel;
    int ret;

    if ( !scmi_data.initialized )
        return 0;

    printk(XENLOG_INFO "scmi: domain_id = %d\n", d->domain_id);

    channel = aquire_scmi_channel(d->domain_id);
    if ( IS_ERR_OR_NULL(channel) )
        return -ENOENT;

#ifdef CONFIG_ARM_32
    printk(XENLOG_INFO
           "scmi: Aquire SCMI channel id = 0x%x , domain_id = %d paddr = 0x%llx\n",
           channel->chan_id, channel->domain_id, channel->paddr);
#else
    printk(XENLOG_INFO
           "scmi: Aquire SCMI channel id = 0x%x , domain_id = %d paddr = 0x%lx\n",
           channel->chan_id, channel->domain_id, channel->paddr);
#endif

    if ( is_hardware_domain(d) )
    {
        ret = mem_permit_access(d, channel->paddr, PAGE_SIZE);
        if ( IS_ERR_VALUE(ret) )
            goto error;

        ret = dt_update_domain_range(channel->paddr, PAGE_SIZE);
        if ( IS_ERR_VALUE(ret) )
        {
            int rc = mem_deny_access(d, channel->paddr, PAGE_SIZE);
            if ( rc )
                printk(XENLOG_ERR "Unable to mem_deny_access\n");

            goto error;
        }
    }

    d->arch.sci = channel;

    /*
     * TODO: Report agent paddr to toolstack. Currently this is
     * imppossible because arm_sci_agent_paddr blows up size of domctl
     * struct above 128 bytes
     */
    /* if ( config ) */
    /*     config->arm_sci_agent_paddr = channel->paddr; */

    return 0;
error:
    relinquish_scmi_channel(channel);

    return ret;
}

static int scmi_add_device_by_devid(struct domain *d, uint32_t scmi_devid)
{
    struct scmi_channel *channel, *agent_channel;
    scmi_msg_header_t hdr;
    struct scmi_perms_tx {
        uint32_t agent_id;
        uint32_t device_id;
        uint32_t flags;
    } tx;
    struct rx_t {
        int32_t status;
        uint32_t attributes;
    } rx;
    int ret;

    if ( !scmi_data.initialized )
        return 0;

    printk(XENLOG_DEBUG "scmi: scmi_devid = %d\n", scmi_devid);

    agent_channel = d->arch.sci;
    if ( IS_ERR_OR_NULL(agent_channel) )
        return PTR_ERR(agent_channel);

    channel = get_channel_by_id(HYP_CHANNEL);
    if ( IS_ERR_OR_NULL(channel) )
        return PTR_ERR(channel);

    hdr.id = SCMI_BASE_SET_DEVICE_PERMISSIONS;
    hdr.type = 0;
    hdr.protocol = SCMI_BASE_PROTOCOL;

    tx.agent_id = agent_channel->agent_id;
    tx.device_id = scmi_devid;
    tx.flags = SCMI_ALLOW_ACCESS;

    ret = do_smc_xfer(channel, &hdr, &tx, sizeof(tx), &rx, sizeof(&rx));
    if ( IS_ERR_VALUE(ret) )
        return ret;

    ret = check_scmi_status(rx.status);
    if ( IS_ERR_VALUE(ret) )
        return ret;

    return 0;
}

static int scmi_add_dt_device(struct domain *d, struct dt_device_node *dev)
{
    uint32_t scmi_devid;

    if ( (!scmi_data.initialized) || (!d->arch.sci) )
        return 0;

    if ( !dt_property_read_u32(dev, "scmi_devid", &scmi_devid) )
        return 0;

    printk(XENLOG_INFO "scmi: dt_node = %s\n", dt_node_full_name(dev));

    return scmi_add_device_by_devid(d, scmi_devid);
}

static int scmi_relinquish_resources(struct domain *d)
{
    int ret;
    struct scmi_channel *channel, *agent_channel;
    scmi_msg_header_t hdr;
    struct reset_agent_tx {
        uint32_t agent_id;
        uint32_t flags;
    } tx;
    uint32_t rx;

    if ( !d->arch.sci )
        return 0;

    agent_channel = d->arch.sci;

    spin_lock(&agent_channel->lock);
    tx.agent_id = agent_channel->agent_id;
    spin_unlock(&agent_channel->lock);

    channel = get_channel_by_id(HYP_CHANNEL);
    if ( !channel )
    {
        printk(XENLOG_ERR
               "scmi: Unable to get Hypervisor scmi channel for domain %d\n",
               d->domain_id);
        return -EINVAL;
    }

    hdr.id = SCMI_BASE_RESET_AGENT_CONFIGURATION;
    hdr.type = 0;
    hdr.protocol = SCMI_BASE_PROTOCOL;

    tx.flags = 0;

    ret = do_smc_xfer(channel, &hdr, &tx, sizeof(tx), &rx, sizeof(rx));
    if ( ret )
        return ret;

    ret = check_scmi_status(rx);

    return ret;
}

static void scmi_domain_destroy(struct domain *d)
{
    struct scmi_channel *channel;

    if ( !d->arch.sci )
        return;

    channel = d->arch.sci;
    spin_lock(&channel->lock);

    relinquish_scmi_channel(channel);
    printk(XENLOG_DEBUG "scmi: Free domain %d\n", d->domain_id);

    d->arch.sci = NULL;

    mem_deny_access(d, channel->paddr, PAGE_SIZE);
    spin_unlock(&channel->lock);
}

static bool scmi_handle_call(struct domain *d, void *args)
{
    bool res = false;
    struct scmi_channel *agent_channel;
    struct arm_smccc_res resp;
    struct cpu_user_regs *regs = args;

    if ( !d->arch.sci )
        return false;

    agent_channel = d->arch.sci;
    spin_lock(&agent_channel->lock);

    if ( agent_channel->func_id != regs->r0 )
    {
        res = false;
        goto unlock;
    }

    arm_smccc_smc(agent_channel->func_id, 0, 0, 0, 0, 0, 0,
                  agent_channel->chan_id, &resp);

    set_user_reg(regs, 0, resp.a0);
    set_user_reg(regs, 1, resp.a1);
    set_user_reg(regs, 2, resp.a2);
    set_user_reg(regs, 3, resp.a3);
    res = true;
unlock:
    spin_unlock(&agent_channel->lock);

    return res;
}

static const struct dt_device_match scmi_smc_match[] __initconst =
{
    DT_MATCH_SCMI_SMC,
    { /* sentinel */ },
};

static const struct sci_mediator_ops scmi_ops =
{
    .probe = scmi_probe,
    .domain_init = scmi_domain_init,
    .domain_destroy = scmi_domain_destroy,
    .add_dt_device = scmi_add_dt_device,
    .relinquish_resources = scmi_relinquish_resources,
    .handle_call = scmi_handle_call,
};

REGISTER_SCI_MEDIATOR(scmi_smc, "SCMI-SMC", XEN_DOMCTL_CONFIG_ARM_SCI_SCMI_SMC,
                      scmi_smc_match, &scmi_ops);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
