/* fm6000_memfill.c - do the FM6000's 129 ordered memory fills by DIRECT MMIO, replacing the CRM engine
 * (which has been "Running stuck" in our clean-room since phase107b).
 *
 * Source of truth: notes/reference/fm6000-crm-fill-sequence.md (SDK-mined from libFocalpointSDK.so).
 * These tables are memory-backed and hold UNINITIALISED SRAM GARBAGE on a cold chip; EOS fills them via
 * the CRM at boot. Proven necessary: CM_PORT_RXMP_PRIVATE_WM/HOG_WM (#90/#91, fill 0xffffffff) gate
 * buffer admission, and cold they read random values -> CPU-injected frames are rejected at fabric
 * ingress with no drop counter anywhere.
 *
 * Fills MUST run IN ORDER starting at PARSER 0x100200 (#0). Prereqs (all satisfied by fm6000-coldlink.sh):
 * InitSBus has run, SOFT_RESET(0x9) bits{0,1,2,3,4} clear, FC_MRL 0x28020/0x28022 set.
 * Two doc-elided runs (#71-88 L2AR, #108-112 MOD) are reconstructed from the doc's summary lines.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uint32_t *M;
static uint32_t rd(uint32_t w){uint32_t v=M[w];__sync_synchronize();return v;}
static const struct { uint32_t base; int words; uint32_t val; const char *blk; } F[] = {
 {0x100200,  14336,0x00000000,"PARSER"},
 {0x108000,    152,0x00000000,"PARSER"},
 {0x108200,    304,0x00000000,"PARSER"},
 {0x121000,   4096,0x00000000,"MAPPER"},
 {0x122000,   4096,0x00000000,"MAPPER"},
 {0x123000,    152,0x00000000,"MAPPER"},
 {0x123100,     48,0x00000000,"MAPPER"},
 {0x123140,     48,0x00000000,"MAPPER"},
 {0x123180,     96,0x00000000,"MAPPER"},
 {0x123200,     16,0x00000000,"MAPPER"},
 {0x123210,     16,0x00000000,"MAPPER"},
 {0x123220,     32,0x00000000,"MAPPER"},
 {0x123300,     48,0x00000000,"MAPPER"},
 {0x123380,     96,0x00000000,"MAPPER"},
 {0x123400,     16,0x00000000,"MAPPER"},
 {0x123420,     32,0x00000000,"MAPPER"},
 {0x123440,     16,0x00000000,"MAPPER"},
 {0x123450,     16,0x00000000,"MAPPER"},
 {0x123460,     16,0x00000000,"MAPPER"},
 {0x123470,     16,0x00000000,"MAPPER"},
 {0x123800,    128,0x00000000,"MAPPER"},
 {0x123880,    128,0x00000000,"MAPPER"},
 {0x123900,    256,0x00000000,"MAPPER"},
 {0x123a00,    128,0x00000000,"MAPPER"},
 {0x123a80,    128,0x00000000,"MAPPER"},
 {0x123b00,    256,0x00000000,"MAPPER"},
 {0x123c00,     16,0x00000000,"MAPPER"},
 {0x123c10,     16,0x00000000,"MAPPER"},
 {0x123c20,     32,0x00000000,"MAPPER"},
 {0x123c40,     16,0x00000000,"MAPPER"},
 {0x123c50,     16,0x00000000,"MAPPER"},
 {0x123c60,     32,0x00000000,"MAPPER"},
 {0x123c80,     16,0x00000000,"MAPPER"},
 {0x123c90,     16,0x00000000,"MAPPER"},
 {0x123ca0,     16,0x00000000,"MAPPER"},
 {0x123cb0,     16,0x00000000,"MAPPER"},
 {0x123d80,    128,0x00000000,"MAPPER"},
 {0x123d00,    128,0x00000000,"MAPPER"},
 {0x123cc0,     16,0x00000000,"MAPPER"},
 {0x124280,     64,0x00000000,"MAPPER"},
 {0x1242c0,     64,0x00000000,"MAPPER"},
 {0x124300,     32,0x00000000,"MAPPER"},
 {0x124320,     32,0x00000000,"MAPPER"},
 {0x300000, 131072,0x00000000,"FFU(BST_ACTION)"},
 {0x308000,  65536,0x00000000,"FFU(BST_KEY)"},
 {0x30c000,    256,0x00000000,"FFU(BST_SCEN_CAM)"},
 {0x30c040,    128,0x00000000,"FFU(BST_SCEN_CFG1)"},
 {0x30c060,    128,0x00000000,"FFU(BST_SCEN_CFG2)"},
 {0x30c080,    128,0x00000000,"FFU(BST_ROOT_KEYS)"},
 {0x381000,  49152,0x00000000,"FFU(SLICE_ACTION)"},
 {0x380000,  98304,0x00000000,"FFU(SLICE_CAM)"},
 {0x381800,   1536,0x00000000,"FFU(SLICE_SCN_CAM)"},
 {0x381840,   1536,0x00000000,"FFU(SLICE_SCN_CFG)"},
 {0x3f8000,     64,0x00000000,"FFU(REMAP_SCN_CAM)"},
 {0x3fc400,   1024,0x00000000,"FFU(HASH_L3_PTBL)"},
 {0x160000, 131072,0x00000000,"NEXTHOP"},
 {0x00b400,   1024,0x00000000,"HASH"},
 {0x00b800,   1024,0x00000000,"HASH"},
 {0x280000, 262144,0x00000000,"L2L_MAC"},
 {0x032000,   8192,0x00000000,"L2L"},
 {0x034000,   8192,0x00000000,"L2L"},
 {0x036000,   4096,0x00000000,"L2L"},
 {0x037000,   4096,0x00000000,"L2L"},
 {0x00d200,    512,0x00000000,"L2L_SWEEPER"},
 {0x180000,  98304,0x00000000,"L2F"},
 {0x1a0000,   3072,0x00000000,"L2F"},
 {0x00e800,   2048,0x00000000,"GLORT"},
 {0x130000,  16384,0x00000000,"POLICERS"},
 {0x134000,   1024,0x00000000,"POLICERS"},
 {0x138000,  16384,0x00000000,"POLICERS"},
 {0x13c000,   1024,0x00000000,"POLICERS"},
 {0x146200,    128,0x00000000,"L2AR(elided)"},
 {0x146300,    128,0x00000000,"L2AR(elided)"},
 {0x146400,    128,0x00000000,"L2AR(elided)"},
 {0x146500,    128,0x00000000,"L2AR(elided)"},
 {0x146600,    128,0x00000000,"L2AR(elided)"},
 {0x146700,    128,0x00000000,"L2AR(elided)"},
 {0x146800,    128,0x00000000,"L2AR(elided)"},
 {0x146900,    128,0x00000000,"L2AR(elided)"},
 {0x146a00,    128,0x00000000,"L2AR(elided)"},
 {0x146b00,    128,0x00000000,"L2AR(elided)"},
 {0x146c00,    128,0x00000000,"L2AR(elided)"},
 {0x146d00,    128,0x00000000,"L2AR(elided)"},
 {0x146e00,    128,0x00000000,"L2AR(elided)"},
 {0x146f00,    128,0x00000000,"L2AR(elided)"},
 {0x147000,    128,0x00000000,"L2AR(elided)"},
 {0x147100,    128,0x00000000,"L2AR(elided)"},
 {0x147200,    128,0x00000000,"L2AR(elided)"},
 {0x147300,    128,0x00000000,"L2AR(elided)"},
 {0x118800,    960,0x00000000,"CM"},
 {0x112800,    912,0xffffffff,"CM"},
 {0x113000,   1216,0xffffffff,"CM"},
 {0x115000,    912,0xffffffff,"CM"},
 {0x115800,    912,0xffffffff,"CM"},
 {0x117000,    912,0x20001000,"CM"},
 {0x020800,    960,0x00000000,"CMM"},
 {0x021000,    960,0x00000000,"CMM"},
 {0x116600,    304,0x00000000,"CM"},
 {0x117800,     76,0x00000000,"CM"},
 {0x116100,    152,0x00000000,"CM"},
 {0x116200,     76,0x00000000,"CM"},
 {0x114000,   1280,0x00003fff,"CM"},
 {0x240000,  12288,0x00000000,"MCAST_MID"},
 {0x260000,  32768,0x00000000,"MCAST_POST"},
 {0x003000,    912,0x00000000,"MONITOR"},
 {0x003c00,   1024,0x00000000,"MONITOR"},
 {0x150000,  12288,0x00000000,"MOD"},
 {0x154000,  12288,0x00000000,"MOD"},
 {0x15a000,   4096,0x00000000,"MOD(elided)"},
 {0x15b000,   4096,0x00000000,"MOD(elided)"},
 {0x15c000,   4096,0x00000000,"MOD(elided)"},
 {0x15d000,   4096,0x00000000,"MOD(elided)"},
 {0x15e000,   4096,0x00000000,"MOD(elided)"},
 {0x01e000,   4096,0x00000000,"MGMT2/CRM_DATA"},
 {0x118800,    960,0x00000000,"CM"},
 {0x200000,  65536,0xffffffff,"STATS_BANK"},
 {0x100000,  14336,0x00000000,"PARSER"},
 {0x00d080,    128,0x00000000,"L2L_SWEEPER"},
 {0x006000,    192,0x00000000,"EACL"},
 {0x006100,    152,0x00000000,"EACL"},
 {0x014000,     76,0x00000000,"LBS"},
 {0x144000,   4096,0x00000000,"L2AR"},
 {0x010000,   2560,0x00000000,"L3AR"},
 {0x00e000,   1024,0x00000000,"GLORT"},
 {0x018000,   1536,0x00000000,"STATS_AR"},
 {0x018a00,    512,0x00000000,"STATS_AR"},
 {0x018c00,    360,0x00000000,"STATS_AR"},
 {0x158000,   4096,0x00000000,"MOD"},
 {0x030800,   2048,0x00000000,"L2L"},
};
#define NF (sizeof(F)/sizeof(F[0]))
int main(int argc,char**argv){
 const char *bdf = argc>1?argv[1]:"0000:02:00.0";
 unsigned pace = argc>2?(unsigned)strtoul(argv[2],0,0):0;   /* us per 4k words */
 char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",bdf);
 int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
 M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
 if(M==MAP_FAILED){perror("mmap");return 1;}
 setvbuf(stdout,NULL,_IONBF,0);
 if(rd(0x1c021)!=0x208u){printf("chip not alive (PIN=0x%08x)\n",rd(0x1c021));return 1;}
 printf("fm6000_memfill: %u fills, PIN=0x%08x\n",(unsigned)NF,rd(0x1c021));
 unsigned long total=0;
 for(unsigned i=0;i<NF;i++){
   for(int w=0;w<F[i].words;w++){
     M[F[i].base+w]=F[i].val;
     if((w&0xfff)==0xfff){ __sync_synchronize(); if(pace) usleep(pace);
       if(rd(0x1c021)!=0x208u){printf("\n OFF-BUS in fill %u (%s @0x%06x) at +%d\n",i,F[i].blk,F[i].base,w);return 2;} }
   }
   __sync_synchronize(); total+=F[i].words;
   if(rd(0x1c021)!=0x208u){printf("\n OFF-BUS after fill %u (%s @0x%06x)\n",i,F[i].blk,F[i].base);return 2;}
   if((i%10)==0||i==NF-1) printf("  [%3u/%u] %-18s @0x%06x x%-6d  total=%lu PIN=ok\n",
                                 i,(unsigned)NF,F[i].blk,F[i].base,F[i].words,total);
 }
 printf("ALL FILLS DONE: %lu words. PIN=0x%08x\n",total,rd(0x1c021));
 printf("verify: PRIVATE_WM[p0]=0x%08x HOG_WM[p0]=0x%08x MCAST_MID=0x%08x\n",
        rd(0x112800),rd(0x113000),rd(0x240000));
 return 0;}
