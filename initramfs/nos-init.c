/* Minimal init for EdgeNOS initramfs (PPC32 e500v2).
 *
 * Uses raw Linux syscalls only - NO libc linkage.
 * The e500v2 core has no classic FPU (uses SPE instead), and
 * Debian's powerpc glibc is built for generic PPC with FPU,
 * so any statically linked glibc binary will crash with
 * "illegal instruction" on e500v2.
 *
 * Boot sequence:
 *   1. Mount proc/sys/devtmpfs
 *   2. Wait for /dev/sda6 (squashfs rootfs on USB flash)
 *   3. Mount squashfs on /lower (read-only)
 *   4. Mount ext2 from /dev/sda3 on /rw, set up overlayfs
 *   5. switch_root to /newroot, exec /sbin/init
 *   6. Fallback: read-only squashfs root if overlay fails
 */

/* Raw syscall interface - no libc */
#define __NR_exit       1
#define __NR_write      4
#define __NR_mkdir      39
#define __NR_chdir      12
#define __NR_chroot     61
#define __NR_mount      21
#define __NR_umount2    52
#define __NR_mknod      14
#define __NR_stat       106
#define __NR_execve     11
#define __NR_nanosleep  162
#define __NR_pivot_root 203
#define __NR_reboot     88
#define __NR_fork       2
#define __NR_waitpid    7

/* stat64 structure for powerpc */
struct kernel_stat {
    unsigned long long st_dev;
    unsigned long long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long long st_rdev;
    unsigned short __pad2;
    long long st_size;
    long st_blksize;
    long long st_blocks;
    long st_atime;
    unsigned long st_atime_nsec;
    long st_mtime;
    unsigned long st_mtime_nsec;
    long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long __unused4;
    unsigned long __unused5;
};

struct timespec {
    long tv_sec;
    long tv_nsec;
};

static long syscall0(long nr)
{
    register long r0 __asm__("r0") = nr;
    register long r3 __asm__("r3") = 0;
    __asm__ __volatile__("sc" : "+r"(r3) : "r"(r0) : "memory", "cr0");
    return r3;
}

static long syscall1(long nr, long a1)
{
    register long r0 __asm__("r0") = nr;
    register long r3 __asm__("r3") = a1;
    __asm__ __volatile__("sc" : "+r"(r3) : "r"(r0) : "memory", "cr0");
    return r3;
}

static long syscall2(long nr, long a1, long a2)
{
    register long r0 __asm__("r0") = nr;
    register long r3 __asm__("r3") = a1;
    register long r4 __asm__("r4") = a2;
    __asm__ __volatile__("sc" : "+r"(r3) : "r"(r0), "r"(r4) : "memory", "cr0");
    return r3;
}

static long syscall3(long nr, long a1, long a2, long a3)
{
    register long r0 __asm__("r0") = nr;
    register long r3 __asm__("r3") = a1;
    register long r4 __asm__("r4") = a2;
    register long r5 __asm__("r5") = a3;
    __asm__ __volatile__("sc" : "+r"(r3) : "r"(r0), "r"(r4), "r"(r5) : "memory", "cr0");
    return r3;
}

static long syscall5(long nr, long a1, long a2, long a3, long a4, long a5)
{
    register long r0 __asm__("r0") = nr;
    register long r3 __asm__("r3") = a1;
    register long r4 __asm__("r4") = a2;
    register long r5 __asm__("r5") = a3;
    register long r6 __asm__("r6") = a4;
    register long r7 __asm__("r7") = a5;
    __asm__ __volatile__("sc" : "+r"(r3) : "r"(r0), "r"(r4), "r"(r5), "r"(r6), "r"(r7) : "memory", "cr0");
    return r3;
}

#define MS_RDONLY  1
#define MS_MOVE    8192
#define MS_BIND    4096

#define S_IFBLK    0060000
#define S_ISBLK(m) (((m) & 0170000) == S_IFBLK)

static void wrstr(const char *s)
{
    const char *p = s;
    while (*p) p++;
    if (p > s)
        syscall3(__NR_write, 1, (long)s, p - s);
}

static void msg(const char *s)
{
    wrstr("nos-init: ");
    wrstr(s);
    wrstr("\n");
}

static int do_mount(const char *src, const char *dst, const char *fs,
                    unsigned long flags, const char *data)
{
    return syscall5(__NR_mount, (long)src, (long)dst, (long)fs, flags, (long)data);
}

static int do_mkdir(const char *path, int mode)
{
    return syscall2(__NR_mkdir, (long)path, mode);
}

static int do_stat(const char *path, struct kernel_stat *st)
{
    return syscall2(__NR_stat, (long)path, (long)st);
}

static int do_mknod(const char *path, int mode, int dev)
{
    return syscall3(__NR_mknod, (long)path, mode, dev);
}

static void do_sleep(int secs)
{
    struct timespec ts = { secs, 0 };
    syscall2(__NR_nanosleep, (long)&ts, 0);
}

static int do_chdir(const char *path)
{
    return syscall1(__NR_chdir, (long)path);
}

static int do_chroot(const char *path)
{
    return syscall1(__NR_chroot, (long)path);
}

static void do_exit(int code)
{
    syscall1(__NR_exit, code);
}

#define makedev(maj, min) (((maj) << 8) | (min))

