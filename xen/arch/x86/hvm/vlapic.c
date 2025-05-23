/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vlapic.c: virtualize LAPIC for HVM vcpus.
 *
 * Copyright (c) 2004, Intel Corporation.
 * Copyright (c) 2006 Keir Fraser, XenSource Inc.
 */

#include <xen/types.h>
#include <xen/mm.h>
#include <xen/xmalloc.h>
#include <xen/domain.h>
#include <xen/domain_page.h>
#include <xen/event.h>
#include <xen/nospec.h>
#include <xen/trace.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/numa.h>
#include <asm/current.h>
#include <asm/page.h>
#include <asm/apic.h>
#include <asm/io_apic.h>
#include <asm/vpmu.h>
#include <asm/hvm/emulate.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/io.h>
#include <asm/hvm/support.h>
#include <asm/hvm/nestedhvm.h>
#include <asm/hvm/viridian.h>
#include <public/hvm/ioreq.h>
#include <public/hvm/params.h>

#define VLAPIC_VERSION                  0x00050014
#define VLAPIC_LVT_NUM                  6

#define LVT_MASK \
    (APIC_LVT_MASKED | APIC_SEND_PENDING | APIC_VECTOR_MASK)

#define LINT_MASK   \
    (LVT_MASK | APIC_DM_MASK | APIC_INPUT_POLARITY |\
    APIC_LVT_REMOTE_IRR | APIC_LVT_LEVEL_TRIGGER)

static const unsigned int vlapic_lvt_mask[VLAPIC_LVT_NUM] =
{
     /* LVTT */
     LVT_MASK | APIC_TIMER_MODE_MASK,
     /* LVTTHMR */
     LVT_MASK | APIC_DM_MASK,
     /* LVTPC */
     LVT_MASK | APIC_DM_MASK,
     /* LVT0-1 */
     LINT_MASK, LINT_MASK,
     /* LVTERR */
     LVT_MASK
};

#define vlapic_lvtt_period(vlapic)                              \
    ((vlapic_get_reg(vlapic, APIC_LVTT) & APIC_TIMER_MODE_MASK) \
     == APIC_TIMER_MODE_PERIODIC)

#define vlapic_lvtt_oneshot(vlapic)                             \
    ((vlapic_get_reg(vlapic, APIC_LVTT) & APIC_TIMER_MODE_MASK) \
     == APIC_TIMER_MODE_ONESHOT)

#define vlapic_lvtt_tdt(vlapic)                                 \
    ((vlapic_get_reg(vlapic, APIC_LVTT) & APIC_TIMER_MODE_MASK) \
     == APIC_TIMER_MODE_TSC_DEADLINE)

static void vlapic_do_init(struct vlapic *vlapic);

static int vlapic_find_highest_vector(const void *bitmap)
{
    const uint32_t *word = bitmap;
    unsigned int word_offset = X86_IDT_VECTORS / 32;

    /* Work backwards through the bitmap (first 32-bit word in every four). */
    while ( (word_offset != 0) && (word[(--word_offset)*4] == 0) )
        continue;

    return (fls(word[word_offset*4]) - 1) + (word_offset * 32);
}

/*
 * IRR-specific bitmap update & search routines.
 */

static int vlapic_test_and_set_irr(int vector, struct vlapic *vlapic)
{
    return vlapic_test_and_set_vector(vector, &vlapic->regs->data[APIC_IRR]);
}

static void vlapic_clear_irr(int vector, struct vlapic *vlapic)
{
    vlapic_clear_vector(vector, &vlapic->regs->data[APIC_IRR]);
}

static int vlapic_find_highest_irr(struct vlapic *vlapic)
{
    hvm_sync_pir_to_irr(vlapic_vcpu(vlapic));

    return vlapic_find_highest_vector(&vlapic->regs->data[APIC_IRR]);
}

static void vlapic_error(struct vlapic *vlapic, unsigned int err_bit)
{
    /*
     * Whether LVTERR is delivered on a per-bit basis, or only on
     * pending_esr becoming nonzero is implementation specific.
     *
     * Xen implements the per-bit behaviour as it can be expressed
     * locklessly.
     */
    if ( !test_and_set_bit(err_bit, &vlapic->hw.pending_esr) )
    {
        uint32_t lvterr = vlapic_get_reg(vlapic, APIC_LVTERR);
        bool inj = false;

        if ( !(lvterr & APIC_LVT_MASKED) )
        {
            /*
             * If LVTERR is unmasked and has an illegal vector, vlapic_set_irq()
             * will end up back here.  Break the cycle by only injecting LVTERR
             * if it will succeed, and folding in RECVILL otherwise.
             */
            if ( APIC_VECTOR_VALID(lvterr) )
                inj = true;
            else
                set_bit(ilog2(APIC_ESR_RECVILL), &vlapic->hw.pending_esr);
        }

        if ( inj )
            vlapic_set_irq(vlapic, lvterr & APIC_VECTOR_MASK, 0);
    }
}

bool vlapic_test_irq(const struct vlapic *vlapic, uint8_t vec)
{
    if ( unlikely(!APIC_VECTOR_VALID(vec)) )
        return false;

    if ( hvm_funcs.test_pir &&
         alternative_call(hvm_funcs.test_pir, const_vlapic_vcpu(vlapic), vec) )
        return true;

    return vlapic_test_vector(vec, &vlapic->regs->data[APIC_IRR]);
}

void vlapic_set_irq(struct vlapic *vlapic, uint8_t vec, uint8_t trig)
{
    struct vcpu *target = vlapic_vcpu(vlapic);

    if ( unlikely(!APIC_VECTOR_VALID(vec)) )
    {
        vlapic_error(vlapic, ilog2(APIC_ESR_RECVILL));
        return;
    }

    if ( trig )
        vlapic_set_vector(vec, &vlapic->regs->data[APIC_TMR]);
    else
        vlapic_clear_vector(vec, &vlapic->regs->data[APIC_TMR]);

    if ( hvm_funcs.update_eoi_exit_bitmap )
        alternative_vcall(hvm_funcs.update_eoi_exit_bitmap, target, vec, trig);

    if ( hvm_funcs.deliver_posted_intr )
        alternative_vcall(hvm_funcs.deliver_posted_intr, target, vec);
    else if ( !vlapic_test_and_set_irr(vec, vlapic) )
        vcpu_kick(target);
}

static int vlapic_find_highest_isr(const struct vlapic *vlapic)
{
    return vlapic_find_highest_vector(&vlapic->regs->data[APIC_ISR]);
}

static uint32_t vlapic_get_ppr(const struct vlapic *vlapic)
{
    uint32_t tpr, isrv, ppr;
    int isr;

    tpr  = vlapic_get_reg(vlapic, APIC_TASKPRI);
    isr  = vlapic_find_highest_isr(vlapic);
    isrv = (isr != -1) ? isr : 0;

    if ( (tpr & 0xf0) >= (isrv & 0xf0) )
        ppr = tpr & 0xff;
    else
        ppr = isrv & 0xf0;

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC_INTERRUPT,
                "vlapic %p, ppr %#x, isr %#x, isrv %#x",
                vlapic, ppr, isr, isrv);

    return ppr;
}

uint32_t vlapic_set_ppr(struct vlapic *vlapic)
{
   uint32_t ppr = vlapic_get_ppr(vlapic);

   vlapic_set_reg(vlapic, APIC_PROCPRI, ppr);
   return ppr;
}

