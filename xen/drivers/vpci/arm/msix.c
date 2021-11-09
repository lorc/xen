/*
 * Handlers for accesses to the MSI-X capability structure and the memory
 * region.
 *
 * Copyright (C) 2017 Citrix Systems R&D
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/sched.h>
#include <xen/vpci.h>
#include <xen/vmap.h>
#include <asm/msi.h>
#include <asm/p2m.h>
#include <asm/io.h>

void register_msix_mmio_handler(struct domain *d) { }
void vpci_msix_add_to_msix_table(struct vpci_msix *msix, struct domain *d) { }
int pci_msi_conf_write_intercept(struct pci_dev *pdev, unsigned int reg,
                                 unsigned int size, uint32_t *data)
{
    return 0;
}

static inline paddr_t vmsix_guest_table_base(const struct vpci *vpci,
                                             unsigned int nr)
{
    return (vpci->header.bars[vpci->msix->tables[nr] &
           PCI_MSIX_BIRMASK].guest_addr & PCI_BASE_ADDRESS_MEM_MASK);
}

static inline paddr_t vmsix_guest_table_addr(const struct vpci *vpci,
                                             unsigned int nr)
{
    return vmsix_guest_table_base(vpci, nr) +
           (vpci->msix->tables[nr] & ~PCI_MSIX_BIRMASK);
}

static bool access_allowed(const struct pci_dev *pdev, unsigned long addr,
                           unsigned int len)
{
    /* Only allow aligned 32/64b accesses. */
    if ( (len == 4 || len == 8) && !(addr & (len - 1)) )
        return true;

    gprintk(XENLOG_WARNING,
            "%pp: unaligned or invalid size MSI-X table access\n", &pdev->sbdf);

    return false;
}

static struct vpci_msix_entry *get_entry(struct vpci_msix *msix,
                                         paddr_t addr)
{
    paddr_t start;
    if ( pci_is_hardware_domain(current->domain, msix->pdev->seg,
                                msix->pdev->bus) )
        start = vmsix_table_addr(msix->pdev->vpci, VPCI_MSIX_TABLE);
    else
        start = vmsix_guest_table_addr(msix->pdev->vpci, VPCI_MSIX_TABLE);

    return &msix->entries[(addr - start) / PCI_MSIX_ENTRY_SIZE];
}

static int msix_read(struct vcpu *v, mmio_info_t *info,
                     register_t *r, void *priv)
{
    struct vpci *vpci = (struct vpci *)priv;
    struct vpci_msix *msix = vpci->msix;
    const struct vpci_msix_entry *entry;
    unsigned int len = 1U << info->dabt.size;
    unsigned long addr = info->gpa;
    unsigned int offset;

    if ( !msix )
        return 1;

    if ( !access_allowed(msix->pdev, addr, len) )
        return 1;

    if ( VMSIX_ADDR_IN_RANGE(addr, msix->pdev->vpci, VPCI_MSIX_PBA) )
    {
        /*
         * Access to PBA.
         *
         * TODO: note that this relies on having the PBA identity mapped to the
         * guest address space. If this changes the address will need to be
         * translated.
         */
        switch ( len )
        {
        case 4:
            *r = readl(&addr);
            break;

        case 8:
            *r = readq(&addr);
            break;

        default:
            ASSERT_UNREACHABLE();
            break;
        }

        return 1;
    }

    spin_lock(&msix->pdev->vpci->lock);
    entry = get_entry(msix, addr);
    offset = addr & (PCI_MSIX_ENTRY_SIZE - 1);

    switch ( offset )
    {
    case PCI_MSIX_ENTRY_LOWER_ADDR_OFFSET:
        *r = entry->addr;
        break;

    case PCI_MSIX_ENTRY_UPPER_ADDR_OFFSET:
        *r = entry->addr >> 32;
        break;

    case PCI_MSIX_ENTRY_DATA_OFFSET:
        *r = entry->data;
        if ( len == 8 )
            *r |=
                (uint64_t)(entry->masked ? PCI_MSIX_VECTOR_BITMASK : 0) << 32;
        break;

    case PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET:
        *r = entry->masked ? PCI_MSIX_VECTOR_BITMASK : 0;
        break;

    default:
        ASSERT_UNREACHABLE();
        break;
    }
    spin_unlock(&msix->pdev->vpci->lock);

    return 1;
}

