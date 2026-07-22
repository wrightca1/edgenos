/*
 * resettool.c - try a specific x86 hardware-reset method by name (reboot debug).
 * The 7150's AMD SB700 CF9 reset is a documented hang; public guidance is
 * reboot=kbd (8042) or reboot=acpi. This tries each mechanism at runtime.
 *   resettool kbd | cf9 | cf9cold
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/io.h>

int main(int argc, char **argv)
{
	const char *m = (argc > 1) ? argv[1] : "kbd";
	if (ioperm(0x60, 0x10, 1) || ioperm(0xcf9, 1, 1)) {
		perror("ioperm (need root/CAP_SYS_RAWIO)"); return 1;
	}
	printf("resettool: sync + trying '%s' reset...\n", m);
	fflush(stdout); sync(); sync(); usleep(100000);
	if (!strcmp(m, "kbd")) {
		outb(0xfe, 0x64);
	} else if (!strcmp(m, "cf9")) {
		outb(0x02, 0xcf9); usleep(50); outb(0x06, 0xcf9);
	} else if (!strcmp(m, "cf9cold")) {
		outb(0x02, 0xcf9); usleep(50); outb(0x0e, 0xcf9);
	} else {
		fprintf(stderr, "unknown method '%s' (kbd|cf9|cf9cold)\n", m); return 2;
	}
	usleep(700000);
	printf("resettool: '%s' did NOT reset (still alive)\n", m);
	return 0;
}
