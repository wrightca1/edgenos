/* fm6000_drops.c - per-stage drop instrumentation.
 *   CM_PORT_TX_DROP_COUNT(i) = CM_BASE(0x110000) + 0x2100 + 2*i   (64-bit, 80 entries, per internal port)
 *   CM_GLOBAL_USAGE          = 0x110200 (2w)   CM_RXMP_USAGE(i) = 0x110210+i
 *   CM_PORT_RXMP_USAGE(a,b)  = 0x111000 + 0x20*a + 2*b
 *   FIBM_DROP_CTR            = 0x05009
 * Dumps everything non-zero so a before/after diff shows WHICH stage ate the frame. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
int main(int argc,char**argv){
 char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argc>1?argv[1]:"0000:02:00.0");
 int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
 volatile uint32_t*M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 if(M==MAP_FAILED){perror("mmap");return 1;}
 for(int i=0;i<80;i++){
   uint64_t v=((uint64_t)M[0x112100+2*i+1]<<32)|M[0x112100+2*i];
   printf("CM_TXDROP[%02d] %llu\n", i, (unsigned long long)v);
 }
 printf("FIBM_DROP_CTR %u\n", M[0x5009]);
 printf("CM_GLOBAL_USAGE %u %u\n", M[0x110200], M[0x110201]);
 for(int i=0;i<16;i++) printf("CM_RXMP_USAGE[%02d] %u\n", i, M[0x110210+i]);
 return 0;}
