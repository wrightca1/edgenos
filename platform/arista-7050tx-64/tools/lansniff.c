/*
 * lansniff -- who is on this wire, and what address do they think they have?
 *
 * Built for finding a device whose IP nobody knows: a PDU, a console server,
 * anything that arrives on a lab port with a factory default. Guessing subnets
 * and sweeping them is slow and can miss entirely; listening costs nothing and
 * the device usually tells you within a minute of being plugged in.
 *
 * What actually gives the answer:
 *   - gratuitous ARP        the device announcing its own address
 *   - ARP request           reveals the sender's IP and its gateway's
 *   - DHCP DISCOVER         it has no address yet, and names itself
 *   - any IPv4 broadcast    source address is the answer
 *
 * The initrd has no tcpdump and busybox has no capture applet, so this is the
 * smallest thing that answers the question. AF_PACKET, no dependencies, static.
 *
 *   lansniff <iface> [seconds]      default 60
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

static const char *ipstr(const unsigned char *p, char *buf)
{
    sprintf(buf, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return buf;
}

static void macstr(const unsigned char *m, char *buf)
{
    sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x",
            m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* Remember what we have already reported, so a chatty device does not bury a
 * quiet one. Keyed on the whole line, which is crude and sufficient. */
#define MAX_SEEN 128
static char seen[MAX_SEEN][160];
static int nseen;

static int already(const char *line)
{
    int i;
    for (i = 0; i < nseen; i++) {
        if (!strcmp(seen[i], line)) return 1;
    }
    if (nseen < MAX_SEEN) {
        snprintf(seen[nseen++], sizeof seen[0], "%s", line);
    }
    return 0;
}

static void report(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!already(line)) {
        printf("%s\n", line);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    const char *iface;
    int secs = 60, fd;
    struct sockaddr_ll sll;
    struct ifreq ifr;
    time_t end;
    unsigned long total = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: lansniff <iface> [seconds]\n");
        return 2;
    }
    iface = argv[1];
    if (argc > 2) secs = atoi(argv[2]);

    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("socket"); return 1; }

    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); return 1; }

    memset(&sll, 0, sizeof sll);
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) { perror("bind"); return 1; }

    /* Promiscuous, so we see traffic that is not addressed to us -- which is
     * most of what is useful here. */
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_PROMISC;
        (void)ioctl(fd, SIOCSIFFLAGS, &ifr);
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    printf("listening on %s for %d s\n", iface, secs);
    fflush(stdout);

    end = time(NULL) + secs;
    while (time(NULL) < end) {
        unsigned char buf[2048];
        char m1[32], a1[24], a2[24];
        struct sockaddr_ll from;
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0,
                             (struct sockaddr *)&from, &fl);
        int et;

        if (n < 14) continue;
        /* ⚠ Packet sockets see our OWN transmissions. Without this filter the
         * tool reports the tap's own Neighbor Discovery as a discovered
         * device -- which it did, and which sent us chasing a PDU that was
         * actually this interface's randomly-generated MAC. */
        if (from.sll_pkttype == PACKET_OUTGOING) continue;
        total++;
        et = (buf[12] << 8) | buf[13];
        macstr(buf + 6, m1);

        if (et == 0x0806 && n >= 42) {              /* ARP */
            int op = (buf[20] << 8) | buf[21];
            const unsigned char *sip = buf + 28, *tip = buf + 38;
            ipstr(sip, a1); ipstr(tip, a2);
            if (op == 1 && !memcmp(sip, tip, 4)) {
                report("  %s  GRATUITOUS ARP  I am %s", m1, a1);
            } else if (op == 1) {
                report("  %s  ARP who-has %s   tell %s", m1, a2, a1);
            } else if (op == 2) {
                report("  %s  ARP reply       %s is me", m1, a1);
            }
        } else if (et == 0x0800 && n >= 34) {        /* IPv4 */
            const unsigned char *ip = buf + 14;
            int ihl = (ip[0] & 0x0f) * 4;
            int proto = ip[9];
            ipstr(ip + 12, a1); ipstr(ip + 16, a2);
            if (proto == 17 && n >= 14 + ihl + 8) {
                const unsigned char *udp = ip + ihl;
                int sp = (udp[0] << 8) | udp[1], dp = (udp[2] << 8) | udp[3];
                if (sp == 68 || dp == 67) {
                    report("  %s  DHCP from %s -- no address yet, watch for the offer", m1, a1);
                    continue;
                }
            }
            report("  %s  IPv4 %s -> %s  proto %d", m1, a1, a2, proto);
        } else if (et == 0x86dd && n >= 54) {        /* IPv6 */
            /* Worth decoding even when hunting an IPv4 address: a device that
             * has not got its v4 configuration up yet still does Neighbor
             * Discovery, which proves it is alive and gives us its MAC. And a
             * link-local built the EUI-64 way carries the real MAC even when
             * the device randomises the one it puts in the Ethernet header. */
            const unsigned char *ip6 = buf + 14;
            int nh = ip6[6];
            char s6[64], d6[64];
            snprintf(s6, sizeof s6, "%02x%02x:%02x%02x::%02x%02x:%02x%02x",
                     ip6[8], ip6[9], ip6[10], ip6[11],
                     ip6[20], ip6[21], ip6[22], ip6[23]);
            snprintf(d6, sizeof d6, "%02x%02x:%02x%02x::%02x%02x:%02x%02x",
                     ip6[24], ip6[25], ip6[26], ip6[27],
                     ip6[36], ip6[37], ip6[38], ip6[39]);
            if (nh == 58 && n >= 55) {
                int t = buf[54];
                const char *w = t == 133 ? "Router Solicitation" :
                                t == 134 ? "Router Advertisement" :
                                t == 135 ? "Neighbour Solicitation" :
                                t == 136 ? "Neighbour Advertisement" :
                                t == 143 ? "MLDv2 report" : "ICMPv6";
                report("  %s  %s  src %s", m1, w, s6);
            } else if (nh == 89 && n >= 56) {
                /* OSPFv3 rides directly on IPv6. Decoding the packet TYPE is
                 * the difference between "we hear the neighbour" and knowing
                 * WHICH exchange is failing: an adjacency stuck in ExStart is
                 * hellos arriving and database-description packets not. */
                int t = buf[55];
                const char *w = t == 1 ? "Hello" :
                                t == 2 ? "DATABASE DESCRIPTION" :
                                t == 3 ? "LS Request" :
                                t == 4 ? "LS Update" :
                                t == 5 ? "LS Ack" : "type?";
                report("  %s  OSPFv3 %s  src %s", m1, w, s6);
            } else if (nh == 17) {
                report("  %s  IPv6/UDP src %s -> %s (DHCPv6?)", m1, s6, d6);
            } else {
                report("  %s  IPv6 next-header %d src %s", m1, nh, s6);
            }
        } else if (et == 0x88cc) {
            report("  %s  LLDP", m1);
        } else if (buf[0] == 0x01 && buf[1] == 0x80 && buf[2] == 0xc2) {
            report("  %s  STP/BPDU", m1);
        } else {
            /* Never stay silent about a frame we did not understand. An
             * earlier version reported only the four types above, so a frame
             * arrived and the tool printed nothing -- indistinguishable from
             * an empty wire, which is the one thing this tool must never do. */
            report("  %s  ethertype 0x%04x len %d  %02x%02x %02x%02x %02x%02x %02x%02x",
                   m1, et, (int)n, buf[14], buf[15], buf[16], buf[17],
                   buf[18], buf[19], buf[20], buf[21]);
        }
    }

    printf("done: %lu frames, %d distinct events\n", total, nseen);
    return 0;
}
