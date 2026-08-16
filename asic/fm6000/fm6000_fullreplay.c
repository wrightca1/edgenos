/* fm6000_fullreplay.c - replay EOS's COMPLETE cold-boot write stream (minus microcode ranges and
 * MGMT1/2 clock/reset regs) from a file of "aaaaaaaa vvvvvvvv" lines, in boot order.
 *
 * Source: notes/reference/scd-dumps/fm6000-COMPLETE-cold-boot-trace.txt.gz (394,647 writes). The
 * previously-used trace was truncated to 38% and contained ZERO EPL writes.
 *
 * JSS/SBus (0xF001/0xF002) must NOT be replayed as blind MMIO: 0xF001 with the Execute bit starts a
 * transaction whose Busy bit must clear before the next one, or transactions are lost. We detect the
 * F002-then-F001 pattern and run a proper transaction with a Busy poll.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#define PIN 0x1C021u
static volatile uint32_t *M;
static uint32_t rd(uint32_t w){uint32_t v=M[w];__sync_synchronize();return v;}
static void     wr(uint32_t w,uint32_t v){M[w]=v;__sync_synchronize();}
static long sbus_to=0, sbus_n=0;
static void sbus(uint32_t cmd,uint32_t data){
  wr(0xF002,data); wr(0xF001,0); wr(0xF001,cmd);
  for(long i=0;i<200000;i++){ uint32_t s=rd(0xF001);
    if(s==0xffffffffu) return;
    if(!(s&(1u<<25))) { sbus_n++; return; } }
  sbus_to++;
}
/* --- SPICO IMEM injection -------------------------------------------------
 * The SerDes SPICO firmware is third-party and is NOT distributed with the
 * replay set. Where it was stripped out, the replay carries a marker line:
 *
 *     @SPICO_IMEM
 *
 * On reaching it we upload the operator-supplied firmware, at exactly the point
 * in the boot order where EOS did. This MATTERS: the replay later resets and
 * starts the SPICO, so a firmware loaded earlier (e.g. by running fm6000_spico
 * before the replay) is wiped and the SPICO runs with an empty IMEM. That is
 * what silently broke the 10GBASE-CR (DAC) link on Et2 while leaving the
 * 10GBASE-SR link on Et1 working.
 *
 * Word format: 10 bits. reg 0x07 = data[7:0]; reg 0x06 = data[9:8] with bit3 =
 * IMEM write enable and bit2 = strobe. See docs/SPICO-RE.md.
 */
static const char *spico_path = NULL;

static void spico_sw(uint32_t reg, uint32_t val)   /* one SBus write to the SPICO */
{
    sbus((reg & 0xFFFFu) | (0x21u << 16) | (1u << 24), val);
}

static long spico_upload(void)
{
    FILE *bf;
    uint16_t *code;
    long bytes, nwords, i;

    if (!spico_path) {
        printf("  @SPICO_IMEM marker reached but no firmware given "
               "(-s <file>) -- SKIPPING. Copper/CR ports will not link.\n");
        return 0;
    }
    bf = fopen(spico_path, "rb");
    if (!bf) { perror("  open spico firmware"); return -1; }
    fseek(bf, 0, SEEK_END); bytes = ftell(bf); fseek(bf, 0, SEEK_SET);
    nwords = bytes / 2;
    code = malloc((size_t)nwords * 2);
    if (!code || fread(code, 2, (size_t)nwords, bf) != (size_t)nwords) {
        fprintf(stderr, "  spico firmware read failed\n"); fclose(bf); free(code); return -1;
    }
    fclose(bf);

    printf("  @SPICO_IMEM: uploading %ld words from %s\n", nwords, spico_path);
    /* The per-word writes are bracketed by an IMEM-write-enable and a matching
     * disable. Both are reg 0x06, so the strip that removed the upload removed
     * these too -- omitting them leaves write-enable asserted when the SPICO is
     * told to run, and it never executes. */
    spico_sw(0xFD06, 0x8);                          /* IMEM write enable */
    for (i = 0; i < nwords; i++) {
        uint32_t w = code[i];
        if (w > 0x3FFu) {
            fprintf(stderr, "  word %ld = 0x%x exceeds 10 bits -- wrong image?\n", i, w);
            free(code); return -1;
        }
        spico_sw(0xFD04, (uint32_t)((i >> 8) & 0xFF));
        spico_sw(0xFD05, (uint32_t)(i & 0xFF));
        spico_sw(0xFD07, w & 0xFF);
        spico_sw(0xFD06, ((w >> 8) & 0x3) | 0xC);   /* data[9:8] + we + strobe */
        spico_sw(0xFD06, ((w >> 8) & 0x3) | 0x8);   /* strobe released         */
    }
    spico_sw(0xFD06, 0x0);                          /* IMEM write enable off */
    free(code);
    printf("  @SPICO_IMEM: done (bracketed), PIN=0x%08x\n", rd(PIN));
    return nwords;
}

