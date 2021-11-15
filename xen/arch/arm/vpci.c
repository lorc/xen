/*
 * xen/arch/arm/vpci.c
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
#include <xen/sched.h>
#include <xen/vpci.h>
#include <xen/keyhandler.h>

#include <asm/mmio.h>

static pci_sbdf_t vpci_sbdf_from_gpa(uint16_t segment, uint8_t busn_start,
                                     paddr_t base_addr, paddr_t gpa)
{
    pci_sbdf_t sbdf;

    sbdf.sbdf = VPCI_ECAM_BDF(gpa - base_addr);
    sbdf.seg = segment;
    sbdf.bus += busn_start;
    return sbdf;
}

static int vpci_mmio_read(struct vcpu *v, mmio_info_t *info,
                          register_t *r, bool is_virt, pci_sbdf_t sbdf)
{
    /* data is needed to prevent a pointer cast on 32bit */
    unsigned long data;

    /*
     * For the passed through devices we need to map their virtual SBDF
     * to the physical PCI device being passed through.
     */
    if ( is_virt && !vpci_translate_virtual_device(v->domain, &sbdf) )
    {
        *r = ~0ul;
        return 1;
    }

    if ( vpci_ecam_read(sbdf, ECAM_REG_OFFSET(info->gpa),
                        1U << info->dabt.size, &data) )
    {
        *r = data;
        return 1;
    }

    *r = ~0ul;

    return 0;
}

static int vpci_mmio_read_root(struct vcpu *v, mmio_info_t *info,
                          register_t *r, void *p)
{
    struct pci_host_bridge *bridge = p;
    pci_sbdf_t sbdf;

    if ( bridge )
        sbdf = vpci_sbdf_from_gpa(bridge->segment,
                                  bridge->cfg->busn_start,
                                  bridge->cfg->phys_addr,
                                  info->gpa);
    else
        sbdf = vpci_sbdf_from_gpa(0, 0, GUEST_VPCI_ECAM_BASE, info->gpa);

    return vpci_mmio_read(v, info, r, !bridge, sbdf);
}

static int vpci_mmio_read_child(struct vcpu *v, mmio_info_t *info,
                          register_t *r, void *p)
{
    struct pci_host_bridge *bridge = p;
    pci_sbdf_t sbdf = vpci_sbdf_from_gpa(bridge->segment,
                                         bridge->child_cfg->busn_start,
                                         bridge->child_cfg->phys_addr,
                                         info->gpa);

    return vpci_mmio_read(v, info, r, !bridge, sbdf);
}

static int vpci_mmio_write(struct vcpu *v, mmio_info_t *info,
                           register_t r, bool is_virt, pci_sbdf_t sbdf)
{
    /*
     * For the passed through devices we need to map their virtual SBDF
     * to the physical PCI device being passed through.
     */
    if ( is_virt && !vpci_translate_virtual_device(v->domain, &sbdf) )
        return 1;

    return vpci_ecam_write(sbdf, ECAM_REG_OFFSET(info->gpa),
                           1U << info->dabt.size, r);
}

static int vpci_mmio_write_root(struct vcpu *v, mmio_info_t *info,
                                register_t r, void *p)
{
    struct pci_host_bridge *bridge = p;
    pci_sbdf_t sbdf;

    if ( bridge )
        sbdf = vpci_sbdf_from_gpa(bridge->segment,
                                  bridge->cfg->busn_start,
                                  bridge->cfg->phys_addr,
                                  info->gpa);
    else
        sbdf = vpci_sbdf_from_gpa(0, 0, GUEST_VPCI_ECAM_BASE, info->gpa);

    return vpci_mmio_write(v, info, r, !bridge, sbdf);
}

