/*
 *  xen/drivers/cpufreq/cpufreq_meta.c
 *
 *  Copyright (C)  2019 EPAM Systems
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
 *
 */

#include <xen/cpu.h>
#include <xen/cpufreq.h>
#include <xen/cpumask.h>
#include <xen/init.h>
#include <xen/percpu.h>
#include <xen/sched.h>

#define MAX_GOVS 8
#define MAX_POLICIES 8

struct gov_meta_state {
    struct cpufreq_governor *gov;
    struct cpufreq_policy *policies[MAX_POLICIES];
};

static struct gov_meta_state enabled_govs[MAX_GOVS] =
{
    {.gov = &cpufreq_gov_vscmi},
    {.gov = &cpufreq_gov_dbs},
};

static enum cpufreq_gov_meta_mode default_mode = CPUFREQ_GOV_META_MAX;

static struct cpufreq_policy *meta_policies[MAX_POLICIES];

/*
 * Idea is to track all enabled policies and provide each sub-governor
 * with own instance of policy. In this way we will always know what
 * governor is trying to change target frequency. Index of policy in
 * `meta_policies[]` must correspond to policy index in
 * `gov_meta_state->policies[]`
 */

/* Set the new target for the given policy */
static int cpufreq_meta_update_target(int pol_idx)
{
    int gov;
    unsigned int target_freq = 0;
    unsigned int relation;
    unsigned long int target_sum;
    struct cpufreq_policy *pol;

    switch ( meta_policies[pol_idx]->meta_mode )
    {
    case CPUFREQ_GOV_META_MAX:
        relation = CPUFREQ_RELATION_L;
        target_freq = 0;
        break;
    case CPUFREQ_GOV_META_MIN:
        relation = CPUFREQ_RELATION_H;
        target_freq = UINT_MAX;
    case CPUFREQ_GOV_META_AVG:
        target_sum = 0;
        relation = CPUFREQ_RELATION_L;
        break;
    }

    for ( gov = 0; gov < MAX_GOVS; gov++ ) {
        if ( !enabled_govs[gov].gov )
            break;

        pol = enabled_govs[gov].policies[pol_idx];

        switch ( meta_policies[pol_idx]->meta_mode )
        {
        case CPUFREQ_GOV_META_MAX:
            if ( pol->cur > target_freq)
                target_freq = pol->cur;
            break;
        case CPUFREQ_GOV_META_MIN:
            if ( pol->cur < target_freq)
                target_freq = pol->cur;
            break;
        case CPUFREQ_GOV_META_AVG:
            target_sum += pol->cur;
            break;
        }
    }

    if ( meta_policies[pol_idx]->meta_mode == CPUFREQ_GOV_META_AVG )
        target_freq = target_sum / gov;

    /* Clamp */
    if ( target_freq > meta_policies[pol_idx]->max )
        target_freq = meta_policies[pol_idx]->max;

    if ( target_freq < meta_policies[pol_idx]->min )
        target_freq = meta_policies[pol_idx]->min;

    return __cpufreq_driver_target(meta_policies[pol_idx], target_freq,
                                   relation);
}

int write_meta_mode(unsigned int cpu, unsigned int mode)
{
    struct cpufreq_policy *policy;
    int pol_idx;

    if (!cpu_online(cpu) || !(policy = per_cpu(cpufreq_cpu_policy, cpu)))
        return -EINVAL;

    if (mode >= CPUFREQ_GOV_META_LAST)
        return -EINVAL;

    for ( pol_idx = 0; pol_idx < MAX_POLICIES; pol_idx++ )
        if ( meta_policies[pol_idx] == policy )
            break;

    if ( meta_policies[pol_idx] != policy )
        return -EINVAL;

    policy->meta_mode = mode;

    cpufreq_meta_update_target(pol_idx);

    return 0;
}


