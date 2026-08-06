/* fm6000_sched_replay.c — FAITHFUL replay of EOS's LIVE FM6000 SSCHED scheduler-start sequence.
 *
 * Captured live via fmPlatformTraceRegOps on EOS 4.16.8M (fm6000-sched-start-LIVE-trace.txt).
 * This is the GROUND TRUTH the earlier earlier from-scratch attempts were missing. Key differences from our
 * prior fm6000_sched_std:
 *   1. TOKEN FIFO ARMING: write 0x8062<-0 and 0x8022<-0 BEFORE streaming (loader is an
 *      auto-incrementing FIFO; must reset its index). We never did this.
 *   2. 64 TOKENS (the real interleaved port-visit ring), not 5.
 *   3. SOFT_RESET (0x09) stays 0x16 during bring-up, cleared to 0 only AFTER the engine is running.
 *      (Prior attempts set it to 0 first, which off-bused/failed.)
 *   4. 0x1c022 PLL/clock-lock handshake polled to 0x313 before any 0x80xx write.
 *   5. Full 7-value 0x1c03a clock sequence; second-pass 0x1c048-4c FMODE cfg.
 *
 * usage: fm6000_sched_replay <BDF>
 * Build: gcc -O2 -o fm6000_sched_replay fm6000_sched_replay.c
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

static volatile uint32_t *M;
static inline void wr(uint32_t w, uint32_t v){ M[w]=v; __sync_synchronize(); }
static inline uint32_t rd(uint32_t w){ uint32_t v=M[w]; __sync_synchronize(); return v; }
#define PIN 0x1C021
static int live(const char*t){ uint32_t p=rd(PIN); fprintf(stderr,"[rpl] %s PIN=0x%08x\n",t,p); return p!=0xffffffffu; }

/* the exact 64-token port-visit ring captured live (streamed to 0x8060 then 0x8020, in order) */
static const uint32_t TOK[64] = {
  0x14,0x18,0x1c,0x200, 0x15,0x19,0x1d,0x201, 0x16,0x1a,0x1e,0x203, 0x17,0x1b,0x1f,0x20,
  0x24,0x28,0x2c,0x21,  0x25,0x29,0x2d,0x22,  0x26,0x2a,0x2e,0x23,  0x27,0x2b,0x2f,0x40,
  0x34,0x38,0x3c,0x41,  0x35,0x39,0x3d,0x42,  0x36,0x3a,0x3e,0x43,  0x37,0x3b,0x3f,0x202,
  0x44,0x48,0x0c,0x10,  0x45,0x49,0x0d,0x11,  0x46,0x4a,0x0e,0x12,  0x47,0x4b,0x0f,0x13 };

