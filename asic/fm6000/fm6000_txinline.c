/* fm6000_txinline.c - transmit with the F64 tag INLINE in the frame, the way EOS actually does it.
 *
 * Caught on a live EOS (2026-08-05): the descriptor's F64 field is ALL ZERO and the 8-byte tag sits
 * in the packet buffer at offset 12, right after SMAC:
 *   80 a2 35 81 ca b4 | 44 4c a8 31 5d ab | 01 00 03 ee ff 00 00 00 | 08 00 45 00 ...
 *      DMAC                  SMAC             <---- F64 tag ---->      ethertype
 * Our fpdma_tx_f64() did the opposite (tag into the BD, nothing in the payload), so every frame we
 * ever injected carried no tag at all.
 *
 * Uses the working TX sequence: TX_STOP (resets desc index) -> fill READY -> TX_START.
 *   fm6000_txinline [rounds] [tagword0..3 hex] ; default tag = EOS's 0100 03ee ff00 0000
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "fpdma.h"
#include "fpdma_kmod.h"
#include "fm6000_regs.h"
static struct fpdma fp; static struct fm6000_dev dev;
int main(int argc,char**argv){
  unsigned rounds = argc>1?(unsigned)strtoul(argv[1],0,0):20;
  uint16_t t0 = argc>2?(uint16_t)strtoul(argv[2],0,16):0x0100;
  uint16_t t1 = argc>3?(uint16_t)strtoul(argv[3],0,16):0x03ee;
  uint16_t t2 = argc>4?(uint16_t)strtoul(argv[4],0,16):0xff00;
  uint16_t t3 = argc>5?(uint16_t)strtoul(argv[5],0,16):0x0000;
  int bcast   = argc>6?atoi(argv[6]):0;
  struct fpdma_kmod*k=NULL; if(fpdma_kmod_open(&k)<0){printf("kmod fail\n");return 1;}
  size_t bsz=0; volatile void*b=fpdma_kmod_bar0(k,&bsz);
  fm6000_hw_attach(&dev,b,bsz,"0000:02:00.0");
  struct fpdma_backing back=fpdma_kmod_backing(k);
  if(fpdma_init(&fp,&dev,&back,4,4)<0){printf("init fail\n");return 1;}

  /* frame: DMAC | SMAC | F64 tag (8B, inline) | ethertype | payload */
  uint8_t f[128]; memset(f,0,sizeof f);
  static const uint8_t dmac_5610[6]={0x80,0xa2,0x35,0x81,0xca,0xb4};
  if(bcast) memset(f,0xff,6); else memcpy(f,dmac_5610,6);
  static const uint8_t smac[6]={0x44,0x4c,0xa8,0x31,0x5d,0xab};   /* Et1's MAC, as EOS uses */
  memcpy(f+6,smac,6);
  uint16_t tw[4]={t0,t1,t2,t3};
  for(int w=0;w<4;w++){ f[12+2*w]=tw[w]>>8; f[12+2*w+1]=tw[w]&0xff; }   /* TAG AT OFFSET 12 */
  f[20]=0x08; f[21]=0x00;                                               /* ethertype IPv4 */
  f[22]=0xDE; f[23]=0xAD; f[24]=0xBE; f[25]=0xEF;                       /* marker */
  uint16_t len=72;   /* 12 hdr + 8 tag + 52 body */

  printf("frame[0:24]:"); for(int i=0;i<24;i++) printf(" %02x",f[i]); printf("\n");
  printf("tag@12 = %04x %04x %04x %04x, len=%u, BD F64 field left ZERO\n",t0,t1,t2,t3,len);

  unsigned q=0,done=0;
  for(unsigned r=0;r<rounds;r++){
    fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_TX_STOP); usleep(2000);
    fp.tx.head=0; fp.tx.tail=0;
    memset((void*)fp.tx.desc,0,fp.tx.size*FM6000_DESC_STRIDE);
    for(int s=0;s<3;s++) if(fpdma_tx(&fp,f,len)==0) q++;   /* fpdma_tx -> f64=NULL -> BD field zeroed */
    fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_TX); usleep(20000);
    for(int s=0;s<3;s++) if(fp.tx.desc[s*FM6000_DESC_STRIDE]&0x04) done++;
  }
  printf("queued=%u DONE=%u STATUS=0x%08x IP=0x%08x\n",q,done,
         fm6000_dma_read(&dev,FM6000_DMA_STATUS),fm6000_dma_read(&dev,FM6000_DMA_IP));
  return 0;}
