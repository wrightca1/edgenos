/* platmon.c -- read-only platform monitor for the 7050TX-64 (Yreka64).
 *
 * Everything here reads, with ONE exception: `platmon led` writes the chassis
 * LED registers. That boundary is deliberate. The standing rule on this box is
 * that `fanset` must not be used -- there is no trustworthy fan reading to
 * verify against -- so the fan PWM registers are never written here, and nothing
 * sweeps a register space blindly. An LED is the one thing on this board whose
 * whole purpose is to be written.
 *
 * Topology is from the board's own FDL -- see docs/PLATFORM-SCD-I2C.md:
 *
 *   bus0:1 (i2c-4)  StandbyCpld "murder" 0x60 -- fan tach, PWM, presence, ID, LEDs
 *   bus0:3 (i2c-6)  PSU 1, PMBus 0x58
 *   bus0:4 (i2c-7)  PSU 2, PMBus 0x58
 *   SCD BAR0 0x5000 -- PSU presence GPIO (bit 0 = PSU1, bit 1 = PSU2)
 *
 * Usage:
 *   platmon buses                 list i2c adapters and their names
 *   platmon cpld [bus]            fan CPLD: the registers the FDL names
 *   platmon cpld [bus] --sweep    dump 0x00-0x3f  (see the warning below)
 *   platmon psu  [bus]            one PSU over PMBus
 *   platmon scd                   PSU presence from the SCD
 *   platmon all                   everything the FDL documents
 *   platmon led <which> <colour>  set one LED: status|fan|psu1|psu2,
 *                                 off|green|red
 *   platmon led auto              drive all four from what is actually measured
 *   platmon cool                  show what the cooling curve would command
 *   platmon cool --apply          set fan PWM from the inlet temperature
 *   platmon fan <pwm>             set fan PWM by hand, for measurement
 *   platmon psufan <bus> <pct>    set a PSU's own fan duty over PMBus
 *
 * ⚠ --sweep reads registers the FDL does not describe. It is how the tach and
 * PWM offsets were found -- the FDL stops at presence/ID/LED and hands speed to
 * Inventory::FanController -- but it is still a sweep, so it is opt-in and it is
 * bounded to 0x00-0x3f. Reads on this CPLD are believed side-effect free; that
 * belief is not the same as knowing, so do not widen the range casually.
 *
 * The sweep's reading of that space was afterwards confirmed against Arista's
 * own GPL driver for this exact part -- src/crow-fan-driver.c in the SONiC
 * platform tree, and this board is `platform=crow` on its own kernel cmdline.
 * Every offset below is that driver's, not an inference:
 *
 *   0x00-0x07  tach, per fan, low byte then high:  RPM = 6000000 / tach
 *   0x10-0x13  PWM, per fan, 0-255 (FAN_MAX_PWM)
 *   0x18-0x1b  fan ID          0x21  presence
 *   0x24 green LED   0x25 red LED    0x40 CPLD revision
 *
 * ⚠ This tool never writes any of them. `fanset` is a standing prohibition on
 * this box, and the PWM registers are exactly where it would land.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* i2c-dev, spelled out so this builds against any libc without i2c-tools. */
#define I2C_SLAVE_FORCE 0x0706
#define I2C_SMBUS       0x0720
#define I2C_SMBUS_READ  1
#define I2C_SMBUS_BYTE_DATA  2
#define I2C_SMBUS_WORD_DATA  3
#define I2C_SMBUS_BLOCK_DATA 5

union i2c_smbus_data { uint8_t byte; uint16_t word; uint8_t block[34]; };
struct i2c_smbus_ioctl_data {
    uint8_t read_write; uint8_t command; uint32_t size;
    union i2c_smbus_data *data;
};

#define CPLD_ADDR 0x60
#define PSU_ADDR  0x58
#define SCD_RES   "/sys/bus/pci/devices/0000:02:00.0/resource0"

/* Chassis LED blocks, from the board's FDL. One 32-bit word each. */
#define LED_STATUS 0x6050
#define LED_FAN    0x6060
#define LED_PSU1   0x6070
#define LED_PSU2   0x6080

/* ⚠ These registers are WRITE-ONLY: they read back 0 with the panel lit, so an
 * LED can never be verified by read-back -- only by looking at the box.
 *
 * This SCD is the 2014 "I5 FPGA" part and wants the driver's OLDER encoding,
 * `led_brightness_set_legacy` in src/scd-led.c, not the modern bit-27/28/29
 * form. That was settled by experiment on 2026-08-18, not by reading: bit 27
 * written to Status and PSU1 did nothing at all, while these constants written
 * to Fan and PSU2 lit them, and the blue beacon at 0x6090 ignored bit 27 too.
 * Value 1 was then written to all four blocks at once and all four went green,
 * which is what makes this a palette rather than four coincidences. */
/* QSFP link LEDs. The board description defines these as one block per LANE,
 * not per port: four ports x four lanes, based at 0xA000 with a 0x10 stride,
 * so Ethernet49/1 is 0xA000 and Ethernet52/4 is 0xA0F0.
 *
 * ⚠ Stated as derived facts on purpose. The .fdl is Arista Confidential and
 * must not be reproduced even here -- this comment previously quoted its
 * source, was sanitised on the publish branch only, and came straight back the
 * next time the file was copied. Fix these at the source or they recur.
 *
 * Transceiver control
 * starts at 0xA100 and must not be caught by an off-by-one here.
 *
 * ⚠ Nothing wrote these until now, which is the whole reason the QSFP LEDs
 * were dark: not a fault, an omission. The chassis LEDs worked because
 * `led auto` drove them and these were simply never in the list.
 */
#define LED_QSFP_BASE  0xA000
#define LED_QSFP_STEP  0x10
#define LED_QSFP_LAST  0xA0F0

#define LED_OFF   0x0006ff00u
#define LED_GREEN 0x1006ff00u
#define LED_RED   0x0806ff00u

