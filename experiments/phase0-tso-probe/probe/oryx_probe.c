// SPDX-License-Identifier: GPL-2.0
/*
 * oryx_probe.c — Phase 0 Step 2: Oryon memory-ordering control-bit finder.
 *
 * ⚠  DANGER. This module reads and WRITES IMPLEMENTATION DEFINED EL1 system
 *    registers on a live SoC. A wrong write can hang, panic, or corrupt state.
 *    Load ONLY on a dedicated, bootloader-unlocked test device with a recovery
 *    path. Never on a device you care about.
 *
 * Purpose
 * -------
 * We believe the Oryon Gen 3 cores expose a Rosetta-style hardware x86 memory
 * mode (Windows Prism uses it), controlled by a bit in an IMPDEF EL1 register
 * (on Apple it is a bit in ACTLR_EL1). This module lets us:
 *
 *   1. DUMP the candidate registers on a chosen CPU.
 *   2. POKE a single bit of a chosen candidate register on a chosen CPU.
 *
 * The method (see probe_scan.sh): for each (register, bit), POKE it on a core,
 * then run the userspace `oryx_litmus` MP test pinned to that core. The bit
 * whose setting flips the pair from WEAK to TSO (while SB still fires) is the
 * memory-ordering control we are hunting.
 *
 * Interface (debugfs, root-only):
 *   /sys/kernel/debug/oryx_probe/control   (write commands)
 *   /sys/kernel/debug/oryx_probe/log       (read accumulated output)
 *
 * Commands (write to .../control):
 *   dump <cpu>                     read every candidate register on <cpu>
 *   poke <idx> <bit> <cpu> <0|1>   clear/set bit <bit> of candidate <idx> on <cpu>
 *   list                           list candidate registers and their indices
 *   clearlog                       reset the log buffer
 *
 * Because AArch64 MRS/MSR need the encoding as an assembled immediate, the
 * candidate set is a fixed compile-time table; add encodings here as reverse-
 * engineering narrows them down.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <asm/sysreg.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Project Oryx");
MODULE_DESCRIPTION("Phase 0: Oryon x86 memory-ordering control-bit finder (DANGEROUS)");

/* ---- Candidate registers ------------------------------------------------- */
/*
 * ACTLR_EL1 and AIDR_EL1 are architected; the S3_0_C15_* block is the IMPDEF
 * space where vendor controls typically live. These S3_0_C15_* encodings are
 * GUESSES to be confirmed/replaced by on-device reverse engineering.
 */
enum {
	IDX_ACTLR_EL1 = 0,
	IDX_AIDR_EL1,			/* read-only: may advertise TSO support */
	IDX_S3_0_C15_C0_0,
	IDX_S3_0_C15_C1_0,
	IDX_S3_0_C15_C2_0,
	IDX_S3_0_C15_C3_0,
	CANDIDATE_COUNT
};

struct candidate {
	const char *name;
	bool writable;
};

static const struct candidate candidates[CANDIDATE_COUNT] = {
	[IDX_ACTLR_EL1]     = { "ACTLR_EL1",      true  },
	[IDX_AIDR_EL1]      = { "AIDR_EL1",       false },
	[IDX_S3_0_C15_C0_0] = { "S3_0_C15_C0_0",  true  },
	[IDX_S3_0_C15_C1_0] = { "S3_0_C15_C1_0",  true  },
	[IDX_S3_0_C15_C2_0] = { "S3_0_C15_C2_0",  true  },
	[IDX_S3_0_C15_C3_0] = { "S3_0_C15_C3_0",  true  },
};

/* Runtime encoding selection must switch over compile-time constants. */
static u64 read_candidate(int idx)
{
	switch (idx) {
	case IDX_ACTLR_EL1:     return read_sysreg(actlr_el1);
	case IDX_AIDR_EL1:      return read_sysreg_s(SYS_AIDR_EL1);
	case IDX_S3_0_C15_C0_0: return read_sysreg_s(sys_reg(3, 0, 15, 0, 0));
	case IDX_S3_0_C15_C1_0: return read_sysreg_s(sys_reg(3, 0, 15, 1, 0));
	case IDX_S3_0_C15_C2_0: return read_sysreg_s(sys_reg(3, 0, 15, 2, 0));
	case IDX_S3_0_C15_C3_0: return read_sysreg_s(sys_reg(3, 0, 15, 3, 0));
	default:                return 0;
	}
}

static void write_candidate(int idx, u64 val)
{
	switch (idx) {
	case IDX_ACTLR_EL1:     write_sysreg(val, actlr_el1); break;
	case IDX_S3_0_C15_C0_0: write_sysreg_s(val, sys_reg(3, 0, 15, 0, 0)); break;
	case IDX_S3_0_C15_C1_0: write_sysreg_s(val, sys_reg(3, 0, 15, 1, 0)); break;
	case IDX_S3_0_C15_C2_0: write_sysreg_s(val, sys_reg(3, 0, 15, 2, 0)); break;
	case IDX_S3_0_C15_C3_0: write_sysreg_s(val, sys_reg(3, 0, 15, 3, 0)); break;
	default: break; /* read-only or unknown */
	}
	isb(); /* ensure the write takes effect before subsequent instructions */
}

/* ---- Log buffer ---------------------------------------------------------- */
#define LOGSZ 8192
static char log_buf[LOGSZ];
static size_t log_len;
static DEFINE_MUTEX(oryx_lock);

