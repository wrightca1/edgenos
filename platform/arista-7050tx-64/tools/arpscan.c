/*
 * arpscan -- find a device whose IP nobody knows, on a wire we control.
 *
 * A PDU, console server or IP KVM arrives with a factory default address and
 * says nothing until spoken to, so passive listening finds it only if it
 * happens to ARP for a gateway. ARP is link-local and does not care about our
 * own addressing: we can ask "who has X" for any X on this wire and anything
 * that owns X answers, even if X is in a subnet we are not configured for.
 *
 * Sender protocol address is 0.0.0.0 by default -- an ARP probe, RFC 5227 --
 * so the scan cannot claim an address or poison anyone's cache. Pass a source
 * if a device is fussy enough to ignore probes.
 *
 *   arpscan <iface> <a.b.c.d> <a.b.c.d> [src-ip]      inclusive range
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <errno.h>

int main(int argc, char **argv)
{
    const char *iface;
    unsigned int first, last, ip;
    unsigned char src[4] = {0,0,0,0};
    unsigned char mac[6];
    int fd, ifindex, found = 0;
    unsigned long sent = 0;
    int senderr = 0;
    struct ifreq ifr;
    struct sockaddr_ll sll;
    time_t end;

    if (argc < 4) {
        fprintf(stderr, "usage: arpscan <iface> <first-ip> <last-ip> [src-ip]\n");
        return 2;
    }
    iface = argv[1];
    first = ntohl(inet_addr(argv[2]));
    last  = ntohl(inet_addr(argv[3]));
    if (argc > 4) {
        unsigned int s = inet_addr(argv[4]);
        memcpy(src, &s, 4);
    }
    if (last < first) { fprintf(stderr, "range is backwards\n"); return 2; }

    fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ARP));
    if (fd < 0) { perror("socket"); return 1; }

    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); return 1; }
    ifindex = ifr.ifr_ifindex;
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) { perror("SIOCGIFHWADDR"); return 1; }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);

    memset(&sll, 0, sizeof sll);
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ARP);
    sll.sll_ifindex = ifindex;
    sll.sll_halen = 6;
    memset(sll.sll_addr, 0xff, 6);
    if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) { perror("bind"); return 1; }

    /* Non-blocking drain. An earlier version waited 20 ms per probe for a
     * reply that is not synchronous anyway, which capped the scan at ~42
     * addresses/sec and made anything wider than a /24 impractical. Replies
     * are collected opportunistically and again at the end. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    printf("probing %s .. %s on %s\n", argv[2], argv[3], iface);
    fflush(stdout);

    for (ip = first; ip <= last; ip++) {
        unsigned char f[42];
        unsigned int nip = htonl(ip);
        unsigned char rx[256];
        ssize_t n;

        memset(f, 0, sizeof f);
        memset(f, 0xff, 6);                 /* broadcast */
        memcpy(f + 6, mac, 6);
        f[12] = 0x08; f[13] = 0x06;         /* ARP */
        f[14] = 0x00; f[15] = 0x01;         /* Ethernet */
        f[16] = 0x08; f[17] = 0x00;         /* IPv4 */
        f[18] = 6; f[19] = 4;
        f[20] = 0x00; f[21] = 0x01;         /* request */
        memcpy(f + 22, mac, 6);
        memcpy(f + 28, src, 4);
        memcpy(f + 38, &nip, 4);
        /* sendto, not send: a SOCK_RAW packet socket needs the link address
         * for every frame even when bound. And never swallow the error -- an
         * earlier version used send() and discarded the result, so the scan
         * transmitted nothing and reported "0 answered", which reads exactly
         * like an empty wire. */
        if (sendto(fd, f, sizeof f, 0, (struct sockaddr *)&sll, sizeof sll) < 0) {
            if (senderr++ < 3) perror("  sendto");
        } else {
            sent++;
        }

        /* Pace so we do not outrun a 100M link or the far end's ARP handling.
         * 200 us is ~5000/sec, about 2.5 Mbps of 64-byte frames. */
        if ((ip & 0x3f) == 0) usleep(2000);

        /* Drain whatever has arrived so far; replies are not synchronous. */
        while ((n = recv(fd, rx, sizeof rx, MSG_DONTWAIT)) >= 42) {
            int op = (rx[20] << 8) | rx[21];
            if (op != 2) continue;
            printf("  FOUND  %u.%u.%u.%u  is at  %02x:%02x:%02x:%02x:%02x:%02x\n",
                   rx[28], rx[29], rx[30], rx[31],
                   rx[22], rx[23], rx[24], rx[25], rx[26], rx[27]);
            fflush(stdout);
            found++;
        }
    }

    /* Late replies, especially from something slow on a half-duplex link. */
    end = time(NULL) + 3;
    while (time(NULL) < end) {
        unsigned char rx[256];
        ssize_t n = recv(fd, rx, sizeof rx, MSG_DONTWAIT);
        if (n < 0) { usleep(20000); continue; }
        if (n >= 42 && ((rx[20] << 8) | rx[21]) == 2) {
            printf("  FOUND  %u.%u.%u.%u  is at  %02x:%02x:%02x:%02x:%02x:%02x\n",
                   rx[28], rx[29], rx[30], rx[31],
                   rx[22], rx[23], rx[24], rx[25], rx[26], rx[27]);
            fflush(stdout);
            found++;
        }
    }

    printf("done: %lu probes sent, %d answered\n", sent, found);
    return 0;
}