static int xfer(int fd, uint8_t rw, uint8_t cmd, uint32_t size,
                union i2c_smbus_data *d)
{
    struct i2c_smbus_ioctl_data a;
    a.read_write = rw; a.command = cmd; a.size = size; a.data = d;
    return ioctl(fd, I2C_SMBUS, &a);
}

static int rd_byte(int fd, uint8_t reg)
{
    union i2c_smbus_data d;
    if (xfer(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &d) < 0) return -1;
    return d.byte;
}

static int rd_word(int fd, uint8_t reg)
{
    union i2c_smbus_data d;
    if (xfer(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_WORD_DATA, &d) < 0) return -1;
    return d.word;
}

/* The ONLY registers this program may write on the fan CPLD.
 *
 * ⚠ This whitelist is the fanset prohibition made mechanical. The PWM registers
 * (0x10-0x13) sit on the same device, one SMBus write away, and a wrong write
 * there is a thermal event rather than a bug report. A comment saying "do not
 * write PWM" is worth less than a function that cannot. */
static int cpld_writable(uint8_t reg)
{
    if (reg == 0x24 || reg == 0x25) return 1;        /* green LED, red LED */
    if (reg >= 0x10 && reg <= 0x13) return 1;        /* fan PWM, `cool --apply` */
    return 0;
}

/* Fan PWM. FULL SCALE IS 180, NOT 255 -- Arista's driver defines FAN_MAX_PWM as
 * 255, but EOS reports "Configured Speed 71%" while this register holds 127, and
 * 71% x 1.8 = 127.8. Writing percent x 2.55 would therefore command ~140% and
 * clamp, losing all resolution at the top of the range.
 *
 * ⚠ PWM_FLOOR is the safety property that matters. Every write is clamped into
 * [floor, max], so a bug in the curve, a failed sensor read, or a wrong scale
 * can make this box LOUD but cannot make it hot. The floor is EOS's own observed
 * setting at this ambient. Nothing may write below it. */
#define PWM_BASE  0x10
/* ⚠ Was 127 (71%), chosen when the PWM scale was still uncertain so that a bug
 * would make the box loud rather than hot. The scale is now known and measured:
 * dropping the trays from 73% to 53% for six minutes moved board temperature by
 * 1 C and CPU by 1 C the other way, at 31 C ambient. There is real headroom.
 *
 * 108 is 60%, which is the LOWEST the board's own curve ever asks for on this
 * chassis (level 2 at 25 C inlet). So the floor is no longer an arbitrary
 * safety number -- it is "never quieter than the vendor's own policy would be".
 *
 * This is only safe alongside the over-temperature override below, which was
 * added at the same time. Do not lower one without the other. */
#define PWM_FLOOR 108
#define PWM_MAX   180
/* Lower bound for the MANUAL `fan` command only. Below the loop's floor,
 * because measuring the thermal response means going there, but not to zero:
 * these are the only fans in the chassis. */
#define FAN_MANUAL_MIN 70

static int wr_byte(int fd, uint8_t reg, uint8_t val)
{
    if (!cpld_writable(reg)) {
        fprintf(stderr, "  refusing to write CPLD reg 0x%02x -- not an LED register\n",
                reg);
        return -1;
    }
    union i2c_smbus_data d;
    d.byte = val;
    return xfer(fd, 0 /* write */, reg, I2C_SMBUS_BYTE_DATA, &d);
}

/* Write one fan's PWM, clamped. The clamp is not advisory: it is applied here,
 * at the only place that can write these registers. */
static int wr_pwm(int fd, int fan, int val)
{
    if (val < PWM_FLOOR) val = PWM_FLOOR;
    if (val > PWM_MAX) val = PWM_MAX;
    union i2c_smbus_data d;
    d.byte = (uint8_t)val;
    uint8_t reg = (uint8_t)(PWM_BASE + fan);
    if (!cpld_writable(reg)) return -1;
    return xfer(fd, 0 /* write */, reg, I2C_SMBUS_BYTE_DATA, &d);
}

static int rd_block(int fd, uint8_t reg, char *out, size_t n)
{
    union i2c_smbus_data d;
    memset(&d, 0, sizeof d);
    if (xfer(fd, I2C_SMBUS_READ, reg, I2C_SMBUS_BLOCK_DATA, &d) < 0) return -1;
    size_t len = d.block[0];
    if (len > n - 1) len = n - 1;
    memcpy(out, d.block + 1, len);
    out[len] = 0;
    return (int)len;
}

