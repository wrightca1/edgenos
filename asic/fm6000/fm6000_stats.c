/* fm6000_stats.c - dump the STATS_DISCRETE counter bank (0x1A000) as 64-bit values, for before/after
 * diffing across a packet injection. 128 FRAME counters + 128 BYTE counters. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
int main(int argc,char**argv){
 char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argc>1?argv[1]:"0000:02:00.0");
 int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
 volatile uint32_t*M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 if(M==MAP_FAILED){perror("mmap");return 1;}
 for(int i=0;i<128;i++){
   uint64_t f=((uint64_t)M[0x1A000+2*i+1]<<32)|M[0x1A000+2*i];
   printf("F%03d %llu\n",i,(unsigned long long)f);
 }
 for(int i=0;i<128;i++){
   uint64_t b=((uint64_t)M[0x1A080+2*i+1]<<32)|M[0x1A080+2*i];
   printf("B%03d %llu\n",i,(unsigned long long)b);
 }
 return 0;}
