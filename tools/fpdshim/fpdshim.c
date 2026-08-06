typedef unsigned int   u32;
typedef unsigned long long u64;
typedef unsigned int   size_t32;

/* --- libc, declared by hand (no headers on this build host) --- */
extern int   open(const char *, int, ...);
extern long  write(int, const void *, unsigned long);
extern void *mmap(void *, unsigned long, int, int, int, long);
extern int   printf(const char *, ...);
extern void  exit(int);

#define O_RDWR   02
#define O_SYNC   04010000
#define PROT_RW  3
#define MAP_SHARED 1
/* Standard i386 crt1 entry: libc must be initialised via __libc_start_main or stdio/locale
 * are unusable (a naive _start segfaults). Mirrors glibc's sysdeps/i386/start.S. */
extern int fpd_main(void);
__asm__(
".globl _start\n"
"_start:\n"
"  xor %ebp, %ebp\n"
"  pop %esi\n"                 /* argc */
"  mov %esp, %ecx\n"           /* argv */
"  and $0xfffffff0, %esp\n"
"  push %eax\n"                /* rtld_fini */
"  push %esp\n"                /* stack_end */
"  push %edx\n"
"  push $0\n"                  /* fini */
"  push $0\n"                  /* init */
"  push %ecx\n"                /* argv */
"  push %esi\n"                /* argc */
"  push $fpd_main\n"           /* main */
"  call __libc_start_main\n"
"  hlt\n"
);
extern void *dlopen(const char *, int);
extern char *dlerror(void);
extern void *dlsym(void *, const char *);
#define RTLD_LAZY 1

static volatile u32 *BAR;
static u32 hits_r, hits_w;

/* the 8 hardware-access callbacks the SDK calls; word addr -> BAR0 + (word<<2) */
static int hw_read  (int s,u32 a,u32 *v){ *v=BAR[a]; hits_r++; return 0; }
static int hw_write (int s,u32 a,u32 v){ BAR[a]=v; hits_w++; return 0; }
static int hw_readm (int s,u32 a,u32 n,u32 *v){ u32 i; for(i=0;i<n;i++) v[i]=BAR[a+i]; hits_r+=n; return 0; }
static int hw_writem(int s,u32 a,u32 n,const u32*v){ u32 i; for(i=0;i<n;i++) BAR[a+i]=v[i]; hits_w+=n; return 0; }
static int hw_read64 (int s,u32 a,u64 *v){ u32 lo=BAR[a],hi=BAR[a+1]; *v=((u64)hi<<32)|lo; hits_r+=2; return 0; }
static int hw_write64(int s,u32 a,u64 v){ BAR[a]=(u32)v; BAR[a+1]=(u32)(v>>32); hits_w+=2; return 0; }
static int hw_readm64 (int s,u32 a,u32 n,u64 *v){ u32 i; for(i=0;i<n;i++) hw_read64(s,a+2*i,&v[i]); return 0; }
static int hw_writem64(int s,u32 a,u32 n,const u64*v){ u32 i; for(i=0;i<n;i++) hw_write64(s,a+2*i,v[i]); return 0; }

static void mark(const char *m){ const char *p=m; unsigned long n=0; while(p[n]) n++; write(1,m,n); }

int fpd_main(void)
{
    mark("M1 enter\n");
    int fd = open("/sys/bus/pci/devices/0000:02:00.0/resource0", O_RDWR|O_SYNC);
    if (fd < 0) { printf("open resource0 FAILED\n"); return 1; }
    BAR = (volatile u32 *)mmap(0, 32u*1024*1024, PROT_RW, MAP_SHARED, fd, 0);
    if ((long)BAR == -1) { printf("mmap FAILED\n"); return 1; }
    printf("BAR0 mapped. PIN_STRAP=0x%08x (0x208=alive)\n", BAR[0x1c021]);

    mark("M2 mapped\n");
    void *h = dlopen("/tmp/lib32/libFocalpointSDK.so", RTLD_LAZY);
    if (!h) { printf("dlopen FAILED: %s\n", dlerror()); return 1; }

    void (*hwinit)(void*,void*,void*,void*,void*,void*,void*,void*,void*,void*,void*,void*)
        = dlsym(h, "fmPlatformHwAccessInitialize");
    int (*readcsr)(int,u32,u32*) = dlsym(h, "fmPlatformReadCSR");
    int (*writecsr)(int,u32,u32) = dlsym(h, "fmPlatformWriteCSR");
    mark("M3 dlopen ok\n");
    int (*osinit)(void)   = dlsym(h, "fmOSInitialize");
    int (*platinit)(int *) = dlsym(h, "fmPlatformInitialize");  /* out-param: numSwitches */
    int (*apiinit)(void)  = dlsym(h, "fmInitialize");
    printf("SDK loaded: hwinit=%p readcsr=%p osinit=%p platinit=%p apiinit=%p\n",
           hwinit, readcsr, osinit, platinit, apiinit);
    if (!hwinit || !readcsr) { printf("missing symbols\n"); return 1; }

    /* the agent passes 8 fn ptrs + 4 NULLs (libFocalPointV2Agent.so @0x7e8b71) */
    mark("M4 calling hwinit\n");
    hwinit(hw_read, hw_write, hw_readm, hw_writem,
           hw_read64, hw_write64, hw_readm64, hw_writem64, 0, 0, 0, 0);
    mark("M5 hwinit returned\n");
    printf("callbacks registered.\n");

    int r;
    if (osinit)  { mark("I1 fmOSInitialize\n");   r = osinit();   printf("  fmOSInitialize   rc=%d\n", r); }
    /* fmPlatformInitialize reads the switch count from *(int*)fmPlatformConfig (6708-byte .bss
     * object, zero-filled at load -> 0 switches -> no per-switch lock -> accessor faults).
     * fmPlatformConfigure() is what normally fills it; set the count directly as a probe. */
    u32 *pcfg = dlsym(h, "fmPlatformConfig");
    printf("fmPlatformConfig=%p  numSwitches(before)=%d\n", pcfg, pcfg ? (int)pcfg[0] : -1);
    if (pcfg) { pcfg[0] = 1; printf("  forced numSwitches=1\n"); }

    int nsw = -1;
    if (platinit){ mark("I2 fmPlatformInitialize\n"); r = platinit(&nsw);
                   printf("  fmPlatformInitialize rc=%d numSwitches=%d\n", r, nsw);
                   { char b[64]; b[0]='I'; b[1]='2'; b[2]='='; b[3]='0'+(r<0?0:(r%10));
                     b[4]=' '; b[5]='n'; b[6]='='; b[7]='0'+(nsw<0?0:(nsw%10)); b[8]=10; b[9]=0;
                     write(1,b,9); } }
    if (apiinit) { mark("I3 fmInitialize\n");     r = apiinit();  printf("  fmInitialize     rc=%d\n", r); }
    mark("I4 init chain done\n");

    u32 v = 0xdeadbeef;
    mark("M6 calling readcsr\n");
    int rc = readcsr(0, 0x1c021, &v);
    printf("fmPlatformReadCSR(0x1c021) rc=%d val=0x%08x  [cb r=%u w=%u]\n", rc, v, hits_r, hits_w);
    if (hits_r && v == 0x208)
        printf("*** SDK IS DRIVING THE CHIP THROUGH OUR SHIM ***\n");
    else
        printf("(callbacks not used -- SDK uses a different accessor path)\n");
    return 0;
}
