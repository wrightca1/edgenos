/* fm6000_trace_replay.c — replay a captured fmPlatformTraceRegOps trace verbatim.
 *
 * Executes every "Write: 0xADDR <- 0xVAL" line as wr(word=ADDR, VAL), and polls on known status
 * registers for each "Read: 0xADDR == 0xVAL" line (PLL-lock 0x1c022, BM-march 0x1d08e, CRM 0x1f001,
 * BM-int 0x1d08c). This is the highest-fidelity cold bring-up: run EXACTLY what EOS's fm6000BootSwitch
 * does, in order — BM-march -> clock handshake(->0x313) -> scheduler tokens.
 *
 * PACED (BIST_PACE_US, default 40us) + watchdog-pet built in (the BM-march 0x1d2xx writes hang the host
 * if unpaced). PIN liveness checked periodically; aborts on off-bus.
 *
 * usage: fm6000_trace_replay <BDF> <trace-file>
 * Build: gcc -O2 -o fm6000_trace_replay fm6000_trace_replay.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

static volatile uint32_t *M;
static volatile uint32_t *SCD;
static inline void wr(uint32_t w,uint32_t v){ M[w]=v; __sync_synchronize(); }
static inline uint32_t rd(uint32_t w){ uint32_t v=M[w]; __sync_synchronize(); return v; }
#define PIN 0x1C021

static int is_poll(uint32_t a){ return a==0x1c022||a==0x1d08e||a==0x1f001||a==0x1d08c; }

int main(int argc,char**argv){
  if(argc<3){ fprintf(stderr,"usage: %s <BDF> <trace-file>\n",argv[0]); return 2; }
  int pace = getenv("BIST_PACE_US")?atoi(getenv("BIST_PACE_US")):40;
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap fm6000");return 1;}
  /* SCD BAR0 for watchdog pet (0x0120) */
  int sfd=open("/sys/bus/pci/devices/0000:04:00.0/resource0",O_RDWR|O_SYNC);
  if(sfd>=0){ SCD=mmap(NULL,0x10000,PROT_READ|PROT_WRITE,MAP_SHARED,sfd,0); if(SCD==MAP_FAILED) SCD=NULL; }
  #define PET() do{ if(SCD){ SCD[0x0120>>2]=0xC0000BB8; __sync_synchronize(); } }while(0)

  FILE*f=fopen(argv[2],"r"); if(!f){perror("open trace");return 1;}
  char line[512];
  long nw=0,nr=0,npoll_ok=0,npoll_to=0; uint32_t pin=rd(PIN);
  fprintf(stderr,"[trace] start PIN=0x%08x pace=%dus\n",pin,pace);
  PET();
  while(fgets(line,sizeof line,f)){
    uint32_t addr,val;
    if(sscanf(line," Write: 0x%x <- 0x%x",&addr,&val)==2){
      wr(addr,val); nw++;
      if(pace) usleep(pace);
      if((nw & 0x1f)==0){ PET(); pin=rd(PIN);
        if(pin==0xffffffffu){ fprintf(stderr,"[trace] *** OFF-BUS after %ld writes (last 0x%x<-0x%x) ***\n",nw,addr,val); return 1; } }
    } else if(sscanf(line," Read:  0x%x == 0x%x",&addr,&val)==2){
      nr++;
      if(is_poll(addr)){
        int ok=0; for(int i=0;i<2000;i++){ uint32_t r=rd(addr); if(r==val||(addr==0x1c022&&(r&0xfff)==0x313)){ok=1;break;} usleep(200); if((i&0x3f)==0)PET(); }
        if(ok) npoll_ok++; else { npoll_to++; if(npoll_to<=6) fprintf(stderr,"[trace] poll TIMEOUT 0x%x want 0x%x got 0x%x (line ~%ld)\n",addr,val,rd(addr),nw+nr); }
      } else rd(addr); /* mimic the access */
    }
  }
  fclose(f);
  PET();
  fprintf(stderr,"[trace] done: %ld writes, %ld reads (%ld polls ok, %ld timed out) PIN=0x%08x\n",nw,nr,npoll_ok,npoll_to,rd(PIN));
  /* scheduler running check */
  uint32_t rx=rd(0x8062), tx=rd(0x8022), esched;
  fprintf(stderr,"[trace] SSCHED: 0x8062=0x%08x (want 0x00200200)  0x8022=0x%08x (want 0xc0300200)\n",rx,tx);
  esched=rd(0x2000);
  fprintf(stderr,"[trace] ESCHED 0x2000=0x%08x -> %s\n",esched, esched==0xffffffffu?"OFF-BUS":"ON-BUS!");
  int running=(rx==0x00200200u)|| esched!=0xffffffffu;
  fprintf(stderr,"[trace] === SCHEDULER %s ===\n", running?"RUNNING / ESCHED ON-BUS":"not confirmed");
  return running?0:2;
}