static bool vlapic_match_logical_addr(const struct vlapic *vlapic, uint32_t mda)
{
    bool result = false;
    uint32_t logical_id = vlapic_get_reg(vlapic, APIC_LDR);

    if ( vlapic_x2apic_mode(vlapic) )
        return ((logical_id >> 16) == (mda >> 16)) &&
               (uint16_t)(logical_id & mda);

    logical_id = GET_xAPIC_LOGICAL_ID(logical_id);
    mda = (uint8_t)mda;

    switch ( vlapic_get_reg(vlapic, APIC_DFR) )
    {
    case APIC_DFR_FLAT:
        if ( logical_id & mda )
            result = true;
        break;
    case APIC_DFR_CLUSTER:
        if ( ((logical_id >> 4) == (mda >> 0x4)) && (logical_id & mda & 0xf) )
            result = true;
        break;
    default:
        printk(XENLOG_G_WARNING "%pv: bad LAPIC DFR value %08x\n",
               const_vlapic_vcpu(vlapic),
               vlapic_get_reg(vlapic, APIC_DFR));
        break;
    }

    return result;
}

bool vlapic_match_dest(
    const struct vlapic *target, const struct vlapic *source,
    int short_hand, uint32_t dest, bool dest_mode)
{
    HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "target %p, source %p, dest %#x, "
                "dest_mode %#x, short_hand %#x",
                target, source, dest, dest_mode, short_hand);

    switch ( short_hand )
    {
    case APIC_DEST_NOSHORT:
        if ( dest_mode )
            return vlapic_match_logical_addr(target, dest);
        return (dest == _VLAPIC_ID(target, 0xffffffffU)) ||
               (dest == VLAPIC_ID(target));

    case APIC_DEST_SELF:
        return (target == source);

    case APIC_DEST_ALLINC:
        return true;

    case APIC_DEST_ALLBUT:
        return (target != source);

    default:
        gdprintk(XENLOG_WARNING, "Bad dest shorthand value %x\n", short_hand);
        break;
    }

    return false;
}

static void vlapic_init_sipi_one(struct vcpu *target, uint32_t icr)
{
    vcpu_pause(target);

    switch ( icr & APIC_DM_MASK )
    {
    case APIC_DM_INIT: {
        bool fpu_initialised;
        int rc;

        /* No work on INIT de-assert for P4-type APIC. */
        if ( (icr & (APIC_INT_LEVELTRIG | APIC_INT_ASSERT)) ==
             APIC_INT_LEVELTRIG )
            break;
        /* Nothing to do if the VCPU is already reset. */
        if ( !target->is_initialised )
            break;
        hvm_vcpu_down(target);
        domain_lock(target->domain);
        /* Reset necessary VCPU state. This does not include FPU state. */
        fpu_initialised = target->fpu_initialised;
        rc = vcpu_reset(target);
        ASSERT(!rc);
        target->fpu_initialised = fpu_initialised;
        vlapic_do_init(vcpu_vlapic(target));
        domain_unlock(target->domain);
        break;
    }

    case APIC_DM_STARTUP: {
        uint16_t reset_cs = (icr & 0xffu) << 8;
        hvm_vcpu_reset_state(target, reset_cs, 0);
        break;
    }

    default:
        BUG();
    }

    hvmemul_cancel(target);

    vcpu_unpause(target);
}

static void cf_check vlapic_init_sipi_action(void *data)
{
    struct vcpu *origin = data;
    uint32_t icr = vcpu_vlapic(origin)->init_sipi.icr;
    uint32_t dest = vcpu_vlapic(origin)->init_sipi.dest;
    uint32_t short_hand = icr & APIC_SHORT_MASK;
    bool dest_mode = icr & APIC_DEST_MASK;
    struct vcpu *v;

    if ( icr == 0 )
        return;

    for_each_vcpu ( origin->domain, v )
    {
        if ( vlapic_match_dest(vcpu_vlapic(v), vcpu_vlapic(origin),
                               short_hand, dest, dest_mode) )
            vlapic_init_sipi_one(v, icr);
    }

    vcpu_vlapic(origin)->init_sipi.icr = 0;
    vcpu_unpause(origin);
}

/* Add a pending IRQ into lapic. */
static void vlapic_accept_irq(struct vcpu *v, uint32_t icr_low)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    uint8_t vector = (uint8_t)icr_low;

    switch ( icr_low & APIC_DM_MASK )
    {
    case APIC_DM_FIXED:
    case APIC_DM_LOWEST:
        if ( vlapic_enabled(vlapic) )
            vlapic_set_irq(vlapic, vector, 0);
        break;

    case APIC_DM_REMRD:
        gdprintk(XENLOG_WARNING, "Ignoring delivery mode 3\n");
        break;

    case APIC_DM_SMI:
        gdprintk(XENLOG_WARNING, "Ignoring guest SMI\n");
        break;

    case APIC_DM_NMI:
        if ( !test_and_set_bool(v->arch.nmi_pending) )
        {
            bool wake = false;

            domain_lock(v->domain);
            if ( v->is_initialised )
                wake = test_and_clear_bit(_VPF_down, &v->pause_flags);
            domain_unlock(v->domain);
            if ( wake )
                vcpu_wake(v);
            vcpu_kick(v);
        }
        break;

    case APIC_DM_INIT:
    case APIC_DM_STARTUP:
        BUG(); /* Handled in vlapic_ipi(). */

    default:
        gdprintk(XENLOG_ERR, "TODO: unsupported delivery mode in ICR %x\n",
                 icr_low);
        domain_crash(v->domain);
        break;
    }
}

struct vlapic *vlapic_lowest_prio(
    struct domain *d, const struct vlapic *source,
    int short_hand, uint32_t dest, bool dest_mode)
{
    int old = hvm_domain_irq(d)->round_robin_prev_vcpu;
    uint32_t ppr, target_ppr = UINT_MAX;
    struct vlapic *vlapic, *target = NULL;
    struct vcpu *v;

    if ( unlikely(!d->vcpu) || unlikely((v = d->vcpu[old]) == NULL) )
        return NULL;

    do {
        v = v->next_in_list ? : d->vcpu[0];
        vlapic = vcpu_vlapic(v);
        if ( vlapic_match_dest(vlapic, source, short_hand, dest, dest_mode) &&
             vlapic_enabled(vlapic) &&
             ((ppr = vlapic_get_ppr(vlapic)) < target_ppr) )
        {
            target = vlapic;
            target_ppr = ppr;
        }
    } while ( v->vcpu_id != old );

    if ( target != NULL )
        hvm_domain_irq(d)->round_robin_prev_vcpu =
           vlapic_vcpu(target)->vcpu_id;

    return target;
}

void vlapic_EOI_set(struct vlapic *vlapic)
{
    struct vcpu *v = vlapic_vcpu(vlapic);
    /*
     * If APIC assist was set then an EOI may have been avoided and
     * hence this EOI actually relates to a lower priority vector.
     * Thus it is necessary to first emulate the EOI for the higher
     * priority vector and then recurse to handle the lower priority
     * vector.
     */
    bool missed_eoi = viridian_apic_assist_completed(v);
    int vector;

 again:
    vector = vlapic_find_highest_isr(vlapic);

    /* Some EOI writes may not have a matching to an in-service interrupt. */
    if ( vector == -1 )
        return;

    /*
     * If APIC assist was set but the guest chose to EOI anyway then
     * we need to clean up state.
     * NOTE: It is harmless to call viridian_apic_assist_clear() on a
     *       recursion, even though it is not necessary.
     */
    if ( !missed_eoi )
        viridian_apic_assist_clear(v);

    vlapic_clear_vector(vector, &vlapic->regs->data[APIC_ISR]);

    if ( hvm_funcs.handle_eoi )
        alternative_vcall(hvm_funcs.handle_eoi, vector,
                          vlapic_find_highest_isr(vlapic));

    vlapic_handle_EOI(vlapic, vector);

    if ( missed_eoi )
    {
        missed_eoi = false;
        goto again;
    }
}

