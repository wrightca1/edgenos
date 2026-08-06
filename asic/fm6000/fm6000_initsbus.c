/* fm6000_initsbus.c - independent reimplementation of the FM6000 SBus master init,
 * plus the SBus transaction engine and its Write/Reset wrappers.
 *
 * Behaviour derived by disassembling libFocalpointSDK.so (fm6000InitSBus @0x478a1f,
 * transaction engine @0x477c54) for hardware interoperability. Contains no
 * third-party code.
 *
 * Brings the JSS/SBus management bus up so
 * the CRM memory-fill engine can reach the block memories (without this the CRM fill off-buses at trigger).
 *
 * Boot order: PrebootSwitch(BistMemoryInit + MrlRegisterFix) -> InitSBus -> ValidateSchedulerToken -> CRM fills.
 * Run AFTER BM-march + MRL + (SOFT_RESET cleared), BEFORE the CRM fills.
 *
 * SBus protocol (decoded phase90): 5-field descriptor -> SBUS_COMMAND(0xF001) dance.
 *   SBUS_COMMAND 0xF001: [7:0]=Register [15:8]=Address [23:16]=Op(0x20 reset/0x21 write/0x22 read)
 *                        [24]=Execute [25]=Busy(poll while set) [28:26]=ResultCode(reset0/write1/read4)
 *   SBUS_REQUEST 0xF002 = data out; SBUS_RESPONSE 0xF003 = data in (reads).
 *
 *   fm6000_initsbus <BDF>
 * Build: gcc -O2 -o fm6000_initsbus fm6000_initsbus.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

#define R_SOFT_RESET 0x00009
#define R_PIN_STRAP  0x1C021
#define R_SBUS_CFG   0x0F000
#define R_SBUS_CMD   0x0F001
#define R_SBUS_REQ   0x0F002
#define R_SBUS_RESP  0x0F003

static volatile uint32_t *M;
static inline void wr(uint32_t w, uint32_t v) { M[w] = v; __sync_synchronize(); }
static inline uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }

/* shared SBus transaction engine (0x477c54). op: 0x20 reset / 0x21 write / 0x22 read. Returns 0 on success
 * (ResultCode matched), 1 on RC mismatch, -2 off-bus, -3 timeout. */
static int sbus_txn(uint8_t op, uint8_t addr_hi, uint8_t reg_lo, uint32_t data_in, uint8_t exp_rc, uint32_t *out)
{
	wr(R_SBUS_REQ, data_in);                 /* REQUEST = data (always) */
	wr(R_SBUS_CMD, 0);                        /* clear stale Execute */
	uint32_t cmd = (uint32_t)reg_lo | ((uint32_t)addr_hi << 8) | ((uint32_t)op << 16) | (1u << 24);
	wr(R_SBUS_CMD, cmd);                      /* trigger (Execute bit24) */
	uint32_t sc; long spins = 0;
	do {
		sc = rd(R_SBUS_CMD);
		if (sc == 0xffffffff) { fprintf(stderr, "[sbus] OFF-BUS during op=0x%x poll\n", op); return -2; }
		if (++spins > 2000000L) { fprintf(stderr, "[sbus] TIMEOUT op=0x%x (Busy stuck, cmd=0x%08x)\n", op, sc); return -3; }
	} while (sc & (1u << 25));                /* wait while Busy(bit25) */
	uint8_t rc = (sc >> 26) & 0x7;
	if (op == 0x22 && out) *out = rd(R_SBUS_RESP);
	if (rc != exp_rc) { fprintf(stderr, "[sbus] op=0x%x rc=%u exp=%u (cmd=0x%08x)\n", op, rc, exp_rc, sc); return 1; }
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <BDF>\n", argv[0]); return 2; }
	char path[256];
	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", argv[1]);
	int fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) { perror("open resource0"); return 1; }
	size_t len = 32u * 1024 * 1024;
	M = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	fprintf(stderr, "[initsbus] start PIN_STRAP=0x%08x SOFT_RESET=0x%08x SBUS_CFG=0x%08x\n",
		rd(R_PIN_STRAP), rd(R_SOFT_RESET), rd(R_SBUS_CFG));

	usleep(1);                                        /* fmDelay(0, 780ns) */
	uint32_t v = rd(R_SOFT_RESET); v &= ~0x8u; wr(R_SOFT_RESET, v);   /* clear bit3 JSSReset */
	uint32_t c = rd(R_SBUS_CFG); if (c != 0) wr(R_SBUS_CFG, 0);       /* clear SBUS_CFG */
	fprintf(stderr, "[initsbus] SOFT_RESET=0x%08x SBUS_CFG=0x%08x; running SBus ops\n", rd(R_SOFT_RESET), rd(R_SBUS_CFG));

	int r1 = sbus_txn(0x21, 0xFE, 0x0A, 0x4, 1, NULL);   /* fm6000WriteSBus(0xFE0A, 0x4) */
	fprintf(stderr, "[initsbus] WriteSBus(0xFE0A,0x4) -> %d  PIN_STRAP=0x%08x\n", r1, rd(R_PIN_STRAP));
	int r2 = sbus_txn(0x20, 0x00, 0x00, 0x0, 0, NULL);   /* fm6000ResetSBus() */
	fprintf(stderr, "[initsbus] ResetSBus -> %d  PIN_STRAP=0x%08x\n", r2, rd(R_PIN_STRAP));

	uint32_t pin = rd(R_PIN_STRAP);
	fprintf(stderr, "[initsbus] done: F000=%08x F001=%08x F002=%08x F003=%08x F004=%08x PIN=%08x (WriteSBus=%d ResetSBus=%d)\n",
		rd(R_SBUS_CFG), rd(R_SBUS_CMD), rd(R_SBUS_REQ), rd(R_SBUS_RESP), rd(0x0F004), pin, r1, r2);
	munmap((void *)M, len); close(fd);   /* read PIN BEFORE unmapping (was a use-after-munmap segfault) */
	return (pin == 0xffffffff) ? 1 : 0;
}
