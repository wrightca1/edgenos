/* fm6000_wrrdn.c - atomic N-word write THEN read-back in one mmap. N = entry width (MSW at word N-1 commits).
 *   fm6000_wrrdn <BDF> <base_word> <nwords> [w0..w_{n-1}]   (omit values to only read) */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
int main(int c,char**v){
	if(c<4){fprintf(stderr,"usage: %s <BDF> <base> <nwords> [w..]\n",v[0]);return 2;}
	char p[256];snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",v[1]);
	int fd=open(p,O_RDWR|O_SYNC);if(fd<0){perror("open");return 1;}
	size_t len=32u*1024*1024;volatile uint32_t*M=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
	if(M==MAP_FAILED){perror("mmap");return 1;}
	uint32_t base=strtoul(v[2],0,0); int n=atoi(v[3]); if(n<1||n>8)n=4;
	fprintf(stderr,"[wrrdn] PIN=0x%08x base=0x%05x n=%d\n",M[0x1C021],base,n);
	if(c>=4+n){ for(int i=0;i<n;i++){M[base+i]=strtoul(v[4+i],0,0);__sync_synchronize();}
		fprintf(stderr,"[wrrdn] wrote (LSW..MSW commits @word %d)\n",n-1);}
	uint32_t r[8]; for(int i=0;i<n;i++){r[i]=M[base+i];__sync_synchronize();}
	fprintf(stderr,"[wrrdn] read 0x%05x:",base); for(int i=0;i<n;i++)fprintf(stderr," %08x",r[i]);
	uint32_t pin=M[0x1C021];__sync_synchronize(); fprintf(stderr,"  PIN_after=0x%08x\n",pin);
	munmap((void*)M,len);close(fd); return pin==0xffffffff?1:0;
}