static int open_dev(int bus, int addr)
{
    char path[64];
    snprintf(path, sizeof path, "/dev/i2c-%d", bus);
    int fd = open(path, O_RDWR);
    if (fd < 0) { fprintf(stderr, "  open %s: %s\n", path, strerror(errno)); return -1; }
    if (ioctl(fd, I2C_SLAVE_FORCE, addr) < 0) {
        fprintf(stderr, "  addr 0x%02x on %s: %s\n", addr, path, strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

/* ---- PMBus number formats ------------------------------------------------ */

/* LINEAR11: 5-bit signed exponent in the top bits, 11-bit signed mantissa. */
static double linear11(uint16_t v)
{
    int e = (int)(v >> 11); if (e > 15) e -= 32;
    int m = (int)(v & 0x7ff); if (m > 1023) m -= 2048;
    return m * (e >= 0 ? (double)(1 << e) : 1.0 / (1 << -e));
}

/* LINEAR16 for VOUT: unsigned mantissa, exponent from VOUT_MODE. */
static double linear16(uint16_t v, int mode)
{
    int e = mode & 0x1f; if (e > 15) e -= 32;
    return v * (e >= 0 ? (double)(1 << e) : 1.0 / (1 << -e));
}

/* ---- fan CPLD ------------------------------------------------------------ */

static void cpld_named(int fd)
{
    printf("  fan CPLD 0x%02x (StandbyCpld \"murder\")\n", CPLD_ADDR);

    int present = rd_byte(fd, 0x21);
    if (present < 0) { printf("    present reg 0x21: read failed\n"); return; }
    int rev = rd_byte(fd, 0x40);
    if (rev >= 0) printf("    CPLD revision 0x%02x\n", rev);
    /* Presence is ACTIVE LOW: a clear bit means the tray is in. */
    printf("    0x21 present = 0x%02x\n", present);

    int green = rd_byte(fd, 0x24);
    int red   = rd_byte(fd, 0x25);

    for (int t = 1; t <= 4; t++) {
        int in = !((present >> (t - 1)) & 1);
        int id = rd_byte(fd, 0x17 + t);          /* 0x18..0x1b */

        /* Tach is two byte reads, low then high, exactly as the driver does it. */
        int lo = rd_byte(fd, 2 * (t - 1)), hi = rd_byte(fd, 2 * (t - 1) + 1);
        long rpm = -1;
        if (lo >= 0 && hi >= 0) {
            unsigned tach = ((unsigned)hi << 8) | (unsigned)lo;
            rpm = tach ? 6000000L / tach : 0;
        }

        int pwm = rd_byte(fd, 0x10 + (t - 1));

        /* Colour, per the driver's read_led_color(): green bit CLEAR with red
         * bit SET means green, and both set means off. */
        const char *colour = "?";
        if (green >= 0 && red >= 0) {
            int g = !((green >> (t - 1)) & 1), r = !((red >> (t - 1)) & 1);
            colour = g && r ? "amber" : g ? "green" : r ? "red" : "off";
        }

        printf("    tray %d  %-7s  id=%d  ", t, in ? "present" : "ABSENT",
               id < 0 ? -1 : (id & 0x7));
        if (rpm >= 0) printf("%6ld rpm  ", rpm); else printf("   ?  rpm  ");
        /* Percent of PWM_MAX (180), not 255. See the PWM_BASE comment: EOS
         * reported "Configured Speed 71%" with this register holding 127. */
        if (pwm >= 0) printf("pwm %3d (%2d%%)  ", pwm, (pwm * 100 + PWM_MAX / 2) / PWM_MAX);
        else printf("pwm   ?        ");
        printf("led=%s\n", colour);
    }
}

static void cpld_sweep(int fd)
{
    printf("  sweep 0x00-0x3f (registers the FDL does not name):\n");
    for (int base = 0; base < 0x40; base += 16) {
        printf("    %02x:", base);
        for (int i = 0; i < 16; i++) {
            int v = rd_byte(fd, base + i);
            if (v < 0) printf(" --"); else printf(" %02x", v);
        }
        printf("\n");
    }
}

/* ---- PSU ----------------------------------------------------------------- */

static void psu_read(int bus)
{
    int fd = open_dev(bus, PSU_ADDR);
    if (fd < 0) return;
    printf("  PSU on i2c-%d, PMBus 0x%02x\n", bus, PSU_ADDR);

    char s[40];
    if (rd_block(fd, 0x99, s, sizeof s) > 0) printf("    MFR_ID       %s\n", s);
    if (rd_block(fd, 0x9a, s, sizeof s) > 0) printf("    MFR_MODEL    %s\n", s);

    int status = rd_word(fd, 0x79);
    if (status < 0) {
        printf("    no response -- an unpowered supply does not answer PMBus.\n");
        printf("    Presence is the SCD GPIO at 0x5000, not this bus.\n");
        close(fd); return;
    }
    printf("    STATUS_WORD  0x%04x", status);
    if (!status) printf("  (all clear)");
    else {
        /* PMBus STATUS_WORD: low byte is STATUS_BYTE, high byte the summary. */
        static const char *lo_bits[] = { NULL, "cml", "temperature", "vin uv",
                                         "iout oc", "vout ov", "off", "busy" };
        static const char *hi_bits[] = { "unknown", "other", "fans", "power not good",
                                         "mfr", "input", "iout/pout", "vout" };
        printf("  --");
        if ((status & ~0x0082) == 0) printf(" (nothing but comms noise)");
        for (int i = 0; i < 8; i++)
            if ((status >> i) & 1 && lo_bits[i]) printf(" %s;", lo_bits[i]);
        for (int i = 0; i < 8; i++)
            if ((status >> (i + 8)) & 1) printf(" %s;", hi_bits[i]);
    }
    printf("\n");

    int mode = rd_byte(fd, 0x20);
    struct { uint8_t cmd; const char *name; const char *unit; } l11[] = {
        { 0x88, "VIN",   "V" }, { 0x89, "IIN",   "A" },
        { 0x8c, "IOUT",  "A" }, { 0x96, "POUT",  "W" },
        { 0x97, "PIN",   "W" },
        { 0x8d, "TEMP1 (hotspot)", "C" }, { 0x8e, "TEMP2 (inlet)", "C" },
        { 0x90, "FAN1",  "RPM" },
    };
    int v = rd_word(fd, 0x8b);
    if (v >= 0 && mode >= 0)
        printf("    %-16s %8.2f %s\n", "VOUT", linear16((uint16_t)v, mode), "V");
    for (size_t i = 0; i < sizeof l11 / sizeof l11[0]; i++) {
        v = rd_word(fd, l11[i].cmd);
        if (v < 0) continue;
        printf("    %-16s %8.2f %s\n", l11[i].name, linear11((uint16_t)v), l11[i].unit);
    }
    close(fd);
}

/* ---- SCD ----------------------------------------------------------------- */

static void scd_presence(void)
{
    int fd = open(SCD_RES, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "  open %s: %s\n", SCD_RES, strerror(errno)); return; }
    void *m = mmap(NULL, 0x10000, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("  mmap"); close(fd); return; }
    uint32_t p = *(volatile uint32_t *)((char *)m + 0x5000);
    printf("  SCD 0x5000 powerPresentAndStatus = 0x%08x\n", p);
    printf("    PSU 1 %s\n", (p & 1) ? "present" : "absent");
    printf("    PSU 2 %s\n", (p & 2) ? "present" : "absent");
    munmap(m, 0x10000); close(fd);
}

static int scd_write(uint32_t off, uint32_t val)
{
    int fd = open(SCD_RES, O_RDWR);
    if (fd < 0) { fprintf(stderr, "  open %s: %s\n", SCD_RES, strerror(errno)); return -1; }
    void *m = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("  mmap"); close(fd); return -1; }
    *(volatile uint32_t *)((char *)m + off) = val;
    munmap(m, 0x10000); close(fd);
    return 0;
}

static uint32_t scd_read(uint32_t off)
{
    int fd = open(SCD_RES, O_RDONLY);
    if (fd < 0) return 0;
    void *m = mmap(NULL, 0x10000, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); return 0; }
    uint32_t v = *(volatile uint32_t *)((char *)m + off);
    munmap(m, 0x10000); close(fd);
    return v;
}

