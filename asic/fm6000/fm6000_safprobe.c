/* fm6000_safprobe.c - establish the REAL MMIO geometry of SAF_MATRIX (0xA0000).
 * The macro says WIDTH=4, stride 4*port, 76 entries -- but after writing per-port values,
 * SAF[0] and SAF[1] read back IDENTICAL while SAF[40] was distinct. Before drawing any
 * conclusion about SAF's role we must know how writes map to reads.
 * Method: write a unique sentinel (index) into every word of the range, then read the whole
 * range back and report the mapping. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#define BASE 0xA0000u
#define N    256u
int main(int argc,char**argv){
 char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argc>1?argv[1]:"0000:02:00.0");
 int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
 volatile uint32_t*M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 if(M==MAP_FAILED){perror("mmap");return 1;}
 printf("PIN=0x%08x\n", M[0x1c021]);
 static uint32_t before[N], after[N];
 for(unsigned i=0;i<N;i++) before[i]=M[BASE+i];
 /* sentinel: 0x5A000000 | word-index */
 for(unsigned i=0;i<N;i++){ M[BASE+i]=0x5A000000u|i; __sync_synchronize(); }
 usleep(50000);
 for(unsigned i=0;i<N;i++) after[i]=M[BASE+i];
 printf("word : before    -> after     (sentinel 0x5A0000xx = word index that landed)\n");
 unsigned exact=0, aliased=0, dead=0;
 for(unsigned i=0;i<64;i++){
   uint32_t a=after[i];
   const char *tag="?";
   if(a==(0x5A000000u|i)) {tag="exact"; exact++;}
   else if((a&0xFFFF0000u)==0x5A000000u){tag="ALIASED-from"; aliased++;}
   else {tag="not-written"; dead++;}
   if(i<24 || (a&0xFFFF0000u)!=0x5A000000u || a!=(0x5A000000u|i))
     printf("  %3u (port %2u w%u): %08x -> %08x  %s%s\n", i, i/4, i%4, before[i], a, tag,
            (((a&0xFFFF0000u)==0x5A000000u)&&a!=(0x5A000000u|i)) ? " word" : "");
 }
 for(unsigned i=64;i<N;i++){
   uint32_t a=after[i];
   if(a==(0x5A000000u|i)) exact++; else if((a&0xFFFF0000u)==0x5A000000u) aliased++; else dead++;
 }
 printf("SUMMARY over %u words: exact=%u aliased=%u not-written=%u\n",N,exact,aliased,dead);
 printf("PIN=0x%08x\n", M[0x1c021]);
 return 0;}
