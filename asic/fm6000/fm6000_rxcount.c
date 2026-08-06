/* fm6000_rxcount.c - count frames received into the CPU DMA ring over a fixed window, and show a few.
 * Re-arms each descriptor (READY) and issues RX_POST so the ring keeps cycling. */
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
  unsigned secs=argc>1?(unsigned)strtoul(argv[1],0,0):20;
  struct fpdma_kmod*k=NULL; if(fpdma_kmod_open(&k)<0)return 1;
  size_t bsz=0; volatile void*b=fpdma_kmod_bar0(k,&bsz);
  fm6000_hw_attach(&dev,b,bsz,"0000:02:00.0");
  struct fpdma_backing back=fpdma_kmod_backing(k);
  if(fpdma_init(&fp,&dev,&back,4,64)<0)return 1;
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX_STOP); usleep(10000);
  for(unsigned i=0;i<fp.rx.size;i++) fp.rx.desc[i*FM6000_DESC_STRIDE]=FM6000_DESC_RX_READY;
  __sync_synchronize();
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX); usleep(10000);
  unsigned got=0,bcast=0,shown=0;
  for(unsigned t=0;t<secs*200;t++){
    usleep(5000);
    for(unsigned i=0;i<fp.rx.size;i++){
      volatile uint8_t *d=fp.rx.desc+i*FM6000_DESC_STRIDE;
      if(!(d[0]&0x04)) continue;
      uint16_t len=*(volatile uint16_t*)(d+2);
      uint8_t *buf=(uint8_t*)fp.rx.buf_va[i];
      int isb = buf[0]==0xff&&buf[1]==0xff&&buf[2]==0xff&&buf[3]==0xff&&buf[4]==0xff&&buf[5]==0xff;
      if(isb) bcast++;
      if(shown<4){ printf("  RX len=%-4u dmac=%02x:%02x:%02x:%02x:%02x:%02x smac=%02x:%02x:%02x:%02x:%02x:%02x et=%02x%02x\n",
          len,buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],buf[8],buf[9],buf[10],buf[11],buf[12],buf[13]); shown++; }
      got++;
      d[0]=FM6000_DESC_RX_READY; __sync_synchronize();
      fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX_POST);
    }
  }
  printf("RX TOTAL=%u  (broadcast=%u)  STATUS=0x%08x IP=0x%08x\n",got,bcast,
         fm6000_dma_read(&dev,FM6000_DMA_STATUS),fm6000_dma_read(&dev,FM6000_DMA_IP));
  return 0;}
