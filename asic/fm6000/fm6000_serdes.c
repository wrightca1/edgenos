/* fm6000_serdes.c — FM6000 per-port SerDes lane config + EPL enable, toward a trained 10G link.
 *
 * Per-lane SerDes registers (drive/pre/post/polarity, PLL/DFE status) are accessed via the JSS SBus
 * (0xF001/2/3) to receiver = serdes+5, register = ETH reg index — CONFIRMED from fm6000WriteSBus disasm
 * (addr 0xB0500+0x100*serdes+index → SBus Address[15:8]=(addr>>8)&0xff=serdes+5, Register[7:0]=index).
 * EPL config is direct MMIO to 0xE0000+0x400*epl (golden values captured from a warm/up Et1).
 * Encodings from AltaLib.py + fm6000SetTxConfig disasm.
 *
 * PREREQ: fm6000_initsbus (SBus master up) + fm6000_spico (SPICO loaded/alive) first.
 *
 * usage: fm6000_serdes <BDF> <port#>   (port# = intf id, 1-based, from fm6000_serdes_ports.h)
 * Build: gcc -O2 -o fm6000_serdes fm6000_serdes.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include "fm6000_serdes_ports.h"

static volatile uint32_t *M;
static inline void cw(uint32_t w,uint32_t v){ M[w]=v; __sync_synchronize(); }
static inline uint32_t cr(uint32_t w){ uint32_t v=M[w]; __sync_synchronize(); return v; }
#define SBUS_COMMAND 0xF001u
#define SBUS_REQUEST 0xF002u
#define SBUS_RESPONSE 0xF003u
#define OP_WRITE 0x21u
#define OP_READ  0x22u
static int sbus_xact(uint32_t op,uint32_t reg,uint32_t *val){
  uint32_t cmd,st=0; int i;
  cw(SBUS_REQUEST, op==OP_WRITE?*val:0); cw(SBUS_COMMAND,0);
  cmd=(reg&0xFFFFu)|(op<<16)|(1u<<24); cw(SBUS_COMMAND,cmd);
  for(i=0;i<100000;i++){ st=cr(SBUS_COMMAND); if(!(st&(1u<<25)))break; }
  if(st&(1u<<25)){ fprintf(stderr,"[sd] SBus busy timeout reg=0x%04x\n",reg); return -1; }
  if(((st>>26)&7u)!=(op==OP_WRITE?1u:4u)){ fprintf(stderr,"[sd] SBus rc=%u reg=0x%04x\n",(st>>26)&7u,reg); return -1; }
  if(op==OP_READ)*val=cr(SBUS_RESPONSE);
  return 0;
}
/* ETH reg N of serdes sd: SBus receiver = sd+5, register = N */
static int ew(unsigned sd,unsigned n,uint32_t v){ return sbus_xact(OP_WRITE, (((sd+5)&0xff)<<8)|(n&0xff), &v); }
static int er(unsigned sd,unsigned n,uint32_t *v){ return sbus_xact(OP_READ, (((sd+5)&0xff)<<8)|(n&0xff), v); }
static uint32_t erv(unsigned sd,unsigned n){ uint32_t v=0; er(sd,n,&v); return v; }

/* serdes = eplBase[epl] + lane (fm6000SerDesRemapTable) */
static unsigned epl_base(unsigned epl){
  static const int b[25]={[5]=8,[7]=12,[9]=16,[11]=20,[13]=24,[14]=68,[15]=28,[16]=64,
    [17]=40,[18]=52,[19]=32,[20]=60,[21]=36,[22]=56,[23]=44,[24]=48};
  return (epl<25)?b[epl]:0;
}

