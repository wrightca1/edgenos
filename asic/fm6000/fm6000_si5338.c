/* fm6000_si5338.c — port of Arista Si5338.configure (EOS Si5338.py) for the DCS-7150S-52 ("Rosa"/Quartzy).
 *
 * THE missing cold-boot step: EOS switches the FM6000 reference clock to the Si5338 "Quartzy" output over
 * I2C + a southbridge GPIO — entirely outside the FM6000 register space. Without it the frame-handler PLL
 * runs off the power-on default and the SSCHED scheduler tick never advances (FoundTok=0). The FocalPoint
 * SDK never programs the Si5338; board Python (NorCalInit.py -> Si5338.configure) does. This tool replays it.
 *
 * Steps (Silicon Labs AN428): GPIO ungate -> preConfig -> stream Rosa-Quartzy-0101.si5338 -> postConfig(lock).
 *
 * usage: fm6000_si5338 <i2c-bus-num> <config-file> [nogpio]
 *   i2c-bus-num = the /dev/i2c-N adapter (SCD SMBus) carrying the Si5338 at 7-bit addr 0x70.
 * Build: gcc -O2 -o fm6000_si5338 fm6000_si5338.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/io.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define SI5338_ADDR 0x70

static int I2C = -1;

static int smb(int rw, uint8_t cmd, int size, union i2c_smbus_data *d) {
	struct i2c_smbus_ioctl_data a = { .read_write = rw, .command = cmd, .size = size, .data = d };
	return ioctl(I2C, I2C_SMBUS, &a);
}
static int r8(uint8_t reg) {
	union i2c_smbus_data d;
	if (smb(I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &d) < 0) return -1;
	return d.byte & 0xff;
}
static int w8(uint8_t reg, uint8_t v) {
	union i2c_smbus_data d; d.byte = v;
	return smb(I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &d);
}
static void setPage0(void) { int p = r8(255); if (p >= 0 && (p & 0xff) != 0) w8(255, 0); }

/* configReg: mask 0=skip, 0xFF=write, else read-modify-write (AN428 / Si5338.py configReg) */
static void configReg(int addr, int data, int mask) {
	if (mask == 0) return;
	if (mask == 0xFF) { w8(addr, data); return; }
	int cur = r8(addr); if (cur < 0) cur = 0;
	w8(addr, (cur & ~mask) | (data & mask));
}
/* checkLos: wait until (reg218 & mask)==0 (input-clk / PLL-lock), up to 20 tries */
static int checkLos(int mask) {
	for (int i = 0; i < 20; i++) { int v = r8(218); if (v >= 0 && (v & mask) == 0) return 1; usleep(1000); }
	return 0;
}

/* GPIO: route the 156.25MHz Quartzy oscillator to the Si5338 input via AMD FCH GPIO 66.
 * == `kabinigpio 66 1` (RE'd): FCH AcpiMmio 0xFED80000, GPIO66 reg = 0xFED8166C, set OUTPUT_VALUE bit22. */
static void gpio_enable(void) {
	/* 1. Enable AMD FCH AcpiMmio decode so 0xFED80000 is accessible (PM reg 0x24 bit0, via index/data
	 *    ports 0xCD6/0xCD7). BIOS/EOS sets this; EdgeNOS doesn't -> 0xFED80000 reads 0xffffffff. */
	if (iopl(3) == 0) {
		outb(0x24, 0xCD6); unsigned char pm = inb(0xCD7);
		fprintf(stderr, "[si5338] FCH PM[0x24]=0x%02x\n", pm);
		if (!(pm & 1)) { outb(0x24, 0xCD6); outb(pm | 1, 0xCD7); fprintf(stderr, "[si5338] AcpiMmio decode enabled\n"); }
	} else {
		fprintf(stderr, "[si5338] iopl(3) failed (%s) — cannot enable AcpiMmio\n", strerror(errno));
	}
	/* 2. 7150 = SB800/non-Crow (Si5338.py non-Crow path): AcpiMmio 0xFED80000, byte writes
	 *    0xdbf=1 and 0x1bf=0x40 -> SB800 GPIO 191 (0x1bf-0x100=0xbf=191) = "enable Quartzy clock". */
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { fprintf(stderr, "[si5338] /dev/mem open failed (%s) — skipping GPIO\n", strerror(errno)); return; }
	void *m = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xFED80000);
	if (m == MAP_FAILED) { fprintf(stderr, "[si5338] mmap 0xFED80000 failed (%s) — skipping GPIO\n", strerror(errno)); close(fd); return; }
	volatile uint8_t *g = (volatile uint8_t *)m;
	uint8_t b0 = g[0x1bf];
	g[0xdbf] = 1;
	g[0x1bf] = 0x40;
	__sync_synchronize();
	fprintf(stderr, "[si5338] SB800 GPIO191 (AcpiMmio 0x1bf): 0x%02x -> 0x%02x, 0xdbf<=1 (Quartzy enabled)\n", b0, g[0x1bf]);
	munmap(m, 0x2000); close(fd);
	usleep(50000);
}

