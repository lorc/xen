/*
 * Based on Linux drivers/pci/ecam.c
 * Based on Linux drivers/pci/controller/pci-host-common.c
 * Based on Linux drivers/pci/controller/pci-host-generic.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/init.h>
#include <xen/pci.h>
#include <xen/rwlock.h>
#include <xen/sched.h>
#include <xen/vmap.h>

#include <asm/setup.h>

/*
 * List for all the pci host bridges.
 */

static LIST_HEAD(pci_host_bridges);

static atomic_t domain_nr = ATOMIC_INIT(-1);

static inline void __iomem *pci_remap_cfgspace(paddr_t start, size_t len)
{
    return ioremap_nocache(start, len);
}

static void pci_ecam_free(struct pci_config_window *cfg)
{
    if ( cfg->win )
        iounmap(cfg->win);

    xfree(cfg);
}

static void __init gen_pci_init_bus_range(struct dt_device_node *dev,
                                          struct pci_host_bridge *bridge,
                                          struct pci_config_window *cfg)
{
    u32 bus_range[2];
    int err;

    err = dt_property_read_u32_array(dev, "bus-range", bus_range,
                                     ARRAY_SIZE(bus_range));
    if ( err ) {
        cfg->busn_start = 0;
        cfg->busn_end = 0xff;
        printk(XENLOG_INFO "%s: No bus range found for pci controller\n",
               dt_node_full_name(dev));
    } else {
        cfg->busn_start = bus_range[0];
        cfg->busn_end = bus_range[1];
        if ( cfg->busn_end > cfg->busn_start + 0xff )
            cfg->busn_end = cfg->busn_start + 0xff;
    }
}

static void __init gen_pci_init_bus_range_child(struct dt_device_node *dev,
                                                struct pci_host_bridge *bridge,
                                                struct pci_config_window *cfg)
{
    cfg->busn_start = bridge->cfg->busn_start + 1;
    cfg->busn_end = bridge->cfg->busn_end;
    bridge->cfg->busn_end = bridge->cfg->busn_start;

    printk(XENLOG_INFO "Root bus end updated: [bus %x-%x]\n",
           bridge->cfg->busn_start, bridge->cfg->busn_end);
}

static struct pci_config_window * __init
gen_pci_init(struct dt_device_node *dev, struct pci_host_bridge *bridge,
             const struct pci_ecam_ops *ops,
             void (*init_bus_range)(struct dt_device_node *dev,
                                    struct pci_host_bridge *bridge,
                                    struct pci_config_window *cfg))
{
    int err, cfg_reg_idx;
    paddr_t addr, size;
    struct pci_config_window *cfg;

    cfg = xzalloc(struct pci_config_window);
    if ( !cfg )
        return NULL;

    init_bus_range(dev, bridge, cfg);

    if ( ops->cfg_reg_index )
    {
        cfg_reg_idx = ops->cfg_reg_index(dev);
        if ( cfg_reg_idx < 0 )
            goto err_exit;
    }
    else
        cfg_reg_idx = 0;

    /* Parse our PCI ecam register address */
    err = dt_device_get_address(dev, cfg_reg_idx, &addr, &size);
    if ( err )
        goto err_exit;

    cfg->phys_addr = addr;
    cfg->size = size;

    /*
     * On 64-bit systems, we do a single ioremap for the whole config space
     * since we have enough virtual address range available.  On 32-bit, we
     * ioremap the config space for each bus individually.
     * As of now only 64-bit is supported 32-bit is not supported.
     *
     * TODO: For 32-bit implement the ioremap/iounmap of config space
     * dynamically for each read/write call.
     */
    cfg->win = pci_remap_cfgspace(cfg->phys_addr, cfg->size);
    if ( !cfg->win )
    {
        printk(XENLOG_ERR "ECAM ioremap failed\n");
        goto err_exit;
    }
    printk("ECAM at [mem 0x%"PRIpaddr"-0x%"PRIpaddr"] for [bus %x-%x] \n",
            cfg->phys_addr, cfg->phys_addr + cfg->size - 1,
            cfg->busn_start, cfg->busn_end);

    if ( ops->init )
    {
        err = ops->init(cfg);
        if ( err )
            goto err_exit;
    }