int main(int argc,char**argv){
  if(argc<2){fprintf(stderr,
      "usage: %s <file> [bdf] [pace_us_per_4k] [-s <spico-firmware>]\n"
      "  -s  firmware to upload at the @SPICO_IMEM marker (required for CR/DAC links)\n",
      argv[0]);return 2;}
  for(int i=1;i<argc;i++)
    if(!strcmp(argv[i],"-s") && i+1<argc){ spico_path=argv[i+1]; argv[i]=argv[i+1]=(char*)""; }
  const char *bdf = (argc>2 && argv[2][0])?argv[2]:"0000:02:00.0";
  unsigned pace = (argc>3 && argv[3][0])?(unsigned)strtoul(argv[3],0,0):0;
  char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",bdf);
  int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
  M=mmap(NULL,32u*1024*1024,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if(M==MAP_FAILED){perror("mmap");return 1;}
  FILE *f=fopen(argv[1],"r"); if(!f){perror("open trace");return 1;}
  setvbuf(stdout,NULL,_IONBF,0);
  if(rd(PIN)!=0x208u){printf("chip not alive (PIN=0x%08x)\n",rd(PIN));return 1;}
  printf("fullreplay start PIN=0x%08x\n",rd(PIN));
  char line[64]; unsigned long n=0,mmio=0; uint32_t pend=0; int aborted=0;
  while(fgets(line,sizeof line,f)){
    uint32_t a,v;
    if(line[0]=='@'){                       /* injection marker */
      if(!strncmp(line,"@SPICO_IMEM",11)){
        if(spico_upload()<0){ printf("  spico upload failed\n"); aborted=1; break; }
      }
      continue;
    }
    if(sscanf(line,"%x %x",&a,&v)!=2) continue;
    n++;
    if(a==0xF002u){ pend=v; continue; }
    if(a==0xF001u){ if(v==0) continue; sbus(v,pend); continue; }
    wr(a,v); mmio++;
    if((n & 0x3fff)==0){
      if(pace) usleep(pace);
      if(rd(PIN)!=0x208u){ printf("\n OFF-BUS at line %lu (0x%05x <- 0x%08x)\n",n,a,v); aborted=1; break; }
      /* Sample Et2 as the replay runs. Et2 links on only ~half of identical
       * boots, and its outcome is already fixed by the time STEP5 finishes
       * (docs/PORT3-BRINGUP.md) -- so the transition happens somewhere INSIDE
       * these 300k writes and nothing downstream can see where. One extra MMIO
       * read per 16k ops gives ~18 samples across the replay, which is enough
       * to bisect a good boot's dark->up transition to a 16k-op window.
       *   et2 0x0815 = dark, 0x08c0/0x0cc0 = linked. */
      printf("  %lu ops (mmio=%lu sbus=%ld to=%ld) PIN=ok et2=0x%08x/%08x\n",
             n,mmio,sbus_n,sbus_to,rd(0xe4000),rd(0xe4038));
    }
  }
  fclose(f);
  printf("%s: %lu ops, mmio=%lu sbus=%ld timeouts=%ld, PIN=0x%08x\n",
         aborted?"ABORTED":"DONE",n,mmio,sbus_n,sbus_to,rd(PIN));
  printf("PORT_STATUS=0x%08x pcsRx=0x%08x sched=0x%08x MCAST=0x%08x\n",
         rd(0xe3800),rd(0xe3826),rd(0x8062),rd(0x240000));
  return aborted?2:0;}
