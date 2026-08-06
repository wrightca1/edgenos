/* fm6000_seq_replay.c - replay a captured "Write: 0xADDR <- 0xVAL" register sequence verbatim.
 * For the SerDes bring-up: EPL14 lane handshake + ETH SBus (0xF001 poll Busy) + SPICO interrupts.
 * usage: fm6000_seq_replay <BDF> <seqfile>   Build: gcc -O2 -o fm6000_seq_replay fm6000_seq_replay.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uint32_t *M,*SCD;
static inline void wr(uint32_t w,uint32_t v){ M[w]=v; __sync_synchronize(); }
static inline uint32_t rd(uint32_t w){ uint32_t v=M[w]; __sync_synchronize(); return v; }
int main(int argc,char**argv){
  if(argc<3){ fprintf(stderr,"usage: %s <BDF> <seqfile>\n",argv[0]); return 2; }
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  int sf=open("/sys/bus/pci/devices/0000:04:00.0/resource0",O_RDWR|O_SYNC);
  if(sf>=0){ SCD=mmap(NULL,0x10000,PROT_READ|PROT_WRITE,MAP_SHARED,sf,0); if(SCD==MAP_FAILED)SCD=NULL; }
  #define PET() do{ if(SCD){SCD[0x0120>>2]=0xC0000BB8;__sync_synchronize();} }while(0)
  FILE*f=fopen(argv[2],"r"); if(!f){perror("seq");return 1;}
  char line[256]; long n=0; uint32_t addr,val; PET();
  fprintf(stderr,"[seq] start PIN=0x%08x\n",rd(0x1c021));
  while(fgets(line,sizeof line,f)){
    if(sscanf(line," Write: 0x%x <- 0x%x",&addr,&val)!=2) continue;
    wr(addr,val); n++;
    if(addr==0xF001u && (val&0x1000000u)){          /* SBus command: poll Busy(bit25) clear */
      for(int i=0;i<100000;i++){ if(!(rd(0xF001u)&(1u<<25))) break; }
    }
    if((n&0xf)==0){ PET(); if(rd(0x1c021)==0xffffffffu){ fprintf(stderr,"[seq] *** OFF-BUS after %ld (0x%x<-0x%x) ***\n",n,addr,val); return 1; } }
  }
  fclose(f); PET();
  fprintf(stderr,"[seq] done: %ld writes. PIN=0x%08x\n",n,rd(0x1c021));
  /* Et1 = epl14 ch0 PORT_STATUS at 0xE3800 */
  uint32_t ps=rd(0xE3800u);
  fprintf(stderr,"[seq] === Et1 PORT_STATUS=0x%08x RxLinkUp(6)=%d HeartbeatOk(7)=%d ===\n",ps,(ps>>6)&1,(ps>>7)&1);
  munmap((void*)M,32u*1024*1024); close(fd);
  return ((ps>>6)&1)?0:2;
}
