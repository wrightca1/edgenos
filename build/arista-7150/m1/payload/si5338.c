/*
 * si5338.c - clean-room Silabs Si5338 clock-generator programmer for EdgeNOS M2.
 *
 * The FM6000's PCIe reference clock comes from an Si5338 on the SCD SMBus
 * (accel #1, bus 1, i2c addr 0x70). Without it programmed, the FM6000 has no
 * refclk, its PCIe link never trains, and 02:00.0 never enumerates. This tool
 * loads a Silabs AN428 "JumpStart" register map (.si5338 / reg-map.h format)
 * over a Linux i2c-dev adapter and runs the AN428 lock sequence.
 *
 * Behaviour is reconstructed from the public Silabs AN428 application note
 * (register semantics: reg 255 page select, 230 OEB, 241 LoL, 218 LOS status,
 * 246 soft reset, 49 FCAL override, 45-47/235-237 FCAL copy). No Arista code is
 * reproduced; the board's register-map data file is supplied at runtime and is
 * NOT bundled (stage it on-box from /usr/share/firmware/).
 *
 * Usage:
 *   si5338 <i2c-bus> <regmap.si5338> [-a addr] [-n] [-v]
 *     <i2c-bus>       e.g. 3  -> /dev/i2c-3   (the SCD accel1/bus1 adapter)
 *     <regmap>        Silabs AN428 header:  { addr, 0xval, 0xmask },
 *     -a addr         i2c slave address (default 0x70)
 *     -n              parse + dry-run (no i2c writes); prints the plan
 *     -v              verbose (per-register trace)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

/* --- SMBus byte-data primitives via I2C_SMBUS ioctl (no libi2c dependency) --- */
static int g_fd = -1;
static int g_dry = 0, g_verbose = 0;

static int smbus_xfer(uint8_t rw, uint8_t cmd, int size, union i2c_smbus_data *d)
{
	struct i2c_smbus_ioctl_data a = { .read_write = rw, .command = cmd,
					  .size = size, .data = d };
	return ioctl(g_fd, I2C_SMBUS, &a);
}

static int rd(uint8_t reg)			/* returns 0..255, or -1 on error */
{
	union i2c_smbus_data d;
	if (g_dry)
		return 0;
	if (smbus_xfer(I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &d) < 0)
		return -1;
	return d.byte & 0xff;
}

static int wr(uint8_t reg, uint8_t val)
{
	union i2c_smbus_data d;
	d.byte = val;
	if (g_verbose)
		printf("    w reg %3u <= 0x%02x\n", reg, val);
	if (g_dry)
		return 0;
	return smbus_xfer(I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &d);
}

static void msleep(long ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

/* --- AN428 register map (from the .si5338 file) --- */
struct reg { uint8_t addr, val, mask; };
#define MAX_REGS 512
static struct reg g_map[MAX_REGS];
static int g_nregs;

/* Parse lines of the form  { addr, 0xval, 0xmask },  (C-header AN428 export).
 * Also accepts  { addr, 0xval },  (mask defaults 0xff, reg.txt style). Ignores
 * comment and boilerplate lines (only rows beginning with '{' are parsed). */
static int parse_map(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256];
	if (!f) { perror(path); return -1; }
	g_nregs = 0;
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p != '{')			/* only "{ a, v, m }" data rows */
			continue;
		p++;
		unsigned a, v, m = 0xff;
		int got = sscanf(p, " %i , %i , %i", &a, &v, &m);
		if (got < 2)
			continue;
		if (a > 255 || v > 255 || m > 255) {
			fprintf(stderr, "bad row (out of range): %s", line);
			fclose(f); return -1;
		}
		if (g_nregs >= MAX_REGS) {
			fprintf(stderr, "too many registers (>%d)\n", MAX_REGS);
			fclose(f); return -1;
		}
		g_map[g_nregs].addr = a;
		g_map[g_nregs].val  = v;
		g_map[g_nregs].mask = m;
		g_nregs++;
	}
	fclose(f);
	return g_nregs;
}

/* --- AN428 helpers (register semantics per the application note) --- */
static int set_page(int page)			/* reg 255 = page select (0/1) */
{
	int pg = rd(255);
	if (pg < 0) return -1;
	if ((pg & 1) != page)
		return wr(255, page);
	return 0;
}

static int rmw(uint8_t reg, uint8_t data, uint8_t mask)
{
	int cur;
	if (mask == 0)				/* mask 0 => read-only, skip */
		return 0;
	if (mask == 0xff)
		return wr(reg, data);
	cur = rd(reg);
	if (cur < 0) return -1;
	return wr(reg, (cur & ~mask) | (data & mask));
}

/* poll LOS status (reg 218) until (val & mask)==0, up to 20 tries */
static int check_los(uint8_t mask)
{
	int i, v;
	for (i = 0; i < 20; i++) {
		v = rd(218);
		if (v < 0) return -1;
		if ((v & mask) == 0)
			return 1;		/* locked/present */
	}
	return 0;				/* still asserted */
}

