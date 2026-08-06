/* fm6000_txstart.c - why won't the Tx packet processor leave STOPPED/Idle?
 * Careful stepwise: settle + read state after EVERY single command. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "fpdma.h"
#include "fpdma_kmod.h"
#include "fm6000_regs.h"
static struct fpdma fp; static struct fm6000_dev dev;
static const char *ts(uint32_t s){switch(s&7){case 0:return"Stopped";case 1:return"RUNNING";
  case 2:return"Idle";case 3:return"Drain";case 4:return"Pause";default:return"?";}}
static void st(const char*t){uint32_t s=fm6000_dma_read(&dev,FM6000_DMA_STATUS);
  printf("  %-26s Tx=%-8s STATUS=0x%08x desc[0]=0x%02x\n",t,ts(s),s,fp.tx.desc[0]);fflush(stdout);}
static void cmd(uint32_t c,const char*n){fm6000_dma_write(&dev,FM6000_DMA_COMMAND,c);usleep(50000);st(n);}
int main(void){
  struct fpdma_kmod*k=NULL; if(fpdma_kmod_open(&k)<0)return 1;
  size_t bsz=0; volatile void*b=fpdma_kmod_bar0(k,&bsz);
  fm6000_hw_attach(&dev,b,bsz,"0000:02:00.0");
  struct fpdma_backing back=fpdma_kmod_backing(k);
  if(fpdma_init(&fp,&dev,&back,4,4)<0)return 1;
  printf("ring regs: tx_base=%08x_%08x tx_end=%08x_%08x  desc_dma=0x%llx size=%u\n",
    fm6000_dma_read(&dev,FM6000_DMA_TX_BD_BASE_HI),fm6000_dma_read(&dev,FM6000_DMA_TX_BD_BASE_LO),
    fm6000_dma_read(&dev,FM6000_DMA_TX_BD_END_HI),fm6000_dma_read(&dev,FM6000_DMA_TX_BD_END_LO),
    (unsigned long long)fp.tx.desc_dma,fp.tx.size);
  printf("cfg=0x%08x cfg2=0x%08x im=0x%08x ip=0x%08x\n",
    fm6000_dma_read(&dev,FM6000_DMA_CFG),fm6000_dma_read(&dev,FM6000_DMA_CFG2),
    fm6000_dma_read(&dev,FM6000_DMA_IM),fm6000_dma_read(&dev,FM6000_DMA_IP));
  st("after init");
  cmd(FM6000_DMA_CMD_TX_STOP,"TX_STOP");
  /* descriptor READY *before* START, so index0 has work the moment it starts */
  uint8_t f[64]; memset(f,0,64); memset(f,0xff,6); f[6]=2; f[11]=1;
  f[12]=0xDE;f[13]=0xAD;f[14]=0xBE;f[15]=0xEF;
  uint16_t tw[4]={0x1000,0,0x0028,0}; uint8_t tag[8];
  for(int w=0;w<4;w++){tag[2*w]=tw[w]>>8;tag[2*w+1]=tw[w]&0xff;}
  fp.tx.head=0; fp.tx.tail=0;
  memset((void*)fp.tx.desc,0,fp.tx.size*FM6000_DESC_STRIDE);
  fpdma_tx_f64(&fp,f,64,tag,8);
  usleep(50000); st("desc READY posted");
  cmd(FM6000_DMA_CMD_TX,"TX_START");
  for(int i=0;i<6;i++){usleep(150000);st("  poll");}
  printf("IP=0x%08x (bit0 = Tx descriptor completed)\n",fm6000_dma_read(&dev,FM6000_DMA_IP));
  return 0;}
