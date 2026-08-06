/* fm6000_txburst.c - transmit N frames using the WORKING sequence:
 *   TX_STOP (resets desc index to 0) -> fill descriptors READY -> TX_START.
 * fpdma_init's START-on-empty-ring leaves the Tx processor Idle, and TX_POST does
 * NOT wake it from Idle -- that was the real "TX never transmits" bug. */
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
  unsigned rounds=argc>1?(unsigned)strtoul(argv[1],0,0):25;
  unsigned dglort=argc>2?(unsigned)strtoul(argv[2],0,0):0x0000;
  unsigned ftype =argc>3?(unsigned)strtoul(argv[3],0,0):0x1000;
  struct fpdma_kmod*k=NULL; if(fpdma_kmod_open(&k)<0){printf("kmod fail\n");return 1;}
  size_t bsz=0; volatile void*b=fpdma_kmod_bar0(k,&bsz);
  fm6000_hw_attach(&dev,b,bsz,"0000:02:00.0");
  struct fpdma_backing back=fpdma_kmod_backing(k);
  if(fpdma_init(&fp,&dev,&back,4,4)<0){printf("init fail\n");return 1;}
  uint8_t f[64]; memset(f,0,64); memset(f,0xff,6);
  f[6]=0x02; f[11]=0x01; f[12]=0xDE;f[13]=0xAD;f[14]=0xBE;f[15]=0xEF;
  uint16_t tw[4]={(uint16_t)ftype,0,0x0028,(uint16_t)dglort}; uint8_t tag[8];
  for(int w=0;w<4;w++){tag[2*w]=tw[w]>>8;tag[2*w+1]=tw[w]&0xff;}
  unsigned done=0,sent=0;
  for(unsigned r=0;r<rounds;r++){
    fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_TX_STOP); usleep(2000);
    fp.tx.head=0; fp.tx.tail=0;
    memset((void*)fp.tx.desc,0,fp.tx.size*FM6000_DESC_STRIDE);
    for(int s=0;s<3;s++){ if(fpdma_tx_f64(&fp,f,64,tag,8)==0) sent++; }
    fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_TX); usleep(20000);
    for(int s=0;s<3;s++) if(fp.tx.desc[s*FM6000_DESC_STRIDE]&0x04) done++;
  }
  uint32_t stv=fm6000_dma_read(&dev,FM6000_DMA_STATUS);
  printf("rounds=%u queued=%u DONE=%u  STATUS=0x%08x IP=0x%08x\n",
     rounds,sent,done,stv,fm6000_dma_read(&dev,FM6000_DMA_IP));
  return 0;}