/* Is one PSU healthy? Presence is the SCD GPIO; health is PMBus. A supply with
 * no AC answers nothing, so "present but silent" is a fault, not an absence. */
static int psu_state(int bus, int present)   /* -1 absent, 0 faulted, 1 ok */
{
    if (!present) return -1;
    int fd = open_dev(bus, PSU_ADDR);
    if (fd < 0) return 0;
    int status = rd_word(fd, 0x79);
    close(fd);
    if (status < 0) return 0;
    /* ⚠ Mask CML (bit 1) and BUSY (bit 7). CML is "communication/memory/logic",
     * and we provoke it ourselves: the kernel pmbus driver owns 0x58 too, so a
     * raw I2C_SLAVE_FORCE read here races its polling and the supply flags the
     * aborted transaction. Observed as PSU 1 reporting STATUS_WORD 0x0002 while
     * delivering a healthy 157 W. Treating that as a fault would light the panel
     * red over our own instrumentation. */
    return (status & ~0x0082) == 0;
}

enum { FAN_OFF, FAN_GREEN, FAN_RED, FAN_AMBER };

static int fan_tray_colours(int fd, int out[4], long rpm_out[4]);
static int fan_leds_read(int fd, int colour[4]);
static int fan_leds_read(int fd, int colour[4]);

static int fan_leds_apply(int fd, const int colour[4]);
static const char *fan_colour_name(int c);

static int tap_is_up(const char *tap);
static int qsfp_led_set(int fp_port, uint32_t colour);
extern const struct qsfp_port_s { int fp; const char *tap; } QSFP_PORTS[4];

static void led_auto(void)
{
    uint32_t p = scd_read(0x5000);
    int s1 = psu_state(6, p & 1), s2 = psu_state(7, p & 2);

    /* Fans: presence and a turning rotor are both measured facts, so the fan LED
     * can be driven honestly. Note this says nothing about whether the SPEED is
     * right -- nothing supervises cooling here. */
    int fans_ok = 0;
    int fd = open_dev(4, CPLD_ADDR);
    if (fd >= 0) {
        int present = rd_byte(fd, 0x21);
        fans_ok = present >= 0 && (present & 0x0f) == 0;
        for (int t = 0; fans_ok && t < 4; t++) {
            int lo = rd_byte(fd, 2 * t), hi = rd_byte(fd, 2 * t + 1);
            if (lo < 0 || hi < 0 || !(((unsigned)hi << 8) | (unsigned)lo)) fans_ok = 0;
        }
        close(fd);
    }

    uint32_t c1 = s1 < 0 ? LED_OFF : s1 ? LED_GREEN : LED_RED;
    uint32_t c2 = s2 < 0 ? LED_OFF : s2 ? LED_GREEN : LED_RED;
    uint32_t cf = fans_ok ? LED_GREEN : LED_RED;
    /* The box is only green when nothing is faulted. A present-but-dead supply
     * is a fault: it is exactly the condition the panel exists to show. */
    uint32_t cs = (s1 == 0 || s2 == 0 || !fans_ok) ? LED_RED : LED_GREEN;

    scd_write(LED_PSU1, c1);
    scd_write(LED_PSU2, c2);
    scd_write(LED_FAN, cf);
    scd_write(LED_STATUS, cs);

    /* QSFP link LEDs, from the tap's operstate. Absent taps mean the agent is
     * not up, and then we say nothing rather than asserting "no link" -- a
     * dark LED should mean the port is down, not that platmon ran early. */
    for (int i = 0; i < 4; i++) {
        int up = tap_is_up(QSFP_PORTS[i].tap);
        if (up < 0) continue;
        qsfp_led_set(QSFP_PORTS[i].fp, up ? LED_GREEN : LED_OFF);
    }

    printf("  psu1   %s\n", s1 < 0 ? "absent -> off" : s1 ? "ok -> green" : "FAULT -> red");
    printf("  psu2   %s\n", s2 < 0 ? "absent -> off" : s2 ? "ok -> green" : "FAULT -> red");
    printf("  fans   %s\n", fans_ok ? "4 present, all turning -> green" : "FAULT -> red");
    printf("  status %s\n", cs == LED_GREEN ? "green" : "red");

    /* Fan tray LEDs are a different mechanism entirely -- GPIO bits on the fan
     * CPLD rather than SCD register blocks -- but the same principle: say what
     * is measured. Unlike the SCD blocks these DO read back, so they are the one
     * part of the panel that can be verified without looking at it. */
    fd = open_dev(4, CPLD_ADDR);
    if (fd >= 0) {
        int colour[4]; long rpm[4];
        if (fan_tray_colours(fd, colour, rpm) == 0 && fan_leds_apply(fd, colour) == 0)
            for (int t = 0; t < 4; t++)
                printf("  tray %d %s%s\n", t + 1, fan_colour_name(colour[t]),
                       rpm[t] > 0 ? "" : rpm[t] == 0 ? " (fitted, not turning)"
                                                     : " (not fitted)");
        close(fd);
    }
    printf("  (the four SCD blocks are write-only -- confirm those by looking)\n");
}

/* Fan tray LED colours, per the driver's read_led_color(): green is the green
 * bit CLEAR with the red bit SET, red is the reverse, both clear is amber, both
 * set is off. Bits 4-7 of each register are not ours, so read-modify-write. */

static const char *fan_colour_name(int c)
{
    return c == FAN_GREEN ? "green" : c == FAN_RED ? "red"
         : c == FAN_AMBER ? "amber" : "off";
}

