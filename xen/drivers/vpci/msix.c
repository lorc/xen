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

#include <asm/msi.h>
#include <asm/p2m.h>

static uint32_t control_read(const struct pci_dev *pdev, unsigned int reg,
                             void *data)
{
    const struct vpci_msix *msix = data;

    return (msix->max_entries - 1) |
           (msix->enabled ? PCI_MSIX_FLAGS_ENABLE : 0) |
           (msix->masked ? PCI_MSIX_FLAGS_MASKALL : 0);
}

void update_entry(struct vpci_msix_entry *entry,
                         const struct pci_dev *pdev, unsigned int nr)
{
    int rc = vpci_msix_arch_disable_entry(entry, pdev);

    /* Ignore ENOENT, it means the entry wasn't setup. */
    if ( rc && rc != -ENOENT )
    {
        gprintk(XENLOG_WARNING,
                "%pp: unable to disable entry %u for update: %d\n",
                &pdev->sbdf, nr, rc);
        return;
    }

    rc = vpci_msix_arch_enable_entry(entry, pdev,
                                     vmsix_table_base(pdev->vpci,
                                                      VPCI_MSIX_TABLE));
    if ( rc )
    {
        gprintk(XENLOG_WARNING, "%pp: unable to enable entry %u: %d\n",
                &pdev->sbdf, nr, rc);
        /* Entry is likely not properly configured. */
        return;
    }

    entry->updated = false;
}

static void control_write(const struct pci_dev *pdev, unsigned int reg,
                          uint32_t val, void *data)
{
    struct vpci_msix *msix = data;
    bool new_masked = val & PCI_MSIX_FLAGS_MASKALL;
    bool new_enabled = val & PCI_MSIX_FLAGS_ENABLE;
    unsigned int i;
    int rc;

    if ( new_masked == msix->masked && new_enabled == msix->enabled )
        return;

    /*
     * According to the PCI 3.0 specification, switching the enable bit to 1
     * or the function mask bit to 0 should cause all the cached addresses
     * and data fields to be recalculated.
     *
     * In order to avoid the overhead of disabling and enabling all the
     * entries every time the guest sets the maskall bit, Xen will only
     * perform the disable and enable sequence when the guest has written to
     * the entry.
     */
    if ( new_enabled && !new_masked && (!msix->enabled || msix->masked) )
    {
        for ( i = 0; i < msix->max_entries; i++ )
            if ( !msix->entries[i].masked && msix->entries[i].updated )
                update_entry(&msix->entries[i], pdev, i);
    }
    else if ( !new_enabled && msix->enabled )
    {
        /* Guest has disabled MSIX, disable all entries. */
        for ( i = 0; i < msix->max_entries; i++ )
        {
            /*
             * NB: vpci_msix_arch_disable can be called for entries that are
             * not setup, it will return -ENOENT in that case.
             */
            rc = vpci_msix_arch_disable_entry(&msix->entries[i], pdev);
            switch ( rc )
            {
            case 0:
                /*
                 * Mark the entry successfully disabled as updated, so that on
                 * the next enable the entry is properly setup. This is done
                 * so that the following flow works correctly:
                 *
                 * mask entry -> disable MSIX -> enable MSIX -> unmask entry
                 *
                 * Without setting 'updated', the 'unmask entry' step will fail
                 * because the entry has not been updated, so it would not be
                 * mapped/bound at all.
                 */
                msix->entries[i].updated = true;
                break;
            case -ENOENT:
                /* Ignore non-present entry. */
                break;
            default:
                gprintk(XENLOG_WARNING, "%pp: unable to disable entry %u: %d\n",
                        &pdev->sbdf, i, rc);
                return;
            }
        }
    }

    msix->masked = new_masked;
    msix->enabled = new_enabled;

    val = control_read(pdev, reg, data);
    if ( pci_msi_conf_write_intercept(msix->pdev, reg, 2, &val) >= 0 )
        pci_conf_write16(pdev->sbdf, reg, val);
}

static int init_msix(struct pci_dev *pdev)
{
    struct domain *d = pdev->domain;
    uint8_t slot = PCI_SLOT(pdev->devfn), func = PCI_FUNC(pdev->devfn);
    unsigned int msix_offset, i, max_entries;
    uint16_t control;
    struct vpci_msix *msix;
    int rc;

    if ( !is_hardware_domain(pdev->domain) )
        return 0;

    msix_offset = pci_find_cap_offset(pdev->seg, pdev->bus, slot, func,
                                      PCI_CAP_ID_MSIX);
    if ( !msix_offset )
        return 0;

    control = pci_conf_read16(pdev->sbdf, msix_control_reg(msix_offset));

    max_entries = msix_table_size(control);

    msix = xzalloc_flex_struct(struct vpci_msix, entries, max_entries);
    if ( !msix )
        return -ENOMEM;

    rc = vpci_add_register(pdev->vpci, control_read, control_write,
                           msix_control_reg(msix_offset), 2, msix);
    if ( rc )
    {
        xfree(msix);
        return rc;
    }

    msix->max_entries = max_entries;
    msix->pdev = pdev;
    pdev->vpci->msix = msix;

    msix->tables[VPCI_MSIX_TABLE] =
        pci_conf_read32(pdev->sbdf, msix_table_offset_reg(msix_offset));
    msix->tables[VPCI_MSIX_PBA] =
        pci_conf_read32(pdev->sbdf, msix_pba_offset_reg(msix_offset));

    for ( i = 0; i < max_entries; i++)
    {
        msix->entries[i].masked = true;
        msix->entries[i].entry_nr = i;
        vpci_msix_arch_init_entry(&msix->entries[i]);
    }

    register_msix_mmio_handler(d);
    vpci_msix_add_to_msix_table(msix, d);

    return 0;
}
REGISTER_VPCI_INIT(init_msix, VPCI_PRIORITY_HIGH);

static int vpci_add_msix_ctrl_hanlder(struct pci_dev *pdev)
{
    uint8_t slot = PCI_SLOT(pdev->devfn), func = PCI_FUNC(pdev->devfn);
    unsigned int msix_offset;
    int  rc;

    if ( is_hardware_domain(pdev->domain) )
        return 0;

    msix_offset = pci_find_cap_offset(pdev->seg, pdev->bus, slot, func,
                                      PCI_CAP_ID_MSIX);
    if ( !msix_offset )
        return 0;

    rc = vpci_add_register(pdev->vpci, control_read, control_write,
                           msix_control_reg(msix_offset), 2, pdev->vpci->msix);
    if ( rc )
        return rc;

    pdev->vpci->msix->enabled = 0;
    pdev->vpci->msix->masked = 0;

    return 0;
}
REGISTER_VPCI_INIT(vpci_add_msix_ctrl_hanlder, VPCI_PRIORITY_HIGH);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
