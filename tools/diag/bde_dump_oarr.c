#include <stdio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
struct rio { unsigned int dev, addr, val; };
#define _PPC_IOC(d,t,n,s) (((d)<<30)|((s)<<16)|((t)<<8)|(n))
#define R _PPC_IOC(3, 'b', 1, sizeof(struct rio))
int main() {
    int fd = open("/dev/linux-kernel-bde", O_RDWR);
    // PAXB OARR + outbound config registers, all in BAR0 0x2xxx region
    struct { uint32_t a; const char *n; } regs[] = {
        {0x2104, "PCIE_EP_AXI_CONFIG"},
        {0x2c00, "IMAP0_0"}, {0x2c04, "IMAP0_1"}, {0x2c08, "IMAP0_2"},
        {0x2c0c, "IMAP0_3"}, {0x2c10, "IMAP0_4"}, {0x2c14, "IMAP0_5"},
        {0x2c18, "IMAP0_6"}, {0x2c1c, "IMAP0_7"},
        {0x2d10, "OARR_0"},  {0x2d14, "OARR_0_UPPER"},
        {0x2d50, "OARR_1"},  {0x2d54, "OARR_1_UPPER"},
        {0x2d60, "OARR_2"},  {0x2d64, "OARR_2_UPPER"},
        {0x2030, "PAXB_ENDIAN"},
        {0x000, "BAR0[0] (chip-rev / device-id)"},
        {0x100, "CMIC_DMA_CTRL"},
        {0x104, "CMIC_DMA_STAT"},
        {0x110, "CMIC_DMA_DESC0r ch0"},
        {0x114, "CMIC_DMA_DESC0r ch1"},
    };
    for (size_t i=0; i<sizeof(regs)/sizeof(regs[0]); i++) {
        struct rio r = {.dev=0, .addr=regs[i].a, .val=0};
        ioctl(fd, R, &r);
        printf("0x%04x %-35s = 0x%08x\n", regs[i].a, regs[i].n, r.val);
    }
    return 0;
}
