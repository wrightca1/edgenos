#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>

int main(void) {
    // Direct /dev/mem mmap of BAR0 — bypasses BDE's AXI remap entirely.
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    volatile uint32_t *bar0 = mmap(NULL, 256*1024, PROT_READ|PROT_WRITE,
                                    MAP_SHARED, fd, 0xa0000000);
    if (bar0 == MAP_FAILED) { perror("mmap"); return 1; }

    // Each test address — show raw and byte-swapped (BE host).
    uint32_t addrs[] = {
        0x00174,   // ENDIAN_SEL (we know it = 0x04040404, sub-window 0)
        0x07140,   // sub-window 7 area (should be remapped page+0x140)
        0x10000,   // CMIC_COMMON 0x10000 - direct BAR0 if accessible
        0x10178,   // DEV_REV_ID per Cumulus capture (expect 0x0002b846)
        0x31140,   // CMICM DMA_CTRL[0] - this is what edged writes via AXI remap
        0x31144,   // DMA_CTRL[1]
        0x31158,   // DMA_DESC[0] - we should see a DMA pool addr here
        0x32800,   // SCHAN_CTRL (we know this should be 0)
        0x33000,   // SCHAN_MSG[0]
    };
    const char *names[] = {
        "ENDIAN_SEL",
        "BAR0+0x7140 (sub-win7)",
        "CMIC_COMMON",
        "DEV_REV_ID? (expect 0xb846)",
        "DMA_CTRL[0]",
        "DMA_CTRL[1]",
        "DMA_DESC[0]",
        "SCHAN_CTRL",
        "SCHAN_MSG[0]",
    };
    for (int i = 0; i < 9; i++) {
        uint32_t v = bar0[addrs[i] / 4];
        // Byte-swap for LE interpretation since PPC reads raw BE
        uint32_t s = ((v & 0xff) << 24) | ((v & 0xff00) << 8)
                   | ((v & 0xff0000) >> 8) | ((v >> 24) & 0xff);
        printf("0x%05x %-30s raw=0x%08x  swap=0x%08x\n",
               addrs[i], names[i], v, s);
    }
    return 0;
}
