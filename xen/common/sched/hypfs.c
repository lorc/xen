/*
 * Scheduler HYPFS interface
 *
 * Copyright (C) 2020 EPAM Systems
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

#include <xen/guest_access.h>
#include <xen/sched.h>
#include <xen/hypfs.h>
#include "private.h"

static HYPFS_DIR_INIT(scheduler, "scheduler");
static HYPFS_DIR_INIT(stats, "stats");

static int read_irq_time(const struct hypfs_entry *entry,
						 XEN_GUEST_HANDLE_PARAM(void) uaddr)
{
    uint64_t irq_time = 0;
    int cpu;

    for_each_present_cpu( cpu )
        irq_time += per_cpu(sched_stats, cpu).irq_time;

    return copy_to_guest(uaddr, &irq_time, entry->size) ? -EFAULT: 0;
}

static int read_hyp_time(const struct hypfs_entry *entry,
						 XEN_GUEST_HANDLE_PARAM(void) uaddr)
{
    uint64_t hyp_time = 0;
    int cpu;

    for_each_present_cpu( cpu )
        hyp_time += per_cpu(sched_stats, cpu).hyp_time;

    return copy_to_guest(uaddr, &hyp_time, entry->size) ? -EFAULT: 0;
}

struct hypfs_entry __read_mostly irq_time_entry = {
    .type = XEN_HYPFS_TYPE_UINT,
    .encoding = XEN_HYPFS_ENC_PLAIN,
    .name = "irq_time",
    .size = sizeof(uint64_t),
    .max_size = 0,
    .read = read_irq_time,
    .write = NULL,
};

struct hypfs_entry __read_mostly hyp_time_entry = {
    .type = XEN_HYPFS_TYPE_UINT,
    .encoding = XEN_HYPFS_ENC_PLAIN,
    .name = "hyp_time",
    .size = sizeof(uint64_t),
    .max_size = 0,
    .read = read_hyp_time,
    .write = NULL,
};

static int __init stats_init(void)
{
    int ret;

    hypfs_add_dir(&hypfs_root, &scheduler, true);
    hypfs_add_dir(&scheduler, &stats, true);
    ret = hypfs_add_entry(&stats, &irq_time_entry);
    if ( ret )
        return ret;
    ret = hypfs_add_entry(&stats, &hyp_time_entry);

    return ret;
}
__initcall(stats_init);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
