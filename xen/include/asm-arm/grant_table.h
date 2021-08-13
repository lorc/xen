#ifndef __ASM_GRANT_TABLE_H__
#define __ASM_GRANT_TABLE_H__

#include <xen/grant_table.h>
#include <xen/kernel.h>
#include <xen/pfn.h>
#include <xen/sched.h>

#include <asm/guest_atomics.h>

#define INITIAL_NR_GRANT_FRAMES 1U
#define GNTTAB_MAX_VERSION 1

static inline void gnttab_clear_flags(struct domain *d,
                                      unsigned int mask, uint16_t *addr)
{
    guest_clear_mask16(d, mask, addr);
}

static inline void gnttab_mark_dirty(struct domain *d, mfn_t mfn)
{
#ifndef NDEBUG
    printk_once(XENLOG_G_WARNING "gnttab_mark_dirty not implemented yet\n");
#endif
}

int create_grant_host_mapping(unsigned long gpaddr, mfn_t mfn,
                              unsigned int flags, unsigned int cache_flags);
#define gnttab_host_mapping_get_page_type(ro, ld, rd) (0)
int replace_grant_host_mapping(unsigned long gpaddr, mfn_t mfn,
                               unsigned long new_gpaddr, unsigned int flags);
#define gnttab_release_host_mappings(domain) 1

/*
 * The region used by Xen on the memory will never be mapped in DOM0
 * memory layout. Therefore it can be used for the grant table.
 *
 * Only use the text section as it's always present and will contain
 * enough space for a large grant table
 */
#define gnttab_dom0_frames()                                             \
    min_t(unsigned int, opt_max_grant_frames, PFN_DOWN(_etext - _stext))

#define gnttab_set_frame_gfn(gt, st, idx, gfn)                           \
    do {                                                                 \
        struct page_info *pg_ = (st) ? gnttab_status_page(gt, idx)       \
                                     : gnttab_shared_page(gt, idx);      \
        page_set_frame_gfn(pg_, gfn);                                    \
    } while ( 0 )

#define gnttab_get_frame_gfn(gt, st, idx) ({                             \
   (st) ? gnttab_status_gfn(NULL, gt, idx)                               \
        : gnttab_shared_gfn(NULL, gt, idx);                              \
})

#define gnttab_shared_page(t, i) ({                                      \
    ASSERT((t)->shared_raw[i]);                                          \
    mfn_to_page(_mfn(__virt_to_mfn((t)->shared_raw[i])));                \
})

#define gnttab_status_page(t, i) ({                                      \
    ASSERT((t)->status[i]);                                              \
    mfn_to_page(_mfn(__virt_to_mfn((t)->status[i])));                    \
})

#define gnttab_shared_gfn(d, t, i) ({                                    \
    struct page_info *pg_ = gnttab_shared_page(t, i);                    \
    page_get_frame_gfn(pg_);                                             \
})

#define gnttab_status_gfn(d, t, i) ({                                    \
    struct page_info *pg_ = gnttab_status_page(t, i);                    \
    page_get_frame_gfn(pg_);                                             \
})

#define gnttab_need_iommu_mapping(d)                    \
    (is_domain_direct_mapped(d) && is_iommu_enabled(d))

#endif /* __ASM_GRANT_TABLE_H__ */
/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
