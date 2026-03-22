/* Minimal static init for open-nos-as5610 initramfs (PPC32).
 * Boot sequence:
 *   1. Mount proc/sys/devtmpfs
 *   2. Wait for sda6 (squashfs rootfs on USB flash)
 *   3. Mount sda6 (squashfs) on /lower (read-only)
 *   4. Mount sda3 (ext2 rw-overlay) on /rw, set up upper+work dirs
 *   5. Mount overlayfs (lower=/lower, upper=/rw/upper, work=/rw/work) on /newroot
 *   6. Fallback: if overlay fails, use /lower (squashfs only, read-only root)
 *   7. switch_root to /newroot → exec /sbin/init
 */
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

static void msg(const char *s)
{
	write(1, "nos-init: ", 10);
	write(1, s, strlen(s));
	write(1, "\n", 1);
}

static int wait_blk(const char *dev, int major, int minor, int secs)
{
	struct stat st;
	int i;
	for (i = 0; i < secs; i++) {
		if (stat(dev, &st) == 0 && S_ISBLK(st.st_mode))
			return 0;
		sleep(1);
	}
	/* Not in devtmpfs: create node manually */
	mknod(dev, S_IFBLK | 0600, makedev(major, minor));
	if (stat(dev, &st) == 0 && S_ISBLK(st.st_mode))
		return 0;
	return -1;
}

/* Move /proc, /sys, /dev from initramfs into newroot so systemd has them. */
static void move_mounts(const char *newroot)
{
	char dst[128];
	const char *srcs[] = { "/proc", "/sys", "/dev", NULL };
	const char **s;
	for (s = srcs; *s; s++) {
		snprintf(dst, sizeof(dst), "%s%s", newroot, *s);
		if (mount(*s, dst, NULL, MS_MOVE, NULL) != 0)
			; /* best-effort; systemd may remount */
	}
}

static void do_switch_root(const char *newroot)
{
	char *const argv[] = { "/sbin/init", NULL };
	char *const envp[] = {
		"HOME=/", "TERM=vt100",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL
	};

	move_mounts(newroot);

	if (chdir(newroot) != 0) { msg("chdir newroot failed"); return; }
	if (mount(".", "/", NULL, MS_MOVE, NULL) != 0) { msg("MS_MOVE failed"); return; }
	if (chroot(".") != 0) { msg("chroot failed"); return; }
	chdir("/");

	msg("exec /sbin/init");
	execve("/sbin/init", argv, envp);
	execve("/etc/init",  argv, envp);
	execve("/bin/init",  argv, envp);
	msg("exec /sbin/init failed");
}

int main(void)
{
	struct stat st;

	mkdir("/proc",   0755); mount("proc",     "/proc", "proc",     0, NULL);
	mkdir("/sys",    0755); mount("sysfs",    "/sys",  "sysfs",    0, NULL);
	mkdir("/dev",    0755); mount("devtmpfs", "/dev",  "devtmpfs", 0, NULL);

	/* Wait for USB flash partitions */
	msg("waiting for /dev/sda6 (rootfs)...");
	if (wait_blk("/dev/sda6", 8, 6, 30) != 0) {
		msg("ERROR: /dev/sda6 not found after 30s");
		for (;;) sleep(1);
	}

	/* Mount squashfs rootfs (read-only) on /lower */
	mkdir("/lower", 0755);
	msg("mounting squashfs from /dev/sda6...");
	if (mount("/dev/sda6", "/lower", "squashfs", MS_RDONLY, NULL) != 0) {
		msg("ERROR: squashfs mount failed");
		for (;;) sleep(1);
	}

	mkdir("/newroot", 0755);

	/* Try overlayfs with sda3 as rw layer */
	mkdir("/rw", 0755);
	if (wait_blk("/dev/sda3", 8, 3, 5) == 0 &&
	    mount("/dev/sda3", "/rw", "ext2", 0, NULL) == 0) {

		mkdir("/rw/upper", 0755);
		mkdir("/rw/work",  0700);

		msg("mounting overlayfs...");
		if (mount("overlay", "/newroot", "overlay", 0,
			  "lowerdir=/lower,upperdir=/rw/upper,workdir=/rw/work") == 0) {
			msg("overlay mounted; switch_root...");
			do_switch_root("/newroot");
			/* If we get here, switch_root failed — fall through */
		} else {
			msg("WARNING: overlayfs mount failed; falling back to read-only root");
		}
	} else {
		msg("WARNING: /dev/sda3 not available; using read-only root");
	}

	/* Fallback: bind-mount /lower to /newroot and boot read-only */
	mount("/lower", "/newroot", NULL, MS_BIND, NULL);
	msg("switch_root (read-only fallback)...");
	do_switch_root("/newroot");

	msg("FATAL: switch_root failed");
	for (;;) sleep(1);
}
