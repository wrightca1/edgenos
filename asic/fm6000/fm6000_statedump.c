/* fm6000_statedump.c - dump the FM6000's CONTROL/CONFIG register state for a cold-vs-warm diff.
 * Deliberately covers control registers and per-port config, NOT the huge memory arrays (those are
 * memfilled and would dwarf the diff). Liveness-checked: aborts if the chip goes off-bus.
 * Output: "aaaaa vvvvvvvv" per line, so two dumps diff directly. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uint32_t *M;
static uint32_t rd(uint32_t w){uint32_t v=M[w];__sync_synchronize();return v;}
struct R { uint32_t lo, hi; const char *n; };
static const struct R RG[] = {
 {0x00000,0x00100,"MGMT1"},      {0x01000,0x01100,"PCIE"},
 {0x02000,0x02200,"ESCHED"},     {0x03000,0x03100,"MONITOR"},
 {0x03800,0x03900,"ESCHED_DRR"}, {0x04000,0x04100,"MSB"},
 {0x05000,0x05100,"FIBM"},       {0x06000,0x06100,"EACL"},
 {0x07000,0x07100,"LAG"},        {0x08000,0x08100,"SSCHED"},
 {0x0b000,0x0b040,"HASH"},       {0x0c000,0x0c040,"ALU"},
 {0x0d000,0x0d200,"L2L_SWEEP"},  {0x0e000,0x0e100,"GLORT_CAM"},
 {0x0e800,0x0e900,"GLORT_RAM"},  {0x0f000,0x0f020,"JSS"},
 {0x14000,0x14100,"LBS"},        {0x18e80,0x18f80,"STATS_PORTMAP"},
 {0x1c000,0x1c100,"MGMT2"},      {0x20000,0x20040,"CMM"},
 {0x28000,0x28040,"FC_BEM"},     {0x0a000,0x0a140,"SAF"},
 {0x110000,0x110300,"CM_cfg"},   {0x116000,0x116800,"CM_pause"},
 {0x117800,0x117900,"ERL"},      {0x1f000,0x1f300,"CRM"},
 {0xe3800,0xe3c00,"EPL14"},      {0xe0300,0xe0400,"EPL0_cfg"},
 {0x180000,0x180100,"L2F_dmask"},{0x180400,0x180420,"L2F_dmask257"},
 {0,0,0}
};
int main(int argc,char**argv){
 char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argc>1?argv[1]:"0000:02:00.0");
 int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
 M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 if(M==MAP_FAILED){perror("mmap");return 1;}
 if(rd(0x1c021)!=0x208u){fprintf(stderr,"chip not alive\n");return 1;}
 for(int i=0;RG[i].n;i++){
   for(uint32_t a=RG[i].lo;a<RG[i].hi;a++){
     printf("%05x %08x\n",a,rd(a));
     if(((a-RG[i].lo)&0xff)==0xff && rd(0x1c021)!=0x208u){
       fprintf(stderr,"OFF-BUS in %s at 0x%05x\n",RG[i].n,a); return 2; }
   }
 }
 fprintf(stderr,"dump complete, PIN=0x%08x\n",rd(0x1c021));
 return 0;}