void vlapic_handle_EOI(struct vlapic *vlapic, u8 vector)
{
    struct vcpu *v = vlapic_vcpu(vlapic);
    struct domain *d = v->domain;

    if ( vlapic_test_vector(vector, &vlapic->regs->data[APIC_TMR]) )
        vioapic_update_EOI(d, vector);

    hvm_dpci_msi_eoi(d, vector);
}

static bool is_multicast_dest(struct vlapic *vlapic, unsigned int short_hand,
                              uint32_t dest, bool dest_mode)
{
    if ( vlapic_domain(vlapic)->max_vcpus <= 2 )
        return false;

    if ( short_hand )
        return short_hand != APIC_DEST_SELF;

    if ( vlapic_x2apic_mode(vlapic) )
        return dest_mode ? multiple_bits_set((uint16_t)dest)
                         : dest == 0xffffffffU;

    if ( dest_mode )
    {
        dest &= GET_xAPIC_DEST_FIELD(vlapic_get_reg(vlapic, APIC_DFR));
        return multiple_bits_set((uint8_t)dest);
    }

    return dest == 0xff;
}

void vlapic_ipi(
    struct vlapic *vlapic, uint32_t icr_low, uint32_t icr_high)
{
    unsigned int dest;
    unsigned int short_hand = icr_low & APIC_SHORT_MASK;
    bool dest_mode = icr_low & APIC_DEST_MASK;

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "icr = 0x%08x:%08x", icr_high, icr_low);

    dest = _VLAPIC_ID(vlapic, icr_high);

    switch ( icr_low & APIC_DM_MASK )
    {
    case APIC_DM_INIT:
    case APIC_DM_STARTUP:
        if ( vlapic->init_sipi.icr != 0 )
        {
            WARN(); /* should be impossible but don't BUG, just in case */
            break;
        }
        vcpu_pause_nosync(vlapic_vcpu(vlapic));
        vlapic->init_sipi.icr = icr_low;
        vlapic->init_sipi.dest = dest;
        tasklet_schedule(&vlapic->init_sipi.tasklet);
        break;

    case APIC_DM_LOWEST: {
        struct vlapic *target = vlapic_lowest_prio(
            vlapic_domain(vlapic), vlapic, short_hand, dest, dest_mode);

        if ( unlikely(!APIC_VECTOR_VALID(icr_low)) )
            vlapic_error(vlapic, ilog2(APIC_ESR_SENDILL));
        else if ( target )
            vlapic_accept_irq(vlapic_vcpu(target), icr_low);
        break;
    }

    case APIC_DM_FIXED:
        if ( unlikely(!APIC_VECTOR_VALID(icr_low)) )
        {
            vlapic_error(vlapic, ilog2(APIC_ESR_SENDILL));
            break;
        }
        /* fall through */
    default: {
        struct vcpu *v;
        bool batch = is_multicast_dest(vlapic, short_hand, dest, dest_mode);

        if ( batch )
            cpu_raise_softirq_batch_begin();
        for_each_vcpu ( vlapic_domain(vlapic), v )
        {
            if ( vlapic_match_dest(vcpu_vlapic(v), vlapic,
                                   short_hand, dest, dest_mode) )
                vlapic_accept_irq(v, icr_low);
        }
        if ( batch )
            cpu_raise_softirq_batch_finish();
        break;
    }
    }
}

static uint32_t vlapic_get_tmcct(const struct vlapic *vlapic)
{
    const struct vcpu *v = const_vlapic_vcpu(vlapic);
    uint32_t tmcct = 0, tmict = vlapic_get_reg(vlapic, APIC_TMICT);
    uint64_t counter_passed;

    counter_passed = ((hvm_get_guest_time(v) - vlapic->timer_last_update)
                      / (APIC_BUS_CYCLE_NS * vlapic->hw.timer_divisor));

    /* If timer_last_update is 0, then TMCCT should return 0 as well.  */
    if ( tmict && vlapic->timer_last_update )
    {
        if ( vlapic_lvtt_period(vlapic) )
            counter_passed %= tmict;
        if ( counter_passed < tmict )
            tmcct = tmict - counter_passed;
    }

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER,
                "timer initial count %d, timer current count %d, "
                "offset %"PRId64,
                tmict, tmcct, counter_passed);

    return tmcct;
}

static void vlapic_set_tdcr(struct vlapic *vlapic, unsigned int val)
{
    /* Only bits 0, 1 and 3 are settable; others are MBZ. */
    val &= APIC_TDR_DIV_MASK;
    vlapic_set_reg(vlapic, APIC_TDCR, val);

    /* Update the demangled hw.timer_divisor. */
    val = ((val & 3) | ((val & 8) >> 1)) + 1;
    vlapic->hw.timer_divisor = 1 << (val & 7);

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER,
                "timer_divisor: %d", vlapic->hw.timer_divisor);
}

static uint32_t vlapic_read_aligned(const struct vlapic *vlapic,
                                    unsigned int offset)
{
    switch ( offset )
    {
    case APIC_PROCPRI:
        return vlapic_get_ppr(vlapic);

    case APIC_TMCCT: /* Timer CCR */
        if ( !vlapic_lvtt_oneshot(vlapic) && !vlapic_lvtt_period(vlapic) )
            break;
        return vlapic_get_tmcct(vlapic);

    case APIC_TMICT: /* Timer ICR */
        if ( !vlapic_lvtt_oneshot(vlapic) && !vlapic_lvtt_period(vlapic) )
            break;
        /* fall through */
    default:
        return vlapic_get_reg(vlapic, offset);
    }

    return 0;
}

static int cf_check vlapic_mmio_read(
    struct vcpu *v, unsigned long address, unsigned int len,
    unsigned long *pval)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    unsigned int offset = address - vlapic_base_address(vlapic);
    unsigned int alignment = offset & 0xf, result = 0;

    /*
     * APIC registers are 32-bit values, aligned on 128-bit boundaries, and
     * should be accessed with 32-bit wide loads.
     *
     * Some processors support smaller accesses, so we allow any access which
     * fully fits within the 32-bit register.
     */
    if ( (alignment + len) <= 4 && offset <= (APIC_TDCR + 3) )
    {
        uint32_t reg = vlapic_read_aligned(vlapic, offset & ~0xf);

        switch ( len )
        {
        case 1: result = (uint8_t) (reg >> (alignment * 8)); break;
        case 2: result = (uint16_t)(reg >> (alignment * 8)); break;
        case 4: result = reg;                                break;
        }

        HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "offset %#x with length %#x, "
                    "and the result is %#x", offset, len, result);
    }

    *pval = result;
    return X86EMUL_OKAY;
}