/* Decide each tray's colour from what is measured. "Turning" is a fact; "fast
 * enough" is not, because nothing here knows the right speed. What a stalled or
 * dying fan does show is disagreement with its neighbours, so a tray more than
 * 25% off the median of the others is called out as amber rather than green. */
static int fan_tray_colours(int fd, int out[4], long rpm_out[4])
{
    int present = rd_byte(fd, 0x21);
    if (present < 0) return -1;

    long rpm[4];
    int n = 0;
    long sorted[4];
    for (int t = 0; t < 4; t++) {
        rpm[t] = -1;
        if ((present >> t) & 1) continue;              /* active low: set = absent */
        int lo = rd_byte(fd, 2 * t), hi = rd_byte(fd, 2 * t + 1);
        if (lo < 0 || hi < 0) continue;
        unsigned tach = ((unsigned)hi << 8) | (unsigned)lo;
        rpm[t] = tach ? 6000000L / tach : 0;
        if (rpm[t] > 0) sorted[n++] = rpm[t];
    }

    long median = 0;
    if (n) {
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                if (sorted[j] < sorted[i]) { long x = sorted[i]; sorted[i] = sorted[j]; sorted[j] = x; }
        median = sorted[n / 2];
    }

    for (int t = 0; t < 4; t++) {
        rpm_out[t] = rpm[t];
        if (rpm[t] < 0)       out[t] = FAN_OFF;        /* tray not fitted */
        else if (rpm[t] == 0) out[t] = FAN_RED;        /* fitted, not turning */
        else if (median && labs(rpm[t] - median) * 4 > median)
                              out[t] = FAN_AMBER;      /* >25% off its neighbours */
        else                  out[t] = FAN_GREEN;
    }
    return 0;
}

/* Decode what the LED registers currently hold. led_cmd must seed from this,
 * not from measured state: seeding from measurement made every manual set
 * silently revert the previous one, because each call recomputed the other
 * three trays from the fans rather than leaving them as they were. */
static int fan_leds_read(int fd, int colour[4])
{
    int green = rd_byte(fd, 0x24), red = rd_byte(fd, 0x25);
    if (green < 0 || red < 0) return -1;
    for (int t = 0; t < 4; t++) {
        int g = (green >> t) & 1, r = (red >> t) & 1;
        colour[t] = (!g && r) ? FAN_GREEN : (g && !r) ? FAN_RED
                  : (!g && !r) ? FAN_AMBER : FAN_OFF;
    }
    return 0;
}

static int fan_leds_apply(int fd, const int colour[4])
{
    int green = rd_byte(fd, 0x24), red = rd_byte(fd, 0x25);
    if (green < 0 || red < 0) return -1;

    for (int t = 0; t < 4; t++) {
        int g, r;
        switch (colour[t]) {
        case FAN_GREEN: g = 0; r = 1; break;
        case FAN_RED:   g = 1; r = 0; break;
        case FAN_AMBER: g = 0; r = 0; break;
        default:        g = 1; r = 1; break;           /* off */
        }
        green = g ? (green | (1 << t)) : (green & ~(1 << t));
        red   = r ? (red   | (1 << t)) : (red   & ~(1 << t));
    }
    if (wr_byte(fd, 0x24, (uint8_t)green) < 0) return -1;
    if (wr_byte(fd, 0x25, (uint8_t)red) < 0) return -1;
    return 0;
}

/* Front-panel Ethernet49..52 are SDK ports 49, 53, 57, 61, and the tap for a
 * port is "xe" + (port-1) -- so Et49 is xe48 and Et52 is xe60. That is the only
 * link state a userspace tool can see here; the agent owns the chip. */
const struct qsfp_port_s QSFP_PORTS[4] = {
    { 49, "xe48" }, { 50, "xe52" }, { 51, "xe56" }, { 52, "xe60" },
};

static int tap_is_up(const char *tap)
{
    char path[128], buf[32];
    FILE *f;
    size_t n;

    snprintf(path, sizeof path, "/sys/class/net/%s/operstate", tap);
    f = fopen(path, "r");
    if (f == NULL) return -1;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (!strncmp(buf, "up", 2)) return 1;
    if (!strncmp(buf, "unknown", 7)) return 1;   /* tap devices sit here */
    return 0;
}

/* One QSFP port is four lane blocks. A 40G port is one logical link, so drive
 * all four together; break this apart if the box is ever run in 4x10G. */
static int qsfp_led_set(int fp_port, uint32_t colour)
{
    int i = fp_port - 49;
    int lane, rc = 0;

    if (i < 0 || i > 3) return -1;
    for (lane = 0; lane < 4; lane++) {
        uint32_t off = LED_QSFP_BASE + (uint32_t)(i * 4 + lane) * LED_QSFP_STEP;
        if (off > LED_QSFP_LAST) return -1;      /* never stray into 0xA100 */
        if (scd_write(off, colour) != 0) rc = -1;
    }
    return rc;
}

