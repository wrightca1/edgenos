/* Minimal AS5610 CPLD fan PWM setter.
 * CPLD base = 0xea000000, reg 0x0D = fan speed (0..31, 5-bit). */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define CPLD_BASE   0xea000000UL
#define CPLD_SIZE   0x20
#define REG_FAN     0x0D
#define PAGE_SIZE   4096
#define PAGE_MASK   (PAGE_SIZE - 1)

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <pwm 0..31>\n", argv[0]);
        return 1;
    }
    unsigned pwm = (unsigned)strtoul(argv[1], NULL, 0);
    if (pwm > 31) {
        fprintf(stderr, "pwm out of range (0..31)\n");
        return 1;
    }
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }
    unsigned long base = CPLD_BASE & ~PAGE_MASK;
    unsigned long off  = CPLD_BASE & PAGE_MASK;
    void *m = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (m == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    volatile unsigned char *cpld = (unsigned char *)m + off;
    unsigned char before = cpld[REG_FAN];
    cpld[REG_FAN] = (unsigned char)pwm;
    unsigned char after = cpld[REG_FAN];
    fprintf(stderr, "fan_pwm: 0x%02x -> 0x%02x (read-back 0x%02x)\n",
            before, pwm, after);
    munmap(m, PAGE_SIZE);
    close(fd);
    return 0;
}
