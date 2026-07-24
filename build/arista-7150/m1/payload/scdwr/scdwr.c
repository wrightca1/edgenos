// scdwr - one-shot in-kernel write to a SCD BAR register (bypasses the M1 userspace
// sysfs-resource0-mmap write gating). Kernel iowrite32 to an ioremap'd BAR reaches the
// device (that's how the scd driver's accel i2c works), unlike the userspace mmap.
//   insmod scdwr.ko off=0x7000 val=0xdead        # SCD power-cycle (reboots to EOS)
//   insmod scdwr.ko off=0x4010 val=0x6 ; rmmod scdwr   # release FM6000 reset, etc.
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/io.h>
static unsigned int off = 0x7000;
static unsigned int val = 0xdead;
static unsigned long base = 0xe1000000UL;   // SCD BAR0 phys (7150 Bodega)
module_param(off, uint, 0);
module_param(val, uint, 0);
module_param(base, ulong, 0);
static int __init scdwr_init(void)
{
	void __iomem *p = ioremap(base + off, 4);
	if (!p) { pr_err("scdwr: ioremap %#lx failed\n", base + off); return -ENOMEM; }
	pr_info("scdwr: [%#lx] %#x -> %#x\n", base + off, ioread32(p), val);
	iowrite32(val, p);
	wmb();
	(void)ioread32(p); /* post the write */
	iounmap(p);
	return 0;
}
static void __exit scdwr_exit(void) {}
module_init(scdwr_init);
module_exit(scdwr_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("one-shot in-kernel SCD BAR register write");
