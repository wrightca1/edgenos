/* fm6000_rxstat.c — READ-ONLY dump of a SerDes's RX-relevant ETH registers, to diagnose why the RX
 * isn't locking (sig-detect=0 despite optical light present). No writes. usage: fm6000_rxstat <BDF> <serdes> */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uint32_t *M;
static inline void cw(uint32_t w,uint32_t v){ M[w]=v; __sync_synchronize(); }
static inline uint32_t cr(uint32_t w){ uint32_t v=M[w]; __sync_synchronize(); return v; }
static uint32_t er(unsigned sd,unsigned n){          /* SBus read ETH reg n of serdes sd (recv sd+5) */
  uint32_t reg=(((sd+5)&0xff)<<8)|(n&0xff),st=0; int i;
  cw(0xF002,0); cw(0xF001,0); cw(0xF001,(reg&0xFFFF)|(0x22<<16)|(1u<<24));
  for(i=0;i<100000;i++){ st=cr(0xF001); if(!(st&(1u<<25)))break; }
  return (st&(1u<<25))?0xffffffff:cr(0xF003);
}
int main(int argc,char**argv){
  if(argc<3){ fprintf(stderr,"usage: %s <BDF> <serdes>\n",argv[0]); return 2; }
  unsigned sd=atoi(argv[2]);
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  uint32_t r0=er(sd,0),r3=er(sd,3),r6=er(sd,6),r7=er(sd,7),r13=er(sd,13),r34=er(sd,34);
  uint32_t r15=er(sd,15),r20=er(sd,20),r23=er(sd,23),r31=er(sd,31);
  fprintf(stderr,"[rx] serdes %u (recv 0x%02x):\n",sd,sd+5);
  fprintf(stderr,"  reg0=0x%02x  rate/mode bits[6:1]=0x%02x en[0]=%d\n", r0&0xff, (r0>>1)&0x3f, r0&1);
  fprintf(stderr,"  reg3=0x%02x  TxEn[0]=%d\n", r3&0xff, r3&1);
  fprintf(stderr,"  reg6=0x%02x  RxEn[3]=%d\n", r6&0xff, (r6>>3)&1);
  fprintf(stderr,"  reg7=0x%02x  rxPolInv[4]=%d\n", r7&0xff, (r7>>4)&1);
  fprintf(stderr,"  reg13=0x%02x datapath[0]=%d [4]=%d loopback[7]=%d\n", r13&0xff, r13&1,(r13>>4)&1,(r13>>7)&1);
  fprintf(stderr,"  reg34=0x%02x RXdatapath[1:0]=0x%x\n", r34&0xff, r34&3);
  fprintf(stderr,"  reg15=0x%02x PLLlock[3]=%d RXready[0]=%d\n", r15&0xff, (r15>>3)&1, r15&1);
  fprintf(stderr,"  reg20=0x%02x sig-detect[6]=%d\n", r20&0xff, (r20>>6)&1);
  fprintf(stderr,"  reg23=0x%02x DFE-DACrange[4:0]=0x%02x\n", r23&0xff, r23&0x1f);
  fprintf(stderr,"  reg31=0x%02x DFE coarse[5:4]=%d fine[3:2]=%d (2=done)\n", r31&0xff, (r31>>4)&3,(r31>>2)&3);
  fprintf(stderr,"  RX status regs 0x1f..0x27:");
  for(unsigned n=0x1f;n<=0x27;n++) fprintf(stderr," [%02x]=%02x",n,er(sd,n)&0xff);
  fprintf(stderr,"\n");
  munmap((void*)M,32u*1024*1024); close(fd);
  return 0;
}
