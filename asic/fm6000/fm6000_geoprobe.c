/* fm6000_geoprobe.c - generic table-geometry probe: write a unique sentinel into every word of a
 * region, read it all back, and report which words are exact / truncated / aliased / read-only.
 * Use this BEFORE concluding a readback is anomalous -- it has already explained two "corruptions"
 * (SAF word2 is an 18-bit field; MOD_CAM/L2F need an MSW commit).
 *   fm6000_geoprobe <bdf> <base_hex> <nwords>
 * DESTRUCTIVE: overwrites the region. Re-run the config replay afterwards. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
int main(int argc,char**argv){
 if(argc<4){fprintf(stderr,"usage: %s <bdf> <base> <nwords>\n",argv[0]);return 2;}
 uint32_t base=strtoul(argv[2],0,16); unsigned N=strtoul(argv[3],0,0);
 char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
 int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
 volatile uint32_t*M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 if(M==MAP_FAILED){perror("mmap");return 1;}
 printf("PIN=0x%08x probing 0x%05x x%u\n",M[0x1c021],base,N);
 for(unsigned i=0;i<N;i++){ M[base+i]=0x5A000000u|i; __sync_synchronize(); }
 usleep(50000);
 unsigned exact=0,trunc=0,alias=0,ro=0;
 for(unsigned i=0;i<N;i++){
   uint32_t v=M[base+i];
   const char*tag;
   if(v==(0x5A000000u|i)) {tag="exact"; exact++;}
   else if(v==i)          {tag="TRUNCATED(low bits only)"; trunc++;}
   else if((v&0xFF000000u)==0x5A000000u){tag="ALIASED"; alias++;}
   else if(v==0)          {tag="reads-0 (RO/unused)"; ro++;}
   else                   {tag="other"; }
   if(i<24) printf("  +%-3u (%s) = %08x  %s\n",i,(i%4==0?"w0":i%4==1?"w1":i%4==2?"w2":"w3"),v,tag);
 }
 printf("SUMMARY %u words: exact=%u truncated=%u aliased=%u reads0=%u  PIN=0x%08x\n",
        N,exact,trunc,alias,ro,M[0x1c021]);
 return 0;}
