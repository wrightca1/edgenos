/* fm6000_rxdump.c - print every received frame's header so we can see what actually reaches the CPU. */
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
  unsigned secs=argc>1?(unsigned)strtoul(argv[1],0,0):30;
  struct fpdma_kmod*k=NULL; if(fpdma_kmod_open(&k)<0)return 1;
  size_t bsz=0; volatile void*b=fpdma_kmod_bar0(k,&bsz);
  fm6000_hw_attach(&dev,b,bsz,"0000:02:00.0");
  struct fpdma_backing back=fpdma_kmod_backing(k);
  if(fpdma_init(&fp,&dev,&back,8,64)<0)return 1;
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX_STOP); usleep(10000);
  for(unsigned i=0;i<fp.rx.size;i++) fp.rx.desc[i*FM6000_DESC_STRIDE]=FM6000_DESC_RX_READY;
  __sync_synchronize();
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX); usleep(10000);
  unsigned n=0;
  for(unsigned t=0;t<secs*200 && n<40;t++){
    usleep(5000);
    for(unsigned i=0;i<fp.rx.size && n<40;i++){
      volatile uint8_t *d=fp.rx.desc+i*FM6000_DESC_STRIDE;
      if(!(d[0]&0x04)) continue;
      uint16_t len=*(volatile uint16_t*)(d+2);
      uint8_t *p=(uint8_t*)fp.rx.buf_va[i];
      printf("[%2u] len=%-4u ",n,len);
      for(int j=0;j<32;j++) printf("%02x%s",p[j],(j==5||j==11||j==13||j==19)?"|":" ");
      printf("\n"); n++;
      d[0]=FM6000_DESC_RX_READY; __sync_synchronize();
      fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX_POST);
    }
  }
  printf("dumped %u frames\n",n);
  return 0;}
