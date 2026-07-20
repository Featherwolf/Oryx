/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * oryxmm.h — thin userspace client for the Oryx per-thread memory-model driver.
 *
 * Emulators (Box64/FEX) call this per translated thread. Everything fails
 * closed: if the driver is absent or has no confirmed control bit, the enable
 * calls return -1 and the caller MUST keep emulating the memory model in
 * software. Never treat a failure as "TSO is on."
 */
#ifndef ORYXMM_H
#define ORYXMM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Capability/state flags returned by oryx_mm_query(). Mirror of the uapi. */
#define ORYX_MM_CAP_AVAILABLE  (1u << 0)
#define ORYX_MM_STATE_TSO      (1u << 1)
#define ORYX_MM_CAP_CORE_LIMIT (1u << 2)

/*
 * Open the memory-model device. Returns an fd >= 0 on success, or -1 if the
 * driver is not present (device missing / no permission). -1 means "no hardware
 * TSO available — use software ordering."
 */
int oryx_mm_open(void);

/* Enable hardware TSO for the CALLING thread. 0 on success, -1 on failure
 * (including when the driver has no Phase-0-confirmed control bit). */
int oryx_mm_enable_tso(int fd);

/* Restore weak ordering for the calling thread. 0 on success, -1 on failure. */
int oryx_mm_disable(int fd);

/* Return the capability/state flag bitmask, or 0 if unavailable. */
unsigned oryx_mm_query(int fd);

/* Close the fd; this also restores weak ordering for the thread (fd-bound grant). */
void oryx_mm_close(int fd);

/*
 * Convenience for the common emulator path: open + enable TSO for the current
 * thread in one call. Returns an fd >= 0 that the caller must keep open for the
 * thread's lifetime and close on thread exit, or -1 if hardware TSO is
 * unavailable (caller keeps software ordering).
 */
int oryx_mm_thread_tso_on(void);

#ifdef __cplusplus
}
#endif

#endif /* ORYXMM_H */
