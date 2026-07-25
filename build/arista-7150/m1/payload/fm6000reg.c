/* fm6000reg.c - read/write FM6000 CSRs via the PCIe BAR0 (sysfs resource0 mmap).
 *
 *   fm6000reg <BDF> <word_reg>          # read   (FM6000 word address; BAR byte offset = word<<2)
 *   fm6000reg <BDF> <word_reg> <val>    # write
 *   e.g.  fm6000reg 0000:02:00.0 0x1C021        -> PIN_STRAP_STAT (expect 0x208)
 *
 * Clean-room: mmaps /sys/bus/pci/devices/<BDF>/resource0 (bypasses STRICT_DEVMEM, unlike /dev/mem)
 * and uses the documented FM6000 word-addressing (byte offset = word<<2). No proprietary content.
 * Build: gcc -O2 -o fm6000reg fm6000reg.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s <BDF> <word_reg> [val]\n", argv[0]); return 2; }
	char path[256];
	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", argv[1]);
	int fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open resource0"); return 1; }
	size_t len = 32u * 1024 * 1024;			/* FM6000 BAR0 = 32M */
	volatile uint32_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	unsigned long reg = strtoul(argv[2], NULL, 0);	/* FM6000 word address */
	if (reg >= len / 4) { fprintf(stderr, "reg 0x%lx out of 32M BAR\n", reg); return 1; }
	if (argc >= 4) {
		uint32_t v = (uint32_t)strtoul(argv[3], NULL, 0);
		m[reg] = v; __sync_synchronize();
		printf("%s reg[0x%lx] <= 0x%08x\n", argv[1], reg, v);
	} else {
		uint32_t v = m[reg];
		printf("%s reg[0x%lx] = 0x%08x\n", argv[1], reg, v);
	}
	munmap((void *)m, len); close(fd);
	return 0;
}
