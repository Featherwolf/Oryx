// SPDX-License-Identifier: GPL-2.0
/*
 * oryx_memmodel.c — Project Oryx Part A: per-thread hardware x86 memory mode.
 *
 * Grants an opted-in thread hardware Total Store Order on Oryon cores by keeping
 * the discovered memory-ordering control bit set while that thread runs, and
 * clearing it when the thread is scheduled out — so only translated threads pay
 * the ~9% TSO cost and the rest of the system keeps ARM's weak ordering.
 *
 * This is the production counterpart to experiments/phase0-tso-probe. The
 * (register, bit) it programs are exactly what Phase 0 confirmed; they are
 * module parameters so the same binary adapts as the values are pinned down.
 *
 * Requirements:
 *   - arm64, CONFIG_PREEMPT_NOTIFIERS=y (set when KVM is built).
 *   - A confirmed control bit (set oryx_confirmed=1 once Phase 0 passes).
 *
 * Mechanism: one preempt_notifier per opted-in thread. sched_in sets the bit on
 * the current (eligible) core; sched_out clears it. The grant is bound to an
 * open fd (one per thread); releasing the fd unregisters the notifier and
 * restores weak ordering.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/preempt.h>
#include <linux/cpumask.h>
#include <linux/uaccess.h>
#include <asm/sysreg.h>

#include "include/uapi/oryx_memmodel.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Project Oryx");
MODULE_DESCRIPTION("Per-thread hardware x86 (TSO) memory mode for Oryon cores");

/* ---- Configuration (pinned by Phase 0) ---------------------------------- */
static int oryx_reg_idx = 0;      /* which candidate register (see switch below) */
static int oryx_bit     = 0;      /* which bit within it */
static int oryx_confirmed;        /* set 1 only after Phase 0 confirms the bit  */
static ulong oryx_eligible = ~0UL;/* bitmask of cores where TSO is permitted     */
module_param(oryx_reg_idx, int, 0444);
module_param(oryx_bit, int, 0444);
module_param(oryx_confirmed, int, 0444);
module_param(oryx_eligible, ulong, 0444);
MODULE_PARM_DESC(oryx_confirmed, "1 iff the (reg,bit) is Phase-0 confirmed; else ENODEV");

/* Candidate registers must be switched over compile-time encodings (MRS/MSR). */
enum { REG_ACTLR_EL1 = 0, REG_S3_0_C15_C0_0, REG_S3_0_C15_C1_0, REG_S3_0_C15_C2_0 };

static inline u64 oryx_read_reg(void)
{
	switch (oryx_reg_idx) {
	case REG_ACTLR_EL1:     return read_sysreg(actlr_el1);
	case REG_S3_0_C15_C0_0: return read_sysreg_s(sys_reg(3, 0, 15, 0, 0));
	case REG_S3_0_C15_C1_0: return read_sysreg_s(sys_reg(3, 0, 15, 1, 0));
	case REG_S3_0_C15_C2_0: return read_sysreg_s(sys_reg(3, 0, 15, 2, 0));
	default:                return 0;
	}
}

static inline void oryx_write_reg(u64 v)
{
	switch (oryx_reg_idx) {
	case REG_ACTLR_EL1:     write_sysreg(v, actlr_el1); break;
	case REG_S3_0_C15_C0_0: write_sysreg_s(v, sys_reg(3, 0, 15, 0, 0)); break;
	case REG_S3_0_C15_C1_0: write_sysreg_s(v, sys_reg(3, 0, 15, 1, 0)); break;
	case REG_S3_0_C15_C2_0: write_sysreg_s(v, sys_reg(3, 0, 15, 2, 0)); break;
	default: break;
	}
	isb();
}

/* Set/clear the control bit on the CURRENT core. Runs in atomic context. */
static void oryx_apply(bool on)
{
	u64 v = oryx_read_reg();
	u64 nv = on ? (v | (1ULL << oryx_bit)) : (v & ~(1ULL << oryx_bit));

	if (nv != v)
		oryx_write_reg(nv);
}

static inline bool oryx_core_eligible(int cpu)
{
	return (cpu < BITS_PER_LONG) && (oryx_eligible & (1UL << cpu));
}

/* ---- Per-thread grant ---------------------------------------------------- */
struct oryx_grant {
	struct preempt_notifier pn;
	bool enabled;
};

static void oryx_sched_in(struct preempt_notifier *pn, int cpu)
{
	struct oryx_grant *g = container_of(pn, struct oryx_grant, pn);

	if (g->enabled && oryx_core_eligible(cpu))
		oryx_apply(true);
}

