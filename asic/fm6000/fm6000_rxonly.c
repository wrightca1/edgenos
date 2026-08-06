/* fm6000_rxonly.c - CALIBRATE THE RX INSTRUMENT: arm the RX ring and poll for frames arriving
 * from the WIRE (known-good source: the AS5610 flooding Et1). If nothing lands here while the
 * STATS counters climb, the DMA RX path is broken and every "RX total frames=0" loopback result
 * in this session is meaningless. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include "fpdma.h"
#include "fpdma_kmod.h"
#include "fm6000_regs.h"
static struct fpdma fp; static struct fm6000_dev dev;
static const char *ts(uint32_t s){switch(s&7){case 0:return"Stopped";case 1:return"RUNNING";
  case 2:return"Idle";case 3:return"Drain";case 4:return"Pause";default:return"?";}}
int main(int argc,char**argv){
  unsigned secs=argc>1?(unsigned)strtoul(argv[1],0,0):30;
  struct fpdma_kmod*k=NULL; if(fpdma_kmod_open(&k)<0)return 1;
  size_t bsz=0; volatile void*b=fpdma_kmod_bar0(k,&bsz);
  fm6000_hw_attach(&dev,b,bsz,"0000:02:00.0");
  struct fpdma_backing back=fpdma_kmod_backing(k);
  if(fpdma_init(&fp,&dev,&back,4,16)<0)return 1;
  /* RX: stop (resets index to 0) -> descriptors are already READY from ring_alloc -> start */
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX_STOP); usleep(10000);
  for(unsigned i=0;i<fp.rx.size;i++) fp.rx.desc[i*FM6000_DESC_STRIDE]=FM6000_DESC_RX_READY;
  __sync_synchronize();
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX); usleep(10000);
  uint32_t s=fm6000_dma_read(&dev,FM6000_DMA_STATUS);
  printf("RX armed: Rx=%u(%s-ish) STATUS=0x%08x  size=%u\n",(s>>3)&7,ts((s>>3)&7),s,fp.rx.size);
  unsigned got=0;
  for(unsigned t=0;t<secs*10;t++){
    usleep(100000);
    for(unsigned i=0;i<fp.rx.size;i++){
      volatile uint8_t *d=fp.rx.desc+i*FM6000_DESC_STRIDE;
      if(d[0]&0x04){
        uint16_t len=*(volatile uint16_t*)(d+2);
        uint8_t *buf=(uint8_t*)fp.rx.buf_va[i];
        printf("  RX[%u] st=0x%02x len=%u :",i,d[0],len);
        for(int j=0;j<20;j++) printf(" %02x",buf[j]);
        printf("\n"); fflush(stdout); got++;
        d[0]=FM6000_DESC_RX_READY; __sync_synchronize();
        fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX_POST);
      }}
    if(got>=8) break;
  }
  s=fm6000_dma_read(&dev,FM6000_DMA_STATUS);
  printf("RX total=%u  STATUS=0x%08x IP=0x%08x\n",got,s,fm6000_dma_read(&dev,FM6000_DMA_IP));
  return 0;}
