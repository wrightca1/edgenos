/* fm6000_l3.c - minimal L3 endpoint on the FM6000, cold, under EdgeNOS.
 * Answers ARP who-has and ICMP echo-request for our IP, entirely from userspace over the packet DMA.
 *
 * RX frames arrive with NO F64 tag (ethertype is at offset 12).
 * TX frames MUST carry the 8-byte F64 tag inline at offset 12 -- the fabric requires it to forward,
 * and the egress MODIFY stage strips it, so a clean Ethernet frame reaches the wire. Verified:
 * tagged len=72 -> far-end +30 ; untagged len=64 -> far-end 0.
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
static uint8_t MYMAC[6]={0x44,0x4c,0xa8,0x31,0x5d,0xab};
static uint8_t MYIP[4] ={10,101,101,26};
/* TAG[1] is the egress GLORT and therefore selects the physical port. The GLORT<->port mapping is
 * assigned by EOS and is NOT stable across configurations -- read it from the boot trace, never assume.
 * Override with GLORT=<hex>.  (2-port+ECMP capture: 0x03ef=Et1, 0x03ee=Et2.) */
static uint16_t TAG[4]={0x0100,0x03ef,0xff00,0x0000};

static uint16_t cks(const uint8_t*d,int n,uint32_t init){
  uint32_t s=init; for(int i=0;i+1<n;i+=2) s+=(d[i]<<8)|d[i+1];
  if(n&1) s+=d[n-1]<<8;
  while(s>>16) s=(s&0xffff)+(s>>16);
  return (uint16_t)~s;
}
/* send: body = the frame WITHOUT the tag (DMAC|SMAC|ethertype|...); we splice the tag in at 12 */
static int xmit(const uint8_t*body,int blen){
  uint8_t f[1600];
  if(blen+8>(int)sizeof f) return -1;
  memcpy(f,body,12);
  for(int w=0;w<4;w++){ f[12+2*w]=TAG[w]>>8; f[12+2*w+1]=TAG[w]&0xff; }
  memcpy(f+20,body+12,blen-12);
  int len=blen+8; if(len<72) len=72;
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_TX_STOP); usleep(1500);
  fp.tx.head=0; fp.tx.tail=0;
  memset((void*)fp.tx.desc,0,fp.tx.size*FM6000_DESC_STRIDE);
  int r=fpdma_tx(&fp,f,(uint16_t)len);
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_TX); usleep(8000);
  return r;
}
int main(int argc,char**argv){
  unsigned secs=argc>1?(unsigned)strtoul(argv[1],0,0):60;
  { const char *g=getenv("GLORT"); if(g) TAG[1]=(uint16_t)strtoul(g,0,16); }
  struct fpdma_kmod*k=NULL; if(fpdma_kmod_open(&k)<0){printf("kmod fail\n");return 1;}
  size_t bsz=0; volatile void*b=fpdma_kmod_bar0(k,&bsz);
  fm6000_hw_attach(&dev,b,bsz,"0000:02:00.0");
  struct fpdma_backing back=fpdma_kmod_backing(k);
  if(fpdma_init(&fp,&dev,&back,8,64)<0){printf("init fail\n");return 1;}
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX_STOP); usleep(10000);
  for(unsigned i=0;i<fp.rx.size;i++) fp.rx.desc[i*FM6000_DESC_STRIDE]=FM6000_DESC_RX_READY;
  __sync_synchronize();
  fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX); usleep(10000);
  printf("L3 endpoint up: %u.%u.%u.%u  %02x:%02x:%02x:%02x:%02x:%02x  egress GLORT=0x%04x  (%us)\n",
     MYIP[0],MYIP[1],MYIP[2],MYIP[3],MYMAC[0],MYMAC[1],MYMAC[2],MYMAC[3],MYMAC[4],MYMAC[5],TAG[1],secs);
  unsigned arp=0,icmp=0,seen=0;
  for(unsigned t=0;t<secs*200;t++){
    usleep(5000);
    for(unsigned i=0;i<fp.rx.size;i++){
      volatile uint8_t *d=fp.rx.desc+i*FM6000_DESC_STRIDE;
      if(!(d[0]&0x04)) continue;
      uint16_t len=*(volatile uint16_t*)(d+2);
      uint8_t *raw=(uint8_t*)fp.rx.buf_va[i]; seen++;
      uint8_t out[1600];
      /* Find the ARP/IPv4 header: scan for an ethertype in the first 40 bytes, allowing for a
       * buffer prefix and/or an inline 8-byte F64 tag (frame may be at 0, +8, or offset by a
       * receive prefix). p points at the DMAC of the located frame. */
      uint8_t *p=NULL; uint16_t et=0;
      for(int off=0; off+42<=(int)len && off<40; off++){
        uint16_t e=(raw[off+12]<<8)|raw[off+13];
        if(e==0x0806 || e==0x0800){ p=raw+off; et=e; break; }
        uint16_t e2=(raw[off+20]<<8)|raw[off+21];        /* tag present at +12 */
        if(e2==0x0806 || e2==0x0800){
          static uint8_t tmp[1600];
          memcpy(tmp,raw+off,12); memcpy(tmp+12,raw+off+20,len-off-20);
          p=tmp; et=e2; break; }
      }
      if(!p) { d[0]=FM6000_DESC_RX_READY; __sync_synchronize();
               fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX_POST); continue; }
      if(et==0x0806 && len>=42 && p[20]==0 && p[21]==1 && !memcmp(p+38,MYIP,4)){
        memcpy(out,p+6,6); memcpy(out+6,MYMAC,6);         /* to requester, from us */
        out[12]=0x08; out[13]=0x06;
        out[14]=0;out[15]=1;out[16]=8;out[17]=0;out[18]=6;out[19]=4;out[20]=0;out[21]=2; /* reply */
        memcpy(out+22,MYMAC,6); memcpy(out+28,MYIP,4);    /* sender = us */
        memcpy(out+32,p+22,6);  memcpy(out+38,p+28,4);    /* target = requester */
        if(xmit(out,42)==0){arp++; printf("  ARP who-has %u.%u.%u.%u -> replied (#%u)\n",
             MYIP[0],MYIP[1],MYIP[2],MYIP[3],arp);}
      } else if(et==0x0800 && len>=42 && p[23]==1 && !memcmp(p+30,MYIP,4) && p[34]==8){
        int ihl=(p[14]&0xf)*4, iptot=(p[16]<<8)|p[17], flen=14+iptot;
        if(flen>(int)sizeof out) continue;
        memcpy(out,p,flen);
        memcpy(out,p+6,6); memcpy(out+6,MYMAC,6);          /* swap MACs */
        memcpy(out+26,p+30,4); memcpy(out+30,p+26,4);      /* swap IPs */
        out[24]=0; out[25]=0;
        uint16_t ic=cks(out+14,ihl,0); out[24]=ic>>8; out[25]=ic&0xff;
        out[14+ihl]=0;                                     /* type 8 -> 0 (echo reply) */
        out[14+ihl+2]=0; out[14+ihl+3]=0;
        uint16_t cc=cks(out+14+ihl,iptot-ihl,0);
        out[14+ihl+2]=cc>>8; out[14+ihl+3]=cc&0xff;
        if(xmit(out,flen)==0){icmp++; printf("  ICMP echo-request -> replied (#%u)\n",icmp);}
      }
      d[0]=FM6000_DESC_RX_READY; __sync_synchronize();
      fm6000_dma_write(&dev,FM6000_DMA_COMMAND,FM6000_DMA_CMD_RX_POST);
    }
    if(arp>=4 && icmp>=6) break;
  }
  printf("DONE: rx_seen=%u arp_replies=%u icmp_replies=%u\n",seen,arp,icmp);
  return 0;}
