/*
 * Generic functionality for handling SR-IOV for guests.
 *
 * Copyright (C) 2021 EPAM Systems
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

static const struct pci_dev *get_physfn_pdev(const struct pci_dev *pdev)
{
    if ( !pdev->info.is_virtfn )
        return NULL;

    return pci_get_pdev(/*seg*/0, pdev->info.physfn.bus,
                        pdev->info.physfn.devfn);
}

static uint32_t guest_vendor_id_read(const struct pci_dev *pdev,
                                     unsigned int reg, void *data)
{
    struct vpci_header *header = &pdev->vpci->header;

    if ( !pdev->info.is_virtfn )
        return vpci_hw_read32(pdev, reg, data);
    return header->vf_ven_dev_id;
}

static void guest_vendor_id_write(const struct pci_dev *pdev, unsigned int reg,
                                  uint32_t val, void *data)
{
}

static uint32_t guest_get_vf_ven_dev_id(const struct pci_dev *pdev)
{
    const struct pci_dev *physfn_pdev;
    uint32_t dev_ven_id, pos;

    /*
     * Find the physical function for this virtual function device
     * and use its VendorID and read our DeviceID.
     */
    physfn_pdev = get_physfn_pdev(pdev);
    if ( !physfn_pdev )
    {
        gprintk(XENLOG_ERR, "%pp cannot find physfn\n",
                &pdev->sbdf);
        return ~0;
    }

    /* Vendor ID is the same as the PF's Venodr ID. */
    dev_ven_id = pci_conf_read16(physfn_pdev->sbdf, PCI_VENDOR_ID);
    /* Device ID comes from the SR-IOV extended capability. */
    pos = pci_find_ext_capability(physfn_pdev->sbdf.seg,
                                  physfn_pdev->sbdf.bus,
                                  physfn_pdev->sbdf.devfn,
                                  PCI_EXT_CAP_ID_SRIOV);
    if ( !pos )
    {
        gprintk(XENLOG_ERR, "%pp cannot find SR-IOV extended capability, PF %pp\n",
                &pdev->sbdf, &physfn_pdev->sbdf);
        return ~0;
    }
    return dev_ven_id | pci_conf_read16(physfn_pdev->sbdf,
                                        pos + PCI_SRIOV_VF_DID) << 16;
}

static unsigned int get_sriov_pf_pos(const struct pci_dev *pdev)
{
    if ( pdev->info.is_virtfn )
        return 0;

    return pci_find_ext_capability(pdev->seg, pdev->bus, pdev->devfn,
                                   PCI_EXT_CAP_ID_SRIOV);
}

/*
 * This is called for the physical functions which live in the hardware domain
 * and is used to prepare vf_bars.
 * No device, but physical function has PCI_EXT_CAP_ID_SRIOV, so it is used
 * as a check for device eligibility.
 */
static int vf_init_bars(struct pci_dev *pdev)
{
    struct vpci_bar *bars;
    unsigned int i, vf_pos;

    vf_pos = get_sriov_pf_pos(pdev);
    if ( !vf_pos )
        return 0;

    /* Read BARs for VFs out of PF's SR-IOV extended capability. */
    bars = pdev->vpci->vf_bars;
    for ( i = 0; i < PCI_SRIOV_NUM_BARS; i++)
    {
        unsigned int idx = vf_pos + PCI_SRIOV_BAR + i * 4;
        uint32_t bar;

        /* FIXME: pdev->vf_rlen already has the size of the BAR after sizing. */
        bars[i].size = pdev->vf_rlen[i];
        bars[i].type = VPCI_BAR_EMPTY;

        if ( i && bars[i - 1].type == VPCI_BAR_MEM64_LO )
        {
            bars[i].type = VPCI_BAR_MEM64_HI;
            continue;
        }

        if ( !bars[i].size )
            continue;

        bar = pci_conf_read32(pdev->sbdf, idx);
        /* No VPCI_BAR_ROM or VPCI_BAR_IO expected for VF. */
        if ( (bar & PCI_BASE_ADDRESS_SPACE) ==
              PCI_BASE_ADDRESS_SPACE_IO )
        {
            printk(XENLOG_WARNING
                   "SR-IOV device %pp with vf BAR%u in IO space\n",
                   &pdev->sbdf, i);
            continue;
        }

        if ( (bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK) ==
             PCI_BASE_ADDRESS_MEM_TYPE_64 )
            bars[i].type = VPCI_BAR_MEM64_LO;
        else
            bars[i].type = VPCI_BAR_MEM32;

        bars[i].prefetchable = bar & PCI_BASE_ADDRESS_MEM_PREFETCH;
    }

    /* Also add handlers for the SR-IOV PF/VF BARs. */
    for ( i = 0; i < PCI_SRIOV_NUM_BARS; i++)
    {
        int rc;

        /*
         * FIXME: VFs ROM BAR is read-only and is all zeros. VF may provide
         * access to the PFs ROM via emulation though.
         */
        if ( (bars[i].type == VPCI_BAR_IO) ||
             (bars[i].type == VPCI_BAR_EMPTY) ||
             (bars[i].type == VPCI_BAR_ROM) )
            continue;

        /* This is either VPCI_BAR_MEM32 or VPCI_BAR_MEM64_{LO|HI}. */
        rc = vpci_add_register(pdev->vpci, vpci_hw_read32, vpci_bar_write,
                               vf_pos + PCI_SRIOV_BAR + i * 4, 4, &bars[i]);
        if ( rc )
            return rc;
    }

    return 0;
}
REGISTER_VPCI_INIT(vf_init_bars, VPCI_PRIORITY_MIDDLE);