    return cfg;

err_exit:
    pci_ecam_free(cfg);

    return NULL;
}

struct pci_host_bridge *pci_alloc_host_bridge(void)
{
    struct pci_host_bridge *bridge = xzalloc(struct pci_host_bridge);

    if ( !bridge )
        return NULL;

    INIT_LIST_HEAD(&bridge->node);

    return bridge;
}

void pci_add_host_bridge(struct pci_host_bridge *bridge)
{
    list_add_tail(&bridge->node, &pci_host_bridges);
}

static int pci_set_msi_base(struct pci_host_bridge *bridge)
{
    struct dt_device_node *msi_node;
    const __be32 *map = NULL;
    uint64_t addr, size;

    map = dt_get_property(bridge->dt_node, "msi-map", NULL);
    if ( !map )
        return -ENODEV;

    msi_node = dt_find_node_by_phandle(be32_to_cpup(map + 1));
    if ( !msi_node )
        return -ENODEV;

    if ( dt_device_get_address(msi_node, 0, &addr, &size) )
        return -ENODEV;

    bridge->its_msi_base = addr;

    return 0;
}

int pci_get_new_domain_nr(void)
{
    return atomic_inc_return(&domain_nr);
}

static int pci_bus_find_domain_nr(struct dt_device_node *dev)
{
    static int use_dt_domains = -1;
    int domain;

    domain = dt_get_pci_domain_nr(dev);

    /*
     * Check DT domain and use_dt_domains values.
     *
     * If DT domain property is valid (domain >= 0) and
     * use_dt_domains != 0, the DT assignment is valid since this means
     * we have not previously allocated a domain number by using
     * pci_get_new_domain_nr(); we should also update use_dt_domains to
     * 1, to indicate that we have just assigned a domain number from
     * DT.
     *
     * If DT domain property value is not valid (ie domain < 0), and we
     * have not previously assigned a domain number from DT
     * (use_dt_domains != 1) we should assign a domain number by
     * using the:
     *
     * pci_get_new_domain_nr()
     *
     * API and update the use_dt_domains value to keep track of method we
     * are using to assign domain numbers (use_dt_domains = 0).
     *
     * All other combinations imply we have a platform that is trying
     * to mix domain numbers obtained from DT and pci_get_new_domain_nr(),
     * which is a recipe for domain mishandling and it is prevented by
     * invalidating the domain value (domain = -1) and printing a
     * corresponding error.
     */
    if ( domain >= 0 && use_dt_domains )
    {
        use_dt_domains = 1;
    }
    else if ( domain < 0 && use_dt_domains != 1 )
    {
        use_dt_domains = 0;
        domain = pci_get_new_domain_nr();
    }
    else
    {
        domain = -1;
    }

    return domain;
}

struct pci_host_bridge *
pci_host_common_probe(struct dt_device_node *dev,
                      const struct pci_ecam_ops *ops,
                      const struct pci_ecam_ops *child_ops,
                      size_t priv_sz)
{
    struct pci_host_bridge *bridge;
    struct pci_config_window *cfg;
    int err;

    bridge = pci_alloc_host_bridge();
    if ( !bridge )
        return ERR_PTR(-ENOMEM);

    /* Parse and map our Configuration Space windows */
    cfg = gen_pci_init(dev, bridge, ops, gen_pci_init_bus_range);
    if ( !cfg )
    {
        err = -ENOMEM;
        goto err_exit;
    }

    bridge->dt_node = dev;
    bridge->cfg = cfg;
    bridge->ops = &ops->pci_ops;

    if ( child_ops )
    {
        /* Parse and map child's Configuration Space windows */
        cfg = gen_pci_init(dev, bridge, child_ops,
                           gen_pci_init_bus_range_child);
        if ( !cfg )
        {
            err = -ENOMEM;
            goto err_child;
        }

        bridge->child_cfg = cfg;
        bridge->child_ops = &child_ops->pci_ops;
    }

    bridge->segment = pci_bus_find_domain_nr(dev);
    if ( bridge->segment < 0 )
    {
        printk(XENLOG_ERR "Inconsistent \"linux,pci-domain\" property in DT\n");
        BUG();
    }

    if ( priv_sz )
    {
        bridge->priv = xzalloc_bytes(priv_sz);
        if ( !bridge->priv )
        {
            err = -ENOMEM;
            goto err_priv;
        }
    }

    pci_add_host_bridge(bridge);

    pci_set_msi_base(bridge);

    return bridge;

err_priv:
    xfree(bridge->priv);

err_child:
    xfree(bridge->cfg);

err_exit:
    xfree(bridge);

    return ERR_PTR(err);
}