static int program(void)
{
	int i, v, lo, mi, hi, o;

	/* preConfig: page 0, disable output drive (230 |=0x10), pause LoL (241 |=0x80) */
	if (set_page(0) < 0) goto ioerr;
	if ((v = rd(230)) < 0) goto ioerr;
	if (wr(230, v | 0x10) < 0) goto ioerr;		/* OEB_ALL: outputs off */
	if ((v = rd(241)) < 0) goto ioerr;
	if (wr(241, v | 0x80) < 0) goto ioerr;		/* pause LoL */

	/* write the register map in file order (reg-255 rows switch the page) */
	printf("  writing %d-register map...\n", g_nregs);
	for (i = 0; i < g_nregs; i++)
		if (rmw(g_map[i].addr, g_map[i].val, g_map[i].mask) < 0)
			goto ioerr;

	/* postConfig / AN428 lock sequence */
	if (set_page(0) < 0) goto ioerr;
	if (!check_los(0x04))				/* input clock present? */
		fprintf(stderr, "  WARN: input-clock LOS (reg218 & 0x04) still set; continuing\n");

	if ((v = rd(49)) < 0) goto ioerr;
	if (wr(49, v & ~0x80) < 0) goto ioerr;		/* freqCalibrateOvrd(false) */
	if (wr(246, 0x02) < 0) goto ioerr;		/* soft reset -> init PLL lock */
	if ((v = rd(241)) < 0) goto ioerr;
	if (wr(241, v & ~0x80) < 0) goto ioerr;		/* restart LoL */
	msleep(25);

	if (!check_los(0x15)) {				/* PLL locked + calibrated? */
		fprintf(stderr, "  ERROR: PLL not locked (reg218 & 0x15 set after 25ms)\n");
		return 2;
	}

	/* freqOvrd: copy FCAL result (235/236/237) into PLL config (45/46/47) */
	if ((lo = rd(235)) < 0 || wr(45, lo) < 0) goto ioerr;
	if ((mi = rd(236)) < 0 || wr(46, mi) < 0) goto ioerr;
	if ((hi = rd(237)) < 0) goto ioerr;
	if ((o  = rd(47))  < 0) goto ioerr;
	if (wr(47, (o & 0xfc) | (hi & 0x03)) < 0) goto ioerr;

	if ((v = rd(49)) < 0) goto ioerr;
	if (wr(49, v | 0x80) < 0) goto ioerr;		/* freqCalibrateOvrd(true) */
	if ((v = rd(230)) < 0) goto ioerr;
	if (wr(230, v & ~0x10) < 0) goto ioerr;		/* enable output drive */

	printf("  Si5338 locked and outputs enabled.\n");
	return 0;
ioerr:
	perror("  i2c transfer");
	return 1;
}

int main(int argc, char **argv)
{
	const char *bus = NULL, *map = NULL;
	int addr = 0x70, i, rc;
	char dev[64];

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-a") && i + 1 < argc)
			addr = strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "-n"))
			g_dry = 1;
		else if (!strcmp(argv[i], "-v"))
			g_verbose = 1;
		else if (!bus)
			bus = argv[i];
		else if (!map)
			map = argv[i];
	}
	if (!bus || !map) {
		fprintf(stderr,
			"usage: si5338 <i2c-bus> <regmap.si5338> [-a addr] [-n] [-v]\n"
			"  <i2c-bus>  number N -> /dev/i2c-N (the SCD accel1/bus1 adapter)\n"
			"  -a addr    slave address (default 0x70)\n"
			"  -n         dry-run (parse only, no i2c)\n");
		return 2;
	}

	if (parse_map(map) < 0)
		return 1;
	printf("si5338: parsed %d registers from %s\n", g_nregs, map);
	if (g_nregs == 0) { fprintf(stderr, "no registers parsed\n"); return 1; }

	if (g_dry) {
		printf("si5338: dry-run (no i2c). Would program addr 0x%02x on %s.\n",
		       addr, bus);
		return program();		/* g_dry short-circuits all i2c */
	}

	snprintf(dev, sizeof dev, "/dev/i2c-%s", bus);
	g_fd = open(dev, O_RDWR);
	if (g_fd < 0) {
		/* allow passing a full path too */
		g_fd = open(bus, O_RDWR);
		if (g_fd < 0) { perror(dev); return 1; }
	}
	if (ioctl(g_fd, I2C_SLAVE, addr) < 0) {
		perror("I2C_SLAVE (busy? wrong bus?)");
		close(g_fd); return 1;
	}
	printf("si5338: programming addr 0x%02x on %s ...\n", addr, dev);
	rc = program();
	close(g_fd);
	return rc;
}
