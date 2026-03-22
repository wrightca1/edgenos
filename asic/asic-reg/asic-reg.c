/*
 * asic-reg - BCM56846 PCIe BAR0 register debug tool
 *
 * Maps the ASIC's PCIe BAR0 (0xa0000000, 256KB) via /dev/mem
 * and provides raw CMIC register read/write access.
 *
 * Usage:
 *   asic-reg read <offset>        Read 32-bit register at BAR0 + offset
 *   asic-reg write <offset> <val> Write 32-bit register at BAR0 + offset
 *   asic-reg -d                   Dump key CMIC registers
 *
 * Cross-compile: make CROSS_COMPILE=powerpc-linux-gnu-
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#define BAR0_PHYS       0xa0000000UL
#define BAR0_SIZE       (256 * 1024)    /* 256 KB */

/* CMIC register offsets */
#define CMIC_CONFIG                     0x00000
#define CMIC_DEV_REV_ID                 0x00178
#define CMIC_CMC0_SCHAN_MESSAGE(n)      (0x2C000 + (n) * 4)  /* n = 0..15 */
#define CMIC_CMC0_SCHAN_CTRL            0x2C050
#define CMIC_CMC0_IRQ_STAT0             0x31400
#define CMIC_CMC0_PCIE_IRQ_MASK0        0x31414

/* Big-endian PPC: the ASIC presents little-endian PCI registers.
 * The kernel's ioremap on PPC typically handles byte-swap, but with
 * /dev/mem raw mmap we get the physical bus view.  On the AS5610-52X
 * the host bridge is configured for byte-swap on MMIO, so a normal
 * 32-bit load gives the correct value.  If you see garbage, try
 * toggling the swap macro below. */
#define NEEDS_SWAP 0

static volatile uint8_t *bar0;

static uint32_t reg_read(uint32_t offset)
{
    volatile uint32_t *p = (volatile uint32_t *)(bar0 + offset);
    uint32_t v = *p;
#if NEEDS_SWAP
    v = __builtin_bswap32(v);
#endif
    return v;
}

static void reg_write(uint32_t offset, uint32_t val)
{
    volatile uint32_t *p = (volatile uint32_t *)(bar0 + offset);
#if NEEDS_SWAP
    val = __builtin_bswap32(val);
#endif
    *p = val;
}

static int map_bar0(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        fprintf(stderr, "Run as root.\n");
        return -1;
    }

    bar0 = mmap(NULL, BAR0_SIZE, PROT_READ | PROT_WRITE,
                MAP_SHARED, fd, BAR0_PHYS);
    close(fd);

    if (bar0 == MAP_FAILED) {
        perror("mmap BAR0");
        return -1;
    }
    return 0;
}

static void unmap_bar0(void)
{
    if (bar0 && bar0 != MAP_FAILED)
        munmap((void *)bar0, BAR0_SIZE);
}

struct named_reg {
    uint32_t    offset;
    const char *name;
};

static const struct named_reg dump_regs[] = {
    { CMIC_CONFIG,              "CMIC_CONFIG" },
    { CMIC_DEV_REV_ID,          "CMIC_DEV_REV_ID" },
    { CMIC_CMC0_SCHAN_CTRL,     "CMIC_CMC0_SCHAN_CTRL" },
    { CMIC_CMC0_SCHAN_MESSAGE(0), "CMIC_CMC0_SCHAN_MESSAGE0" },
    { CMIC_CMC0_SCHAN_MESSAGE(1), "CMIC_CMC0_SCHAN_MESSAGE1" },
    { CMIC_CMC0_IRQ_STAT0,      "CMIC_CMC0_IRQ_STAT0" },
    { CMIC_CMC0_PCIE_IRQ_MASK0, "CMIC_CMC0_PCIE_IRQ_MASK0" },
};

static void dump_registers(void)
{
    unsigned i;
    uint32_t val;

    printf("--- BCM56846 CMIC register dump ---\n");
    printf("BAR0 phys: 0x%08lx  size: 0x%x\n\n", BAR0_PHYS, BAR0_SIZE);

    for (i = 0; i < sizeof(dump_regs) / sizeof(dump_regs[0]); i++) {
        val = reg_read(dump_regs[i].offset);
        printf("  0x%05x  %-30s = 0x%08x\n",
               dump_regs[i].offset, dump_regs[i].name, val);
    }

    /* Decode DEV_REV_ID */
    val = reg_read(CMIC_DEV_REV_ID);
    printf("\n  Device ID : 0x%04x  Rev: 0x%02x\n",
           (val >> 16) & 0xffff, val & 0xff);

    printf("\n--- end ---\n");
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s read <offset>          Read register (offset in hex or decimal)\n"
        "  %s write <offset> <value>  Write register\n"
        "  %s -d                      Dump key CMIC registers\n"
        "\n"
        "Offsets are relative to BAR0 (0x%08lx). Max offset: 0x%x.\n"
        "Values accept 0x prefix for hex.\n",
        prog, prog, prog, BAR0_PHYS, BAR0_SIZE - 4);
}

int main(int argc, char **argv)
{
    uint32_t offset, value;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /* -d: dump mode */
    if (strcmp(argv[1], "-d") == 0) {
        if (map_bar0())
            return 1;
        dump_registers();
        unmap_bar0();
        return 0;
    }

    /* read */
    if (strcmp(argv[1], "read") == 0) {
        if (argc < 3) {
            fprintf(stderr, "read requires <offset>\n");
            return 1;
        }
        offset = strtoul(argv[2], NULL, 0);
        if (offset >= BAR0_SIZE || (offset & 3)) {
            fprintf(stderr, "Bad offset 0x%x (must be 4-byte aligned, < 0x%x)\n",
                    offset, BAR0_SIZE);
            return 1;
        }
        if (map_bar0())
            return 1;
        value = reg_read(offset);
        printf("0x%05x = 0x%08x\n", offset, value);
        unmap_bar0();
        return 0;
    }

    /* write */
    if (strcmp(argv[1], "write") == 0) {
        if (argc < 4) {
            fprintf(stderr, "write requires <offset> <value>\n");
            return 1;
        }
        offset = strtoul(argv[2], NULL, 0);
        value  = strtoul(argv[3], NULL, 0);
        if (offset >= BAR0_SIZE || (offset & 3)) {
            fprintf(stderr, "Bad offset 0x%x (must be 4-byte aligned, < 0x%x)\n",
                    offset, BAR0_SIZE);
            return 1;
        }
        if (map_bar0())
            return 1;
        reg_write(offset, value);
        printf("0x%05x <- 0x%08x\n", offset, value);
        /* read back */
        value = reg_read(offset);
        printf("0x%05x => 0x%08x (readback)\n", offset, value);
        unmap_bar0();
        return 0;
    }

    usage(argv[0]);
    return 1;
}