int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: %s <i2c-bus-num> <config-file> [nogpio]\n", argv[0]); return 2; }
	int nogpio = (argc > 3 && !strcmp(argv[3], "nogpio"));

	if (!nogpio) gpio_enable();

	char path[64]; snprintf(path, sizeof path, "/dev/i2c-%s", argv[1]);
	I2C = open(path, O_RDWR);
	if (I2C < 0) { fprintf(stderr, "[si5338] open %s failed: %s\n", path, strerror(errno)); return 1; }
	if (ioctl(I2C, I2C_SLAVE, SI5338_ADDR) < 0) { fprintf(stderr, "[si5338] I2C_SLAVE 0x70: %s\n", strerror(errno)); return 1; }

	int probe = r8(0);
	fprintf(stderr, "[si5338] probe reg0=0x%02x (bus %s addr 0x70)\n", probe, argv[1]);
	if (probe < 0) { fprintf(stderr, "[si5338] Si5338 not responding on this bus — wrong /dev/i2c-N?\n"); return 1; }

	/* READ-ONLY mode: dump current state (lock + key config) WITHOUT reprogramming, to see if the
	 * Si5338 is already correctly programmed cold. reg218 & 0x15 == 0 => locked/good. */
	if (!strcmp(argv[2], "read")) {
		int s = r8(218);
		fprintf(stderr, "[si5338] STATUS reg218=0x%02x -> %s (LOS_CLKIN=%d LOS_FDBK=%d PLL_LOL=%d SYS_CAL=%d)\n",
			s, (s & 0x15) == 0 ? "LOCKED/GOOD" : "NOT-LOCKED",
			!!(s & 0x04), !!(s & 0x08), !!(s & 0x10), !!(s & 0x01));
		fprintf(stderr, "[si5338] cfg: r6=0x%02x r27=0x%02x r28=0x%02x r29=0x%02x r31=0x%02x r49=0x%02x r230=0x%02x r241=0x%02x\n",
			r8(6), r8(27), r8(28), r8(29), r8(31), r8(49), r8(230), r8(241));
		/* FULL page-0 dump (256 regs) for byte-for-byte warm-vs-cold diff */
		setPage0();
		fprintf(stderr, "[si5338] DUMP256:");
		for (int r = 0; r < 256; r++) { if ((r & 0xf) == 0) fprintf(stderr, "\n%02x:", r); fprintf(stderr, " %02x", r8(r)); }
		fprintf(stderr, "\n[si5338] END_DUMP256\n");
		return (s & 0x15) == 0 ? 0 : 2;
	}

	/* RELOCK mode: clean auto-calibration WITHOUT rewriting config (config already = Rosa-Quartzy).
	 * The prior run forced a GARBAGE FCAL (r49 bit7) after a failed lock, which PREVENTS locking.
	 * Clear it, soft-reset, and let auto-cal lock. Recovers the FM6000's clock. */
	if (!strcmp(argv[2], "relock")) {
		setPage0();
		{ int v = r8(230); w8(230, 0x10 | (v < 0 ? 0 : v)); }          /* disable outputs */
		{ int v = r8(241); w8(241, 0x80 | (v < 0 ? 0 : v)); }          /* pause LOL */
		{ int v = r8(49);  w8(49,  (v < 0 ? 0 : v) & ~0x80); }         /* FCAL override OFF -> auto-cal */
		w8(246, 0x2);                                                  /* soft reset */
		{ int v = r8(241); w8(241, (v < 0 ? 0 : v) & ~0x80); }         /* restart LOL */
		usleep(30000);
		int lock = 0;
		for (int i = 0; i < 60; i++) { int v = r8(218); if (v >= 0 && (v & 0x15) == 0) { lock = 1; break; } usleep(10000); }
		fprintf(stderr, "[si5338] relock: PLL %s (reg218=0x%02x)\n", lock ? "LOCKED" : "still NOT locked", r8(218));
		if (lock) {   /* copy valid FCAL then use it */
			int lo = r8(235); w8(45, lo); int mm = r8(236); w8(46, mm); int hi = r8(237); int o = r8(47); w8(47, (o & 0xFC) | (hi & 3));
			int v = r8(49); w8(49, (v < 0 ? 0 : v) | 0x80);
		}
		{ int v = r8(230); w8(230, (v < 0 ? 0 : v) & ~0x10); }         /* enable outputs */
		usleep(2000);
		fprintf(stderr, "[si5338] relock DONE: lock=%s\n", lock ? "YES" : "NO");
		return lock ? 0 : 2;
	}

	/* preConfig: page0, disable outputs (OEB_ALL reg230|0x10), pause LOL (reg241|0x80) */
	setPage0();
	{ int v = r8(230); w8(230, 0x10 | (v < 0 ? 0 : v)); }
	{ int v = r8(241); w8(241, 0x80 | (v < 0 ? 0 : v)); }

	/* stream config */
	FILE *f = fopen(argv[2], "r");
	if (!f) { fprintf(stderr, "[si5338] open config %s: %s\n", argv[2], strerror(errno)); return 1; }
	int count = 0, a, dat, m; char line[256];
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, " {%i,%i,%i}", &a, &dat, &m) == 3 && a >= 0 && a <= 255) {
			configReg(a, dat, m);
			count++;
		}
	}
	fclose(f);
	fprintf(stderr, "[si5338] wrote %d config registers\n", count);

	/* postConfig: verify input clk, FCAL-ovrd off, soft-reset, restart LOL, wait, confirm PLL lock */
	setPage0();
	int inok = checkLos(0x4);
	fprintf(stderr, "[si5338] input-clock present (LOS&0x4==0): %s\n", inok ? "yes" : "NO");
	{ int v = r8(49); w8(49, (v < 0 ? 0 : v) & ~0x80); }   /* freqCalibrateOvrd off */
	w8(246, 0x2);                                           /* soft reset */
	{ int v = r8(241); w8(241, (v < 0 ? 0 : v) & ~0x80); } /* restart LOL */
	usleep(25000);
	int lock = checkLos(0x15);
	fprintf(stderr, "[si5338] PLL LOCKED (LOS&0x15==0): %s  (reg218=0x%02x)\n", lock ? "YES" : "no", r8(218));
	/* freqOvrd: copy FCAL 235/236/237 -> 45/46/47[1:0] */
	{ int lo = r8(235); w8(45, lo); int mm = r8(236); w8(46, mm); int hi = r8(237); int o = r8(47); w8(47, (o & 0xFC) | (hi & 3)); }
	{ int v = r8(49); w8(49, (v < 0 ? 0 : v) | 0x80); }    /* freqCalibrateOvrd on */
	{ int v = r8(230); w8(230, (v < 0 ? 0 : v) & ~0x10); } /* enable outputs */
	usleep(2000);

	fprintf(stderr, "[si5338] DONE: lock=%s, %d regs. %s\n", lock ? "YES" : "NO", count,
		lock ? "Quartzy clock is up." : "PLL did NOT lock — check GPIO ungate / config / bus.");
	return lock ? 0 : 2;
}
