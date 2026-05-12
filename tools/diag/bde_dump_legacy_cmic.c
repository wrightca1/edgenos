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
    struct { uint32_t a; const char *n; } regs[] = {
        {0x050, "SCHAN_CTRL"},
        {0x100, "CMIC_DMA_CTRL (legacy packed CH0-3)"},
        {0x104, "CMIC_DMA_DESC0"},
        {0x108, "CMIC_DMA_DESC1"},
        {0x10c, "CMIC_DMA_STAT (legacy)"},
        {0x110, "CMIC_DMA_PCI_ADDR"},
        {0x114, "CMIC_DMA_STATCLR"},
        {0x118, "CMIC_MISC_CONTROL"},
        {0x150, "CMIC_DMA_DESC2"},
        {0x154, "CMIC_DMA_DESC3"},
        {0x174, "CMIC_ENDIAN_SEL"},
    };
    for (size_t i=0; i<sizeof(regs)/sizeof(regs[0]); i++) {
        struct rio r = {.dev=0, .addr=regs[i].a, .val=0};
        ioctl(fd, R, &r);
        printf("0x%03x %-42s = 0x%08x\n", regs[i].a, regs[i].n, r.val);
    }
    return 0;
}
