#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    volatile uint32_t *bar0 = mmap(NULL, 256*1024, PROT_READ|PROT_WRITE,
                                   MAP_SHARED, fd, 0xa0000000);
    if (bar0 == MAP_FAILED) { perror("mmap"); return 1; }

    struct { uint32_t a; const char *n; } regs[] = {
        {0x000, "BAR0[0]"},
        {0x050, "SCHAN_CTRL"},
        {0x100, "CMIC_DMA_CTRL"},
        {0x104, "CMIC_DMA_STAT"},
        {0x108, "CMIC_DMA_DESC1?"},
        {0x10c, "CMIC_DMA_STAT2?"},
        {0x110, "CMIC_DMA_DESC0r ch0 (TX)"},
        {0x114, "CMIC_DMA_DESC0r ch1 (RX)"},
        {0x118, "CMIC_DMA_DESC0r ch2"},
        {0x11c, "CMIC_DMA_DESC0r ch3"},
        {0x150, "CMIC_DMA_DESC2"},
        {0x174, "CMIC_ENDIAN_SEL"},
        {0x2104, "PCIE_EP_AXI_CONFIG"},
        {0x2c00, "IMAP0_0"}, {0x2c04, "IMAP0_1"}, {0x2c1c, "IMAP0_7"},
        {0x2d10, "OARR_0"}, {0x2d14, "OARR_0_UPPER"},
        {0x2d50, "OARR_1"}, {0x2d54, "OARR_1_UPPER"},
        {0x2d60, "OARR_2"}, {0x2d64, "OARR_2_UPPER"},
        {0x2030, "PAXB_ENDIAN"},
    };
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        uint32_t v = bar0[regs[i].a / 4];
        uint32_t s = ((v & 0xff) << 24) | ((v & 0xff00) << 8)
                   | ((v & 0xff0000) >> 8) | ((v >> 24) & 0xff);
        printf("0x%04x %-30s raw=0x%08x  swap=0x%08x\n",
               regs[i].a, regs[i].n, v, s);
    }
    return 0;
}