int guest_rdmsr_x2apic(const struct vcpu *v, uint32_t msr, uint64_t *val)
{
    static const unsigned long readable[] = {
#define REG(x) (1UL << (APIC_ ## x >> 4))
        REG(ID)    | REG(LVR)  | REG(TASKPRI) | REG(PROCPRI) |
        REG(LDR)   | REG(SPIV) | REG(ESR)     | REG(ICR)     |
        REG(CMCI)  | REG(LVTT) | REG(LVTTHMR) | REG(LVTPC)   |
        REG(LVT0)  | REG(LVT1) | REG(LVTERR)  | REG(TMICT)   |
        REG(TMCCT) | REG(TDCR) |
#undef REG
#define REGBLOCK(x) (((1UL << (X86_IDT_VECTORS / 32)) - 1) << (APIC_ ## x >> 4))
        REGBLOCK(ISR) | REGBLOCK(TMR) | REGBLOCK(IRR)
#undef REGBLOCK
    };
    const struct vlapic *vlapic = vcpu_vlapic(v);
    uint64_t high = 0;
    uint32_t reg = msr - MSR_X2APIC_FIRST, offset;

    /*
     * The read side looks as if it might be safe to use outside of current
     * context, but the write side is most certainly not.  As we don't need
     * any non-current access, enforce symmetry with the write side.
     */
    ASSERT(v == current);

    if ( !vlapic_x2apic_mode(vlapic) ||
         (reg >= sizeof(readable) * 8) )
        return X86EMUL_EXCEPTION;

    reg = array_index_nospec(reg, sizeof(readable) * 8);
    if ( !test_bit(reg, readable) )
        return X86EMUL_EXCEPTION;

    offset = reg << 4;
    if ( offset == APIC_ICR )
        high = (uint64_t)vlapic_read_aligned(vlapic, APIC_ICR2) << 32;

    *val = high | vlapic_read_aligned(vlapic, offset);

    return X86EMUL_OKAY;
}

static void cf_check vlapic_pt_cb(struct vcpu *v, void *data)
{
    TRACE_TIME(TRC_HVM_EMUL_LAPIC_TIMER_CB);
    *(s_time_t *)data = hvm_get_guest_time(v);
}

static void cf_check vlapic_tdt_pt_cb(struct vcpu *v, void *data)
{
    *(s_time_t *)data = hvm_get_guest_time(v);
    vcpu_vlapic(v)->hw.tdt_msr = 0;
}

/*
 * This function is used when a register related to the APIC timer is updated.
 * It expects the new value for the register TMICT to be set *before*
 * being called, and the previous value of the divisor (calculated from TDCR)
 * to be passed as argument.
 * It expect the new value of LVTT to be set *after* being called, with this
 * new values passed as parameter (only APIC_TIMER_MODE_MASK bits matter).
 */
static void vlapic_update_timer(struct vlapic *vlapic, uint32_t lvtt,
                                bool tmict_updated, uint32_t old_divisor)
{
    uint64_t period, delta = 0;
    bool is_oneshot, is_periodic;

    is_periodic = (lvtt & APIC_TIMER_MODE_MASK) == APIC_TIMER_MODE_PERIODIC;
    is_oneshot = (lvtt & APIC_TIMER_MODE_MASK) == APIC_TIMER_MODE_ONESHOT;

    period = (uint64_t)vlapic_get_reg(vlapic, APIC_TMICT)
        * APIC_BUS_CYCLE_NS * old_divisor;

    /* Calculate the next time the timer should trigger an interrupt. */
    if ( tmict_updated )
        delta = period;
    else if ( period && vlapic->timer_last_update )
    {
        uint64_t time_passed = hvm_get_guest_time(current)
            - vlapic->timer_last_update;

        /* This depends of the previous mode, if a new mode is being set */
        if ( vlapic_lvtt_period(vlapic) )
            time_passed %= period;
        if ( time_passed < period )
            delta = period - time_passed;
    }

    if ( delta && (is_oneshot || is_periodic) )
    {
        uint64_t timer_period = 0;

        if ( vlapic->hw.timer_divisor != old_divisor )
        {
            period = (uint64_t)vlapic_get_reg(vlapic, APIC_TMICT)
                * APIC_BUS_CYCLE_NS * vlapic->hw.timer_divisor;
            delta = delta * vlapic->hw.timer_divisor / old_divisor;
        }

        if ( is_periodic )
            timer_period = period;

        TRACE_TIME(TRC_HVM_EMUL_LAPIC_START_TIMER, delta, delta >> 32,
                   timer_period, timer_period >> 32, vlapic->pt.irq);

        create_periodic_time(current, &vlapic->pt, delta,
                             timer_period, vlapic->pt.irq,
                             is_periodic ? vlapic_pt_cb : NULL,
                             &vlapic->timer_last_update, false);

        vlapic->timer_last_update = vlapic->pt.last_plt_gtime;
        if ( !tmict_updated )
            vlapic->timer_last_update -= period - delta;

        HVM_DBG_LOG(DBG_LEVEL_VLAPIC,
                    "bus cycle is %uns, "
                    "initial count %u, period %"PRIu64"ns",
                    APIC_BUS_CYCLE_NS,
                    vlapic_get_reg(vlapic, APIC_TMICT),
                    period);
    }
    else
    {
        TRACE_TIME(TRC_HVM_EMUL_LAPIC_STOP_TIMER);
        destroy_periodic_time(&vlapic->pt);
        /*
         * From now, TMCCT should return 0 until TMICT is set again.
         * This is because the timer mode was one-shot when the counter reach 0
         * or just because the timer is disable.
         */
        vlapic->timer_last_update = 0;
    }
}

void vlapic_reg_write(struct vcpu *v, unsigned int reg, uint32_t val)
{
    struct vlapic *vlapic = vcpu_vlapic(v);

    memset(&vlapic->loaded, 0, sizeof(vlapic->loaded));

    switch ( reg )
    {
    case APIC_ID:
        vlapic_set_reg(vlapic, APIC_ID, val);
        break;

    case APIC_ESR:
        val = xchg(&vlapic->hw.pending_esr, 0);
        vlapic_set_reg(vlapic, APIC_ESR, val);
        break;

    case APIC_TASKPRI:
        vlapic_set_reg(vlapic, APIC_TASKPRI, val & 0xff);
        break;

    case APIC_EOI:
        vlapic_EOI_set(vlapic);
        break;

    case APIC_LDR:
        vlapic_set_reg(vlapic, APIC_LDR, val & APIC_LDR_MASK);
        break;

    case APIC_DFR:
        vlapic_set_reg(vlapic, APIC_DFR, val | 0x0FFFFFFF);
        break;

    case APIC_SPIV:
        vlapic_set_reg(vlapic, APIC_SPIV, val & 0x3ff);

        if ( !(val & APIC_SPIV_APIC_ENABLED) )
        {
            int i;
            uint32_t lvt_val;

            vlapic->hw.disabled |= VLAPIC_SW_DISABLED;

            for ( i = 0; i < VLAPIC_LVT_NUM; i++ )
            {
                lvt_val = vlapic_get_reg(vlapic, APIC_LVTT + 0x10 * i);
                vlapic_set_reg(vlapic, APIC_LVTT + 0x10 * i,
                               lvt_val | APIC_LVT_MASKED);
            }
        }
        else
        {
            vlapic->hw.disabled &= ~VLAPIC_SW_DISABLED;
            pt_may_unmask_irq(vlapic_domain(vlapic), &vlapic->pt);
            if ( v->arch.hvm.evtchn_upcall_vector &&
                 vcpu_info(v, evtchn_upcall_pending) )
                vlapic_set_irq(vlapic, v->arch.hvm.evtchn_upcall_vector, 0);
        }
        break;

    case APIC_ICR:
        val &= ~(1 << 12); /* always clear the pending bit */
        vlapic_ipi(vlapic, val, vlapic_get_reg(vlapic, APIC_ICR2));
        vlapic_set_reg(vlapic, APIC_ICR, val);
        break;

    case APIC_ICR2:
        vlapic_set_reg(vlapic, APIC_ICR2, val & 0xff000000U);
        break;

    case APIC_LVTT:         /* LVT Timer Reg */
        if ( vlapic_lvtt_tdt(vlapic) !=
             ((val & APIC_TIMER_MODE_MASK) == APIC_TIMER_MODE_TSC_DEADLINE) )
        {
            vlapic_set_reg(vlapic, APIC_TMICT, 0);
            vlapic->hw.tdt_msr = 0;
        }
        vlapic->pt.irq = val & APIC_VECTOR_MASK;

        vlapic_update_timer(vlapic, val, false, vlapic->hw.timer_divisor);

        /* fallthrough */
    case APIC_LVTTHMR:      /* LVT Thermal Monitor */
    case APIC_LVTPC:        /* LVT Performance Counter */
    case APIC_LVT0:         /* LVT LINT0 Reg */
    case APIC_LVT1:         /* LVT Lint1 Reg */
    case APIC_LVTERR:       /* LVT Error Reg */
        if ( vlapic_sw_disabled(vlapic) )
            val |= APIC_LVT_MASKED;
        val &= array_access_nospec(vlapic_lvt_mask, (reg - APIC_LVTT) >> 4);
        vlapic_set_reg(vlapic, reg, val);
        if ( reg == APIC_LVT0 )
        {
            vlapic_adjust_i8259_target(v->domain);
            pt_may_unmask_irq(v->domain, NULL);
        }
        if ( (reg == APIC_LVTT) && !(val & APIC_LVT_MASKED) )
            pt_may_unmask_irq(NULL, &vlapic->pt);
        if ( reg == APIC_LVTPC )
            vpmu_lvtpc_update(val);
        break;

    case APIC_TMICT:
        if ( !vlapic_lvtt_oneshot(vlapic) && !vlapic_lvtt_period(vlapic) )
            break;

        vlapic_set_reg(vlapic, APIC_TMICT, val);

        vlapic_update_timer(vlapic, vlapic_get_reg(vlapic, APIC_LVTT), true,
                            vlapic->hw.timer_divisor);
        break;

    case APIC_TDCR:
    {
        uint32_t current_divisor = vlapic->hw.timer_divisor;

        vlapic_set_tdcr(vlapic, val);

        vlapic_update_timer(vlapic, vlapic_get_reg(vlapic, APIC_LVTT), false,
                            current_divisor);
        HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER, "timer divisor is %#x",
                    vlapic->hw.timer_divisor);
        break;
    }
    }
}

