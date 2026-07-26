/* fm6000dump - bulk-read a word range of FM6000 BAR0, print non-zero words.
 * usage: fm6000dump <BDF> <start_word> <count>   (word addr = byte<<2) */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
int main(int argc, char **argv){
    if(argc<4){ fprintf(stderr,"usage: %s <BDF> <start_word> <count>\n",argv[0]); return 1; }
    char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
    int fd=open(p,O_RDONLY); if(fd<0){ perror("open"); return 1; }
    size_t len=0x2000000;
    volatile uint32_t *bar=mmap(NULL,len,PROT_READ,MAP_SHARED,fd,0);
    if(bar==MAP_FAILED){ perror("mmap"); return 1; }
    uint32_t s=strtoul(argv[2],0,0), c=strtoul(argv[3],0,0);
    for(uint32_t w=s; w<s+c; w++){ uint32_t v=bar[w]; if(v!=0 && v!=0xffffffff) printf("%05x %08x\n",w,v); }
    return 0;
}
