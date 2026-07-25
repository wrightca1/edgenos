/* fm6000load.c - replay an FM6000 CSR image over the PCIe BAR0.
 *
 * Image format is the documented FM6000 CSR/table format: one "<word-addr-hex> <value-hex>" per line
 * (e.g. "00100000 fffffffe"). Each line is a direct CSR write: BAR0[ word<<2 ] = value.
 *
 *   fm6000load <BDF> <image-file> [--verify]
 *
 * IMPORTANT: this TOOL is clean-room / open-source. The IMAGE CONTENT is NOT part of this tool or repo.
 * For a functional pipeline, supply your OWN licensed fm6000Microcode.raw (extract it from an EOS install
 * you own) or a hand-authored minimal personality. This loader ships zero firmware.
 *
 * Build: gcc -O2 -o fm6000load fm6000load.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s <BDF> <image-file> [--verify]\n", argv[0]); return 2; }
	int verify = (argc >= 4 && !strcmp(argv[3], "--verify"));
	char path[256];
	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", argv[1]);
	int fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open resource0"); return 1; }
	size_t len = 32u * 1024 * 1024;			/* FM6000 BAR0 = 32M */
	volatile uint32_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	FILE *f = fopen(argv[2], "r");
	if (!f) { perror("fopen image"); return 1; }

	char line[256];
	unsigned long n = 0, oor = 0;
	while (fgets(line, sizeof line, f)) {
		char *e;
		unsigned long a = strtoul(line, &e, 16);	/* word address */
		if (e == line) continue;			/* blank / comment */
		unsigned long v = strtoul(e, &e, 16);		/* value */
		if (a >= len / 4) { oor++; continue; }
		m[a] = (uint32_t)v;
		n++;
	}
	__sync_synchronize();

	unsigned long mism = 0, checked = 0;
	if (verify) {
		rewind(f);
		while (fgets(line, sizeof line, f)) {
			char *e;
			unsigned long a = strtoul(line, &e, 16);
			if (e == line) continue;
			unsigned long v = strtoul(e, &e, 16);
			if (a >= len / 4) continue;
			checked++;
			if (m[a] != (uint32_t)v) mism++;
		}
	}
	fclose(f); munmap((void *)m, len); close(fd);

	printf("loaded %lu CSR writes (%lu out-of-BAR skipped)", n, oor);
	if (verify)
		printf("; verify: %lu/%lu read-back mismatch (SRAM/TCAM/status regs may legitimately differ)", mism, checked);
	printf("\n");
	return 0;
}