/*
 * Get host bridge node given a device attached to it.
 */
struct dt_device_node *pci_find_host_bridge_node(struct device *dev)
{
    struct pci_host_bridge *bridge;
    struct pci_dev *pdev = dev_to_pci(dev);

    bridge = pci_find_host_bridge(pdev->seg, pdev->bus);
    if ( unlikely(!bridge) )
    {
        printk(XENLOG_ERR "Unable to find PCI bridge for %pp\n", &pdev->sbdf);
        return NULL;
    }
    return bridge->dt_node;
}
/*
 * This function will lookup an hostbridge based on the segment and bus
 * number.
 */
struct pci_host_bridge *pci_find_host_bridge(uint16_t segment, uint8_t bus)
{
    struct pci_host_bridge *bridge;

    list_for_each_entry( bridge, &pci_host_bridges, node )
    {
        if ( bridge->segment != segment )
            continue;
        if ( bridge->child_cfg && (bus >= bridge->child_cfg->busn_start) &&
             (bus <= bridge->child_cfg->busn_end) )
             return bridge;
        if ( (bus >= bridge->cfg->busn_start) && (bus <= bridge->cfg->busn_end) )
             return bridge;
    }

    return NULL;
}

/*
 * This function will lookup an hostbridge based on config space address.
 */
int pci_get_host_bridge_segment(const struct dt_device_node *node,
                                uint16_t *segment)
{
    struct pci_host_bridge *bridge;

    list_for_each_entry( bridge, &pci_host_bridges, node )
    {
        if ( bridge->dt_node != node )
            continue;

        *segment = bridge->segment;
        return 0;
    }

    return -EINVAL;
}

int pci_host_iterate_bridges_and_count(struct domain *d,
                                       int (*cb)(struct domain *d,
                                                 struct pci_host_bridge *bridge))
{
    struct pci_host_bridge *bridge;
    int err, count = 0;

    list_for_each_entry( bridge, &pci_host_bridges, node )
    {
        err = cb(d, bridge);
        if ( err )
            return err;
        count += err;
    }
    return count;
}

int __init pci_host_bridge_mappings(struct domain *d)
{
    struct pci_host_bridge *bridge;
    struct map_range_data mr_data = {
        .d = d,
        .p2mt = p2m_mmio_direct_dev,
        .skip_mapping = false
    };

    /*
     * For each PCI host bridge we need to only map those ranges
     * which are used by Domain-0 to properly initialize the bridge,
     * e.g. we do not want to map ECAM configuration space which lives in
     * "reg" device tree property, but we want to map other regions of
     * the host bridge. The PCI aperture defined by the "ranges" device
     * tree property should also be skipped.
     */
    list_for_each_entry( bridge, &pci_host_bridges, node )
    {
        const struct dt_device_node *dev = bridge->dt_node;
        unsigned int i;
        bool need_mapping;

        for ( i = 0; i < dt_number_of_address(dev); i++ )
        {
            uint64_t addr, size;
            int err;

            err = dt_device_get_address(dev, i, &addr, &size);
            if ( err )
            {
                printk(XENLOG_ERR
                       "Unable to retrieve address range index=%u for %s\n",
                       i, dt_node_full_name(dev));
                return err;
            }

            need_mapping = bridge->ops->need_p2m_hwdom_mapping(d, bridge, addr);
            if ( need_mapping && bridge->child_ops )
                  need_mapping = bridge->child_ops->
                      need_p2m_hwdom_mapping(d, bridge, addr);
            if ( need_mapping )
            {
                err = map_range_to_domain(dev, addr, size, &mr_data);
                if ( err )
                    return err;
            }
        }
    }

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
