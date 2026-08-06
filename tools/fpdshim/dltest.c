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
#define RTLD_NOW 2
#define RTLD_LAZY 1
int fpd_main(void){
  printf("dlopen(libFocalpointSDK.so, RTLD_LAZY)...\n");
  void *h = dlopen("/tmp/lib32/libFocalpointSDK.so", RTLD_LAZY);
  if(!h){ printf("  FAILED: %s\n", dlerror()); return 1; }
  printf("  loaded, handle=%p\n", h);
  void *f1 = dlsym(h,"fmPlatformHwAccessInitialize");
  void *f2 = dlsym(h,"fm6000BootSwitch");
  void *f3 = dlsym(h,"fmPlatformReadCSR");
  printf("  fmPlatformHwAccessInitialize=%p\n  fm6000BootSwitch=%p\n  fmPlatformReadCSR=%p\n",f1,f2,f3);
  return 0;
}
