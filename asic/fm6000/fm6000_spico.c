/* fm6000_spico.c — load the FM6000 SerDes SPICO microcontroller firmware over the JSS SBus master.
 *
 * Implements the SBus transaction (0xF001/2/3, receiver 0xFD) + the SPICO IMEM upload + alive/CRC
 * interrupt handshake, decoded verbatim from libFocalpointSDK.so (fm6000LoadSpicoCode @0x4793a1,
 * fm6000InterruptSpico @0x47935a). This is the foundational unblock for SerDes/link bring-up:
 * without the SPICO running, the SerDes RX equalizer never adapts and ports don't train.
 *
 * PREREQ: the JSS SBus master must be initialized first (fm6000_initsbus) so 0xF001 transactions work.
 *
 * usage: fm6000_spico <BDF> <spico-code.bin>
 * Build: gcc -O2 -o fm6000_spico fm6000_spico.c
 * SPDX-License-Identifier: GPL-2.0-or-later
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

#define SBUS_COMMAND  0xF001u   /* Register[7:0]|Address[15:8]|Op[23:16]|Exec[24]|Busy[25]|Rc[28:26] */
#define SBUS_REQUEST  0xF002u   /* write data */
#define SBUS_RESPONSE 0xF003u   /* read data  */
#define SBUS_SPICO    0xF004u   /* Reset[0] Enable[1] */
#define OP_WRITE 0x21u
#define OP_READ  0x22u

/* one SBus transaction: receiver=(reg>>8), register=(reg&0xff). op WRITE uses *val in; READ puts result out. */
static int sbus_xact(uint32_t op, uint32_t reg, uint32_t *val){
  uint32_t cmd,st=0; int i;
  cw(SBUS_REQUEST, op==OP_WRITE ? *val : 0);
  cw(SBUS_COMMAND, 0);
  cmd = (reg & 0xFFFFu) | (op<<16) | (1u<<24);           /* Execute */
  cw(SBUS_COMMAND, cmd);
  for(i=0;i<100000;i++){ st=cr(SBUS_COMMAND); if(!(st&(1u<<25))) break; }   /* poll Busy(bit25) */
  if(st&(1u<<25)){ fprintf(stderr,"[spico] SBus busy timeout reg=0x%04x\n",reg); return -1; }
  if(((st>>26)&7u)!=(op==OP_WRITE?1u:4u)){ fprintf(stderr,"[spico] SBus rc=%u reg=0x%04x (want %u)\n",(st>>26)&7u,reg,op==OP_WRITE?1u:4u); return -1; }
  if(op==OP_READ) *val = cr(SBUS_RESPONSE);
  return 0;
}
static int sw(uint32_t reg,uint32_t val){ return sbus_xact(OP_WRITE,reg,&val); }
static int sr(uint32_t reg,uint32_t *val){ return sbus_xact(OP_READ,reg,val); }

/* SPICO interrupt: num/data; result (10-bit) -> *out; ack = 0xFD01 reads 1 */
static int spico_int(uint32_t num,uint32_t data,uint32_t *out){
  uint32_t v=0,hi,lo; int i;
  if(sw(0xFD02,num&0xFF)||sw(0xFD03,data&0xFF)||sw(0xFD01,(data>>8&3)|((num>>8&3)<<2))) return -1;
  if(sw(0xFD0C,0x18)||sw(0xFD0C,0x08)) return -1;        /* strobe interrupt */
  for(i=0;i<1000000;i++){ if(sr(0xFD01,&v)) return -1; if(v==1) break; }
  if(v!=1){ fprintf(stderr,"[spico] interrupt %u timeout (0xFD01=%u)\n",num,v); return -1; }
  if(out){ sr(0xFD00,&hi); sr(0xFD02,&lo); *out=((hi&3)<<8)|(lo&0xFF); }
  return 0;
}

int main(int argc,char**argv){
  if(argc<3){ fprintf(stderr,"usage: %s <BDF> <spico-code.bin>\n",argv[0]); return 2; }
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  /* load the SPICO blob (little-endian uint16) */
  FILE*bf=fopen(argv[2],"rb"); if(!bf){perror("open blob");return 1;}
  fseek(bf,0,SEEK_END); long bytes=ftell(bf); fseek(bf,0,SEEK_SET);
  int nwords=bytes/2; uint16_t *code=malloc(nwords*2);
  if(fread(code,2,nwords,bf)!=(size_t)nwords){fprintf(stderr,"blob read short\n");return 1;} fclose(bf);
  fprintf(stderr,"[spico] blob %s: %d words. PIN=0x%08x\n",argv[2],nwords,cr(0x1c021));

  /* quick SBus sanity: read the SPICO control reg */
  uint32_t v;
  if(sr(0xFD0C,&v)) { fprintf(stderr,"[spico] SBus read 0xFD0C failed — is InitSBus done?\n"); return 1; }
  fprintf(stderr,"[spico] SBus OK, 0xFD0C=0x%08x\n",v);

  /* 1. spicoEnable(0): 0xF004 is a DIRECT JSS CSR (not an SBus xact): Reset[0],Enable[1] -> 0 */
  v=cr(SBUS_SPICO); cw(SBUS_SPICO, v & ~0x3u);
  /* 2. IMEM upload */
  sw(0xFD0C,3); sw(0xFD0C,1); sw(0xFD06,8);              /* reset, enable, imem-we */
  for(int i=0;i<nwords;i++){ uint32_t w=code[i];
    sw(0xFD04,(i>>8)&0xFF); sw(0xFD05,i&0xFF); sw(0xFD07,w&0xFF);
    sw(0xFD06,((w>>8)&0xFF)|0xC); sw(0xFD06,((w>>8)&0xFF)|0x8);
    if((i&0x3ff)==0) fprintf(stderr,"[spico] IMEM %d/%d PIN=0x%08x\n",i,nwords,cr(0x1c021));
  }
  sw(0xFD06,0); sw(0xFD0C,8);                            /* imem-we off, SPICO run */
  fprintf(stderr,"[spico] IMEM upload done, PIN=0x%08x\n",cr(0x1c021));
  /* 3. spicoEnable(1): direct CSR 0xF004 Reset=0, Enable=1 */
  v=cr(SBUS_SPICO); cw(SBUS_SPICO,(v & ~0x1u)|0x2u);
  /* 4. alive check (interrupt cmd 2 -> 1) */
  uint32_t r=0; int alive = (spico_int(2,0,&r)==0 && r==1);
  fprintf(stderr,"[spico] alive check: int(2)=%u %s\n",r, alive?"ALIVE":"NOT ALIVE");
  /* 5. CRC self-check (interrupt cmd 4) */
  uint32_t crc=0; int crcok = (spico_int(4,0,&crc)==0);
  fprintf(stderr,"[spico] CRC self-check: %s (result=0x%03x)\n", crcok?"ack OK":"FAILED", crc);
  /* 6. control clear */
  sw(0xFD0C,0);
  fprintf(stderr,"[spico] === SPICO %s === PIN=0x%08x\n", (alive&&crcok)?"LOADED + RUNNING":"load incomplete", cr(0x1c021));
  return (alive&&crcok)?0:2;
}
