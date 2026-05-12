#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

struct bde_reg_io { unsigned int dev; unsigned int addr; unsigned int val; };
#define _PPC_IOC(d,t,n,s) (((d)<<30)|((s)<<16)|((t)<<8)|(n))
#define BDE_IOC_REG_READ  _PPC_IOC(3, 'b', 1, sizeof(struct bde_reg_io))

int main(int argc, char **argv) {
    int fd = open("/dev/linux-kernel-bde", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    uint32_t offsets[] = {
        0x31140, 0x31144, 0x31148, 0x3114c,  // DMA_CTRL ch0-3
        0x31158, 0x3115c, 0x31160, 0x31164,  // DMA_DESC ch0-3
        0x31150,                             // DMA_STAT (one reg, status per chan in bits)
        0x174,                               // CMIC_ENDIANESS_SEL
    };
    const char *names[] = {
        "DMA_CTRL[0] (TX)", "DMA_CTRL[1] (RX)", "DMA_CTRL[2]", "DMA_CTRL[3]",
        "DMA_DESC[0]", "DMA_DESC[1]", "DMA_DESC[2]", "DMA_DESC[3]",
        "DMA_STAT", "ENDIAN_SEL",
    };
    for (int i = 0; i < 10; i++) {
        struct bde_reg_io rio = { .dev = 0, .addr = offsets[i], .val = 0 };
        if (ioctl(fd, BDE_IOC_REG_READ, &rio) < 0) { perror("ioctl"); continue; }
        printf("0x%05x %-18s = 0x%08x  DIR=%d EN=%d\n",
               offsets[i], names[i], rio.val, rio.val & 1, (rio.val >> 1) & 1);
    }
    close(fd);
    return 0;
}