int main(int argc,char**argv){
  if(argc<2){ fprintf(stderr,"usage: %s <BDF>\n",argv[0]); return 2; }
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  if(!live("start")) return 1;

  /* --- A. reach BOOT_CTRL 0x313 the PROVEN COLD way: clocks ON + SOFT_RESET=0 + cmd1/2/3.
   * (The live warm trace does cmd1/cmd3 with clocks off @0x80000040 — works warm because freelists
   *  persist, but cold cmd3/freelist-init needs clocks ON. Assumes fm6000_initsbus already ran.) --- */
  wr(0x1c01e,0xfffc0000); wr(0x1c01f,0x0009502f);        /* scan-cfg PLL */
  wr(0x1c03a,0xffffffff); wr(0x1c03b,0xffffffff);        /* all block clocks ON */
  wr(0x00009,0x0);                                        /* SOFT_RESET=0: all modules out of reset */
  { int c; for(c=1;c<=3;c++){ wr(0x1c022,c); int i; for(i=0;i<200;i++){ if(rd(0x1c022)&0x10) break; usleep(1000);} } }
  { uint32_t v=rd(0x1c022); fprintf(stderr,"[rpl] BOOT_CTRL=0x%08x %s\n", v, (v&0xfff)==0x313?"(0x313 OK)":"(not 0x313!)"); }
  wr(0xF010,0x2);                                         /* SSCHED_TICK_CFG (pre-init sets it; add here) */
  if(!live("after clocks+bootcmds+tick")) return 1;

  /* --- A1b. PORT/EPL config sweep — captured verbatim from EOS live trace (lines 27-130): the
   * per-port MAC/EPL config the scheduler circulates through, then a HW port-init completion poll on
   * 0x1d08e draining to 0. EVERY prior from-scratch scheduler attempt SKIPPED this — phase92's splice
   * reached 0x313 then jumped straight to tokens with the ports unconfigured, so nothing serviceable
   * to circulate. This is the missing pre-scheduler state. --- */
  if(getenv("FM6000_PORTCFG")){
    static const uint32_t PC[][2] = {
      {0x1d080,0x6529eda9},{0x1d081,0x9b8ed9b1},{0x1d082,0xefca952b},{0x1d083,0x000fca99},
      {0x1d708,0x6529eda9},{0x1d709,0x9b8ed9b1},{0x1d70a,0xefca952b},{0x1d70b,0x000fca99},
      {0x1d210,0x00200000},{0x1d290,0x00200000},{0x1d310,0x00200000},{0x1d390,0x00200000},
      {0x1d400,0x00200000},{0x1d480,0x00200000},{0x1d500,0x00200000},{0x1d580,0x00200000},
      {0x1d600,0x00200000},{0x1d218,0x000000b4},{0x1d298,0x000000b4},{0x1d318,0x000000b4},
      {0x1d398,0x000000b4},{0x1d241,0x00000004},{0x1d2c1,0x00000004},{0x1d261,0x00000004},
      {0x1d281,0x00000004},{0x1d2a1,0x00000004},{0x1d404,0x0000000c},{0x1d484,0x0000000c},
      {0x1d504,0x0000000c},{0x1d584,0x0000000c},{0x1d604,0x00000004},{0x1d440,0x00000001},
      {0x1d4c0,0x00000001},{0x1d4e0,0x00000001},{0x1d540,0x00000001},{0x1d5c0,0x00000001},
      {0x1d5e0,0x00000001},{0x1d640,0x00000001},{0x1d660,0x00000001},{0x1d409,0x00000fff},
      {0x1d489,0x00007fff},{0x1d509,0x00003fff},{0x1d589,0x00000fff},{0x1d609,0x000003ff},
      {0x1d441,0x00000004},{0x1d4c1,0x00000004},{0x1d4e1,0x00000004},{0x1d541,0x00000004},
      {0x1d5c1,0x00000006},{0x1d5e1,0x00000006},{0x1d641,0x0000000a},{0x1d661,0x0000000a},
      {0x1d220,0x00000003},{0x1d2a0,0x00000003},{0x1d320,0x00000003},{0x1d3a0,0x00000003},
      {0x1d40b,0x00000000},{0x1d48b,0x00000002},{0x1d50b,0x00000002},{0x1d58b,0x00000002},
      {0x1d60b,0x00000000},
    };
    unsigned i; for(i=0;i<sizeof(PC)/sizeof(PC[0]);i++) wr(PC[i][0],PC[i][1]);
    { int k; uint32_t v=0xffffffff; for(k=0;k<3000;k++){ v=rd(0x1d08e); if(v==0) break; usleep(1000);}
      fprintf(stderr,"[rpl] PORTCFG: %u writes; 0x1d08e drained to 0x%08x after %d polls %s\n",
              (unsigned)(sizeof(PC)/sizeof(PC[0])), v, k, v==0?"(DONE)":"(NOT drained!)"); }
    if(!live("after PORTCFG")) return 1;
  }

  /* --- A2. FC_BEM config (preboot order: BOOT_CTRL 0x313 -> FC_BEM -> scheduler -> CRM).
   * The CRM Memory-Set engine writes forwarding RAM *through* FC_BEM; it needs this configured and
   * needs BOOT_CTRL normal-mode first (just reached). Live EOS post-boot values. Enabled by env so
   * the plain scheduler replay is unchanged unless requested. --- */
  if(getenv("FM6000_FCBEM")){
    wr(0x28022,0x00000418); wr(0x28020,0x40100190);        /* FC_BEM top-level cfg */
    { uint32_t sr=rd(0x00009); wr(0x00009, sr & ~0x6u); }  /* SOFT_RESET RMW: clear MSB/FIBM */
    fprintf(stderr,"[rpl] FC_BEM: 0x28022=0x%08x 0x28020=0x%08x SOFT_RESET=0x%08x\n",
            rd(0x28022), rd(0x28020), rd(0x00009));
    if(!live("after FC_BEM")) return 1;
  }
  /* --- A5. ESCHED (egress scheduler) per-port config. Cold it reads 0xffffffff (uninitialized); the
   * golden warm state has ESCHED_CFG_1 (0x2000+i)/CFG_2 (0x2080+i)=0x00ffffff (port0 special), DRR
   * (0x3800+i) alternating. The scheduler-start trace does NOT write these (done in pre-init) so my
   * replay never did. A token targeting a port whose egress scheduler is uninitialized can't be
   * accepted -> no bit21/bit30. Initialize all 76 ports. --- */
  if(getenv("FM6000_ESCHED")){
    int i;
    for(i=0;i<76;i++){
      wr(0x2000+i, (i==0)?0x00fff800u:0x00ffffffu);       /* ESCHED_CFG_1 */
      wr(0x2080+i, (i==0)?0x00fff000u:0x00ffffffu);       /* ESCHED_CFG_2 */
      wr(0x2100+i, 0x0);                                   /* ESCHED_CFG_3 */
      wr(0x3800+i, (i&1)?0x14ffffffu:0x00ffffffu);         /* ESCHED_DRR_CFG alternating */
    }
    fprintf(stderr,"[rpl] ESCHED init 76 ports: 0x2000=0x%08x 0x2080=0x%08x 0x3800=0x%08x 0x3801=0x%08x\n",
            rd(0x2000),rd(0x2080),rd(0x3800),rd(0x3801));
    if(!live("after ESCHED")) return 1;
  }

  int minring = getenv("FM6000_MINRING")!=NULL;           /* minimal ring: mgmt port only, no EPL ports */

  /* --- A4. SOFT_RESET = 0x16 for the scheduler bring-up. The EOS live trace keeps 0x09=0x16 through
   * the ENTIRE token/INIT_COMPLETE sequence, clearing to 0 only AFTER the engine confirms running
   * (line 445). Cold cmd3 (reaching 0x313) needed 0x09=0, but circulation may need the cold-default
   * 0x16 (MSB/FIBM/EPL block-reset state the scheduler expects). Set it back here, post-0x313. --- */
  if(getenv("FM6000_SR16")){
    wr(0x00009,0x16);
    fprintf(stderr,"[rpl] SOFT_RESET set to 0x%08x for bring-up\n", rd(0x00009));
    if(!live("after SOFT_RESET=0x16")) return 1;
  }

  /* --- B. arm token FIFO (index reset) then stream tokens (RX 0x8060, TX 0x8020) --- */
  wr(0x8062,0x0); wr(0x8022,0x0);                         /* ARM the auto-incrementing loader */
  if(minring){
    /* minimal ring: 4 idle/sync tokens (port 0 locked + mgmt 78) — no unconfigured EPL ports */
    uint32_t mr[4]={0x200,0x200,0x200,0x24e};
    for(int i=0;i<4;i++){ wr(0x8060,mr[i]); wr(0x8020,mr[i]); }
    fprintf(stderr,"[rpl] MINRING (4 tokens: 3x port0-locked + mgmt78)\n");
  } else {
    for(int i=0;i<64;i++){ wr(0x8060,TOK[i]); wr(0x8020,TOK[i]); }
  }
  if(!live("after tokens")) return 1;

  /* --- C. NEXT_PORT: [0]=0x03020100, [19]=0x004e0000 (port 78), rest 0 (TX 0x8040/RX 0x8000) --- */
  for(int i=0;i<20;i++){ uint32_t v=(i==0)?0x03020100u:(i==19)?0x004e0000u:0; wr(0x8040+i,v); wr(0x8000+i,v); }
  /* --- D. SLOW_PORT --- */
  wr(0x8070,0xf); wr(0x8071,0); wr(0x8072,0); wr(0x8073,0); wr(0x8074,0);
  if(!live("after NEXT_PORT+SLOW")) return 1;

  /* --- E. INIT_COMPLETE strobes = GO (RX then TX) --- */
  wr(0x8061,0x1);
  if(!live("after RX_INIT_COMPLETE")) return 1;
  wr(0x8021,0x1);
  if(!live("after TX_INIT_COMPLETE")) return 1;

  /* --- F. second pass FMODE cfg, then the REAL running check = fm6000ValidateSchedulerToken primitive
   * (@0x3a7c9a decoded): for a port, write (port & 0x7f) to REPLACE_TOKEN 0x8062 (RX)/0x8022 (TX),
   * fmDelay(0xc350)=50us so the engine PROCESSES the token, read back, check bit21(RX)/bit30(TX)=accepted.
   * My prior check wrote 0 and read back with NO delay -> always 0. The 50us wait is essential. --- */
  wr(0x1c048,0); wr(0x1c049,0); wr(0x1c04a,0); wr(0x1c04b,0); wr(0x1c04c,0x8);
  uint32_t rx=0, tx=0;
  { int ports[3]={0,1,0x4e}, n; for(n=0;n<3;n++){ int p=ports[n]&0x7f;
      wr(0x8062,p); usleep(50000); uint32_t r=rd(0x8062);
      wr(0x8022,p); usleep(50000); uint32_t t=rd(0x8022);
      fprintf(stderr,"[rpl] validate port 0x%02x: 0x8062=0x%08x (bit21=%u) 0x8022=0x%08x (bit30=%u)\n",
              p, r,(r>>21)&1u, t,(t>>30)&1u);
      if(p==0){ rx=r; tx=t; } } }
  fprintf(stderr,"[rpl] RUNNING check (port0): 0x8062=0x%08x (want bit21; golden 0x00200200)  0x8022=0x%08x (want bit30; golden 0xc0300200)\n",rx,tx);

  /* --- G. clear SOFT_RESET to 0 only AFTER the engine is running --- */
  wr(0x00009,0x0);
  if(!live("after SOFT_RESET=0")) return 1;

  if(getenv("FM6000_DUMP")){
    /* dump the scheduler-relevant state for diff against golden warm dump */
    unsigned a;
    fprintf(stderr,"[dump] JSS 0xF000-0xF012:"); for(a=0xf000;a<=0xf012;a++) fprintf(stderr," %05x=%08x",a,rd(a)); fprintf(stderr,"\n");
    fprintf(stderr,"[dump] SSCHED cfg: 8000=%08x 8013=%08x 8022=%08x 8040=%08x 8053=%08x 8062=%08x\n",rd(0x8000),rd(0x8013),rd(0x8022),rd(0x8040),rd(0x8053),rd(0x8062));
    fprintf(stderr,"[dump] SSCHED slow 8070-8074:"); for(a=0x8070;a<=0x8074;a++) fprintf(stderr," %04x=%08x",a,rd(a)); fprintf(stderr,"\n");
    fprintf(stderr,"[dump] ESCHED 0x2000=%08x 2001=%08x 2080=%08x 2100=%08x; DRR 3800=%08x 3801=%08x\n",rd(0x2000),rd(0x2001),rd(0x2080),rd(0x2100),rd(0x3800),rd(0x3801));
    fprintf(stderr,"[dump] FMODE 1c048-1c04c:"); for(a=0x1c048;a<=0x1c04c;a++) fprintf(stderr," %05x=%08x",a,rd(a)); fprintf(stderr,"\n");
  }
  int running = ((rx>>21)&1u) && ((tx>>30)&1u);          /* RX bit21 + TX bit30 = token accepted */
  fprintf(stderr,"[rpl] === SCHEDULER %s ===\n", running?"RUNNING (rings match golden!)":"not confirmed running");
  munmap((void*)M,32u*1024*1024); close(fd);
  return running?0:2;
}