/*
 * This is called for the virtual functions of the physical function
 * and is used to prepare BARs of the virtual function's pdev.
 */
static int vf_init_bars_virtfn(struct pci_dev *pdev)
{
    const struct pci_dev *physfn_pdev;
    struct vpci_bar *bars;
    struct vpci_bar *physfn_vf_bars;
    unsigned int i, vf_pos;

    if ( !pdev->info.is_virtfn)
        return 0;

    physfn_pdev = get_physfn_pdev(pdev);
    if ( !physfn_pdev )
    {
        gprintk(XENLOG_ERR, "%pp cannot find physfn\n",
                &pdev->sbdf);
        return -ENODEV;
    }

    /*
     * Set up BARs for this VF out of PF's VF BARs taking into account
     * the index of the VF.
     */
    bars = pdev->vpci->header.bars;
    physfn_vf_bars = physfn_pdev->vpci->vf_bars;
    vf_pos = get_sriov_pf_pos(physfn_pdev);

    for ( i = 0; i < PCI_SRIOV_NUM_BARS; i++)
    {
        uint16_t offset = pci_conf_read16(physfn_pdev->sbdf,
                                          vf_pos + PCI_SRIOV_VF_OFFSET);
        uint16_t stride = pci_conf_read16(physfn_pdev->sbdf,
                                          vf_pos + PCI_SRIOV_VF_STRIDE);
        char str[32];
        int vf_idx;

        snprintf(str, sizeof(str), "%pp:BAR%d", &pdev->sbdf, i);

        vf_idx = pdev->sbdf.sbdf;
        vf_idx -= physfn_pdev->sbdf.sbdf + offset;
        if ( vf_idx < 0 )
            return -EINVAL;
        if ( stride )
        {
            if ( vf_idx % stride )
                return -EINVAL;
            vf_idx /= stride;
        }

        bars[i].type = physfn_vf_bars[i].type;
        bars[i].addr = physfn_vf_bars[i].addr + vf_idx * physfn_vf_bars[i].size;
        bars[i].size = physfn_vf_bars[i].size;
        bars[i].prefetchable = physfn_vf_bars[i].prefetchable;
        bars[i].mem = rangeset_new(pdev->domain, str, RANGESETF_no_print);
    }

    return 0;
}
REGISTER_VPCI_INIT(vf_init_bars_virtfn, VPCI_PRIORITY_MIDDLE);

static uint32_t vf_cmd_read(const struct pci_dev *pdev, unsigned int reg,
                         void *data)
{
    if ( pdev->info.is_virtfn &&
         pci_is_hardware_domain(pdev->domain, pdev->seg, pdev->bus) )
    {
        struct vpci_header *header = data;

        return header->guest_cmd;
    }

    return vpci_hw_read16(pdev, reg, data);
}

static int vf_init_handlers(struct pci_dev *pdev)
{
    struct vpci_header *header = &pdev->vpci->header;
    struct vpci_bar *bars;
    unsigned int i;
    int rc;

    if ( !pdev->info.is_virtfn )
        return 0;

    /* Reset the command register for the guest. */
    vpci_cmd_write(pdev, PCI_COMMAND, 0, header);

    /*
     * Setup a handler for VENDOR_ID for guests only and allow hwdom reading
     * directly: the handler is used for SR-IOV virtual functions.
     */
    header->vf_ven_dev_id = guest_get_vf_ven_dev_id(pdev);
    if ( header->vf_ven_dev_id == ~0 )
        return -EINVAL;

    rc = vpci_add_register(pdev->vpci,
                           guest_vendor_id_read, guest_vendor_id_write,
                           PCI_VENDOR_ID, 4, header);
    if ( rc )
        return rc;

    /* Setup a handler for the command register. */
    rc = vpci_add_register(pdev->vpci, vf_cmd_read, vpci_cmd_write,
                           PCI_COMMAND, 2, header);
    if ( rc )
        return rc;

    /* Also add handlers for the SR-IOV PF/VF BARs. */
    bars = pdev->vpci->header.bars;
    for ( i = 0; i < PCI_SRIOV_NUM_BARS; i++)
    {
        /*
         * FIXME: VFs ROM BAR is read-only and is all zeros. VF may provide
         * access to the PFs ROM via emulation though.
         */
        if ( (bars[i].type == VPCI_BAR_IO) ||
             (bars[i].type == VPCI_BAR_EMPTY) ||
             (bars[i].type == VPCI_BAR_ROM) )
            continue;

        /* This is either VPCI_BAR_MEM32 or VPCI_BAR_MEM64_{LO|HI}. */
        rc = vpci_add_register(pdev->vpci,
                               vpci_guest_bar_read, vpci_guest_bar_write,
                               PCI_BASE_ADDRESS_0 + i * 4, 4, &bars[i]);
        if ( rc )
            return rc;
    }

    return 0;
}
REGISTER_VPCI_INIT(vf_init_handlers, VPCI_PRIORITY_LOW);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