int main(int argc,char**argv){
  if(argc<3){ fprintf(stderr,"usage: %s <BDF> <port#>\n",argv[0]); return 2; }
  int port=atoi(argv[2]);
  const struct fm6000_serdes_port *pp=NULL;
  for(int i=0;i<FM6000_SERDES_NPORTS;i++) if(FM6000_SERDES_PORTS[i].intf==(unsigned)port){ pp=&FM6000_SERDES_PORTS[i]; break; }
  if(!pp){ fprintf(stderr,"port %d not in table\n",port); return 2; }
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  unsigned epl=pp->epl, lane=pp->lane, sd=epl_base(epl)+lane, ch=lane;  /* identity ch for epl14/16 */
  uint32_t eb=0xE0000u+0x400u*epl;
  fprintf(stderr,"[sd] port %d: epl%u lane%u serdes%u (recv 0x%02x) drive%u pre%u post%u txpol%d rxpol%d\n",
    port,epl,lane,sd,sd+5,pp->drive,pp->pre,pp->post,pp->txpol,pp->rxpol);
  fprintf(stderr,"[sd] PIN=0x%08x SOFT_RESET(0x9)=0x%08x\n",cr(0x1c021),cr(0x00009));

  /* --- STEP 0: PRECONDITIONS (the unified fix). Both the ETH-write-not-landing and the EPL-off-bus
   * have the SAME root cause: SOFT_RESET bit4 (EPLReset) held the EPL/SerDes block in reset (0x16).
   * (a) EPL PLL clock: PLL_CTRL golden (EPL_ClkSelect + Enable) so EPL regs respond;
   * (b) clear EPLReset so SerDes writes latch + EPL writes don't off-bus. --- */
  cw(0x1c042, 0x20841438u); cw(0x1c043, 0x00005560u);   /* PLL_CTRL golden (EPL_ClkSelect=1, Enable1) */
  { int i; for(i=0;i<100;i++){ if(cr(0x1c046)&0x1) break; usleep(1000);} }  /* wait PLL_STAT Locked1 */
  fprintf(stderr,"[sd] PLL_STAT(0x1c046)=0x%08x (locked bit0=%d)\n",cr(0x1c046),cr(0x1c046)&1);
  { uint32_t sr=cr(0x00009); cw(0x00009, sr & ~0x10u); }  /* clear EPLReset (bit4) */
  uint32_t probe=cr(0xE0000u+0x400u*epl+0x00);            /* PORT_STATUS = go/no-go off-bus probe */
  fprintf(stderr,"[sd] after EPL-out-of-reset: SOFT_RESET=0x%08x  PORT_STATUS(epl%u)=0x%08x %s\n",
    cr(0x00009),epl,probe, probe==0xffffffffu?"*** OFF-BUS ***":"(EPL accessible)");
  if(probe==0xffffffffu){ fprintf(stderr,"[sd] EPL still off-bus after reset-clear — aborting\n"); return 1; }

  /* --- STEP 1: SerDes ENABLE sequence (fm6000EnableSerDes @0x48131e, 10G ETH). The EQ config only
   * latches AFTER the lane is enabled (rate-select + power). Order is load-bearing. --- */
  int loopback = getenv("FM6000_LOOPBACK")!=NULL;
  uint32_t v;
  /* polarity first (reg7 rxpol[4], reg11 txpol[2]) — applied before enable, as the SDK does */
  { uint32_t r11=erv(sd,11); ew(sd,11,(r11 & ~0x4u)|(pp->txpol?0x4u:0)); }
  { uint32_t r7=erv(sd,7);   ew(sd,7, (r7 & ~0x10u)|(pp->rxpol?0x10u:0)); }
  /* 1. hold RX datapath in reset */
  v=erv(sd,34); ew(sd,34, v & ~0x3u);
  /* 2. rate-select + lane enable: reg0 low7=0x37 (rate 27<<1 | en); reg54/59 = 0x40 (10G divider) */
  v=erv(sd,0);  ew(sd,0, (v & ~0x7Fu) | (0x1bu<<1) | 0x1u);
  ew(sd,29,0x0u);
  v=erv(sd,54); ew(sd,54,(v & ~0x7Fu) | 0x40u);
  v=erv(sd,59); ew(sd,59,(v & ~0x7Fu) | 0x40u);
  v=erv(sd,23); ew(sd,23,(v & ~0x1Fu) | 0x10u);           /* DFE DAC range medium */
  usleep(6400);
  /* 3. release RX datapath / power up */
  v=erv(sd,34); ew(sd,34, v | 0x3u);
  /* 4. wait PLL lock (READ reg15 bit3) */
  { int i; for(i=0;i<1000 && !((erv(sd,15)>>3)&1);i++) usleep(100); }
  fprintf(stderr,"[sd] after enable: PLL(reg15 bit3)=%d\n",(erv(sd,15)>>3)&1);
  /* 5. RX/TX enables (reg6[3], reg3[0]) */
  v=erv(sd,6); ew(sd,6, v | 0x8u);
  v=erv(sd,3); ew(sd,3, v | 0x1u);
  /* 6. signal-detect threshold (reg31[6:1]=8) + commit (reg38[0]=1) */
  v=erv(sd,31); ew(sd,31,(v & ~0x7Eu) | ((8u&0x3f)<<1));
  v=erv(sd,38); ew(sd,38, v | 0x1u);

  /* --- GATE HYPOTHESIS: SerDes config writes only latch once the EPL LANE is ACTIVE.
   * fm6000WriteSBus checks EPL per-lane status 0xE003E+(lane+8*epl)<<7 bit 0x40/0x20 before allowing a write.
   * So enable the EPL lane FIRST (Active_0 + config), THEN write the TX-EQ. --- */
  uint32_t gate = 0xE003Eu + ((lane + 8u*epl) << 7);      /* = eb+0x3E for epl14/lane0 = 0xE383E */
  fprintf(stderr,"[sd] gate reg 0x%05x BEFORE EPL-enable = 0x%08x (lanebit 0x40=%d 0x20=%d)\n",
    gate, cr(gate), (cr(gate)>>6)&1, (cr(gate)>>5)&1);
  /* EPL enable (moved BEFORE EQ): MAC_CFG, SERDES_CFG, LINK_RULES, CFG_B, then Active_<ch> */
  cw(eb+0x10, 0x2000033cu); cw(eb+0x11, 0x400003e0u); cw(eb+0x12, 0x00002414u); cw(eb+0x13, 0x00001841u);
  cw(eb+0x34, 0x0aaa86c0u); cw(eb+0x35, 0x00000001u);
  cw(eb+0x0c, 0x00001841u);
  cw(eb+0x302, 0x00090003u);
  { uint32_t a=cr(eb+0x301); a|=(1u<<(19+ch)); cw(eb+0x301,a); }
  fprintf(stderr,"[sd] EPL%u enabled: CFG_A=0x%08x PORT_STATUS=0x%08x\n",epl,cr(eb+0x301),cr(eb+0x00));
  fprintf(stderr,"[sd] gate reg 0x%05x AFTER  EPL-enable = 0x%08x (lanebit 0x40=%d 0x20=%d)\n",
    gate, cr(gate), (cr(gate)>>6)&1, (cr(gate)>>5)&1);

  /* 7. NOW (lane active) the TX-EQ should latch */
  { uint32_t r61=erv(sd,61),r62=erv(sd,62),r65=erv(sd,65);
    r61=(r61 & ~0x3Cu)|((pp->drive&0xF)<<2);
    r65=(r65 & ~0x0Cu)|(((pp->pre>>2)&1)<<2);
    r62=(r62 & ~0xFFu)|((pp->post&0xF)<<4)|((pp->pre&0x3)<<2)|0x3u;
    ew(sd,61,r61); ew(sd,65,r65); ew(sd,62,r62); }
  uint32_t v61=erv(sd,61), v62=erv(sd,62);
  int cfg_ok=((v61 & 0x3C)==((pp->drive&0xF)<<2)) && ((v62>>4 & 0xF)==(pp->post&0xF));
  fprintf(stderr,"[sd] TX-EQ readback r61=0x%08x r62=0x%08x -> %s (drive=%u post=%u vs wrote %u/%u)\n",
    v61,v62, cfg_ok?"LATCHED!":"still not latching", (v61>>2)&0xF,(v62>>4)&0xF, pp->drive, pp->post);
  /* 8. datapath enable / near-loopback */
  if(loopback){ v=erv(sd,13); ew(sd,13,(v & 0x7Fu)|0x80u|0x1u); fprintf(stderr,"[sd] near-loopback (reg13 bit7)\n"); }
  else        { v=erv(sd,13); ew(sd,13, v | 0x11u); }
  /* 9. signal detect + one-shot DFE */
  { int i; for(i=0;i<1000 && !((erv(sd,20)>>6)&1);i++) usleep(100); }
  v=erv(sd,42); ew(sd,42,(v & ~0x6u)|0x2u);
  fprintf(stderr,"[sd] ETH_READ: reg15=0x%02x(PLL bit3=%d) reg20=0x%02x(sig bit6=%d) reg31=0x%02x\n",
    erv(sd,15)&0xff,(erv(sd,15)>>3)&1, erv(sd,20)&0xff,(erv(sd,20)>>6)&1, erv(sd,31)&0xff);

  /* --- STEP 4: link status --- */
  uint32_t ps=cr(eb+0x00);
  fprintf(stderr,"[sd] === PORT_STATUS=0x%08x RxLinkUp(6)=%d HeartbeatOk(7)=%d ===\n",ps,(ps>>6)&1,(ps>>7)&1);
  munmap((void*)M,32u*1024*1024); close(fd);
  return cfg_ok?0:2;
}