static int cf_check vlapic_mmio_write(
    struct vcpu *v, unsigned long address, unsigned int len, unsigned long val)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    unsigned int offset = address - vlapic_base_address(vlapic);
    unsigned int alignment = offset & 0xf;

    offset &= ~0xf;

    if ( offset != APIC_EOI )
        HVM_DBG_LOG(DBG_LEVEL_VLAPIC,
                    "offset %#x with length %#x, and value is %#lx",
                    offset, len, val);

    /*
     * APIC registers are 32-bit values, aligned on 128-bit boundaries, and
     * should be accessed with 32-bit wide stores.
     *
     * Some processors support smaller accesses, so we allow any access which
     * fully fits within the 32-bit register.
     */
    if ( (alignment + len) <= 4 && offset <= APIC_TDCR )
    {
        if ( unlikely(len < 4) )
        {
            uint32_t reg = vlapic_read_aligned(vlapic, offset);

            alignment *= 8;

            switch ( len )
            {
            case 1:
                val = ((reg & ~(0xffU << alignment)) |
                       ((val &  0xff) << alignment));
                break;

            case 2:
                val = ((reg & ~(0xffffU << alignment)) |
                       ((val &  0xffff) << alignment));
                break;
            }
        }

        vlapic_reg_write(v, offset, val);
    }

    return X86EMUL_OKAY;
}

int vlapic_apicv_write(struct vcpu *v, unsigned int offset)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    uint32_t val = vlapic_get_reg(vlapic, offset & ~0xf);

    if ( vlapic_x2apic_mode(vlapic) )
    {
        if ( offset != APIC_SELF_IPI )
            return X86EMUL_UNHANDLEABLE;

        offset = APIC_ICR;
        val = APIC_DEST_SELF | (val & APIC_VECTOR_MASK);
    }

    vlapic_reg_write(v, offset, val);

    return X86EMUL_OKAY;
}

int guest_wrmsr_x2apic(struct vcpu *v, uint32_t msr, uint64_t val)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    uint32_t offset = (msr - MSR_X2APIC_FIRST) << 4;

    /* The timer handling at least is unsafe outside of current context. */
    ASSERT(v == current);

    if ( !vlapic_x2apic_mode(vlapic) )
        return X86EMUL_EXCEPTION;

    switch ( offset )
    {
    case APIC_TASKPRI:
        if ( val & ~APIC_TPRI_MASK )
            return X86EMUL_EXCEPTION;
        break;

    case APIC_SPIV:
        if ( val & ~(APIC_VECTOR_MASK | APIC_SPIV_APIC_ENABLED |
                     APIC_SPIV_FOCUS_DISABLED |
                     (VLAPIC_VERSION & APIC_LVR_DIRECTED_EOI
                      ? APIC_SPIV_DIRECTED_EOI : 0)) )
            return X86EMUL_EXCEPTION;
        break;

    case APIC_LVTT:
        if ( val & ~(LVT_MASK | APIC_TIMER_MODE_MASK) )
            return X86EMUL_EXCEPTION;
        break;

    case APIC_LVTTHMR:
    case APIC_LVTPC:
    case APIC_CMCI:
        if ( val & ~(LVT_MASK | APIC_DM_MASK) )
            return X86EMUL_EXCEPTION;
        break;

    case APIC_LVT0:
    case APIC_LVT1:
        if ( val & ~LINT_MASK )
            return X86EMUL_EXCEPTION;
        break;

    case APIC_LVTERR:
        if ( val & ~LVT_MASK )
            return X86EMUL_EXCEPTION;
        break;

    case APIC_TMICT:
        break;

    case APIC_TDCR:
        if ( val & ~APIC_TDR_DIV_MASK )
            return X86EMUL_EXCEPTION;
        break;

    case APIC_ICR:
        if ( (uint32_t)val & ~(APIC_VECTOR_MASK | APIC_DM_MASK |
                               APIC_DEST_MASK | APIC_INT_ASSERT |
                               APIC_INT_LEVELTRIG | APIC_SHORT_MASK) )
            return X86EMUL_EXCEPTION;
        vlapic_set_reg(vlapic, APIC_ICR2, val >> 32);
        break;

    case APIC_SELF_IPI:
        if ( val & ~APIC_VECTOR_MASK )
            return X86EMUL_EXCEPTION;
        offset = APIC_ICR;
        val = APIC_DEST_SELF | (val & APIC_VECTOR_MASK);
        break;

    case APIC_EOI:
    case APIC_ESR:
        if ( !val )
            break;
        fallthrough;
    default:
            return X86EMUL_EXCEPTION;
    }

    vlapic_reg_write(v, array_index_nospec(offset, PAGE_SIZE), val);

    return X86EMUL_OKAY;
}

static int cf_check vlapic_range(struct vcpu *v, unsigned long addr)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    unsigned long offset  = addr - vlapic_base_address(vlapic);

    return !vlapic_hw_disabled(vlapic) &&
           !vlapic_x2apic_mode(vlapic) &&
           (offset < PAGE_SIZE);
}

static const struct hvm_mmio_ops vlapic_mmio_ops = {
    .check = vlapic_range,
    .read = vlapic_mmio_read,
    .write = vlapic_mmio_write,
};

static uint32_t x2apic_ldr_from_id(uint32_t id)
{
    return ((id & ~0xf) << 12) | (1 << (id & 0xf));
}

static void set_x2apic_id(struct vlapic *vlapic)
{
    const struct vcpu *v = vlapic_vcpu(vlapic);
    uint32_t apic_id = v->vcpu_id * 2;
    uint32_t apic_ldr = x2apic_ldr_from_id(apic_id);

    /*
     * Workaround for migrated domains to derive LDRs as the source host
     * would've.
     */
    if ( v->domain->arch.hvm.bug_x2apic_ldr_vcpu_id )
        apic_ldr = x2apic_ldr_from_id(v->vcpu_id);

    vlapic_set_reg(vlapic, APIC_ID, apic_id);
    vlapic_set_reg(vlapic, APIC_LDR, apic_ldr);
}

