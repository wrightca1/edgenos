/*
 * leddance -- drive the 7050SX2's front-panel port LEDs directly.
 *
 * Register map, from the board's own description
 * (PortolaSPlus-7050SX2-72Q.fdl.py) and confirmed by reading a running EOS:
 *
 *   0x6000                     LED flash rate      (observed 0x0000014d)
 *   0x6100 + 0x10*n            SFP+ port n+1       n = 0..47
 *   0x6400 + 0x10*(i*4 + j)    QSFP port 49+i lane j+1,  i = 0..5, j = 0..3
 *
 * The "on" value is bit 28. Every connected port on EOS read 0x10000000 and
 * every disconnected one read 0x00000000:
 *
 *   Et1  0x6100 = 0x10000000   connected     Et2  0x6110 = 0x00000000
 *   Et25 0x6280 = 0x10000000   connected
 *   Et48 0x63f0 = 0x10000000   connected
 *
 * The rest of the word is NOT decoded. Colour and blink bits, if any, are
 * unknown -- which is what `probe` is for: it walks single bits one at a time
 * and announces each, so a human watching the front panel can say what each
 * one does. Nothing here infers a colour it has not been told.
 *
 * RUN THIS ON EDGENOS, NOT EOS. Under EOS the LED agent owns these registers
 * and will overwrite anything written here within a second or so.
 *
 * Build:  gcc -O2 -static -o leddance leddance.c
 * Usage:  leddance sweep|bounce|qsfp|wave|probe [arg]
 *         leddance restore          put every LED back to off
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <stdint.h>
#include <sys/mman.h>

#define SFP_BASE   0x6100u
#define SFP_STRIDE 0x10u
#define SFP_PORTS  48
#define QSFP_BASE  0x6400u
#define QSFP_LANES 24                    /* 6 ports x 4 lanes */
#define FLASH_REG  0x6000u
#define LED_ON     0x10000000u
#define MAP_SIZE   0x80000u              /* the SCD BAR is 512 KB */

static volatile uint32_t *scd;
static volatile int stop;

static void on_sig(int s) { (void)s; stop = 1; }

static void wr(uint32_t off, uint32_t v) { scd[off / 4] = v; }
static uint32_t rd(uint32_t off)         { return scd[off / 4]; }

/* The SCD is 3475:0001. Its PCI address differs between kernels -- 05:00.0
 * under EOS, 04:00.0 under our own -- so find it rather than hardcode it. */
static int map_scd(void)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *e;
    char path[256];
    int fd, found = 0;

    if (!d) { perror("opendir"); return -1; }
    while ((e = readdir(d))) {
        FILE *f;
        unsigned vendor = 0;
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", e->d_name);
        if (!(f = fopen(path, "r"))) continue;
        if (fscanf(f, "%x", &vendor) != 1) vendor = 0;
        fclose(f);
        if (vendor != 0x3475) continue;
        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", e->d_name);
        fd = open(path, O_RDWR | O_SYNC);
        if (fd < 0) continue;
        scd = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (scd == MAP_FAILED) { scd = NULL; continue; }
        printf("scd: %s\n", e->d_name);
        found = 1;
        break;
    }
    closedir(d);
    if (!found) fprintf(stderr, "no Arista SCD (3475:*) found\n");
    return found ? 0 : -1;
}

static uint32_t sfp(int n)  { return SFP_BASE  + SFP_STRIDE * (uint32_t)n; }
static uint32_t qsfp(int k) { return QSFP_BASE + SFP_STRIDE * (uint32_t)k; }

static void all_off(void)
{
    int i;
    for (i = 0; i < SFP_PORTS;  i++) wr(sfp(i), 0);
    for (i = 0; i < QSFP_LANES; i++) wr(qsfp(i), 0);
}

static void nap(int ms) { usleep(ms * 1000); }

/* One lit port running the length of the panel and back. */
static void bounce(int rounds, int ms)
{
    int r, i;
    for (r = 0; r < rounds && !stop; r++) {
        for (i = 0; i < SFP_PORTS && !stop; i++) {
            all_off(); wr(sfp(i), LED_ON); nap(ms);
        }
        for (i = SFP_PORTS - 2; i > 0 && !stop; i--) {
            all_off(); wr(sfp(i), LED_ON); nap(ms);
        }
    }
}

/* Fill from both ends toward the middle, then drain. */
static void sweep(int rounds, int ms)
{
    int r, i;
    for (r = 0; r < rounds && !stop; r++) {
        all_off();
        for (i = 0; i < SFP_PORTS / 2 && !stop; i++) {
            wr(sfp(i), LED_ON);
            wr(sfp(SFP_PORTS - 1 - i), LED_ON);
            nap(ms);
        }
        for (i = SFP_PORTS / 2 - 1; i >= 0 && !stop; i--) {
            wr(sfp(i), 0);
            wr(sfp(SFP_PORTS - 1 - i), 0);
            nap(ms);
        }
    }
}