/* Callback that is called by any governor trying to set the freq */
static int cpufreq_meta_set_target(struct cpufreq_policy *policy,
                                   unsigned int target_freq,
                                   unsigned int relation)
{
    int gov;
    int pol;
    /* Find out which governor tries to change the freq */

    for ( gov = 0; gov < MAX_GOVS; gov++ ) {
        if ( !enabled_govs[gov].gov )
            break;
        for ( pol = 0; pol < MAX_POLICIES; pol++ )
            if (enabled_govs[gov].policies[pol] == policy)
            {
                enabled_govs[gov].policies[pol]->cur = target_freq;
                enabled_govs[gov].policies[pol]->rel = relation;

                return cpufreq_meta_update_target(pol);
            }
    }

    /* Looks like it is not our governor */
    return __cpufreq_driver_target(policy, target_freq, relation);
}

static int cpufreq_governor_meta(struct cpufreq_policy *policy,
                                 unsigned int event)
{
    int ret = 0;
    unsigned int cpu;
    int gov;
    int pol_idx;
    struct cpufreq_policy *pol;
    static int start_cnt = 0;

    if (unlikely(!policy) ||
        unlikely(!cpu_online(cpu = policy->cpu)))
        return -EINVAL;

    switch (event) {
    case CPUFREQ_GOV_START:
        if ( start_cnt++ == 0 )
            cpufreq_driver_target = cpufreq_meta_set_target;

        /* Find free policy entry */
        for ( pol_idx = 0; pol_idx < MAX_POLICIES; pol_idx++) {
            if ( !meta_policies[pol_idx] )
                break;
        }

        if ( meta_policies[pol_idx] )
            return -ENOMEM;

        meta_policies[pol_idx] = policy;

        for ( gov = 0; gov < MAX_GOVS; gov++ ) {
            if ( !enabled_govs[gov].gov )
                break;
            pol = xmalloc(struct cpufreq_policy);
            if ( !pol ) {
                ret = -ENOMEM;
                goto fault;
            }

            memcpy(pol, policy, sizeof(*pol));
            pol->governor = enabled_govs[gov].gov;
            pol->meta_mode = default_mode;
            enabled_govs[gov].policies[pol_idx] = pol;
            ret = pol->governor->governor(pol, event);
            if ( ret ) {
                printk(KERN_WARNING"Error %d during starting governor %s\n",
                       ret, enabled_govs[gov].gov->name);

                xfree(pol);
                enabled_govs[gov].policies[pol_idx] = NULL;
                goto fault;
            }
        }

        break;
    fault:
        for ( gov = 0; gov < MAX_GOVS; gov++ ) {
            if ( !enabled_govs[gov].gov )
                break;

            pol = enabled_govs[gov].policies[pol_idx];
            pol->governor->governor(pol, CPUFREQ_GOV_STOP);
            xfree(pol);
            enabled_govs[gov].policies[pol_idx] = NULL;
        }
        return ret;
    case CPUFREQ_GOV_STOP:
        /* Find our policy entry */
        for ( pol_idx = 0; pol_idx < MAX_POLICIES; pol_idx++) {
            if ( meta_policies[pol_idx] == policy )
                break;
        }

        if ( meta_policies[pol_idx] != policy )
            BUG();

        for ( gov = 0; gov < MAX_GOVS; gov++ ) {
            if ( !enabled_govs[gov].gov )
                break;

            pol = enabled_govs[gov].policies[pol_idx];
            ret = pol->governor->governor(pol, event);
            if ( ret )
                printk(KERN_WARNING"Error %d during stopping governor %s\n",
                       ret, enabled_govs[gov].gov->name);

            xfree(pol);
            enabled_govs[gov].policies[pol_idx] = NULL;
        }

        meta_policies[pol_idx] = NULL;

        start_cnt--;
        BUG_ON(start_cnt < 0);
        if ( start_cnt == 0 )
            cpufreq_driver_target = __cpufreq_driver_target;

        break;
    case CPUFREQ_GOV_LIMITS:
        break;
    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

struct cpufreq_governor cpufreq_gov_meta = {
    .name = "meta",
    .governor = cpufreq_governor_meta,
};

static int __init cpufreq_gov_meta_init(void)
{
    return cpufreq_register_governor(&cpufreq_gov_meta);
}
__initcall(cpufreq_gov_meta_init);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
