/* fm6000_serdes_cfg.c — write the FM6000 SerDes 24-register coefficient preset via the SPICO interface.
 * From the EOS boot trace (deterministic, per serdes): 24 config params written via the SPICO reg-write
 * cadence (fd04=param-idx, fd05=serdes, fd07=data, fd06=0x0c then 0x08). Then the SerDes hardware adapts
 * the DFE autonomously — NO host eye-polling loop. PREREQ: InitSBus + SPICO loaded/running.
 * usage: fm6000_serdes_cfg <BDF> <serdes>   Build: gcc -O2 -o fm6000_serdes_cfg fm6000_serdes_cfg.c */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uint32_t *M;
static inline void cw(uint32_t w,uint32_t v){ M[w]=v; __sync_synchronize(); }
static inline uint32_t cr(uint32_t w){ uint32_t v=M[w]; __sync_synchronize(); return v; }
/* SBus write to receiver 0xFD (SPICO), register reg, value val */
static int sbw(uint32_t reg,uint32_t val){
  uint32_t st=0; int i;
  cw(0xF002,val); cw(0xF001,0);
  cw(0xF001,(reg&0xFFFF)|(0x21<<16)|(1u<<24));           /* op 0x21 write, exec */
  for(i=0;i<100000;i++){ st=cr(0xF001); if(!(st&(1u<<25))) break; }
  return (st&(1u<<25))?-1:0;
}
/* serdes-68 coefficient preset (param idx 0x00..0x17) captured from EOS boot trace */
static const uint8_t CFG68[24]={
  0x0f,0x47,0x08,0x73,0x43,0x02,0x82,0x41,0x79,0x45,0x07,0xa6,
  0xb7,0x4c,0x47,0x23,0x44,0x05,0x07,0x05,0xc0,0x45,0xc7,0x59 };

int main(int argc,char**argv){
  if(argc<3){ fprintf(stderr,"usage: %s <BDF> <serdes>\n",argv[0]); return 2; }
  unsigned sd=atoi(argv[2]);
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  fprintf(stderr,"[sdcfg] serdes %u: writing 24-reg coefficient preset via SPICO. PIN=0x%08x\n",sd,cr(0x1c021));
  const uint8_t *cfg = CFG68;   /* serdes-68 preset (Et1); other serdes would need their own vector */
  for(unsigned idx=0; idx<24; idx++){
    sbw(0xFD04, idx);            /* param index */
    sbw(0xFD05, sd & 0xff);      /* serdes target */
    sbw(0xFD07, cfg[idx]);       /* data */
    sbw(0xFD06, 0x0c);           /* exec hi */
    sbw(0xFD06, 0x08);           /* exec lo */
  }
  uint32_t pin = cr(0x1c021);
  fprintf(stderr,"[sdcfg] 24 config regs written. PIN=0x%08x %s\n",
    pin, pin==0xffffffffu?"*** OFF-BUS ***":"(alive)");
  munmap((void*)M,32u*1024*1024); close(fd);
  return pin==0xffffffffu?1:0;
}
