/* fm6000_wrrd128.c - atomic 128-bit write THEN read-back in ONE mmap session (no interleaved re-mmap).
 * Tests whether FM6000 wide-table entries round-trip: per-word fm6000reg reads disturb the temp-cache and
 * off-bus; this reads all 4 words back-to-back from the same mapping. Also a pure-read mode (no args) reads.
 *   fm6000_wrrd128 <BDF> <base_word> [w0 w1 w2 w3]   (omit w* to only read)
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
int main(int argc, char **argv){
	if (argc < 3){ fprintf(stderr,"usage: %s <BDF> <base_word_hex> [w0 w1 w2 w3]\n",argv[0]); return 2; }
	char p[256]; snprintf(p,sizeof p,"/sys/bus/pci/devices/%s/resource0",argv[1]);
	int fd=open(p,O_RDWR|O_SYNC); if(fd<0){perror("open");return 1;}
	size_t len=32u*1024*1024; volatile uint32_t*M=mmap(NULL,len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
	if(M==MAP_FAILED){perror("mmap");return 1;}
	uint32_t base=(uint32_t)strtoul(argv[2],NULL,0);
	uint32_t pin=M[0x1C021]; __sync_synchronize();
	fprintf(stderr,"[wrrd] PIN_STRAP=0x%08x\n",pin);
	if(argc>=7){
		uint32_t w[4]={strtoul(argv[3],0,0),strtoul(argv[4],0,0),strtoul(argv[5],0,0),strtoul(argv[6],0,0)};
		for(int i=0;i<4;i++){ M[base+i]=w[i]; __sync_synchronize(); }   /* LSW->MSW, MSW commits */
		fprintf(stderr,"[wrrd] wrote 0x%05x = %08x %08x %08x %08x\n",base,w[0],w[1],w[2],w[3]);
	}
	/* atomic read-back: 4 words back-to-back, no other access between */
	uint32_t r[4]; for(int i=0;i<4;i++){ r[i]=M[base+i]; __sync_synchronize(); }
	uint32_t pin2=M[0x1C021]; __sync_synchronize();
	fprintf(stderr,"[wrrd] read  0x%05x = %08x %08x %08x %08x  PIN_after=0x%08x\n",base,r[0],r[1],r[2],r[3],pin2);
	munmap((void*)M,len); close(fd);
	return pin2==0xffffffff?1:0;
}
