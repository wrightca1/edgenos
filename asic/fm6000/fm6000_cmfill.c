/* fm6000_cmfill.c - do the CM admission-watermark fills ourselves instead of via the (stuck) CRM.
 * From the SDK-mined CRM fill list (notes/reference/fm6000-crm-fill-sequence.md):
 *   #90  0x112800  CM  fill=0xffffffff  width 1  dims 0xc x 0x4c  =  912 words  (PORT_RXMP_PRIVATE_WM)
 *   #91  0x113000  CM  fill=0xffffffff  width 1  dims 0x10 x 0x4c = 1216 words  (PORT_RXMP_HOG_WM)
 *   #92  0x115000  CM  fill=0xffffffff  width 1  912 words
 *   #93  0x115800  CM  fill=0xffffffff  width 1  912 words
 *   #94  0x117000  CM  fill=0x20001000  width 1  912 words
 *   #101 0x114000  CM  fill=0x3fff      width 1  0x10 x 0x50 = 1280 words
 * These are width-1 entries, so a plain MMIO word write is a complete element (no atomic MSW commit
 * needed). Paced, with an off-bus guard. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uint32_t *M;
static uint32_t rd(uint32_t w){uint32_t v=M[w];__sync_synchronize();return v;}
static void wr(uint32_t w,uint32_t v){M[w]=v;__sync_synchronize();}
static int fill(const char*n,uint32_t base,uint32_t words,uint32_t val,unsigned pace){
  printf("  %-22s @0x%06x x%u <- 0x%08x ... ",n,base,words,val); fflush(stdout);
  for(uint32_t i=0;i<words;i++){
    wr(base+i,val);
    if((i&0x3f)==0x3f){ if(pace) usleep(pace);
      if(rd(0x1c021)!=0x208u){ printf("OFF-BUS at +%u\n",i); return -1; } }
  }
  printf("done (readback[0]=0x%08x)\n", rd(base));
  return 0;
}
int main(int argc,char**argv){
  unsigned pace = argc>2 ? (unsigned)strtoul(argv[2],0,0) : 200;
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argc>1?argv[1]:"0000:02:00.0");
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  printf("PIN=0x%08x  before: PRIVATE_WM[p0]=0x%08x HOG_WM[p0]=0x%08x\n",
         rd(0x1c021), rd(0x112800), rd(0x113000));
  if(rd(0x1c021)!=0x208u){printf("chip not alive\n");return 1;}
  if(fill("PORT_RXMP_PRIVATE_WM",0x112800, 912,0xffffffffu,pace)) return 2;
  if(fill("PORT_RXMP_HOG_WM",   0x113000,1216,0xffffffffu,pace)) return 2;
  if(fill("CM_0x115000",        0x115000, 912,0xffffffffu,pace)) return 2;
  if(fill("CM_0x115800",        0x115800, 912,0xffffffffu,pace)) return 2;
  if(fill("CM_0x117000",        0x117000, 912,0x20001000u,pace)) return 2;
  if(fill("CM_0x114000",        0x114000,1280,0x00003fffu,pace)) return 2;
  printf("after : PRIVATE_WM[p0]=0x%08x HOG_WM[p0]=0x%08x PIN=0x%08x\n",
         rd(0x112800), rd(0x113000), rd(0x1c021));
  return 0;}