/* The QSFP cages, four lanes each -- lanes chase within a port, then the
 * whole port hands over to the next. */
static void qsfp_dance(int rounds, int ms)
{
    int r, p, l, k;
    for (r = 0; r < rounds && !stop; r++) {
        for (p = 0; p < 6 && !stop; p++) {              /* lane chase */
            for (l = 0; l < 4 && !stop; l++) {
                for (k = 0; k < QSFP_LANES; k++) wr(qsfp(k), 0);
                wr(qsfp(p * 4 + l), LED_ON);
                nap(ms);
            }
        }
        for (p = 0; p < 6 && !stop; p++) {              /* whole cages */
            for (k = 0; k < QSFP_LANES; k++) wr(qsfp(k), 0);
            for (l = 0; l < 4; l++) wr(qsfp(p * 4 + l), LED_ON);
            nap(ms * 2);
        }
        for (k = 0; k < QSFP_LANES && !stop; k++) {     /* fill all lanes */
            wr(qsfp(k), LED_ON); nap(ms / 2);
        }
        nap(ms * 3);
        for (k = QSFP_LANES - 1; k >= 0 && !stop; k--) {
            wr(qsfp(k), 0); nap(ms / 2);
        }
    }
}

/* Everything at once: a lit band travelling across the SFP+ row with the QSFP
 * cages flashing as it passes each quarter. */
static void wave(int rounds, int ms)
{
    int r, pos, i, k;
    for (r = 0; r < rounds && !stop; r++) {
        for (pos = 0; pos < SFP_PORTS + 8 && !stop; pos++) {
            for (i = 0; i < SFP_PORTS; i++)
                wr(sfp(i), (i <= pos && i > pos - 8) ? LED_ON : 0);
            for (k = 0; k < QSFP_LANES; k++)
                wr(qsfp(k), (pos / 8) % 6 == k / 4 ? LED_ON : 0);
            nap(ms);
        }
    }
}

/* Walk single bits so a human can map the encoding. Only bit 28 is known. */
static void probe(int port, int ms)
{
    uint32_t off = port < SFP_PORTS ? sfp(port) : qsfp(port - SFP_PORTS);
    int b;

    printf("probing %s index %d at 0x%04x -- watch this port\n",
           port < SFP_PORTS ? "SFP+" : "QSFP lane",
           port < SFP_PORTS ? port + 1 : port - SFP_PORTS, off);
    printf("original value 0x%08x\n", rd(off));
    for (b = 0; b < 32 && !stop; b++) {
        uint32_t v = 1u << b;
        printf("  bit %2d  value 0x%08x%s\n", b, v,
               v == LED_ON ? "   <- the known 'on' bit" : "");
        fflush(stdout);
        wr(off, v);
        nap(ms);
    }
    wr(off, 0);
    printf("done; port set back to 0\n");
}

/* The full routine: the QSFP cages twice, then the whole SFP+ row. */
static void show(void)
{
    printf("QSFP cages, twice...\n");   fflush(stdout);
    qsfp_dance(2, 120);
    printf("wave across the SFP+ row...\n"); fflush(stdout);
    wave(4, 45);
    printf("sweep...\n");   fflush(stdout);
    sweep(3, 45);
    printf("bounce...\n");  fflush(stdout);
    bounce(3, 40);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "wave";
    int arg = argc > 2 ? atoi(argv[2]) : 0;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    if (map_scd() < 0) return 1;

    printf("flash rate register 0x6000 = 0x%08x\n", rd(FLASH_REG));
    printf("LED 'on' value = 0x%08x (bit 28)\n", LED_ON);
    printf("SFP+ 0x%04x..0x%04x, QSFP 0x%04x..0x%04x\n\n",
           sfp(0), sfp(SFP_PORTS - 1), qsfp(0), qsfp(QSFP_LANES - 1));

    if      (!strcmp(mode, "bounce"))  bounce(arg ? arg : 3, 40);
    else if (!strcmp(mode, "sweep"))   sweep(arg ? arg : 3, 45);
    else if (!strcmp(mode, "qsfp"))    qsfp_dance(arg ? arg : 3, 120);
    else if (!strcmp(mode, "wave"))    wave(arg ? arg : 3, 45);
    else if (!strcmp(mode, "probe"))   probe(arg, 1500);
    else if (!strcmp(mode, "show"))    show();
    else if (!strcmp(mode, "restore")) { all_off(); printf("all LEDs off\n"); }
    else {
        printf("usage: leddance show|sweep|bounce|qsfp|wave|probe <idx>|restore [rounds]\n");
        printf("  show = QSFP cages twice, then wave/sweep/bounce over the SFP+ row\n");
        return 2;
    }

    if (strcmp(mode, "probe") && strcmp(mode, "restore")) all_off();
    printf("\n%s\n", stop ? "interrupted; LEDs cleared" : "done; LEDs cleared");
    return 0;
}