int guest_wrmsr_apic_base(struct vcpu *v, uint64_t val)
{
    const struct cpu_policy *cp = v->domain->arch.cpu_policy;
    struct vlapic *vlapic = vcpu_vlapic(v);

    if ( !has_vlapic(v->domain) )
        return X86EMUL_EXCEPTION;

    /* Attempting to set reserved bits? */
    if ( val & ~(APIC_BASE_ADDR_MASK | APIC_BASE_ENABLE | APIC_BASE_BSP |
                 (cp->basic.x2apic ? APIC_BASE_EXTD : 0)) )
        return X86EMUL_EXCEPTION;

    /*
     * Architecturally speaking, we should allow a guest to move the xAPIC
     * MMIO window (within reason - not even hardware allows arbitrary
     * positions).  However, virtualising the behaviour for multi-vcpu guests
     * is problematic.
     *
     * The ability to move the MMIO window was introduced with the Pentium Pro
     * processor, to deconflict the window with other MMIO in the system.  The
     * need to move the MMIO window was obsoleted by the Netburst architecture
     * which reserved the space in physical address space for MSIs.
     *
     * As such, it appears to be a rarely used feature before the turn of the
     * millennium, and entirely unused after.
     *
     * Xen uses a per-domain P2M, but MSR_APIC_BASE is per-vcpu.  In
     * principle, we could emulate the MMIO windows being in different
     * locations by ensuring that all windows are unmapped in the P2M and trap
     * for emulation.  Xen has never had code to modify the P2M in response to
     * APIC_BASE updates, so guests which actually try this are likely to end
     * up without a working APIC.
     *
     * Things are more complicated with hardware APIC acceleration, where Xen
     * has to map a sink-page into the P2M for APIC accesses to be recognised
     * and accelerated by microcode.  Again, this could in principle be
     * emulated, but the visible result in the guest would be multiple working
     * APIC MMIO windows.  Moving the APIC window has never caused the
     * sink-page to move in the P2M, meaning that on all modern hardware, the
     * APIC definitely ceases working if the guest tries to move the window.
     *
     * As such, when the APIC is configured in xAPIC mode, require the MMIO
     * window to be in its default location.  We don't expect any guests which
     * currently run on Xen to be impacted by this restriction, and the #GP
     * fault will be far more obvious to debug than a malfunctioning MMIO
     * window.
     */
    if ( ((val & (APIC_BASE_EXTD | APIC_BASE_ENABLE)) == APIC_BASE_ENABLE) &&
         ((val & APIC_BASE_ADDR_MASK) != APIC_DEFAULT_PHYS_BASE) )
    {
        printk(XENLOG_G_INFO
               "%pv tried to move the APIC MMIO window: val 0x%08"PRIx64"\n",
               v, val);
        return X86EMUL_EXCEPTION;
    }

    if ( (vlapic->hw.apic_base_msr ^ val) & APIC_BASE_ENABLE )
    {
        if ( unlikely(val & APIC_BASE_EXTD) )
            return X86EMUL_EXCEPTION;

        if ( val & APIC_BASE_ENABLE )
        {
            vlapic_reset(vlapic);
            vlapic->hw.disabled &= ~VLAPIC_HW_DISABLED;
            pt_may_unmask_irq(vlapic_domain(vlapic), &vlapic->pt);
        }
        else
        {
            vlapic->hw.disabled |= VLAPIC_HW_DISABLED;
            pt_may_unmask_irq(vlapic_domain(vlapic), NULL);
        }
    }
    else if ( ((vlapic->hw.apic_base_msr ^ val) & APIC_BASE_EXTD) &&
              unlikely(!vlapic_xapic_mode(vlapic)) )
        return X86EMUL_EXCEPTION;

    vlapic->hw.apic_base_msr = val;
    memset(&vlapic->loaded, 0, sizeof(vlapic->loaded));

    if ( vlapic_x2apic_mode(vlapic) )
        set_x2apic_id(vlapic);

    hvm_update_vlapic_mode(vlapic_vcpu(vlapic));

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC,
                "apic base msr is 0x%016"PRIx64, vlapic->hw.apic_base_msr);

    return X86EMUL_OKAY;
}

uint64_t vlapic_tdt_msr_get(struct vlapic *vlapic)
{
    if ( !vlapic_lvtt_tdt(vlapic) )
        return 0;

    return vlapic->hw.tdt_msr;
}

void vlapic_tdt_msr_set(struct vlapic *vlapic, uint64_t value)
{
    uint64_t guest_tsc;
    struct vcpu *v = vlapic_vcpu(vlapic);

    if ( vlapic_hw_disabled(vlapic) )
        return;

    if ( !vlapic_lvtt_tdt(vlapic) )
    {
        HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER, "ignore tsc deadline msr write");
        return;
    }

    /* new_value = 0, >0 && <= now, > now */
    guest_tsc = hvm_get_guest_tsc(v);
    if ( value > guest_tsc )
    {
        uint64_t delta = gtsc_to_gtime(v->domain, value - guest_tsc);
        delta = max_t(int64_t, delta, 0);

        HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER, "delta[0x%016"PRIx64"]", delta);

        vlapic->hw.tdt_msr = value;
        /* .... reprogram tdt timer */
        TRACE_TIME(TRC_HVM_EMUL_LAPIC_START_TIMER, delta, delta >> 32,
                   0, 0, vlapic->pt.irq);
        create_periodic_time(v, &vlapic->pt, delta, 0,
                             vlapic->pt.irq, vlapic_tdt_pt_cb,
                             &vlapic->timer_last_update, false);
        vlapic->timer_last_update = vlapic->pt.last_plt_gtime;
    }
    else
    {
        vlapic->hw.tdt_msr = 0;

        /* trigger a timer event if needed */
        if ( value > 0 )
        {
            TRACE_TIME(TRC_HVM_EMUL_LAPIC_START_TIMER, 0, 0,
                       0, 0, vlapic->pt.irq);
            create_periodic_time(v, &vlapic->pt, 0, 0,
                                 vlapic->pt.irq, vlapic_tdt_pt_cb,
                                 &vlapic->timer_last_update, false);
            vlapic->timer_last_update = vlapic->pt.last_plt_gtime;
        }
        else
        {
            /* .... stop tdt timer */
            TRACE_TIME(TRC_HVM_EMUL_LAPIC_STOP_TIMER);
            destroy_periodic_time(&vlapic->pt);
        }

        HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER, "value[0x%016"PRIx64"]", value);
    }

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC_TIMER,
                "tdt_msr[0x%016"PRIx64"],"
                " gtsc[0x%016"PRIx64"]",
                vlapic->hw.tdt_msr, guest_tsc);
}

static int __vlapic_accept_pic_intr(struct vcpu *v)
{
    struct domain *d = v->domain;
    struct vlapic *vlapic = vcpu_vlapic(v);
    uint32_t lvt0 = vlapic_get_reg(vlapic, APIC_LVT0);
    union vioapic_redir_entry redir0;

    ASSERT(has_vpic(d));

    if ( !has_vioapic(d) )
        return 0;

    redir0 = domain_vioapic(d, 0)->redirtbl[0];

    /* We deliver 8259 interrupts to the appropriate CPU as follows. */
    return ((/* IOAPIC pin0 is unmasked and routing to this LAPIC? */
             ((redir0.fields.delivery_mode == dest_ExtINT) &&
              !redir0.fields.mask &&
              redir0.fields.dest_id == VLAPIC_ID(vlapic) &&
              !vlapic_disabled(vlapic)) ||
             /* LAPIC has LVT0 unmasked for ExtInts? */
             ((lvt0 & (APIC_DM_MASK | APIC_LVT_MASKED)) == APIC_DM_EXTINT) ||
             /* LAPIC is fully disabled? */
             vlapic_hw_disabled(vlapic)));
}