static int msix_write(struct vcpu *v, mmio_info_t *info,
                      register_t r, void *priv)
{
    const struct domain *d = v->domain;
    struct vpci *vpci = (struct vpci *)priv;
    struct vpci_msix *msix = vpci->msix;
    struct vpci_msix_entry *entry;
    unsigned int len = 1U << info->dabt.size;
    unsigned long addr = info->gpa;
    unsigned int offset;

    if ( !msix )
        return 1;

    if ( !access_allowed(msix->pdev, addr, len) )
        return 1;

    if ( VMSIX_ADDR_IN_RANGE(addr, msix->pdev->vpci, VPCI_MSIX_PBA) )
    {
        /* Ignore writes to PBA for DomUs, it's behavior is undefined. */
        if ( pci_is_hardware_domain(d, msix->pdev->seg, msix->pdev->bus) )
        {
            switch ( len )
            {
            case 4:
                writel(r, &addr);
                break;

            case 8:
                writeq(r, &addr);
                break;

            default:
                ASSERT_UNREACHABLE();
                break;
            }
        }

        return 1;
    }

    spin_lock(&msix->pdev->vpci->lock);
    entry = get_entry(msix, addr);
    offset = addr & (PCI_MSIX_ENTRY_SIZE - 1);

    /*
     * NB: Xen allows writes to the data/address registers with the entry
     * unmasked. The specification says this is undefined behavior, and Xen
     * implements it as storing the written value, which will be made effective
     * in the next mask/unmask cycle. This also mimics the implementation in
     * QEMU.
     */
    switch ( offset )
    {
    case PCI_MSIX_ENTRY_LOWER_ADDR_OFFSET:
        entry->updated = true;
        if ( len == 8 )
        {
            entry->addr = r;
            break;
        }
        entry->addr &= ~0xffffffffull;
        entry->addr |= r;
        break;

    case PCI_MSIX_ENTRY_UPPER_ADDR_OFFSET:
        entry->updated = true;
        entry->addr &= 0xffffffff;
        entry->addr |= (uint64_t)r << 32;
        break;

    case PCI_MSIX_ENTRY_DATA_OFFSET:
        entry->updated = true;
        entry->data = r;

        if ( len == 4 )
            break;

        r >>= 32;
        /* fallthrough */
    case PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET:
    {
        bool new_masked = r & PCI_MSIX_VECTOR_BITMASK;
        const struct pci_dev *pdev = msix->pdev;

        if ( entry->masked == new_masked )
            /* No change in the mask bit, nothing to do. */
            break;

        /*
         * Update the masked state before calling vpci_msix_arch_enable_entry,
         * so that it picks the new state.
         */
        entry->masked = new_masked;
        if ( !new_masked && msix->enabled && !msix->masked && entry->updated )
        {
            /*
             * If MSI-X is enabled, the function mask is not active, the entry
             * is being unmasked and there have been changes to the address or
             * data fields Xen needs to disable and enable the entry in order
             * to pick up the changes.
             */
            update_entry(entry, pdev, vmsix_entry_nr(msix, entry));
        }
        else
            vpci_msix_arch_mask_entry(entry, pdev, entry->masked);

        break;
    }

    default:
        ASSERT_UNREACHABLE();
        break;
    }
    spin_unlock(&msix->pdev->vpci->lock);

    return 1;
}

static const struct mmio_handler_ops vpci_msi_mmio_handler = {
    .read  = msix_read,
    .write = msix_write,
};

int vpci_make_msix_hole(const struct pci_dev *pdev)
{
    struct vpci_msix *msix = pdev->vpci->msix;
    paddr_t addr,size;

    for ( int i = 0; msix && i < ARRAY_SIZE(msix->tables); i++ )
    {
        if ( pci_is_hardware_domain(pdev->domain, pdev->seg, pdev->bus) )
            addr = vmsix_table_addr(pdev->vpci, VPCI_MSIX_TABLE);
        else
            addr = vmsix_guest_table_addr(pdev->vpci, VPCI_MSIX_TABLE);

        size = vmsix_table_size(pdev->vpci, VPCI_MSIX_TABLE) - 1;

        register_mmio_handler(pdev->domain, &vpci_msi_mmio_handler,
                              addr, size, pdev->vpci);
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