static int vpci_mmio_write_child(struct vcpu *v, mmio_info_t *info,
                                register_t r, void *p)
{
    struct pci_host_bridge *bridge = p;
    pci_sbdf_t sbdf = vpci_sbdf_from_gpa(bridge->segment,
                                         bridge->child_cfg->busn_start,
                                         bridge->child_cfg->phys_addr,
                                         info->gpa);

    return vpci_mmio_write(v, info, r, !bridge, sbdf);
}

static const struct mmio_handler_ops vpci_mmio_handler = {
    .read  = vpci_mmio_read_root,
    .write = vpci_mmio_write_root,
};

static const struct mmio_handler_ops vpci_mmio_handler_child = {
    .read  = vpci_mmio_read_child,
    .write = vpci_mmio_write_child,
};

static int vpci_setup_mmio_handler_cb(struct domain *d,
                                      struct pci_host_bridge *bridge)
{
    struct pci_config_window *cfg = bridge->cfg;
    int count = 1;

    if ( !pci_is_hardware_domain(d, bridge->segment, cfg->busn_start) )
        return 0;

    register_mmio_handler(d, &vpci_mmio_handler,
                          cfg->phys_addr, cfg->size, bridge);

    if ( bridge->child_ops )
    {
        struct pci_config_window *cfg = bridge->child_cfg;

        register_mmio_handler(d, &vpci_mmio_handler_child,
                              cfg->phys_addr, cfg->size, bridge);
        count++;
    }

    return count;
}

int domain_vpci_init(struct domain *d)
{
    int count;

    if ( !has_vpci(d) )
        return 0;

    /*
     * The hardware domain gets as many MMIOs as required by the
     * physical host bridge.
     * Guests get the virtual platform layout: one virtual host bridge for now.
     *
     * We don't know if this domain has bridges assigned,
     * so let's iterate the bridges and count them:
     * if the count is 0 then this domain doesn't own any
     * bridge and it can either be a control domain or just a
     * regular guest.
     */
    count = pci_host_iterate_bridges_and_count(d, vpci_setup_mmio_handler_cb);
    if ( count )
        return 0;

    if ( !is_control_domain(d) )
        register_mmio_handler(d, &vpci_mmio_handler,
                              GUEST_VPCI_ECAM_BASE, GUEST_VPCI_ECAM_SIZE, NULL);

    return 0;
}

static int vpci_get_num_handlers_cb(struct domain *d,
                                    struct pci_host_bridge *bridge)
{
    int count = 1;

    if ( bridge->child_cfg )
        count++;

    return count;
}

unsigned int domain_vpci_get_num_mmio_handlers(struct domain *d)
{
    unsigned int count;
    int ret;

    if ( !has_vpci(d) )
        return 0;

    /*
     * We don't know if this domain has bridges assigned,
     * so let's iterate the bridges and count them:
     * if the count is 0 then this domain doesn't own any
     * bridge and it can either be a control domain or just a
     * regular guest.
     */
    ret = pci_host_iterate_bridges_and_count(d, vpci_get_num_handlers_cb);
    if ( ret < 0 )
    {
        ASSERT_UNREACHABLE();
        return 0;
    }
    if ( ret )
        return ret;

    if ( is_control_domain(d) )
        count = 0;
    else
    {
        /*
         * For guests each host bridge requires one region to cover the
         * configuration space. At the moment, we only expose a single host bridge.
         */
        count = 1;

        /*
         * There's a single MSI-X MMIO handler that deals with both PBA
         * and MSI-X tables per each PCI device being passed through.
         * Maximum number of emulated virtual devices is VPCI_MAX_VIRT_DEV.
         */
        if ( IS_ENABLED(CONFIG_HAS_PCI_MSI) )
            count += VPCI_MAX_VIRT_DEV;
    }

    return count;
}

static void dump_msi(unsigned char key)
{
    printk("MSI information:\n");

    vpci_dump_msi();
}

static int __init msi_setup_keyhandler(void)
{
    register_keyhandler('M', dump_msi, "dump MSI state", 1);
    return 0;
}
__initcall(msi_setup_keyhandler);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */

