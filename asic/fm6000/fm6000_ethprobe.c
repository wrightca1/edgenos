/* fm6000_ethprobe.c — probe FM6000 SerDes ETH registers to debug the "config writes don't commit" wall.
 * Writes distinctive patterns and reads back to determine: does ANY ETH write to ANY reg commit? Is the
 * WRITE file (0xB0500-path) a different physical reg than the READ file (0xC0500-path)?
 * usage: fm6000_ethprobe <BDF> <serdes>
 * Build: gcc -O2 -o fm6000_ethprobe fm6000_ethprobe.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uint32_t *M;
static inline void cw(uint32_t w,uint32_t v){ M[w]=v; __sync_synchronize(); }
static inline uint32_t cr(uint32_t w){ uint32_t v=M[w]; __sync_synchronize(); return v; }
/* op 0x21 write / 0x22 read; reg = {Address[15:8], Register[7:0]} */
static int xact(uint32_t op,uint32_t reg,uint32_t *val){
  uint32_t cmd,st=0; int i;
  cw(0xF002, op==0x21?*val:0); cw(0xF001,0);
  cmd=(reg&0xFFFF)|(op<<16)|(1u<<24); cw(0xF001,cmd);
  for(i=0;i<100000;i++){ st=cr(0xF001); if(!(st&(1u<<25)))break; }
  if(st&(1u<<25)) return -2;
  int rc=(st>>26)&7;
  if(op==0x22)*val=cr(0xF003);
  return rc;
}
int main(int argc,char**argv){
  if(argc<3){ fprintf(stderr,"usage: %s <BDF> <serdes>\n",argv[0]); return 2; }
  unsigned sd=atoi(argv[2]); unsigned recv=(sd+5)&0xff;
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  fprintf(stderr,"[probe] serdes %u -> SBus receiver 0x%02x. PIN=0x%08x\n",sd,recv,cr(0x1c021));

  /* A. does ANY write to reg61 commit? try 0x00, 0xFF, 0x55 — read back each */
  fprintf(stderr,"[probe] A: write-then-read reg61 with distinctive values --\n");
  uint32_t v;
  for(unsigned test=0; test<3; test++){
    uint32_t w=(test==0)?0x00:(test==1)?0xFF:0x55;
    v=w; int wrc=xact(0x21,(recv<<8)|61,&v);
    uint32_t rb=0; int rrc=xact(0x22,(recv<<8)|61,&rb);
    fprintf(stderr,"    wrote 0x%02x (rc=%d) -> read 0x%02x (rc=%d)  %s\n",
      w,wrc,rb&0xff,rrc, (rb&0xff)==w?"COMMITTED":"unchanged");
  }
  /* B. sweep a few regs: does ANY change? read baseline, write ^0xFF, read */
  fprintf(stderr,"[probe] B: per-reg write-sensitivity (baseline / after write ^0xFF) --\n");
  unsigned regs[]={0,3,6,7,11,13,23,34,38,41,42,54,59,61,62,65};
  for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++){
    unsigned r=regs[i]; uint32_t base=0; xact(0x22,(recv<<8)|r,&base); base&=0xff;
    uint32_t w=base^0xFF; v=w; xact(0x21,(recv<<8)|r,&v);
    uint32_t rb=0; xact(0x22,(recv<<8)|r,&rb); rb&=0xff;
    fprintf(stderr,"    reg%-2u base=0x%02x wrote=0x%02x read=0x%02x %s\n",
      r,base,w,rb, rb==w?"CHANGES":(rb==base?"stuck":"partial"));
    /* restore */ v=base; xact(0x21,(recv<<8)|r,&v);
  }
  /* C. read the SAME reg twice — stable or varying? (detect stale/garbage reads) */
  fprintf(stderr,"[probe] C: read stability (reg61 x3, reg0 x3) --\n");
  for(int k=0;k<3;k++){ uint32_t a=0,b=0; xact(0x22,(recv<<8)|61,&a); xact(0x22,(recv<<8)|0,&b);
    fprintf(stderr,"    reg61=0x%02x reg0=0x%02x\n",a&0xff,b&0xff); }
  munmap((void*)M,32u*1024*1024); close(fd);
  return 0;
}
