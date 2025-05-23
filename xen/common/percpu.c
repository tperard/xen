/* SPDX-License-Identifier: GPL-2.0-only */
#include <xen/cpu.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/numa.h>
#include <xen/percpu.h>
#include <xen/rcupdate.h>
#include <xen/sections.h>

#ifndef INVALID_PERCPU_AREA
#define INVALID_PERCPU_AREA (-(long)__per_cpu_start)
#endif

#define PERCPU_ORDER get_order_from_bytes(__per_cpu_data_end - __per_cpu_start)

extern char __per_cpu_start[];
extern const char __per_cpu_data_end[];

unsigned long __read_mostly __per_cpu_offset[NR_CPUS];

void __init percpu_init_areas(void)
{
    unsigned int cpu;

    for ( cpu = 1; cpu < NR_CPUS; cpu++ )
        __per_cpu_offset[cpu] = INVALID_PERCPU_AREA;
}

static int init_percpu_area(unsigned int cpu)
{
    nodeid_t node = cpu_to_node(cpu);
    unsigned int memflags = node != NUMA_NO_NODE ? MEMF_node(node) : 0;
    char *p;

    if ( __per_cpu_offset[cpu] != INVALID_PERCPU_AREA )
        return park_offline_cpus || system_state == SYS_STATE_resume
               ? 0
               : -EBUSY;

    if ( (p = alloc_xenheap_pages(PERCPU_ORDER, memflags)) == NULL )
        return -ENOMEM;

    memset(p, 0, __per_cpu_data_end - __per_cpu_start);
    __per_cpu_offset[cpu] = p - __per_cpu_start;

    return 0;
}

struct free_info {
    unsigned int cpu;
    struct rcu_head rcu;
};
static DEFINE_PER_CPU(struct free_info, free_info);

static void cf_check _free_percpu_area(struct rcu_head *head)
{
    struct free_info *info = container_of(head, struct free_info, rcu);
    unsigned int cpu = info->cpu;
    char *p = __per_cpu_start + __per_cpu_offset[cpu];

    free_xenheap_pages(p, PERCPU_ORDER);
    __per_cpu_offset[cpu] = INVALID_PERCPU_AREA;
}

static void free_percpu_area(unsigned int cpu)
{
    struct free_info *info = &per_cpu(free_info, cpu);

    info->cpu = cpu;
    call_rcu(&info->rcu, _free_percpu_area);
}

static int cf_check cpu_percpu_callback(
    struct notifier_block *nfb, unsigned long action, void *hcpu)
{
    unsigned int cpu = (unsigned long)hcpu;
    int rc = 0;

    switch ( action )
    {
    case CPU_UP_PREPARE:
        rc = init_percpu_area(cpu);
        break;

    case CPU_UP_CANCELED:
    case CPU_DEAD:
    case CPU_RESUME_FAILED:
        if ( !park_offline_cpus && system_state != SYS_STATE_suspend )
            free_percpu_area(cpu);
        break;

    case CPU_REMOVE:
        if ( park_offline_cpus )
            free_percpu_area(cpu);
        break;

    default:
        break;
    }

    return notifier_from_errno(rc);
}

static struct notifier_block cpu_percpu_nfb = {
    .notifier_call = cpu_percpu_callback,
    .priority = 100 /* highest priority */
};

static int __init cf_check percpu_presmp_init(void)
{
    register_cpu_notifier(&cpu_percpu_nfb);

    return 0;
}
presmp_initcall(percpu_presmp_init);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
