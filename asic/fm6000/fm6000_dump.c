/* fm6000_dump.c - dump EPL14 + serdes-68 core registers (the oracle, in C for the M1 shell). */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uint32_t *M;
static void wr(uint32_t w,uint32_t v){M[w]=v;__sync_synchronize();}
static uint32_t rd(uint32_t w){uint32_t v=M[w];__sync_synchronize();return v;}
static int sbrd(int dev,int reg){
 wr(0xF002,0); wr(0xF001,0);
 wr(0xF001,((dev&0xff)<<8)|(reg&0xff)|(0x22<<16)|(1u<<24));
 for(long i=0;i<500000;i++){uint32_t s=rd(0xF001); if(s==0xffffffffu) return -1;
   if(!(s&(1u<<25))) return (int)(rd(0xF003)&0xff);} return -2;}
int main(int argc,char**argv){
 char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argc>1?argv[1]:"0000:02:00.0");
 int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
 M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 if(M==MAP_FAILED){perror("mmap");return 1;}
 int core[]={0x00,0x03,0x06,0x0d,0x17,0x1d,0x1f,0x20,0x22,0x26,0x36,0x3b,-1};
 printf("core:");
 for(int i=0;core[i]>=0;i++) printf(" %02x=%02x",core[i],sbrd(0x49,core[i]));
 printf("\n");
 struct{const char*n;uint32_t a;} e[]={{"PORT_STATUS",0xe3800},{"LINK_RULES",0xe380c},
  {"PCS_10GBR_CFG",0xe3825},{"PCS_RX_STAT",0xe3826},{"PCS_TX_STAT",0xe3827},
  {"MAC_CFG0",0xe3810},{"MAC_CFG1",0xe3811},{"MAC_CFG2",0xe3812},{"MAC_CFG3",0xe3813},
  {"SERDES_CFG_lo",0xe3834},{"SERDES_CFG_hi",0xe3835},{"LANE_CFG",0xe3837},{"LANE_STATUS",0xe3838},
  {"SERDES_RX_CFG",0xe3839},{"SERDES_TX_CFG_lo",0xe383a},{"SERDES_TX_CFG_hi",0xe383b},
  {"SIGDET",0xe383c},{"STATUS_lo",0xe383e},{"STATUS_hi",0xe383f},{"SERDES_IM",0xe3840},
  {"SERDES_IP",0xe3841},{"EPL_IP",0xe3b00},{"EPL_CFG_A",0xe3b01},{"EPL_CFG_B",0xe3b02},{NULL,0}};
 for(int i=0;e[i].n;i++) printf("  %-17s 0x%05x = 0x%08x\n",e[i].n,e[i].a,rd(e[i].a));
 return 0;}
