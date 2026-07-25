/* scdmbload - load the SCD microblaze ("Saguaro") firmware + start it, so the
 * switch-side SCD accelerator domain (VRM/accel#0 + the FM6000 local bus) powers up.
 *   scdmbload <saguaro.srec>            # load + start
 *   scdmbload <saguaro.srec> --noload   # just start (0x9000 bit0=1)
 * SREC S2 (24-bit addr): addr<0x10000 -> imem BAR0+0x20000+addr ; else -> dmem
 * BAR0+0x10000+(addr-0x10000). Bytes written preserving SREC order (LE word assembly).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#define SCD_RES "/sys/bus/pci/devices/0000:04:00.0/resource0"
static int hx(char a,char b){int v;char s[3]={a,b,0};sscanf(s,"%x",&v);return v;}
int main(int argc,char**argv){
	if(argc<2){fprintf(stderr,"usage: scdmbload <saguaro.srec> [--noload]\n");return 2;}
	int doload = !(argc>2 && !strcmp(argv[2],"--noload"));
	int fd=open(SCD_RES,O_RDWR|O_SYNC); if(fd<0){perror("resource0");return 1;}
	size_t len=0x40000;
	volatile uint8_t*bar=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
	if(bar==MAP_FAILED){perror("mmap");return 1;}
	volatile uint32_t*rst=(volatile uint32_t*)(bar+0x9000);
	printf("0x9000 (mb reset) before = 0x%08x ; accel#0 cs(0x8020) before = 0x%08x\n",*rst,*(volatile uint32_t*)(bar+0x8020));
	long im=0,dm=0;
	if(doload){
		FILE*f=fopen(argv[1],"r"); if(!f){perror("srec");return 1;}
		char ln[1024];
		while(fgets(ln,sizeof ln,f)){
			if(ln[0]!='S'||ln[1]!='2')continue;
			int cnt=hx(ln[2],ln[3]);
			unsigned long addr=(hx(ln[4],ln[5])<<16)|(hx(ln[6],ln[7])<<8)|hx(ln[8],ln[9]);
			int nd=cnt-4; /* data bytes = count - 3 addr - 1 cksum */
			unsigned long off=(addr<0x10000)?(0x20000+addr):(0x10000+(addr-0x10000));
			int j; for(j=0;j+4<=nd;j+=4){
				const char*p=ln+10+j*2;
				uint32_t w=hx(p[0],p[1])|(hx(p[2],p[3])<<8)|(hx(p[4],p[5])<<16)|((uint32_t)hx(p[6],p[7])<<24);
				*(volatile uint32_t*)(bar+off+j)=w;
			}
			if(addr<0x10000)im+=nd; else dm+=nd;
		}
		fclose(f);
		__asm__ __volatile__("mfence":::"memory");
		printf("loaded imem=%ld dmem=%ld bytes\n",im,dm);
	}
	*rst=(*rst)|1; __asm__ __volatile__("mfence":::"memory"); (void)*rst;
	usleep(300000);
	printf("0x9000 after start = 0x%08x\n",*rst);
	printf("accel#0 cs(0x8020) after = 0x%08x  (nonzero => switch-side domain ALIVE)\n",*(volatile uint32_t*)(bar+0x8020));
	printf("accel#3 cs(0x8120) after = 0x%08x\n",*(volatile uint32_t*)(bar+0x8120));
	munmap((void*)bar,len);close(fd);return 0;
}
