/* fm6000_fullreplay.c - replay EOS's COMPLETE cold-boot write stream (minus microcode ranges and
 * MGMT1/2 clock/reset regs) from a file of "aaaaaaaa vvvvvvvv" lines, in boot order.
 *
 * Source: notes/reference/scd-dumps/fm6000-COMPLETE-cold-boot-trace.txt.gz (394,647 writes). The
 * previously-used trace was truncated to 38% and contained ZERO EPL writes.
 *
 * JSS/SBus (0xF001/0xF002) must NOT be replayed as blind MMIO: 0xF001 with the Execute bit starts a
 * transaction whose Busy bit must clear before the next one, or transactions are lost. We detect the
 * F002-then-F001 pattern and run a proper transaction with a Busy poll.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#define PIN 0x1C021u
static volatile uint32_t *M;
static uint32_t rd(uint32_t w){uint32_t v=M[w];__sync_synchronize();return v;}
static void     wr(uint32_t w,uint32_t v){M[w]=v;__sync_synchronize();}
static long sbus_to=0, sbus_n=0;
static void sbus(uint32_t cmd,uint32_t data){
  wr(0xF002,data); wr(0xF001,0); wr(0xF001,cmd);
  for(long i=0;i<200000;i++){ uint32_t s=rd(0xF001);
    if(s==0xffffffffu) return;
    if(!(s&(1u<<25))) { sbus_n++; return; } }
  sbus_to++;
}
int main(int argc,char**argv){
  if(argc<2){fprintf(stderr,"usage: %s <file> [bdf] [pace_us_per_4k]\n",argv[0]);return 2;}
  const char *bdf = argc>2?argv[2]:"0000:02:00.0";
  unsigned pace = argc>3?(unsigned)strtoul(argv[3],0,0):0;
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",bdf);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  FILE *f=fopen(argv[1],"r"); if(!f){perror("open trace");return 1;}
  setvbuf(stdout,NULL,_IONBF,0);
  if(rd(PIN)!=0x208u){printf("chip not alive (PIN=0x%08x)\n",rd(PIN));return 1;}
  printf("fullreplay start PIN=0x%08x\n",rd(PIN));
  char line[64]; unsigned long n=0,mmio=0; uint32_t pend=0; int aborted=0;
  while(fgets(line,sizeof line,f)){
    uint32_t a,v;
    if(sscanf(line,"%x %x",&a,&v)!=2) continue;
    n++;
    if(a==0xF002u){ pend=v; continue; }
    if(a==0xF001u){ if(v==0) continue; sbus(v,pend); continue; }
    wr(a,v); mmio++;
    if((n & 0x3fff)==0){
      if(pace) usleep(pace);
      if(rd(PIN)!=0x208u){ printf("\n OFF-BUS at line %lu (0x%05x <- 0x%08x)\n",n,a,v); aborted=1; break; }
      printf("  %lu ops (mmio=%lu sbus=%ld to=%ld) PIN=ok\n",n,mmio,sbus_n,sbus_to);
    }
  }
  fclose(f);
  printf("%s: %lu ops, mmio=%lu sbus=%ld timeouts=%ld, PIN=0x%08x\n",
         aborted?"ABORTED":"DONE",n,mmio,sbus_n,sbus_to,rd(PIN));
  printf("PORT_STATUS=0x%08x pcsRx=0x%08x sched=0x%08x MCAST=0x%08x\n",
         rd(0xe3800),rd(0xe3826),rd(0x8062),rd(0x240000));
  return aborted?2:0;}
