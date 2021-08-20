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

#include <asm/mmio.h>

static pci_sbdf_t vpci_sbdf_from_gpa(const struct pci_host_bridge *bridge,
                                     paddr_t gpa)
{
    pci_sbdf_t sbdf;

    if ( bridge )
    {
        sbdf.sbdf = VPCI_ECAM_BDF(gpa - bridge->cfg->phys_addr);
        sbdf.seg = bridge->segment;
        sbdf.bus += bridge->cfg->busn_start;
    }
    else
        sbdf.sbdf = VPCI_ECAM_BDF(gpa - GUEST_VPCI_ECAM_BASE);

    return sbdf;
}

static int vpci_mmio_read(struct vcpu *v, mmio_info_t *info,
                          register_t *r, void *p)
{
    struct pci_host_bridge *bridge = p;
    pci_sbdf_t sbdf = vpci_sbdf_from_gpa(bridge, info->gpa);
    /* data is needed to prevent a pointer cast on 32bit */
    unsigned long data;

#ifdef CONFIG_HAS_VPCI_GUEST_SUPPORT
    /*
     * For the passed through devices we need to map their virtual SBDF
     * to the physical PCI device being passed through.
     */
    if ( !bridge && !vpci_translate_virtual_device(v->domain, &sbdf) )
        return 1;
#endif

    if ( vpci_ecam_read(sbdf, ECAM_REG_OFFSET(info->gpa),
                        1U << info->dabt.size, &data) )
    {
        *r = data;
        return 1;
    }

    *r = ~0ul;

    return 0;
}

static int vpci_mmio_write(struct vcpu *v, mmio_info_t *info,
                           register_t r, void *p)
{
    struct pci_host_bridge *bridge = p;
    pci_sbdf_t sbdf = vpci_sbdf_from_gpa(bridge, info->gpa);

#ifdef CONFIG_HAS_VPCI_GUEST_SUPPORT
    /*
     * For the passed through devices we need to map their virtual SBDF
     * to the physical PCI device being passed through.
     */
    if ( !bridge && !vpci_translate_virtual_device(v->domain, &sbdf) )
        return 1;
#endif

    return vpci_ecam_write(sbdf, ECAM_REG_OFFSET(info->gpa),
                           1U << info->dabt.size, r);
}

static const struct mmio_handler_ops vpci_mmio_handler = {
    .read  = vpci_mmio_read,
    .write = vpci_mmio_write,
};

static int vpci_setup_mmio_handler_cb(struct domain *d,
                                      struct pci_host_bridge *bridge)
{
    struct pci_config_window *cfg = bridge->cfg;

    register_mmio_handler(d, &vpci_mmio_handler,
                          cfg->phys_addr, cfg->size, bridge);

    /* We have registered a single MMIO handler. */
    return 1;
}

int domain_vpci_init(struct domain *d)
{
    if ( !has_vpci(d) )
        return 0;

    if ( is_hardware_domain(d) )
    {
        int count;

        count = pci_host_iterate_bridges_and_count(d, vpci_setup_mmio_handler_cb);
        if ( count < 0 )
            return count;

        return 0;
    }

    /* Guest domains use what is programmed in their device tree. */
    register_mmio_handler(d, &vpci_mmio_handler,
                          GUEST_VPCI_ECAM_BASE, GUEST_VPCI_ECAM_SIZE, NULL);

    return 0;
}

static int vpci_get_num_handlers_cb(struct domain *d,
                                    struct pci_host_bridge *bridge)
{
    /* Each bridge has a single MMIO handler for the configuration space. */
    return 1;
}

unsigned int domain_vpci_get_num_mmio_handlers(struct domain *d)
{
    unsigned int count;

    if ( is_hardware_domain(d) )
    {
        int count;

        count = pci_host_iterate_bridges_and_count(d, vpci_get_num_handlers_cb);
        if ( count < 0 )
            return count;

        return 0;
    }

    /*
     * This is a guest domain.
     *
     * There's a single MSI-X MMIO handler that deals with both PBA
     * and MSI-X tables per each PCI device being passed through.
     * Maximum number of supported devices is 32 as virtual bus
     * topology emulates the devices as embedded endpoints.
     * +1 for a single emulated host bridge's configuration space.
     */
    count = 1;
#ifdef CONFIG_HAS_PCI_MSI
    count += 32;
#endif

    return count;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */

