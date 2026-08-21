#!/bin/sh
# fm6000-status.sh -- read live EPL port/lane state on the 7150. Runs ON the switch.
#
# Until this existed, every link reading came out of /mnt/flash/fullseq.log, which
# is written once per boot and truncated by the next one. Two dark traces were lost
# that way. This reads the chip directly, any time, without a reboot.
#
# Addressing. The FM6000 register space is indexed in 32-bit WORDS -- fm6000_hw.c's
# rd()/wr() do M[w] on a uint32_t*. devmem takes BYTES, so byte = BAR0 + word*4.
# BAR0 is read from sysfs rather than hardcoded; it has moved between boots before.
#
#   PORT_STATUS  +0x00   b6 RxLinkUp  b7 HeartbeatOk  b8 HiBer  b9 Tx  b10 Rx  b11 SerXmit
#                        b0-5 LinkFault fields -- these flicker on a healthy port
#   LINK_IM      +0x02   link interrupt mask
#   LINK_IP      +0x04   link interrupt pending (write 1 to clear)
#   pcsRx        +0x26
#   SERDES_RX_CFG +0x39  b25 RxPolarityInvEn
#   SERDES_TX_CFG +0x3a  b8-11 TxOutputEqPost  b12-14 TxOutputEqPre  b30 TxPolarityInvEn
#   LANE_STATUS  +0x38   b6 block lock  b7-12 RxRate.  0x940 == locked.
#
# A lane is up when LANE_STATUS reads 0x940. PORT_STATUS alone is not enough, and
# neither is the TAP interface's carrier -- et2 has reported carrier=1 with the
# lane dark, which is why transit-test.sh reads PORT_STATUS instead.
#
# usage: fm6000-status.sh [port ...]   (front-panel numbers; default 1 2 3)

set -u
BDF=${BDF:-0000:02:00.0}
BAR=$(sed -n '1s/^\(0x[0-9a-f]*\).*/\1/p' "/sys/bus/pci/devices/$BDF/resource" 2>/dev/null)
[ -n "${BAR:-}" ] || { echo "no BAR0 for $BDF" >&2; exit 1; }

rdw() { devmem $((BAR + $1 * 4)) 32; }

# front-panel -> EPL, lane. From FM6000_SERDES_PORTS[] (asic/fm6000/fm6000_serdes_ports.h).
map() {
	case $1 in
	1) echo "14 0";; 2) echo "16 0";; 3) echo "14 1";; 4) echo "16 1";;
	5) echo "14 2";; 6) echo "16 2";; 7) echo "14 3";; 8) echo "16 3";;
	*) echo "";;
	esac
}

for p in ${*:-1 2 3}; do
	set -- $(map "$p")
	[ $# -eq 2 ] || { echo "port $p: no mapping" >&2; continue; }
	base=$((0xE0000 + 0x400 * $1 + 0x80 * $2))
	ps=$(rdw $base)
	ls=$(rdw $((base + 0x38)))
	# ⚠ LANE_STATUS 0x940 is NECESSARY BUT NOT SUFFICIENT. A lane can be locked
	# and still carry no traffic: PORT_STATUS bit 8 (HiBer) set and pcsRx != 1 is
	# a high-bit-error lock, which forwards nothing. Measured 2026-08-18: Et2 at
	# LANE_STATUS=0x940 for 16/16 samples, PORT_STATUS=0x09D5, pcsRx=0x67, rx=0,
	# transit dead. Scoring "up" on LANE_STATUS alone counted that as a link.
	hiber=$(( ($(printf %d "$ps") >> 8) & 1 ))
	up=dark
	if [ "$ls" = "0x00000940" ]; then
		if [ "$hiber" = 1 ]; then up="LOCKED-HiBer(no traffic)"
		else up=LOCKED; fi
	fi
	printf 'port %s  EPL%-2s lane %s  base 0x%05x  %s\n' "$p" "$1" "$2" "$base" "$up"
	printf '   PORT_STATUS=%s  LANE_STATUS=%s  pcsRx=%s\n' "$ps" "$ls" "$(rdw $((base + 0x26)))"
	printf '   TX_CFG=%s  RX_CFG=%s  LINK_IM=%s  LINK_IP=%s\n' \
		"$(rdw $((base + 0x3a)))" "$(rdw $((base + 0x39)))" \
		"$(rdw $((base + 0x02)))" "$(rdw $((base + 0x04)))"
done