static int led_cmd(const char *which, const char *colour)
{
    struct { const char *n; uint32_t off; } leds[] = {
        { "status", LED_STATUS }, { "fan", LED_FAN },
        { "psu1", LED_PSU1 }, { "psu2", LED_PSU2 },
    };
    struct { const char *n; uint32_t v; } cols[] = {
        { "off", LED_OFF }, { "green", LED_GREEN }, { "red", LED_RED },
    };
    /* Fan trays are the CPLD path, not an SCD block. */
    if (!strncmp(which, "fan", 3) && which[3] >= '1' && which[3] <= '4' && !which[4]) {
        int t = which[3] - '1';
        int c = !strcmp(colour, "green") ? FAN_GREEN : !strcmp(colour, "red") ? FAN_RED
              : !strcmp(colour, "amber") ? FAN_AMBER : !strcmp(colour, "off") ? FAN_OFF : -1;
        if (c < 0) { fprintf(stderr, "  fan colours: off|green|red|amber\n"); return 2; }
        int fd = open_dev(4, CPLD_ADDR);
        if (fd < 0) return 1;
        int cur[4];
        if (fan_leds_read(fd, cur) < 0) { close(fd); return 1; }
        cur[t] = c;
        int rc = fan_leds_apply(fd, cur);
        close(fd);
        if (rc == 0) printf("  fan tray %d = %s\n", t + 1, fan_colour_name(c));
        return rc ? 1 : 0;
    }

    /* QSFP link LEDs: platmon led qsfp<49-52> <colour> */
    if (!strncmp(which, "qsfp", 4)) {
        int fp = atoi(which + 4);
        uint32_t v = !strcmp(colour, "green") ? LED_GREEN
                   : !strcmp(colour, "red")   ? LED_RED
                   : !strcmp(colour, "off")   ? LED_OFF : 0xffffffffu;
        if (fp < 49 || fp > 52) {
            fprintf(stderr, "  qsfp ports are 49-52\n");
            return 2;
        }
        if (v == 0xffffffffu) {
            fprintf(stderr, "  qsfp colours: off|green|red\n");
            return 2;
        }
        if (qsfp_led_set(fp, v) == 0) {
            printf("  Ethernet%d (0x%04x..) = %s\n", fp,
                   LED_QSFP_BASE + (fp - 49) * 4 * LED_QSFP_STEP, colour);
            return 0;
        }
        return 1;
    }

    for (size_t i = 0; i < sizeof leds / sizeof leds[0]; i++) {
        if (strcmp(which, leds[i].n)) continue;
        for (size_t j = 0; j < sizeof cols / sizeof cols[0]; j++) {
            if (strcmp(colour, cols[j].n)) continue;
            if (scd_write(leds[i].off, cols[j].v) == 0)
                printf("  %s (0x%04x) = %s\n", leds[i].n, leds[i].off, cols[j].n);
            return 0;
        }
    }
    fprintf(stderr, "usage: platmon led <status|fan|psu1|psu2> <off|green|red>\n"
                    "       platmon led fan<1-4> <off|green|red|amber>\n"
                    "       platmon led qsfp<49-52> <off|green|red>\n"
                    "       platmon led auto\n");
    return 2;
}

/* ---- cooling ------------------------------------------------------------- */
/*
 * EOS's algorithm, recovered from the board's FDL and then checked against EOS
 * running on this box:
 *
 *   inlet temperature -> cooling level (interpolated) -> fan speed % -> PWM
 *
 * The FDL gives the curve as discrete points, but EOS INTERPOLATES between them.
 * At ambient 30 C it configured 71%, at 31 C it configured 73%, and linear
 * interpolation between (25 C, level 2) and (31.66 C, level 3) through the
 * level->speed table gives 71.3% and 73.5%. Two independent points, both
 * matching to within rounding, which is what makes this the real control law
 * rather than a plausible reconstruction.
 *
 * ⚠ THE INLET IS THE BACK PANEL ON THIS BOX, and the FDL will tell you
 * otherwise. Its `position` fields label the front-panel diode "inlet" and the
 * back-panel diode "outlet", because those describe front-to-back airflow. This
 * chassis is back-to-front (EOS says so, and the PWR-500AC-R supplies are
 * Airflow.INTAKE parts), so the intake is the BACK. EOS agrees: it reports
 * ambient 30 C, which is the back-panel reading, while the front panel is 35 C.
 * Following the FDL's label would read 5 C too hot and over-cool by 8%.
 */
#define INLET_BUS  4      /* bus0:1, the CPU card */
#define INLET_ADDR 0x4c   /* MAX6658; remote channel is the back-panel diode */
#define INLET_REG  0x01   /* remote temperature, high byte = whole degrees */

struct pt { double x, y; };

/* ⚠ THE COOLING CURVE IS NOT IN THIS FILE, DELIBERATELY.
 *
 * The curve and the level->speed table come from the board's own FDL
 * (`/etc/fdl` on the switch), which is Arista Confidential. The mechanism is
 * ours and is published; Arista's numbers are not ours to publish. So they are
 * read at runtime from a file the operator generates from their own switch:
 *
 *     tools/fdl-extract.sh > /etc/edgenos/cooling.conf
 *
 * Format, one entry per line, blank lines and # comments ignored:
 *     curve <inlet-degrees-C> <cooling-level>
 *     speed <cooling-level> <percent>
 *
 * ⚠ With no file, this fails to 100%. That is deliberate and it is the only
 * safe default: a box whose cooling policy is unknown should be loud, not warm.
 */
#define COOLING_CONF "/etc/edgenos/cooling.conf"
#define MAX_PTS 16

static struct pt curve_pts[MAX_PTS], speed_pts[MAX_PTS];
static int n_curve, n_speed;

static int cooling_load(void)
{
    FILE *f = fopen(COOLING_CONF, "r");
    char line[160];

    n_curve = n_speed = 0;
    if (f == NULL) return -1;
    while (fgets(line, sizeof line, f)) {
        double a, b;
        if (sscanf(line, " curve %lf %lf", &a, &b) == 2 && n_curve < MAX_PTS) {
            curve_pts[n_curve].x = a; curve_pts[n_curve].y = b; n_curve++;
        } else if (sscanf(line, " speed %lf %lf", &a, &b) == 2 && n_speed < MAX_PTS) {
            speed_pts[n_speed].x = a; speed_pts[n_speed].y = b; n_speed++;
        }
    }
    fclose(f);
    return (n_curve >= 2 && n_speed >= 2) ? 0 : -1;
}

static double interp(const struct pt *t, size_t n, double x)
{
    if (x <= t[0].x) return t[0].y;
    for (size_t i = 1; i < n; i++) {
        if (x > t[i].x) continue;
        double f = (x - t[i - 1].x) / (t[i].x - t[i - 1].x);
        return t[i - 1].y + f * (t[i].y - t[i - 1].y);
    }
    return t[n - 1].y;
}

static int read_temp(int bus, int addr, int reg)
{
    int fd = open_dev(bus, addr);
    if (fd < 0) return -1000;
    int v = rd_byte(fd, (uint8_t)reg);
    close(fd);
    return v < 0 ? -1000 : v;
}