int vlapic_accept_pic_intr(struct vcpu *v)
{
    bool target, accept = false;

    if ( vlapic_hw_disabled(vcpu_vlapic(v)) || !has_vpic(v->domain) )
        return 0;

    target = v == v->domain->arch.hvm.i8259_target;
    if ( target )
        accept = __vlapic_accept_pic_intr(v);

    TRACE_TIME(TRC_HVM_EMUL_LAPIC_PIC_INTR, target, accept);

    return target && accept;
}

void vlapic_adjust_i8259_target(struct domain *d)
{
    struct vcpu *v;

    if ( !has_vpic(d) )
        return;

    for_each_vcpu ( d, v )
        if ( __vlapic_accept_pic_intr(v) )
            goto found;

    v = d->vcpu ? d->vcpu[0] : NULL;

 found:
    if ( d->arch.hvm.i8259_target == v )
        return;
    d->arch.hvm.i8259_target = v;
    pt_adjust_global_vcpu_target(v);
}

int vlapic_has_pending_irq(struct vcpu *v)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    int irr, isr;

    if ( !vlapic_enabled(vlapic) )
        return -1;

    /*
     * Poll the viridian message queues before checking the IRR since
     * a synthetic interrupt may be asserted during the poll.
     */
    if ( has_viridian_synic(v->domain) )
        viridian_synic_poll(v);

    irr = vlapic_find_highest_irr(vlapic);
    if ( irr == -1 )
        return -1;

    if ( hvm_funcs.caps.virtual_intr_delivery &&
         !nestedhvm_vcpu_in_guestmode(v) )
        return irr;

    /*
     * If APIC assist was set then an EOI may have been avoided.
     * If so, we need to emulate the EOI here before comparing ISR
     * with IRR.
     */
    if ( viridian_apic_assist_completed(v) )
        vlapic_EOI_set(vlapic);

    isr = vlapic_find_highest_isr(vlapic);

    /*
     * The specification says that if APIC assist is set and a
     * subsequent interrupt of lower priority occurs then APIC assist
     * needs to be cleared.
     */
    if ( isr >= 0 &&
         (irr & 0xf0) <= (isr & 0xf0) )
    {
        viridian_apic_assist_clear(v);
        return -1;
    }

    return irr;
}

int vlapic_ack_pending_irq(struct vcpu *v, int vector, bool force_ack)
{
    struct vlapic *vlapic = vcpu_vlapic(v);
    int isr;

    if ( !force_ack &&
         hvm_funcs.caps.virtual_intr_delivery )
        return 1;

    /* If there's no chance of using APIC assist then bail now. */
    if ( !has_viridian_apic_assist(v->domain) ||
         vlapic_test_vector(vector, &vlapic->regs->data[APIC_TMR]) )
        goto done;

    isr = vlapic_find_highest_isr(vlapic);
    if ( isr == -1 && vector > 0x10 )
    {
        /*
         * This vector is edge triggered, not in the legacy range, and no
         * lower priority vectors are pending in the ISR. Thus we can set
         * APIC assist to avoid exiting for EOI.
         */
        viridian_apic_assist_set(v);
    }

 done:
    if ( !has_viridian_synic(v->domain) ||
         !viridian_synic_is_auto_eoi_sint(v, vector) )
        vlapic_set_vector(vector, &vlapic->regs->data[APIC_ISR]);

    vlapic_clear_irr(vector, vlapic);

    return 1;
}

bool is_vlapic_lvtpc_enabled(struct vlapic *vlapic)
{
    return (vlapic_enabled(vlapic) &&
            !(vlapic_get_reg(vlapic, APIC_LVTPC) & APIC_LVT_MASKED));
}

/* Reset the VLAPIC back to its init state. */
static void vlapic_do_init(struct vlapic *vlapic)
{
    int i;

    if ( !has_vlapic(vlapic_vcpu(vlapic)->domain) )
        return;

    vlapic_set_reg(vlapic, APIC_LVR, VLAPIC_VERSION);

    for ( i = 0; i < 8; i++ )
    {
        vlapic_set_reg(vlapic, APIC_IRR + 0x10 * i, 0);
        vlapic_set_reg(vlapic, APIC_ISR + 0x10 * i, 0);
        vlapic_set_reg(vlapic, APIC_TMR + 0x10 * i, 0);
    }
    vlapic_set_reg(vlapic, APIC_ICR,     0);
    vlapic_set_reg(vlapic, APIC_ICR2,    0);
    /*
     * LDR is read-only in x2APIC mode. Preserve its value when handling
     * INIT signal in x2APIC mode.
     */
    if ( !vlapic_x2apic_mode(vlapic) )
        vlapic_set_reg(vlapic, APIC_LDR, 0);
    vlapic_set_reg(vlapic, APIC_TASKPRI, 0);
    vlapic_set_reg(vlapic, APIC_TMICT,   0);
    vlapic_set_reg(vlapic, APIC_TMCCT,   0);
    vlapic_set_tdcr(vlapic, 0);

    vlapic_set_reg(vlapic, APIC_DFR, 0xffffffffU);

    for ( i = 0; i < VLAPIC_LVT_NUM; i++ )
        vlapic_set_reg(vlapic, APIC_LVTT + 0x10 * i, APIC_LVT_MASKED);

    vlapic_set_reg(vlapic, APIC_SPIV, 0xff);
    vlapic->hw.disabled |= VLAPIC_SW_DISABLED;

    TRACE_TIME(TRC_HVM_EMUL_LAPIC_STOP_TIMER);
    destroy_periodic_time(&vlapic->pt);
}

/* Reset the VLAPIC back to its power-on/reset state. */
void vlapic_reset(struct vlapic *vlapic)
{
    const struct vcpu *v = vlapic_vcpu(vlapic);

    if ( !has_vlapic(v->domain) )
        return;

    vlapic->hw.apic_base_msr = APIC_BASE_ENABLE | APIC_DEFAULT_PHYS_BASE;
    if ( v->vcpu_id == 0 )
        vlapic->hw.apic_base_msr |= APIC_BASE_BSP;

    vlapic_set_reg(vlapic, APIC_ID, (v->vcpu_id * 2) << 24);
    vlapic_do_init(vlapic);
}

/* rearm the actimer if needed, after a HVM restore */
static void lapic_rearm(struct vlapic *s)
{
    unsigned long tmict;
    uint64_t period, timer_period = 0, tdt_msr;
    bool is_periodic;

    s->pt.irq = vlapic_get_reg(s, APIC_LVTT) & APIC_VECTOR_MASK;

    if ( vlapic_lvtt_tdt(s) )
    {
        if ( (tdt_msr = vlapic_tdt_msr_get(s)) != 0 )
            vlapic_tdt_msr_set(s, tdt_msr);
        return;
    }

    if ( (tmict = vlapic_get_reg(s, APIC_TMICT)) == 0 )
        return;

    period = ((uint64_t)APIC_BUS_CYCLE_NS *
              (uint32_t)tmict * s->hw.timer_divisor);
    is_periodic = vlapic_lvtt_period(s);

    if ( is_periodic )
        timer_period = period;

    TRACE_TIME(TRC_HVM_EMUL_LAPIC_START_TIMER, period, period >> 32,
               timer_period, timer_period >> 32, s->pt.irq);

    create_periodic_time(vlapic_vcpu(s), &s->pt, period,
                         timer_period,
                         s->pt.irq,
                         is_periodic ? vlapic_pt_cb : NULL,
                         &s->timer_last_update, false);
    s->timer_last_update = s->pt.last_plt_gtime;
}

