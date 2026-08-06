/* fm6000_serdes_enable.c — run the decoded fm6000EnableSerDes ordered sequence on a serdes lane, cold,
 * via raw SBus to device (serdes+5). Brings up the TX clock domain so TxRdy asserts and TX enables.
 *
 * From static RE of fm6000EnableSerDes (libFocalpointSDK.so swi-4.16.8M @0x48131e). The rate code (reg0x00
 * bits[6:1]) and the field-merge values for reg0x36/0x3b/0x1f are lane/speed-specific; pass them as the
 * GOLDEN values read from a warm EOS where Et1 is up (read serdes 68 regs 00,36,3b,1f). Bit-op-only regs
 * (0x1d,0x17,0x22,0x26,0x06,0x03,0x0d) are hardcoded per the decode.
 *
 * usage: fm6000_serdes_enable <BDF> <serdes> <reg00> <reg36> <reg3b> <reg1f>   (values hex, 8-bit)
 *        (reg00 = full golden reg0x00 value incl rate+bit0; others = full golden 8-bit values)
 * Build: gcc -O2 -o fm6000_serdes_enable fm6000_serdes_enable.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uint32_t *M;
static void cw(uint32_t w,uint32_t v){ M[w]=v; __sync_synchronize(); }
static uint32_t cr(uint32_t w){ uint32_t v=M[w]; __sync_synchronize(); return v; }
static unsigned A;                       /* SBus device field (serdes+5)<<8 */
static int xact(uint32_t op,uint32_t reg,uint32_t *val){
  uint32_t st=0; int i; cw(0xF002,op==0x21?*val:0); cw(0xF001,0);
  cw(0xF001,(reg&0xFFFF)|(op<<16)|(1u<<24));
  for(i=0;i<200000;i++){ st=cr(0xF001); if(!(st&(1u<<25)))break; }
  if(op==0x22)*val=cr(0xF003); return (st&(1u<<25))?-1:(int)((st>>26)&7);
}
static uint32_t rd(unsigned r){ uint32_t v=0; xact(0x22,A|r,&v); return v&0xff; }
/* write absolute value, verify */
static void wabs(unsigned r,uint32_t nv){
  uint32_t b=rd(r), w=nv; int rc=xact(0x21,A|r,&w); uint32_t a=rd(r);
  fprintf(stderr,"  reg0x%02x: 0x%02x -> 0x%02x (rc=%d) got 0x%02x %s\n",
    r,b,nv&0xff,rc,a,(a==(nv&0xff))?"OK":"NOT-COMMIT");
}
/* read-modify-write: clear `clr` bits, set `set` bits */
static void wrmw(unsigned r,uint32_t clr,uint32_t set){ uint32_t b=rd(r); wabs(r,(b&~clr)|set); }

int main(int c,char**v){
  if(c<3){ fprintf(stderr,"usage: %s <BDF> <serdes> [reg00 reg36 reg3b reg1f (golden hex)]\n",v[0]); return 2; }
  unsigned sd=atoi(v[2]);
  uint32_t g00=(c>3)?strtoul(v[3],0,16):0, g36=(c>4)?strtoul(v[4],0,16):0,
           g3b=(c>5)?strtoul(v[5],0,16):0, g1f=(c>6)?strtoul(v[6],0,16):0;
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",v[1]);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(0,32u<<20,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); if(M==MAP_FAILED){perror("mmap");return 1;}
  A=((sd+5)&0xff)<<8;
  /* 0x3e window (TxRdy/RxRdy) for this serdes: ((epl*8+lane)<<7)+0xe003e ; Et1/serdes68 = 0xe383e/f */
  unsigned win = 0xe383e;  /* serdes 68 (epl14,lane0). NOTE: only valid for serdes 68. */
  fprintf(stderr,"[en] serdes %u dev 0x%02x. before: reg00=0x%02x reg22=0x%02x reg0d=0x%02x  0xe383f=0x%08x (TxRdy=%d RxRdy=%d)\n",
    sd,(sd+5)&0xff,rd(0),rd(0x22),rd(0x0d), cr(win+1),(cr(win+1)>>5)&1,(cr(win+1)>>6)&1);

  fprintf(stderr,"[en] === ordered enable sequence ===\n");
  wrmw(0x22,0x03,0x00);                       /* 1. reset: clear both */
  if(g00) wabs(0x00,g00); else wrmw(0x00,0,0x01);  /* 3. rate + master clk (golden reg00, or at least bit0) */
  wabs(0x1d,0x00);                            /* 4. reg0x1d := 0 */
  if(g36) wabs(0x36,g36);                     /* 5. reg0x36 field merge (golden) */
  if(g3b) wabs(0x3b,g3b);                     /* 6. reg0x3b field merge (golden) */
  wrmw(0x17,0x10,0x10^ (rd(0x17)&0x10));      /* 7. reg0x17 toggle bit4 within [4:0] (approx) */
  wrmw(0x22,0x00,0x03);                       /* 8. TX+RX ENABLE: set both bits */
  fprintf(stderr,"[en] 0xe383f after enable=0x%08x (TxRdy=%d)\n", cr(win+1),(cr(win+1)>>5)&1);
  wrmw(0x06,0x00,0x08);                       /* 10. RxEn */
  wrmw(0x03,0x00,0x01);                       /* 11. TxEn */
  if(g1f) wabs(0x1f,g1f);                     /* 12. reg0x1f field merge (golden) */
  wrmw(0x26,0x00,0x01);                       /* 13. reg0x26 |= 1 */
  /* 14. TX FIR coeffs = SetTxConfig — done separately (fm6000_serdes_cfg) */
  wrmw(0x0d,0x00,0x11);                       /* 15. reg0x0d |= 0x11 (TxRdy-gated) */

  fprintf(stderr,"[en] after: reg00=0x%02x reg22=0x%02x reg03=0x%02x reg06=0x%02x reg0d=0x%02x\n",
    rd(0),rd(0x22),rd(0x03),rd(0x06),rd(0x0d));
  uint32_t hi=cr(win+1);
  fprintf(stderr,"[en] 0xe383f=0x%08x  TxRdy(bit5)=%d RxRdy(bit6)=%d  PORT_STATUS(0xe3800)=0x%08x\n",
    hi,(hi>>5)&1,(hi>>6)&1, cr(0xe3800));
  munmap((void*)M,32u<<20); close(fd);
  return 0;
}
