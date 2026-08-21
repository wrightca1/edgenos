/* fm6000_wdog.c - dataplane watchdog for EdgeNOS.
 *
 * ★ WHY. ⚠ CORRECTED 2026-08-21. This file used to claim the 7150 "already
 * survives a CPU hard lockup" because the kernel command line carries
 * nmi_watchdog=panic and reboot=p. It does not: m0/boot0 passes
 * "rdinit=/init panic=10 nosmp reboot=p,force" and no nmi_watchdog at all.
 * (dmesg confirms the kernel receives that append; /proc/cmdline misleadingly
 * shows Aboot's own line instead, so check dmesg, not /proc/cmdline.)
 *
 * Two things follow, both measured on alpha42:
 *   - /proc/sys/kernel/panic read 0, so a panic HUNG the box rather than
 *     resetting it. init-m1 now sets panic=10 and panic_on_oops=1 explicitly.
 *   - this kernel has NO lockup detectors: softlockup_panic, hung_task_panic
 *     and nmi_watchdog are all absent from /proc/sys/kernel. A silent CPU
 *     lockup is still unrecoverable and needs a kernel config change.
 *
 * So the CPU-wedge path is only PARTLY covered, and this watchdog carries more
 * of the load than the original comment assumed.
 *
 * What is NOT covered is the failure mode EdgeNOS actually produces: Linux
 * healthy, dataplane dead. A bad table write can kill forwarding without
 * touching the CPU, and then nothing reboots the box -- it sits wedged until
 * someone is physically in the lab. boot-config self-revert only acts at BOOT.
 * There is no /dev/watchdog either; the scd driver exposes only
 * interrupt_mask_watchdog5/6/7, not a watchdog device.
 *
 * This closes that gap, which is what makes remote SerDes and table experiments
 * safe to attempt: the worst case becomes "unreachable for ~2 minutes, then back
 * on EOS" instead of "unreachable until someone drives to the lab".
 *
 * ★ WHAT IT CHECKS, in order of how much the signal can be trusted:
 *
 *   1. PIN_STRAP (0x1C021) == 0x208. This is the off-bus test fm6000_linkup
 *      already uses and it is unambiguous: a dead or reset PCI device reads back
 *      all-ones, never 0x208. Reads to a downed device return 0xffffffff rather
 *      than hanging -- observed directly, not assumed.
 *   2. Kernel route count >= a floor. Weaker, and deliberately given a much
 *      longer fuse: routes legitimately churn while OSPF reconverges.
 *
 * ⚠ PING IS NOT USED AS A SIGNAL, on purpose. It collapses to 100% loss on this
 * box for unrelated reasons (CHECKLIST D5) and would reboot a healthy switch.
 *
 * ⚠ ESCAPE HATCH. Touch the disable file and the watchdog stops arming. Every
 * deliberate experiment that takes the dataplane down should create it first --
 * otherwise the watchdog reboots the box mid-experiment and destroys the very
 * state being measured.
 *
 * usage: fm6000_wdog [-b <bdf>] [-i <secs>] [-g <secs>] [-n <strikes>] [-d] [-1]
 *   -d  dry run: report what it WOULD do, never reboot
 *   -1  run one check and exit (for testing)
 *   -g  grace period after start before arming (dataplane bring-up takes time)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/mman.h>

#define PIN_STRAP   0x1C021u
#define PIN_STRAP_OK 0x208u
#define DISABLE_FILE "/mnt/flash/wdog.off"
#define LOG_FILE     "/mnt/flash/wdog.log"

static volatile uint32_t *M;

static void wlog(const char *fmt, ...)
{
	va_list ap;
	FILE *f = fopen(LOG_FILE, "a");
	time_t t = time(NULL);
	char ts[32];
	strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", localtime(&t));
	va_start(ap, fmt);
	if (f) { fprintf(f, "%s ", ts); vfprintf(f, fmt, ap); fputc('\n', f); fclose(f); }
	va_end(ap);
	va_start(ap, fmt);
	fprintf(stderr, "[wdog] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
	va_end(ap);
}


/* capture_pci -- record the ASIC's PCI state into the log before we reboot.
 *
 * Reads the raw config space via sysfs, which keeps working after the device
 * stops responding on MMIO (the upstream bridge answers config cycles). Three
 * things distinguish the failure modes, and the log now says which one it was:
 *
 *   vendor/device == 0xffff  the device is GONE from the bus -- link down, a
 *                            surprise-remove, or the SCD holding it in reset
 *   Status register bit 14   signalled system error / parity, i.e. it faulted
 *   AER uncorrectable status non-zero -> a specific PCIe error was latched
 *
 * Failures here are logged and ignored: this runs on the way to a reboot and
 * must never be what stops the reboot from happening.
 */