static int cf_check lapic_save_hidden(struct vcpu *v, hvm_domain_context_t *h)
{
    if ( !has_vlapic(v->domain) )
        return 0;

    return hvm_save_entry(LAPIC, v->vcpu_id, h, &vcpu_vlapic(v)->hw);
}

static int cf_check lapic_save_regs(struct vcpu *v, hvm_domain_context_t *h)
{
    if ( !has_vlapic(v->domain) )
        return 0;

    hvm_sync_pir_to_irr(v);

    return hvm_save_entry(LAPIC_REGS, v->vcpu_id, h, vcpu_vlapic(v)->regs);
}

/*
 * Following lapic_load_hidden()/lapic_load_regs() we may need to
 * correct ID and LDR when they come from an old, broken hypervisor.
 */
static void lapic_load_fixup(struct vlapic *vlapic)
{
    const struct vcpu *v = vlapic_vcpu(vlapic);
    uint32_t good_ldr = x2apic_ldr_from_id(vlapic->loaded.id);

    /* Skip fixups on xAPIC mode, or if the x2APIC LDR is already correct */
    if ( !vlapic_x2apic_mode(vlapic) ||
         (vlapic->loaded.ldr == good_ldr) )
        return;

    if ( vlapic->loaded.ldr == 1 )
    {
       /*
        * Xen <= 4.4 may have a bug by which all the APICs configured in
        * x2APIC mode got LDR = 1, which is inconsistent on every vCPU
        * except for the one with ID = 0. We'll fix the bug now and assign
        * an LDR value consistent with the APIC ID.
        */
        set_x2apic_id(vlapic);
    }
    else if ( vlapic->loaded.ldr == x2apic_ldr_from_id(v->vcpu_id) )
    {
        /*
         * Migrations from Xen 4.4 to date (4.19 dev window, Nov 2023) may
         * have LDR drived from the vCPU ID, not the APIC ID. We must preserve
         * LDRs so new vCPUs use consistent derivations and existing guests,
         * which may have already read the LDR at the source host, aren't
         * surprised when interrupts stop working the way they did at the
         * other end.
         */
        v->domain->arch.hvm.bug_x2apic_ldr_vcpu_id = true;
    }
    else
        printk(XENLOG_G_WARNING
               "%pv: bogus x2APIC record: ID %#x, LDR %#x, expected LDR %#x\n",
               v, vlapic->loaded.id, vlapic->loaded.ldr, good_ldr);
}


static int lapic_check_common(const struct domain *d, unsigned int vcpuid)
{
    if ( !has_vlapic(d) )
        return -ENODEV;

    /* Which vlapic to load? */
    if ( !domain_vcpu(d, vcpuid) )
    {
        dprintk(XENLOG_G_ERR, "HVM restore: dom%d has no vCPU %u\n",
                d->domain_id, vcpuid);
        return -EINVAL;
    }

    return 0;
}

static int cf_check lapic_check_hidden(const struct domain *d,
                                       hvm_domain_context_t *h)
{
    unsigned int vcpuid = hvm_load_instance(h);
    struct hvm_hw_lapic s;
    int rc = lapic_check_common(d, vcpuid);

    if ( rc )
        return rc;

    if ( hvm_load_entry_zeroextend(LAPIC, h, &s) != 0 )
        return -ENODATA;

    /* EN=0 with EXTD=1 is illegal */
    if ( (s.apic_base_msr & (APIC_BASE_ENABLE | APIC_BASE_EXTD)) ==
         APIC_BASE_EXTD )
        return -EINVAL;

    return 0;
}

static int cf_check lapic_load_hidden(struct domain *d, hvm_domain_context_t *h)
{
    unsigned int vcpuid = hvm_load_instance(h);
    struct vcpu *v = d->vcpu[vcpuid];
    struct vlapic *s = vcpu_vlapic(v);

    if ( hvm_load_entry_zeroextend(LAPIC, h, &s->hw) != 0 )
    {
        ASSERT_UNREACHABLE();
        return -EINVAL;
    }

    s->loaded.hw = 1;
    if ( s->loaded.regs )
        lapic_load_fixup(s);

    hvm_update_vlapic_mode(v);

    return 0;
}

static int cf_check lapic_check_regs(const struct domain *d,
                                     hvm_domain_context_t *h)
{
    unsigned int vcpuid = hvm_load_instance(h);
    int rc;

    if ( (rc = lapic_check_common(d, vcpuid)) )
        return rc;

    if ( !hvm_get_entry(LAPIC_REGS, h) )
        return -ENODATA;

    return 0;
}

static int cf_check lapic_load_regs(struct domain *d, hvm_domain_context_t *h)
{
    unsigned int vcpuid = hvm_load_instance(h);
    struct vcpu *v = d->vcpu[vcpuid];
    struct vlapic *s = vcpu_vlapic(v);

    if ( hvm_load_entry(LAPIC_REGS, h, s->regs) != 0 )
        ASSERT_UNREACHABLE();

    s->loaded.id = vlapic_get_reg(s, APIC_ID);
    s->loaded.ldr = vlapic_get_reg(s, APIC_LDR);
    s->loaded.regs = 1;
    if ( s->loaded.hw )
        lapic_load_fixup(s);

    if ( hvm_funcs.process_isr )
        alternative_vcall(hvm_funcs.process_isr,
                          vlapic_find_highest_isr(s), v);

    vlapic_adjust_i8259_target(d);
    lapic_rearm(s);
    return 0;
}

HVM_REGISTER_SAVE_RESTORE(LAPIC, lapic_save_hidden, lapic_check_hidden,
                          lapic_load_hidden, 1, HVMSR_PER_VCPU);
HVM_REGISTER_SAVE_RESTORE(LAPIC_REGS, lapic_save_regs, lapic_check_regs,
                          lapic_load_regs, 1, HVMSR_PER_VCPU);

int vlapic_init(struct vcpu *v)
{
    struct vlapic *vlapic = vcpu_vlapic(v);

    HVM_DBG_LOG(DBG_LEVEL_VLAPIC, "%d", v->vcpu_id);

    if ( !has_vlapic(v->domain) )
    {
        vlapic->hw.disabled = VLAPIC_HW_DISABLED;
        return 0;
    }

    vlapic->pt.source = PTSRC_lapic;

    vlapic->regs_page = alloc_domheap_page(v->domain, MEMF_no_owner);
    if ( !vlapic->regs_page )
        return -ENOMEM;

    vlapic->regs = __map_domain_page_global(vlapic->regs_page);
    if ( vlapic->regs == NULL )
    {
        free_domheap_page(vlapic->regs_page);
        return -ENOMEM;
    }

    clear_page(vlapic->regs);

    vlapic_reset(vlapic);

    tasklet_init(&vlapic->init_sipi.tasklet, vlapic_init_sipi_action, v);

    if ( v->vcpu_id == 0 )
        register_mmio_handler(v->domain, &vlapic_mmio_ops);

    return 0;
}

void vlapic_destroy(struct vcpu *v)
{
    struct vlapic *vlapic = vcpu_vlapic(v);

    if ( !has_vlapic(v->domain) )
        return;

    tasklet_kill(&vlapic->init_sipi.tasklet);
    TRACE_TIME(TRC_HVM_EMUL_LAPIC_STOP_TIMER);
    destroy_periodic_time(&vlapic->pt);
    unmap_domain_page_global(vlapic->regs);
    free_domheap_page(vlapic->regs_page);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