/* Try to open the device to check if it exists (avoids stat struct issues) */
#define __NR_open  5
#define __NR_close 6
#define O_RDONLY   0

static int wait_blk(const char *dev, int major, int minor, int secs)
{
    int i, fd;
    for (i = 0; i < secs; i++) {
        fd = syscall2(__NR_open, (long)dev, O_RDONLY);
        if (fd >= 0) {
            syscall1(__NR_close, fd);
            return 0;
        }
        do_sleep(1);
    }
    /* Last resort: create node manually */
    do_mknod(dev, S_IFBLK | 0600, makedev(major, minor));
    fd = syscall2(__NR_open, (long)dev, O_RDONLY);
    if (fd >= 0) {
        syscall1(__NR_close, fd);
        return 0;
    }
    return -1;
}

static void move_mount(const char *src, const char *newroot)
{
    /* Build dst = newroot + src manually */
    char dst[64];
    int j = 0;
    const char *p;
    for (p = newroot; *p && j < 60; ) dst[j++] = *p++;
    for (p = src; *p && j < 63; ) dst[j++] = *p++;
    dst[j] = 0;
    do_mount(src, dst, 0, MS_MOVE, 0);
}

static void do_switch_root(const char *newroot)
{
    char *argv[] = { "/sbin/init", 0 };
    char *envp[] = { "HOME=/", "TERM=vt100",
                     "PATH=/sbin:/bin:/usr/sbin:/usr/bin", 0 };

    move_mount("/proc", newroot);
    move_mount("/sys", newroot);
    move_mount("/dev", newroot);

    if (do_chdir(newroot) != 0) { msg("chdir failed"); return; }
    if (do_mount(".", "/", 0, MS_MOVE, 0) != 0) { msg("MS_MOVE failed"); return; }
    if (do_chroot(".") != 0) { msg("chroot failed"); return; }
    do_chdir("/");

    msg("exec /sbin/init");
    syscall3(__NR_execve, (long)argv[0], (long)argv, (long)envp);
    msg("execve failed");
}

void _start(void)
{
    do_mkdir("/proc", 0755);   do_mount("proc", "/proc", "proc", 0, 0);
    do_mkdir("/sys", 0755);    do_mount("sysfs", "/sys", "sysfs", 0, 0);
    do_mkdir("/dev", 0755);    do_mount("devtmpfs", "/dev", "devtmpfs", 0, 0);

    /* Wait for USB storage to enumerate then mount squashfs.
     * Create device node manually (devtmpfs may be slow) and
     * retry mount in a loop since the disk appears ~1s after boot. */
    do_mkdir("/lower", 0755);
    do_mknod("/dev/sda6", S_IFBLK | 0600, makedev(8, 6));
    msg("waiting for /dev/sda6...");
    {
        int i, mounted = 0;
        for (i = 0; i < 30; i++) {
            if (do_mount("/dev/sda6", "/lower", "squashfs", MS_RDONLY, 0) == 0) {
                mounted = 1;
                break;
            }
            do_sleep(1);
        }
        if (!mounted) {
            msg("ERROR: squashfs mount failed after 30s");
            for (;;) do_sleep(10);
        }
    }
    msg("squashfs mounted");

    do_mkdir("/newroot", 0755);
    do_mkdir("/rw", 0755);
    do_mknod("/dev/sda3", S_IFBLK | 0600, makedev(8, 3));

    /* Try mounting sda3; if it fails (unformatted), format it using
     * mke2fs from the squashfs rootfs we just mounted at /lower */
    if (do_mount("/dev/sda3", "/rw", "ext2", 0, 0) != 0) {
        msg("sda3 mount failed - formatting...");
        {
            /* Run /lower/sbin/mke2fs to format sda3 */
            char *argv[] = { "/lower/sbin/mke2fs", "-L", "NOS-RW", "/dev/sda3", 0 };
            char *envp[] = { "PATH=/lower/sbin:/lower/usr/sbin:/lower/bin:/lower/usr/bin",
                             "LD_LIBRARY_PATH=/lower/lib:/lower/usr/lib", 0 };
            int pid = syscall0(__NR_fork);
            if (pid == 0) {
                /* child */
                syscall3(__NR_execve, (long)argv[0], (long)argv, (long)envp);
                do_exit(1);
            } else if (pid > 0) {
                /* parent: wait for child */
                int status = 0;
                syscall3(__NR_waitpid, pid, (long)&status, 0);
            }
        }
        /* Try mount again after format */
        if (do_mount("/dev/sda3", "/rw", "ext2", 0, 0) != 0) {
            msg("sda3 still failed after format - read-only fallback");
            goto readonly;
        }
        msg("sda3 formatted and mounted");
    }

    do_mkdir("/rw/upper", 0755);
    do_mkdir("/rw/work", 0700);

    msg("mounting overlay...");
    if (do_mount("overlay", "/newroot", "overlay", 0,
                 "lowerdir=/lower,upperdir=/rw/upper,workdir=/rw/work") == 0) {
        msg("overlay OK");
        do_switch_root("/newroot");
    } else {
        msg("overlay failed, read-only fallback");
    }

readonly:

    do_mount("/lower", "/newroot", 0, MS_BIND, 0);
    msg("read-only switch_root");
    do_switch_root("/newroot");

    msg("FATAL: switch_root failed");
    for (;;) do_sleep(10);
}
