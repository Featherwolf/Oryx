/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * oryx_memmodel.h — userspace ABI for the Oryx per-thread memory-model driver.
 *
 * A translated (x86-on-ARM) thread opens /dev/oryx_memmodel and requests hardware
 * Total Store Order for itself. While enabled, the driver keeps the discovered
 * Oryon memory-ordering control bit set on whichever eligible core the thread
 * runs on, and cleared when the thread is scheduled out — so the rest of the
 * system keeps ARM's faster weak model.
 *
 * One fd per thread. The fd's lifetime bounds the TSO grant: closing it (or the
 * thread exiting) restores weak ordering for that thread.
 */
#ifndef _UAPI_ORYX_MEMMODEL_H
#define _UAPI_ORYX_MEMMODEL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ORYX_MM_IOC_MAGIC 'X'

/* Enable hardware TSO for the calling thread (must be called by the thread itself). */
#define ORYX_MM_ENABLE_TSO _IO(ORYX_MM_IOC_MAGIC, 1)

/* Disable / restore weak ordering for the calling thread. */
#define ORYX_MM_DISABLE    _IO(ORYX_MM_IOC_MAGIC, 2)

/* Query capability + current state; returns a bitmask of ORYX_MM_* flags below. */
#define ORYX_MM_QUERY      _IOR(ORYX_MM_IOC_MAGIC, 3, __u32)

/* Flags returned by ORYX_MM_QUERY. */
#define ORYX_MM_CAP_AVAILABLE  (1u << 0) /* driver has a confirmed control bit  */
#define ORYX_MM_STATE_TSO      (1u << 1) /* calling thread currently has TSO on */
#define ORYX_MM_CAP_CORE_LIMIT (1u << 2) /* TSO restricted to a core subset     */

#endif /* _UAPI_ORYX_MEMMODEL_H */