static void capture_pci(const char *bdf)
{
	char path[256];
	unsigned char cfg[256];
	int fd, n, i;

	snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/config", bdf);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		wlog("  pci: cannot open %s (%s) -- device may be de-enumerated",
		     path, strerror(errno));
		return;
	}
	n = (int)read(fd, cfg, sizeof cfg);
	close(fd);
	if (n < 8) {
		wlog("  pci: short config read (%d bytes)", n);
		return;
	}
	{
		unsigned ven = cfg[0] | (cfg[1] << 8);
		unsigned dev = cfg[2] | (cfg[3] << 8);
		unsigned cmd = cfg[4] | (cfg[5] << 8);
		unsigned sta = cfg[6] | (cfg[7] << 8);
		wlog("  pci: vendor=0x%04x device=0x%04x command=0x%04x status=0x%04x%s",
		     ven, dev, cmd, sta,
		     ven == 0xffff ? "  <-- DEVICE OFF THE BUS" :
		     (sta & 0x4000) ? "  <-- SIGNALLED SYSTEM ERROR" : "");
	}
	/* first 64 bytes verbatim: cheap, and it is the header that matters */
	for (i = 0; i < 64 && i < n; i += 16)
		wlog("  cfg+%02x: %02x %02x %02x %02x %02x %02x %02x %02x "
		     "%02x %02x %02x %02x %02x %02x %02x %02x", i,
		     cfg[i+0], cfg[i+1], cfg[i+2], cfg[i+3],
		     cfg[i+4], cfg[i+5], cfg[i+6], cfg[i+7],
		     cfg[i+8], cfg[i+9], cfg[i+10], cfg[i+11],
		     cfg[i+12], cfg[i+13], cfg[i+14], cfg[i+15]);

	/* AER, if the kernel exposes it for this device */
	snprintf(path, sizeof path,
	         "/sys/bus/pci/devices/%s/aer_dev_nonfatal", bdf);
	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		char b[512];
		n = (int)read(fd, b, sizeof b - 1);
		if (n > 0) { b[n] = 0; wlog("  aer_nonfatal: %s", b); }
		close(fd);
	}
}

static int route_count(void)
{
	char line[512];
	int n = 0;
	FILE *f = fopen("/proc/net/route", "r");
	if (!f)
		return -1;
	if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }  /* header */
	while (fgets(line, sizeof line, f))
		n++;
	fclose(f);
	return n;
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int interval = 10, grace = 120, strikes = 3, dry = 0, once = 0;
	int route_floor = 2, asic_bad = 0, route_bad = 0;
	char p[256];
	int fd, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (!strcmp(argv[i], "-i") && i + 1 < argc) interval = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-g") && i + 1 < argc) grace = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-n") && i + 1 < argc) strikes = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-r") && i + 1 < argc) route_floor = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-d")) dry = 1;
		else if (!strcmp(argv[i], "-1")) once = 1;
		else { fprintf(stderr, "usage: %s [-b bdf] [-i secs] [-g secs] [-n strikes] [-r floor] [-d] [-1]\n", argv[0]); return 2; }
	}

	snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/resource0", bdf);
	fd = open(p, O_RDWR | O_SYNC);
	if (fd < 0) { perror(p); return 1; }
	M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (M == MAP_FAILED) { perror("mmap"); return 1; }

	wlog("start bdf=%s interval=%ds grace=%ds strikes=%d route_floor=%d%s",
	     bdf, interval, grace, strikes, route_floor, dry ? " DRY-RUN" : "");

	if (!once && grace > 0)
		sleep(grace);

	for (;;) {
		uint32_t ps = M[PIN_STRAP];
		int rc = route_count();
		int disabled = access(DISABLE_FILE, F_OK) == 0;

		__sync_synchronize();

		if (disabled) {
			asic_bad = route_bad = 0;
			if (once) { wlog("disabled by %s; PIN_STRAP=0x%08x routes=%d", DISABLE_FILE, ps, rc); return 0; }
			sleep(interval);
			continue;
		}

		if (ps != PIN_STRAP_OK) asic_bad++; else asic_bad = 0;
		/* routes get a fuse 4x longer: OSPF reconvergence is not a fault */
		if (rc >= 0 && rc < route_floor) route_bad++; else route_bad = 0;

		if (once) {
			wlog("check: PIN_STRAP=0x%08x (%s) routes=%d (%s)",
			     ps, ps == PIN_STRAP_OK ? "ok" : "OFF-BUS",
			     rc, (rc < route_floor) ? "BELOW FLOOR" : "ok");
			return (ps == PIN_STRAP_OK && rc >= route_floor) ? 0 : 1;
		}

		if (asic_bad >= strikes || route_bad >= strikes * 4) {
			wlog("FIRING: PIN_STRAP=0x%08x (%d strikes) routes=%d (%d strikes)%s",
			     ps, asic_bad, rc, route_bad, dry ? " [dry-run, not rebooting]" : "");
			/* Say WHY, not just THAT. The 2026-08-21 firing recorded
			 * PIN_STRAP=0xffffffff -- the ASIC off the bus -- and nothing
			 * about the cause, which left it un-root-causable. PCI config
			 * space survives the ASIC going quiet (the bridge answers), so
			 * capture it before rebooting: a device that vanished reads
			 * vendor 0xffff, whereas an AER/DPC event leaves status bits
			 * set. This is the difference between "it dropped again" and a
			 * diagnosis. */
			capture_pci(bdf);
			if (!dry) {
				sync();
				execl("/sbin/reboot", "reboot", "-f", (char *)NULL);
				execl("/bin/reboot", "reboot", "-f", (char *)NULL);
				wlog("reboot exec failed; falling back to sysrq");
				fd = open("/proc/sysrq-trigger", O_WRONLY);
				if (fd >= 0) { if (write(fd, "b", 1) != 1) wlog("sysrq write failed"); close(fd); }
				return 1;
			}
			asic_bad = route_bad = 0;
		}
		sleep(interval);
	}
	return 0;
}
