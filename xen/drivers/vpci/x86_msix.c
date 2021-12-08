/*
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

u32 vpci_arch_readl(unsigned long addr)
{
    return readl(addr);
}

u64 vpci_arch_readq(unsigned long addr)
{
    return readq(addr);
}

void vpci_arch_writel(u32 data, unsigned long addr)
{
    writel(data, addr);
}

void vpci_arch_writeq(u64 data, unsigned long addr)
{
    writeq(data, addr);
}

int vpci_make_msix_hole(const struct pci_dev *pdev)
{
    struct domain *d = pdev->domain;
    unsigned int i;

    if ( !pdev->vpci->msix )
        return 0;

    /* Make sure there's a hole for the MSIX table/PBA in the p2m. */
    for ( i = 0; i < ARRAY_SIZE(pdev->vpci->msix->tables); i++ )
    {
        unsigned long start = PFN_DOWN(vmsix_table_addr(pdev->vpci, i));
        unsigned long end = PFN_DOWN(vmsix_table_addr(pdev->vpci, i) +
                                     vmsix_table_size(pdev->vpci, i) - 1);

        for ( ; start <= end; start++ )
        {
            p2m_type_t t;
            mfn_t mfn = get_gfn_query(d, start, &t);

            switch ( t )
            {
            case p2m_mmio_dm:
            case p2m_invalid:
                break;
            case p2m_mmio_direct:
                if ( mfn_x(mfn) == start )
                {
                    clear_identity_p2m_entry(d, start);
                    break;
                }
                /* fallthrough. */
            default:
                put_gfn(d, start);
                gprintk(XENLOG_WARNING,
                        "%pp: existing mapping (mfn: %" PRI_mfn
                        "type: %d) at %#lx clobbers MSIX MMIO area\n",
                        &pdev->sbdf, mfn_x(mfn), t, start);
                return -EEXIST;
            }
            put_gfn(d, start);
        }
    }

    return 0;
}

struct vpci_msix *msix_find(const struct domain *d, unsigned long addr)
{
    struct vpci_msix *msix;

    list_for_each_entry ( msix, &d->arch.hvm.msix_tables, next )
    {
        const struct vpci_bar *bars = msix->pdev->vpci->header.bars;
        unsigned int i;

        for ( i = 0; i < ARRAY_SIZE(msix->tables); i++ )
            if ( bars[msix->tables[i] & PCI_MSIX_BIRMASK].enabled &&
                 VMSIX_ADDR_IN_RANGE(addr, msix->pdev->vpci, i) )
                return msix;
    }

    return NULL;
}

static int x86_msix_accept(struct vcpu *v, unsigned long addr)
{
    return !!msix_find(v->domain, addr);
}

static int x86_msix_write(struct vcpu *v, unsigned long addr, unsigned int len,
                          unsigned long data)
{
    const struct domain *d = v->domain;
    struct vpci_msix *msix = msix_find(d, addr);

    return msix_write(d, msix, addr, len, data);
}

static int x86_msix_read(struct vcpu *v, unsigned long addr, unsigned int len,
                         unsigned long *data)
{
    const struct domain *d = v->domain;
    struct vpci_msix *msix = msix_find(d, addr);

    return msix_read(msix, addr, len, data);
}

static const struct hvm_mmio_ops vpci_msix_table_ops = {
    .check = x86_msix_accept,
    .read = x86_msix_read,
    .write = x86_msix_write,
};

void register_msix_mmio_handler(struct domain *d)
{
    if ( list_empty(&d->arch.hvm.msix_tables) )
        register_mmio_handler(d, &vpci_msix_table_ops);
}

void vpci_msix_add_to_msix_table(struct vpci_msix *msix,
                                 struct domain *d)
{
    list_add(&msix->next, &d->arch.hvm.msix_tables);
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
