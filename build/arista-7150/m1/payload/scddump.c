/* scddump.c - dump full 256KB SCD BAR0 as "offset=value" (M1 has no python).
 *   scddump [resource0-path]   default /sys/bus/pci/devices/0000:04:00.0/resource0
 * NON-DESTRUCTIVE reads; note per-accel +0x30 FIFO / intr-status are pop/clear-on-read.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#define LEN 0x40000
int main(int argc,char**argv){
    const char*p=argc>1?argv[1]:"/sys/bus/pci/devices/0000:04:00.0/resource0";
    int fd=open(p,O_RDWR|O_SYNC); if(fd<0){fd=open(p,O_RDONLY|O_SYNC);} 
    if(fd<0){perror("open");return 1;}
    volatile uint8_t*b=mmap(NULL,LEN,PROT_READ,MAP_SHARED,fd,0);
    if(b==MAP_FAILED){perror("mmap");return 1;}
    for(unsigned o=0;o<LEN;o+=4){
        uint32_t v=*(volatile uint32_t*)(b+o);
        printf("%05x=%08x\n",o,v);
    }
    munmap((void*)b,LEN); close(fd); return 0;
}