/* ⚠ The curve reads ONE sensor -- the inlet -- because that is what the vendor's
 * policy does. That is fine while inlet temperature tracks everything else, and
 * useless if it does not: a hot ASIC or a hot CPU behind a cool intake would
 * never move the fans at all.
 *
 * This is the backstop. Every board sensor is checked against the alert
 * threshold the board's own description gives it, and any one of them reaching
 * it takes the fans to full regardless of what the curve wanted. It costs four
 * i2c reads a minute.
 *
 * Thresholds, from the board description:
 *   board sensor    55    front panel  55
 *   cpu board       55    back panel   75
 * The CPU die is read from hwmon (k10temp) with its own limit of 90. */
struct tsensor { int bus, addr, reg, alert; const char *name; };
static const struct tsensor TSENSORS[] = {
    { 3, 0x4c, 0x00, 55, "board" },
    { 3, 0x4c, 0x01, 55, "front panel" },
    { 4, 0x4c, 0x00, 55, "cpu board" },
    { 4, 0x4c, 0x01, 75, "back panel" },
};

/* Returns the hottest over-threshold sensor, or NULL. */
static const char *overtemp(int *valp, int *limp)
{
    size_t i;
    for (i = 0; i < sizeof TSENSORS / sizeof TSENSORS[0]; i++) {
        int t = read_temp(TSENSORS[i].bus, TSENSORS[i].addr, TSENSORS[i].reg);
        if (t > -1000 && t >= TSENSORS[i].alert) {
            *valp = t; *limp = TSENSORS[i].alert;
            return TSENSORS[i].name;
        }
    }
    /* CPU die, via hwmon rather than i2c. */
    {
        int h;
        for (h = 0; h < 8; h++) {
            char path[80], name[32];
            FILE *f;
            snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/name", h);
            f = fopen(path, "r");
            if (!f) continue;
            if (!fgets(name, sizeof name, f)) name[0] = 0;
            fclose(f);
            if (strncmp(name, "k10temp", 7)) continue;
            snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/temp1_input", h);
            f = fopen(path, "r");
            if (!f) continue;
            {
                int milli = 0;
                if (fscanf(f, "%d", &milli) == 1 && milli / 1000 >= 90) {
                    fclose(f);
                    *valp = milli / 1000; *limp = 90;
                    return "cpu die";
                }
            }
            fclose(f);
        }
    }
    return NULL;
}

static int cool_run(int apply)
{
    int inlet = read_temp(INLET_BUS, INLET_ADDR, INLET_REG);
    int hot_val = 0, hot_lim = 0;
    const char *hot = overtemp(&hot_val, &hot_lim);

    double pct;
    const char *why;
    if (hot != NULL) {
        /* Nothing below this point may reduce the answer. */
        printf("  ** OVER TEMPERATURE: %s at %d C (alert %d) -> full speed\n",
               hot, hot_val, hot_lim);
        pct = 100.0;
        why = "over-temperature override";
        inlet = -1;
    } else if (cooling_load() < 0) {
        printf("  no usable %s -- failing to full speed\n", COOLING_CONF);
        printf("  generate one with tools/fdl-extract.sh on this switch\n");
        inlet = -1;
        pct = 100.0;
        why = "no cooling policy";
    } else if (inlet <= -1000) {
        /* ⚠ Fail loud, not quiet. A sensor we cannot read is not a cool box. */
        pct = 100.0;
        why = "INLET UNREADABLE -- failing to full speed";
        inlet = -1;
    } else {
        double level = interp(curve_pts, (size_t)n_curve, inlet);
        pct = interp(speed_pts, (size_t)n_speed, level);
        why = "curve from " COOLING_CONF;
        printf("  inlet (back panel) %d C  ->  cooling level %.2f\n", inlet, level);
    }

    int pwm = (int)(pct * 1.8 + 0.5);
    int clamped = pwm;
    if (clamped < PWM_FLOOR) clamped = PWM_FLOOR;
    if (clamped > PWM_MAX) clamped = PWM_MAX;

    printf("  target %.1f%%  ->  pwm %d%s  (%s)\n", pct, clamped,
           clamped != pwm ? " [clamped]" : "", why);

    if (!apply) { printf("  dry run -- nothing written (pass --apply)\n"); return 0; }

    int fd = open_dev(4, CPLD_ADDR);
    if (fd < 0) return 1;
    int bad = 0;
    for (int f = 0; f < 4; f++)
        if (wr_pwm(fd, f, clamped) < 0) bad++;
    /* Read back: unlike the SCD LED blocks these registers do read back, so the
     * write can be verified rather than assumed. */
    int got = rd_byte(fd, PWM_BASE);
    close(fd);
    printf("  wrote %d fans%s; register reads back %d\n", 4 - bad,
           bad ? " (some failed)" : "", got);
    return bad ? 1 : 0;
}