static __printf(1, 2) void logf(const char *fmt, ...)
{
	va_list args;
	int n;

	if (log_len >= LOGSZ - 1)
		return;
	va_start(args, fmt);
	n = vscnprintf(log_buf + log_len, LOGSZ - log_len, fmt, args);
	va_end(args);
	log_len += n;
}

/* ---- Per-CPU work -------------------------------------------------------- */
struct dump_work { int cpu; };
struct poke_work { int idx; int bit; int val; u64 before; u64 after; };

static void do_dump(void *info)
{
	/* runs on the target CPU */
	int i;
	for (i = 0; i < CANDIDATE_COUNT; i++) {
		u64 v = read_candidate(i);
		/* logf is not SMP-safe; caller serializes and we only touch it here
		 * because smp_call_function_single(wait=1) blocks the caller. */
		logf("  [%d] %-16s = 0x%016llx%s\n",
		     i, candidates[i].name, v,
		     candidates[i].writable ? "" : "  (ro)");
	}
}

static void do_poke(void *info)
{
	struct poke_work *w = info;
	u64 v = read_candidate(w->idx);
	w->before = v;
	if (w->val)
		v |= (1ULL << w->bit);
	else
		v &= ~(1ULL << w->bit);
	write_candidate(w->idx, v);
	w->after = read_candidate(w->idx);
}

/* ---- Command parser ------------------------------------------------------ */
static void cmd_list(void)
{
	int i;
	logf("candidate registers:\n");
	for (i = 0; i < CANDIDATE_COUNT; i++)
		logf("  [%d] %-16s %s\n", i, candidates[i].name,
		     candidates[i].writable ? "rw" : "ro");
}

static int cmd_dump(int cpu)
{
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return -EINVAL;
	logf("dump on cpu %d:\n", cpu);
	return smp_call_function_single(cpu, do_dump, NULL, 1);
}

static int cmd_poke(int idx, int bit, int cpu, int val)
{
	struct poke_work w = { .idx = idx, .bit = bit, .val = val };
	int ret;

	if (idx < 0 || idx >= CANDIDATE_COUNT) return -EINVAL;
	if (!candidates[idx].writable)         return -EPERM;
	if (bit < 0 || bit > 63)               return -EINVAL;
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu)) return -EINVAL;

	ret = smp_call_function_single(cpu, do_poke, &w, 1);
	if (ret)
		return ret;

	logf("poke %s bit %d = %d on cpu %d: 0x%016llx -> 0x%016llx%s\n",
	     candidates[idx].name, bit, val, cpu, w.before, w.after,
	     (w.after == w.before) ? "  (NO CHANGE — bit ignored/RAZ)" : "");
	return 0;
}

/* ---- debugfs: control (write) ------------------------------------------- */
static ssize_t control_write(struct file *f, const char __user *ubuf,
			     size_t len, loff_t *off)
{
	char kbuf[64];
	int idx, bit, cpu, val, n, ret = 0;

	if (len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	mutex_lock(&oryx_lock);
	if (!strncmp(kbuf, "list", 4)) {
		cmd_list();
	} else if (sscanf(kbuf, "dump %d", &cpu) == 1) {
		ret = cmd_dump(cpu);
	} else if ((n = sscanf(kbuf, "poke %d %d %d %d",
				&idx, &bit, &cpu, &val)) == 4) {
		ret = cmd_poke(idx, bit, cpu, val);
	} else if (!strncmp(kbuf, "clearlog", 8)) {
		log_len = 0;
		log_buf[0] = '\0';
	} else {
		ret = -EINVAL;
	}
	if (ret)
		logf("error: command failed (%d)\n", ret);
	mutex_unlock(&oryx_lock);

	return ret ? ret : (ssize_t)len;
}

static const struct file_operations control_fops = {
	.owner = THIS_MODULE,
	.write = control_write,
};

/* ---- debugfs: log (read) ------------------------------------------------- */
static ssize_t log_read(struct file *f, char __user *ubuf,
			size_t len, loff_t *off)
{
	ssize_t ret;

	mutex_lock(&oryx_lock);
	ret = simple_read_from_buffer(ubuf, len, off, log_buf, log_len);
	mutex_unlock(&oryx_lock);
	return ret;
}

static const struct file_operations log_fops = {
	.owner = THIS_MODULE,
	.read  = log_read,
};

/* ---- Module init/exit ---------------------------------------------------- */
static struct dentry *oryx_dir;

static int __init oryx_probe_init(void)
{
	oryx_dir = debugfs_create_dir("oryx_probe", NULL);
	if (IS_ERR(oryx_dir))
		return PTR_ERR(oryx_dir);

	debugfs_create_file("control", 0200, oryx_dir, NULL, &control_fops);
	debugfs_create_file("log",     0400, oryx_dir, NULL, &log_fops);

	log_len = 0;
	logf("oryx_probe loaded. DANGEROUS. Candidate count = %d.\n",
	     CANDIDATE_COUNT);
	pr_warn("oryx_probe: loaded — pokes IMPDEF EL1 regs; test devices only\n");
	return 0;
}

static void __exit oryx_probe_exit(void)
{
	debugfs_remove_recursive(oryx_dir);
	pr_info("oryx_probe: unloaded\n");
}

module_init(oryx_probe_init);
module_exit(oryx_probe_exit);
