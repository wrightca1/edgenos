/* fm6000_cap - minimal AF_PACKET frame capture. The 7150 has no tcpdump and no
 * python, so this is the only way to see what reaches the CPU.
 *
 * usage: fm6000_cap <ifname> <count> [timeout_sec]
 *        prints "RX|TX len=N <first 64 bytes as hex>" per frame
 *
 * ★ WHY THIS EXISTS. Two blocks (A4 MOD, B1 FFU ByteMux) were recorded as
 * blocked on "no capture path". For INGRESS blocks that was wrong, and this
 * tool plus one observation unblocks them:
 *
 *     stimulus   ping -s <size> -p <hexbyte> <peer>
 *                the peer ECHOES the payload, so bytes we choose arrive back
 *                through the ASIC ingress pipeline. -s controls L3_LENGTH,
 *                which is ByteMux channel 18 -- a field the FFU actually
 *                selects. -p controls payload bytes.
 *     observable this tool: did the frame reach the CPU, and with what content
 *
 * That is a complete loop for parser / mapper / FFU / L3AR: an input we choose
 * and an outcome we can see. No peer access and no port mirror required.
 *
 * ⚠ IT DOES NOT COVER EGRESS. AF_PACKET shows a TX frame as the KERNEL handed it
 * to the driver -- before the ASIC touches it -- so MOD's egress edits are still
 * invisible here. CPU-originated frames also carry the next-hop MAC already
 * resolved in software, so they do not exercise MOD's rewrite at all. A4 still
 * needs a TX mirror to the CPU port or a capture host on the segment.
 *
 * ⚠ busybox ping quirks, both of which cost a run: -p takes ONE hex byte
 * ("-p a5"), not a hex string -- "-p a5a5a5a5" is parsed as 0xa5a5a5a5 and
 * rejected as "not in 0..255". And start the capture BEFORE the ping; a
 * backgrounded ping in a subshell races the socket bind.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
	unsigned char buf[2048];
	struct sockaddr_ll sll;
	struct ifreq ifr;
	struct timeval tv;
	int fd, n, i, got = 0;
	long want = argc > 2 ? atol(argv[2]) : 4;
	long secs = argc > 3 ? atol(argv[3]) : 10;

	if (argc < 2) { fprintf(stderr, "usage: fmcap <if> [count] [secs]\n"); return 2; }
	fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	if (fd < 0) { perror("socket"); return 1; }
	memset(&ifr, 0, sizeof ifr);
	strncpy(ifr.ifr_name, argv[1], IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); return 1; }
	memset(&sll, 0, sizeof sll);
	sll.sll_family = AF_PACKET;
	sll.sll_protocol = htons(ETH_P_ALL);
	sll.sll_ifindex = ifr.ifr_ifindex;
	if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) { perror("bind"); return 1; }
	tv.tv_sec = secs; tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

	while (got < want) {
		struct sockaddr_ll from;
		socklen_t fl = sizeof from;
		n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				printf("timeout after %d frames\n", got);
				break;
			}
			perror("recvfrom"); return 1;
		}
		printf("%s len=%d ", from.sll_pkttype == PACKET_OUTGOING ? "TX" : "RX", n);
		for (i = 0; i < n && i < 64; i++) printf("%02x", buf[i]);
		printf("\n");
		got++;
	}
	return 0;
}