static void oryx_sched_out(struct preempt_notifier *pn,
			   struct task_struct *next)
{
	struct oryx_grant *g = container_of(pn, struct oryx_grant, pn);

	if (g->enabled)
		oryx_apply(false);   /* restore weak ordering for whoever runs next */
}

static const struct preempt_ops oryx_preempt_ops = {
	.sched_in  = oryx_sched_in,
	.sched_out = oryx_sched_out,
};

/* ---- Enable / disable ---------------------------------------------------- */
static int oryx_enable(struct oryx_grant *g)
{
	if (!oryx_confirmed)
		return -ENODEV;         /* no confirmed control bit — fail closed */
	if (g->enabled)
		return 0;

	/* If TSO is core-restricted, confine this thread to eligible cores. */
	if (oryx_eligible != ~0UL) {
		cpumask_var_t mask;
		int cpu, ret;

		if (!alloc_cpumask_var(&mask, GFP_KERNEL))
			return -ENOMEM;
		cpumask_clear(mask);
		for_each_online_cpu(cpu)
			if (oryx_core_eligible(cpu))
				cpumask_set_cpu(cpu, mask);
		ret = set_cpus_allowed_ptr(current, mask);
		free_cpumask_var(mask);
		if (ret)
			return ret;
	}

	preempt_notifier_init(&g->pn, &oryx_preempt_ops);
	preempt_notifier_register(&g->pn);
	g->enabled = true;

	/* Apply immediately for the current run (we may already be on-core). */
	preempt_disable();
	if (oryx_core_eligible(smp_processor_id()))
		oryx_apply(true);
	preempt_enable();
	return 0;
}

static void oryx_disable(struct oryx_grant *g)
{
	if (!g->enabled)
		return;
	g->enabled = false;
	preempt_notifier_unregister(&g->pn);

	/* Ensure the bit is cleared on the core we're currently on. */
	preempt_disable();
	oryx_apply(false);
	preempt_enable();
}

/* ---- Char device --------------------------------------------------------- */
static int oryx_open(struct inode *ino, struct file *f)
{
	struct oryx_grant *g = kzalloc(sizeof(*g), GFP_KERNEL);

	if (!g)
		return -ENOMEM;
	f->private_data = g;
	return 0;
}

static long oryx_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct oryx_grant *g = f->private_data;

	switch (cmd) {
	case ORYX_MM_ENABLE_TSO:
		return oryx_enable(g);
	case ORYX_MM_DISABLE:
		oryx_disable(g);
		return 0;
	case ORYX_MM_QUERY: {
		u32 flags = 0;

		if (oryx_confirmed)          flags |= ORYX_MM_CAP_AVAILABLE;
		if (g->enabled)              flags |= ORYX_MM_STATE_TSO;
		if (oryx_eligible != ~0UL)   flags |= ORYX_MM_CAP_CORE_LIMIT;
		if (put_user(flags, (u32 __user *)arg))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static int oryx_release(struct inode *ino, struct file *f)
{
	struct oryx_grant *g = f->private_data;

	if (g) {
		oryx_disable(g);   /* grant is bound to fd lifetime */
		kfree(g);
	}
	return 0;
}

static const struct file_operations oryx_fops = {
	.owner          = THIS_MODULE,
	.open           = oryx_open,
	.unlocked_ioctl = oryx_ioctl,
	.compat_ioctl   = oryx_ioctl,
	.release        = oryx_release,
};

static struct miscdevice oryx_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "oryx_memmodel",
	.fops  = &oryx_fops,
	.mode  = 0666,   /* gated in practice by SELinux policy + Oryx entitlement */
};

static int __init oryx_mm_init(void)
{
	int ret = misc_register(&oryx_miscdev);

	if (ret)
		return ret;
	pr_info("oryx_memmodel: loaded (reg_idx=%d bit=%d confirmed=%d eligible=0x%lx)\n",
		oryx_reg_idx, oryx_bit, oryx_confirmed, oryx_eligible);
	if (!oryx_confirmed)
		pr_warn("oryx_memmodel: no confirmed control bit — ENABLE_TSO will fail closed\n");
	return 0;
}

static void __exit oryx_mm_exit(void)
{
	misc_deregister(&oryx_miscdev);
	pr_info("oryx_memmodel: unloaded\n");
}

module_init(oryx_mm_init);
module_exit(oryx_mm_exit);
