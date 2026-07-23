/*
 * scdreg.c - read/write SCD FPGA registers via BAR0 resource0 mmap.
 * SCD = PCI 3475:0001 @ 04:00.0, BAR0 phys 0xe1000000, 256 KiB (phase13 probe).
 *   scdreg <byte-off>            # read  (hex, e.g. 0x4000)
 *   scdreg <byte-off> <value>    # write (hex)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define SCD_RES "/sys/bus/pci/devices/0000:04:00.0/resource0"

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: scdreg <off> [value]\n"); return 2; }
	unsigned long off = strtoul(argv[1], NULL, 0);
	int fd = open(SCD_RES, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open resource0 (scd bound? root?)"); return 1; }
	size_t len = 256 * 1024;
	volatile uint8_t *bar = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (bar == MAP_FAILED) { perror("mmap"); return 1; }
	if (off + 4 > len) { fprintf(stderr, "off out of range\n"); return 2; }
	volatile uint32_t *r = (volatile uint32_t *)(bar + off);
	if (argc >= 3) {
		uint32_t v = (uint32_t)strtoul(argv[2], NULL, 0);
		*r = v;
		/* flush any write-combining buffer + post the MMIO write to the
		 * device (munmap does NOT flush WC; only a fence/read-back does). */
		__asm__ __volatile__("mfence" ::: "memory");
		{ volatile uint32_t rb = *r; (void)rb; }
		printf("scd[0x%05lx] <= 0x%08x\n", off, v);
	} else {
		printf("scd[0x%05lx] = 0x%08x\n", off, *r);
	}
	munmap((void *)bar, len); close(fd); return 0;
}