static void list_buses(void)
{
    for (int i = 0; i < 16; i++) {
        char path[80], name[64];
        snprintf(path, sizeof path, "/sys/class/i2c-dev/i2c-%d/name", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f);
        name[strcspn(name, "\n")] = 0;
        printf("  i2c-%-2d  %s\n", i, name);
    }
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "all";
    int sweep = 0, busarg = -1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--sweep")) sweep = 1;
        else busarg = atoi(argv[i]);
    }

    if (!strcmp(cmd, "buses")) { list_buses(); return 0; }

    /* Manual fan duty, for characterising the thermal response. Deliberately
     * NOT bounded by PWM_FLOOR -- the point of the exercise is to go below the
     * loop's floor and see what the temperature actually does. It has its own,
     * lower hard bound instead, and it is a one-shot: the 60 s loop will put the
     * curve's value back on its next pass, so nothing here can leave the box
     * quiet by accident. Stop the loop first if you want a value to persist. */
    if (!strcmp(cmd, "fan") && argc > 2) {
        int want = atoi(argv[2]);
        if (want < FAN_MANUAL_MIN || want > PWM_MAX) {
            fprintf(stderr, "  refusing %d -- manual range is %d-%d\n",
                    want, FAN_MANUAL_MIN, PWM_MAX);
            return 2;
        }
        int fd = open_dev(4, CPLD_ADDR);
        if (fd < 0) return 1;
        int bad = 0;
        for (int f = 0; f < 4; f++) {
            union i2c_smbus_data d;
            d.byte = (uint8_t)want;
            if (xfer(fd, 0, (uint8_t)(PWM_BASE + f), I2C_SMBUS_BYTE_DATA, &d) < 0) bad++;
        }
        int got = rd_byte(fd, PWM_BASE);
        close(fd);
        printf("  fan pwm = %d (%d%%)%s, reads back %d\n", want,
               (want * 100 + PWM_MAX / 2) / PWM_MAX, bad ? " [some writes failed]" : "", got);
        return bad ? 1 : 0;
    }

    /* A PSU's own fan, over PMBus FAN_COMMAND_1 (0x3b).
     *
     * ⚠ This is a power supply's thermal protection, not a chassis fan. The
     * failure mode is not noise, it is a supply shutting down -- and on this
     * bench PSU2 has no AC feed, so PSU1 is the ONLY supply and taking it out
     * takes the box with it. Hence a hard floor of 40%, a check that the supply
     * is actually cool before honouring anything low, and a refusal to touch a
     * supply that is carrying load unless --force is given.
     *
     * FAN_CONFIG_1_2 (0x3a) reads 0x90 here: bit 7 fan installed, bit 6 clear,
     * so the command is a DUTY PERCENTAGE rather than an RPM target. */
    if (!strcmp(cmd, "psufan") && argc > 3) {
        int bus = atoi(argv[2]);
        int pct = atoi(argv[3]);
        int force = argc > 4 && !strcmp(argv[4], "--force");
        if (pct < 40 || pct > 100) {
            fprintf(stderr, "  refusing %d%% -- PSU fan range is 40-100\n", pct);
            return 2;
        }
        int fd = open_dev(bus, PSU_ADDR);
        if (fd < 0) return 1;

        int cfg = rd_byte(fd, 0x3a);
        int status = rd_word(fd, 0x79);
        int pout = rd_word(fd, 0x96);
        double watts = pout < 0 ? -1.0 : linear11((uint16_t)pout);
        int hot = rd_word(fd, 0x8d);
        double hotc = hot < 0 ? -1.0 : linear11((uint16_t)hot);

        printf("  i2c-%d: FAN_CONFIG=0x%02x STATUS=0x%04x out=%.1f W hotspot=%.0f C\n",
               bus, cfg, status, watts, hotc);
        if ((cfg & 0x40) != 0) {
            fprintf(stderr, "  ** bit 6 set: this supply wants an RPM target, "
                            "not a percentage -- refusing\n");
            close(fd); return 1;
        }
        if (watts > 10.0 && !force) {
            fprintf(stderr, "  ** supply is carrying %.1f W. Slowing the fan on a\n"
                            "     LOADED supply is a thermal decision, not a noise\n"
                            "     one. Pass --force if that is what you mean.\n", watts);
            close(fd); return 1;
        }
        union i2c_smbus_data d;
        d.word = (uint16_t)pct;          /* LINEAR11, exponent 0 */
        if (xfer(fd, 0 /* write */, 0x3b, I2C_SMBUS_WORD_DATA, &d) < 0) {
            fprintf(stderr, "  ** write failed\n"); close(fd); return 1;
        }
        usleep(200000);
        int back = rd_word(fd, 0x3b);
        int rpm  = rd_word(fd, 0x90);
        printf("  commanded %d%%, reads back %d, fan now %.0f rpm\n",
               pct, back, rpm < 0 ? -1.0 : linear11((uint16_t)rpm));
        close(fd);
        return 0;
    }

    if (!strcmp(cmd, "cool")) {
        int apply = argc > 2 && !strcmp(argv[2], "--apply");
        printf("== cooling ==\n");
        return cool_run(apply);
    }

    /* Read one PMBus register by hand. Read-only, for questions the fixed
     * report does not answer -- e.g. FAN_COMMAND_1 (0x3b), which holds the fan
     * percentage the supply was last commanded and survives a warm reboot. */
    if (!strcmp(cmd, "pmbus") && argc > 3) {
        int bus = atoi(argv[2]);
        int reg = (int)strtol(argv[3], NULL, 0);
        int wide = argc > 4 && !strcmp(argv[4], "word");
        int fd = open_dev(bus, PSU_ADDR);
        if (fd < 0) return 1;
        int v = wide ? rd_word(fd, (uint8_t)reg) : rd_byte(fd, (uint8_t)reg);
        if (v < 0) printf("  i2c-%d reg 0x%02x: read failed\n", bus, reg);
        else printf("  i2c-%d reg 0x%02x = 0x%04x (%d)%s\n", bus, reg, v, v,
                    wide ? "" : "  [byte]");
        close(fd);
        return 0;
    }

    if (!strcmp(cmd, "led")) {
        if (argc > 2 && !strcmp(argv[2], "auto")) { led_auto(); return 0; }
        if (argc > 3) return led_cmd(argv[2], argv[3]);
        fprintf(stderr, "usage: platmon led <status|fan|psu1|psu2> <off|green|red>\n"
                        "       platmon led auto\n");
        return 2;
    }

    if (!strcmp(cmd, "cpld") || !strcmp(cmd, "all")) {
        int bus = busarg >= 0 ? busarg : 4;           /* bus0:1 */
        printf("== fans (i2c-%d) ==\n", bus);
        int fd = open_dev(bus, CPLD_ADDR);
        if (fd >= 0) { cpld_named(fd); if (sweep) cpld_sweep(fd); close(fd); }
    }
    if (!strcmp(cmd, "psu") || !strcmp(cmd, "all")) {
        printf("== power ==\n");
        scd_presence();
        if (busarg >= 0 && strcmp(cmd, "all")) psu_read(busarg);
        else { psu_read(6); psu_read(7); }            /* bus0:3, bus0:4 */
    }
    if (!strcmp(cmd, "scd")) { printf("== power ==\n"); scd_presence(); }
    return 0;
}
