// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxmm.c — userspace client for /dev/oryx_memmodel. Fail-closed by design.
 */
#define _GNU_SOURCE
#include "oryxmm.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/*
 * We intentionally re-declare the ioctl numbers here (rather than pulling the
 * kernel uapi header) so the library builds standalone on any host. They MUST
 * stay in sync with src/kmod/oryx_memmodel/include/uapi/oryx_memmodel.h.
 */
#include <linux/ioctl.h>
#define ORYX_MM_IOC_MAGIC 'X'
#define ORYX_MM_ENABLE_TSO _IO(ORYX_MM_IOC_MAGIC, 1)
#define ORYX_MM_DISABLE    _IO(ORYX_MM_IOC_MAGIC, 2)
#define ORYX_MM_QUERY      _IOR(ORYX_MM_IOC_MAGIC, 3, unsigned int)

#define ORYX_MM_DEV "/dev/oryx_memmodel"

int oryx_mm_open(void)
{
	int fd = open(ORYX_MM_DEV, O_RDWR | O_CLOEXEC);
	return fd; /* -1 if absent/no-perm: caller uses software ordering */
}

int oryx_mm_enable_tso(int fd)
{
	if (fd < 0)
		return -1;
	return ioctl(fd, ORYX_MM_ENABLE_TSO) == 0 ? 0 : -1;
}

int oryx_mm_disable(int fd)
{
	if (fd < 0)
		return -1;
	return ioctl(fd, ORYX_MM_DISABLE) == 0 ? 0 : -1;
}

unsigned oryx_mm_query(int fd)
{
	unsigned flags = 0;

	if (fd < 0)
		return 0;
	if (ioctl(fd, ORYX_MM_QUERY, &flags) != 0)
		return 0;
	return flags;
}

void oryx_mm_close(int fd)
{
	if (fd >= 0)
		close(fd);
}

int oryx_mm_thread_tso_on(void)
{
	int fd = oryx_mm_open();

	if (fd < 0)
		return -1;
	if (oryx_mm_enable_tso(fd) != 0) {
		oryx_mm_close(fd);
		return -1;
	}
	return fd; /* keep open for the thread's lifetime */
}
