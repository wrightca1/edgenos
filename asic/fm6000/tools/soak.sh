#!/bin/bash
# soak.sh - cold-boot the switch N times per arm and record comparable results.
#
# WHY
#   Every generator in this tree was validated by a SINGLE cold boot. That is
#   enough to show a thing can work, not that it does work: Et2 is known to link
#   only intermittently, and the dataplane has a pre-existing collapse whose
#   onset varies. A one-shot pass can be luck in either direction.
#
#   This runs the same measurement repeatedly on two arms -- the generated
#   config and the stock replay -- so claims can be stated with a denominator.
#
#   NOTE: the first soak (2026-08-07) ran with a broken portd whose RX ring died
#   after ~157 packets, so its ping column was measuring that bug on both arms
#   rather than anything about the replay. Re-run after e124f98.
#
# METHOD NOTES, each learned the hard way
#   - Only COLD boots test forwarding. Re-running fm6000-fullseq.sh in place
#     gives et1 rx=0 for a good replay and a bad one alike.
#   - fullseq.log is authoritative only once FULLSEQ DONE is present AND the
#     running image is the one you installed; reading it earlier catches the
#     previous boot and produces confident nonsense.
#   - edgenos-up.sh leaves daemons holding stdout, so it must be started
#     detached or the ssh session never returns.
#   - The initramfs regenerates its host key each boot -> ssh-keygen -R.
#
# Usage: soak.sh <runs-per-arm>            e.g. soak.sh 2
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail

SW=${SW:-10.1.1.77}
PW=${PW:-arista}
SSHO="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8"
SSH="sshpass -p $PW ssh $SSHO root@$SW"
# Two images, not one edited in place: the initramfs is unpacked fresh from the
# SWI on every boot, so patching /usr/lib/... on the box does not survive a
# reboot. The stock image is the SAME build with GENBLK defaulted to 0, so the
# only variable between arms is who writes the blocks.
GEN_SWI=${GEN_SWI:-edgenos-m1-bisect.swi}
STOCK_SWI=${STOCK_SWI:-edgenos-m1-stock.swi}
RESULTS=${RESULTS:-soak-results.tsv}
N=${1:-2}

say() { echo "[soak] $*"; }
rekey() { ssh-keygen -R "$SW" >/dev/null 2>&1; }

# ⚠ ARM THE WATCHDOG BEFORE ANY MANUAL PROBING.
#
# fm6000-fullseq.sh arms it and pets it, but its EXIT trap DISARMS it again --
# so once bring-up finishes the box is unprotected, which is exactly when we
# start poking at it. A hard host hang with the watchdog disarmed has no remote
# recovery at all: it cost a physical power cycle earlier in this work, with the
# serial console emitting nothing.
#
# (Petting loops have sometimes survived as orphaned subshells, leaving the
# watchdog armed by accident. Do not rely on that -- check and arm.)
arm_watchdog() {
	timeout 30 $SSH '
		pgrep -f "scdreg 0x0120" >/dev/null 2>&1 && exit 0
		scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1
		setsid sh -c "while :; do scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1; sleep 5; done" \
			</dev/null >/dev/null 2>&1 &
		exit 0' >/dev/null 2>&1
	local w
	w=$(timeout 20 $SSH 'scdreg 0x0120 2>/dev/null | sed "s/.*= //"' 2>/dev/null | tail -1)
	say "    watchdog: $w  petters: $(timeout 20 $SSH 'pgrep -fc "scdreg 0x0120"' 2>/dev/null | tail -1)"
}

boot() {   # $1 = expected version substring
	$SSH "echo 4 > /mnt/flash/edgenos-sticky
	      printf 'SWI=flash:/%s\n' '$2' > /mnt/flash/boot-config; sync" >/dev/null 2>&1
	sleep 2
	$SSH '(sleep 2; reboot -f) >/dev/null 2>&1 &' >/dev/null 2>&1
	sleep 50; rekey
	local d=$((SECONDS + 600))
	while [ $SECONDS -lt $d ]; do
		local r
		r=$(timeout 10 $SSH 'V=$(grep -o "0\.3\.0-[a-z0-9]*" /etc/edgenos/version.json 2>/dev/null)
		                     D=$(grep -c "FULLSEQ DONE" /mnt/flash/fullseq.log 2>/dev/null)
		                     echo "$V|$D"' 2>/dev/null | tail -1)
		case "$r" in *"$1|1"*) return 0;; esac
		sleep 12; rekey
	done
	return 1
}

measure() {
	timeout 240 $SSH 'setsid sh /usr/lib/edgenos/platform/edgenos-up.sh >/tmp/up.log 2>&1 </dev/null & exit 0' >/dev/null 2>&1
	sleep 150
	timeout 120 $SSH '
		B=0000:02:00.0
		E1=$(fm6000reg $B 0xe3800 2>/dev/null | sed "s/.*= //")
		E2=$(fm6000reg $B 0xe4000 2>/dev/null | sed "s/.*= //")
		R=$(ip route 2>/dev/null | wc -l)
		X=$(grep -oE "et1 rx=[0-9]+" /tmp/up.log 2>/dev/null | tail -1 | cut -d= -f2)
		F=$(grep -oE "programmed [0-9]+ route" /tmp/up.log 2>/dev/null | grep -oE "[0-9]+")
		P=""
		for i in 1 2 3 4 5 6; do
			L=$(ping -c 10 -W 2 10.101.101.25 2>&1 | grep -oE "[0-9]+% packet" | grep -oE "[0-9]+")
			P="$P${L:-?},"
		done
		R2=$(ip route 2>/dev/null | wc -l)
		echo "$E1 $E2 $R $R2 ${X:-0} ${F:-0} $P"
	' 2>/dev/null | tail -1
}

printf 'arm\trun\tEt1\tEt2\troutes\troutes_end\trx\tfib\tping%%\n' > "$RESULTS"

for arm in generated stock; do
	for r in $(seq 1 "$N"); do
		say "=== $arm run $r/$N ==="
		$SSH 'cp /mnt/flash/fwd4.orig.txt /mnt/flash/fwd4.txt; sync' >/dev/null 2>&1
		if [ "$arm" = generated ]; then SWI=$GEN_SWI; VER=tranche2
		else                            SWI=$STOCK_SWI; VER=stock; fi
		if ! boot "$VER" "$SWI"; then
			say "    did not come back"
			printf '%s\t%d\tNO-BOOT\n' "$arm" "$r" >> "$RESULTS"; continue
		fi
		arm_watchdog
		M=$(measure)
		say "    $M"
		printf '%s\t%d\t%s\n' "$arm" "$r" "$(echo $M | tr ' ' '\t')" >> "$RESULTS"
	done
done

say "results:"
column -t "$RESULTS"
