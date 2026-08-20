/*
 * Front-panel port LEDs that follow the ports.
 *
 * THE BUG THIS FIXES: pulling a transceiver left its LED lit. Nothing tied the
 * LED registers to anything, so they held whatever value was last written --
 * by a test, by an animation, or by EOS before we kexec'd over it.
 *
 * There is no hardware mode to fall back on. These LEDs are software-driven on
 * this board, and we know that empirically rather than by assumption: read off
 * a running EOS, every connected port's register held 0x10000000 and every
 * disconnected one held 0x00000000. EOS is writing them from link state, so we
 * have to as well.
 *
 *   0x6100 + 0x10*n            SFP+  port n+1,  n = 0..47   (Et1..Et48)
 *   0x6400 + 0x10*(i*4 + j)    QSFP  port 49+i lane j+1     (Et49..Et54)
 *   bit 28 is on; 0 is off
 *
 * Arista's GPL scd-led.c carries a richer encoding for these registers -- a
 * seven-entry colour table in bits 26..28 and a blink bit at 25 -- but this
 * board's own software only ever used bit 28, so that is all this writes.
 * Inventing a colour the hardware was never observed to show would be guessing.
 *
 * The link state comes from the SDK, which is the only thing that knows it for
 * all 72 ports, which is why this lives in the bridge rather than in a
 * standalone tool that would have to rediscover it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <stdint.h>

#include <sal/types.h>
#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/port.h>
#include <bcm/link.h>

#define SCD_MAP     0x80000u
#define SFP_BASE    0x6100u
#define QSFP_BASE   0x6400u
#define LED_STRIDE  0x10u
#define LED_ON      0x10000000u
#define SFP_PORTS   48
#define QSFP_LANES  24                 /* 6 QSFP x 4 lanes */
#define LED_PORTS   (SFP_PORTS + QSFP_LANES)

static volatile uint32_t *scd;
static int led_unit;
static uint32_t led_last[LED_PORTS + 1];
static int led_on;

/* The SCD is 3475:0001, and its PCI address moves between kernels (05:00.0
 * under EOS, 04:00.0 under ours), so find it by vendor id rather than hardcode
 * a path that will be wrong half the time. */
static int led_map_scd(void)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    struct dirent *e;
    char path[256];
    int fd;

    if (!d) return -1;
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
        scd = mmap(NULL, SCD_MAP, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (scd == MAP_FAILED) { scd = NULL; continue; }
        printf("led: scd at %s\n", e->d_name);
        closedir(d);
        return 0;
    }
    closedir(d);
    printf("led: no Arista SCD (3475:*) found -- port LEDs will not be driven\n");
    return -1;
}

/* Logical port -> LED register. Ports 1..48 are the SFP+ cages in order;
 * 49..72 are the six QSFPs broken out four lanes each, in the same order the
 * port map assigns them. */
static uint32_t led_reg(int port)
{
    if (port >= 1 && port <= SFP_PORTS)
        return SFP_BASE + LED_STRIDE * (uint32_t)(port - 1);
    if (port > SFP_PORTS && port <= LED_PORTS)
        return QSFP_BASE + LED_STRIDE * (uint32_t)(port - SFP_PORTS - 1);
    return 0;
}

int ledsync_start(int unit)
{
    int i;

    led_unit = unit;
    if (led_map_scd() != 0) return -1;

    /* START FROM A KNOWN STATE. Whatever was on these registers -- EOS's last
     * write before we took the machine, or an animation someone ran -- is not
     * information about the ports now. Clear them, then let the first poll
     * light whatever is genuinely up. */
    for (i = 1; i <= LED_PORTS; i++) {
        uint32_t off = led_reg(i);
        if (off) scd[off / 4] = 0;
        led_last[i] = 0;
    }
    led_on = 1;
    printf("led: driving %d port LEDs from link state\n", LED_PORTS);
    fflush(stdout);
    return 0;
}

/* Called from the bridge's poll loop.
 *
 * COMPARES AGAINST THE HARDWARE, NOT AGAINST WHAT WE LAST WROTE.
 *
 * The first version cached its own last write and only touched a register when
 * the link state changed. That is cheaper and it is wrong: it cannot repair a
 * register that something else has scribbled on, which is the entire class of
 * bug this exists to fix. An LED left lit by an animation, by a test, or by the
 * vendor OS before we took the machine would stay lit for ever, because as far
 * as the cache was concerned nothing had changed.
 *
 * Reading the register back makes this loop authoritative over the panel: if
 * the hardware does not say what the link says, it gets corrected within one
 * interval no matter who wrote it. Seventy-two reads every five seconds on a
 * memory-mapped BAR costs nothing worth measuring. */
void ledsync_poll(void)
{
    int port, changed = 0;

    if (!led_on || !scd) return;

    for (port = 1; port <= LED_PORTS; port++) {
        uint32_t off = led_reg(port), want, have;
        int link = 0;

        if (!off) continue;

        /* A port the SDK will not answer for is treated as DOWN, not skipped.
         * The whole point of this is that a LED must never stay lit for
         * something that is not there. */
        if (bcm_port_link_status_get(led_unit, port, &link) != BCM_E_NONE)
            link = 0;

        want = link ? LED_ON : 0;
        have = scd[off / 4];
        if (have != want) {
            scd[off / 4] = want;
            if (led_last[port] != want) changed++;   /* a real link change */
            led_last[port] = want;
        }
    }
    if (changed) {
        printf("led: %d port LED(s) changed\n", changed);
        fflush(stdout);
    }
}

/* Leave the panel dark rather than frozen on a stale picture. */
void ledsync_stop(void)
{
    int i;

    if (!led_on || !scd) return;
    for (i = 1; i <= LED_PORTS; i++) {
        uint32_t off = led_reg(i);
        if (off) scd[off / 4] = 0;
    }
    led_on = 0;
}
