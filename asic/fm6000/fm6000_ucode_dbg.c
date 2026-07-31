/* fm6000_ucode_dbg.c - checkpointing microcode loader (phase88).
 *
 * Like fm6000load but writes a progress checkpoint (write#, addr, CAM0) to a log file every N writes
 * and stops the instant the chip off-buses — so we learn the EXACT address where the microcode load
 * hits the memory-write wall (which region needs earlier boot-order bring-up). Checkpoints are synced.
 *
 *   fm6000_ucode_dbg <BDF> <microcode.raw> <ckpt-log>
 * Build: gcc -O2 -o fm6000_ucode_dbg fm6000_ucode_dbg.c
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
	if (argc < 4) { fprintf(stderr, "usage: %s <BDF> <microcode.raw> <ckpt-log>\n", argv[0]); return 2; }
	char path[256];
	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", argv[1]);
	int fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open resource0"); return 1; }
	size_t len = 32u * 1024 * 1024;
	volatile uint32_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { perror("mmap"); return 1; }
	FILE *f = fopen(argv[2], "r");
	if (!f) { perror("open microcode"); return 1; }
	FILE *ck = fopen(argv[3], "w");   /* checkpoint log (persistent) */

#define CK(fmt, ...) do { if (ck) { fprintf(ck, fmt "\n", ##__VA_ARGS__); fflush(ck); fsync(fileno(ck)); } } while (0)

	uint32_t cam0 = m[0x0E000]; __sync_synchronize();
	CK("[ucode-dbg] start CAM0=0x%08x", cam0);
	if (cam0 == 0xffffffff) { CK("[ucode-dbg] ABORT already off-bus"); return 1; }

	unsigned long addr, val, n = 0, skipped = 0, last_addr = 0;
	char line[128];
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "%lx %lx", &addr, &val) != 2 || addr >= len / 4) { skipped++; continue; }
		m[addr] = (uint32_t)val;
		last_addr = addr; n++;
		if ((n % 1000) == 0) {
			__sync_synchronize();
			cam0 = m[0x0E000];
			CK("[ucode-dbg] n=%lu addr=0x%05lx CAM0=0x%08x", n, addr, cam0);
			if (cam0 == 0xffffffff) {
				CK("[ucode-dbg] *** OFF-BUS at write %lu, addr=0x%05lx val=0x%08lx ***", n, addr, val);
				fclose(f); return 1;
			}
		}
	}
	__sync_synchronize();
	cam0 = m[0x0E000];
	CK("[ucode-dbg] DONE n=%lu skipped=%lu last=0x%05lx CAM0=0x%08x %s",
	   n, skipped, last_addr, cam0, cam0 == 0xffffffff ? "OFF-BUS" : "alive");
	fclose(f);
	munmap((void *)m, len); close(fd);
	return cam0 == 0xffffffff ? 1 : 0;
}
