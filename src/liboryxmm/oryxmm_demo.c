// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oryxmm_demo.c — shows the emulator integration path and the fail-closed
 * behavior. On a host without the driver it prints "unavailable -> software
 * ordering", which is exactly what Box64/FEX should do.
 */
#include "oryxmm.h"
#include <stdio.h>

int main(void)
{
	int fd = oryx_mm_thread_tso_on();

	if (fd < 0) {
		printf("hardware TSO unavailable -> keep SOFTWARE memory-model emulation\n");
		printf("(this is the correct, safe default on any device without the "
		       "Oryx driver + a Phase-0-confirmed control bit)\n");
		return 0;
	}

	unsigned flags = oryx_mm_query(fd);
	printf("hardware TSO ENABLED for this thread (flags=0x%x)\n", flags);
	printf("  cap_available=%d state_tso=%d core_limited=%d\n",
	       !!(flags & ORYX_MM_CAP_AVAILABLE),
	       !!(flags & ORYX_MM_STATE_TSO),
	       !!(flags & ORYX_MM_CAP_CORE_LIMIT));
	printf("-> translator may disable memory-model emulation for this thread\n");

	/* ... translated guest thread runs here with hardware ordering ... */

	oryx_mm_close(fd); /* on thread exit: restores weak ordering */
	return 0;
}
