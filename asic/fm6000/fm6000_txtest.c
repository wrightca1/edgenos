/* fm6000_txtest.c - ONE long-lived process: init the rings once, then try each command sequence
 * that could move the Tx packet processor out of Idle, reporting descriptor status + TxState.
 * Datasheet Table 7-2 commands / Table 7-3 TxState (0=Stopped 1=Running 2=Idle 3=Drain 4=Pause). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "fpdma.h"
#include "fpdma_kmod.h"
#include "fm6000_regs.h"

static struct fpdma fp;
static struct fm6000_dev dev;
static const char *tstate(uint32_t s){
  switch(s&7){case 0:return"Stopped";case 1:return"RUNNING";case 2:return"Idle";
              case 3:return"Drain";case 4:return"Pause";default:return"?";}}
static void st(const char *tag){
  uint32_t s=fm6000_dma_read(&dev,FM6000_DMA_STATUS);
  volatile uint8_t *d=fp.tx.desc;
  printf("   %-22s STATUS=0x%08x Tx=%s Rx=%u | desc[0].status=0x%02x desc[1].status=0x%02x cmd=0x%08x\n",
     tag,s,tstate(s),(s>>3)&7,d[0],d[FM6000_DESC_STRIDE],
     fm6000_dma_read(&dev,FM6000_DMA_COMMAND));
  fflush(stdout);
}
static void mkframe(uint8_t *f){
  memset(f,0,64); memset(f,0xff,6);
  f[6]=0x02;f[11]=0x01; f[12]=0xDE;f[13]=0xAD;f[14]=0xBE;f[15]=0xEF;
}
int main(void){
  struct fpdma_kmod *k=NULL;
  if(fpdma_kmod_open(&k)<0){printf("kmod open failed\n");return 1;}
  size_t bsz=0; volatile void *bar0=fpdma_kmod_bar0(k,&bsz);
  fm6000_hw_attach(&dev,bar0,bsz,"0000:02:00.0");
  struct fpdma_backing back=fpdma_kmod_backing(k);
  if(fpdma_init(&fp,&dev,&back,4,4)<0){printf("init failed\n");return 1;}
  printf("init done\n"); st("after init");

  uint8_t f[64]; mkframe(f);
  uint16_t tw[4]={0x1000,0,0x0028,0}; uint8_t tag[8];
  for(int w=0;w<4;w++){tag[2*w]=tw[w]>>8;tag[2*w+1]=tw[w]&0xff;}

  struct { const char *name; uint32_t cmds[4]; } S[] = {
    {"TX_POST",             {FM6000_DMA_CMD_TX_POST,0,0,0}},
    {"TX_START",            {FM6000_DMA_CMD_TX,0,0,0}},
    {"POST then START",     {FM6000_DMA_CMD_TX_POST,FM6000_DMA_CMD_TX,0,0}},
    {"START then POST",     {FM6000_DMA_CMD_TX,FM6000_DMA_CMD_TX_POST,0,0}},
    {"STOP,START,POST",     {FM6000_DMA_CMD_TX_STOP,FM6000_DMA_CMD_TX,FM6000_DMA_CMD_TX_POST,0}},
    {"POST|count<<16",      {FM6000_DMA_CMD_TX_POST|(1u<<16),0,0,0}},
    {"POST|count<<4",       {FM6000_DMA_CMD_TX_POST|(1u<<4),0,0,0}},
  };
  for(unsigned i=0;i<sizeof(S)/sizeof(S[0]);i++){
    printf("[%u] %s\n",i,S[i].name);
    fp.tx.head=0; fp.tx.tail=0;
    memset((void*)fp.tx.desc,0,fp.tx.size*FM6000_DESC_STRIDE);
    fpdma_tx_f64(&fp,f,64,tag,8);      /* writes desc[0] + its own TX_POST */
    for(int c=0;c<4 && S[i].cmds[c];c++)
        fm6000_dma_write(&dev,FM6000_DMA_COMMAND,S[i].cmds[c]);
    for(int p=0;p<5;p++){ usleep(40000); }
    st("after");
  }
  printf("done\n");
  return 0;
}
