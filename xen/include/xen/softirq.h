#ifndef XEN_SOFTIRQ_H
#define XEN_SOFTIRQ_H

#ifndef __ASSEMBLY__

/* Low-latency softirqs come first in the following list. */
enum {
    TIMER_SOFTIRQ = 0,
    RCU_SOFTIRQ,
    SCHED_SLAVE_SOFTIRQ,
    SCHEDULE_SOFTIRQ,
    NEW_TLBFLUSH_CLOCK_PERIOD_SOFTIRQ,
    TASKLET_SOFTIRQ,
    NR_COMMON_SOFTIRQS
};

#include <xen/lib.h>
#include <xen/smp.h>
#include <xen/bitops.h>
#include <asm/current.h>
#include <asm/hardirq.h>
#include <asm/softirq.h>

#define NR_SOFTIRQS (NR_COMMON_SOFTIRQS + NR_ARCH_SOFTIRQS)

typedef void (*softirq_handler)(void);

void do_softirq(void);
void open_softirq(int nr, softirq_handler handler);

void cpumask_raise_softirq(const cpumask_t *mask, unsigned int nr);
void cpu_raise_softirq(unsigned int cpu, unsigned int nr);
void raise_softirq(unsigned int nr);

void cpu_raise_softirq_batch_begin(void);
void cpu_raise_softirq_batch_finish(void);

/*
 * Process pending softirqs on this CPU. This should be called periodically
 * when performing work that prevents softirqs from running in a timely manner.
 * Use this instead of do_softirq() when you do not want to be preempted.
 */
void process_pending_softirqs(void);

#endif /* __ASSEMBLY__ */

#endif /* XEN_SOFTIRQ_H */
