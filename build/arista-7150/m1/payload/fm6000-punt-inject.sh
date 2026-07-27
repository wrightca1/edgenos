#!/bin/sh
# fm6000-punt-inject.sh - "path 1" CPU-punt byte-mover: inject a special-delivery
# frame against the microcode's catch-all GLORT and see it return on the RX ring,
# with NO wedge-prone table writes (skips fm6000_l2_probe entirely).
#
# Premise (arista notes phase44 + eos-golden capture): the vendor microcode load
# establishes the catch-all GLORT_CAM[0]=0x007fffff -> RAM DMaskBaseIdx=1 ->
# L2F_TABLE_256[1]={CPU bit0, Et1 bit40}, so ANY DGLORT resolves to the CPU port.
# The F64 tag now goes in the BD F64 field (edgenos d814994), so the fabric sees a
# real special-delivery frame. Inject is non-destructive -> we sweep dglort+endian.
#
# Prereqs (staged on /mnt/flash by the EOS side, copied to /tmp here):
#   ucode_l2.raw ucode_tail.raw fpdma_probe fm6000dma.ko
# Assumes init-m1 already ran FM6000_BIST=1 bring-up (BIST + enum). Run AFTER boot.
# Leaves the watchdog ARMED (any wedge -> 30s powercycle to EOS). Run with 'disarm'
# when done. SPDX-License-Identifier: GPL-2.0-or-later
B=0000:02:00.0
PATH=/tmp:/usr/bin:$PATH; export PATH
R(){ fm6000reg $B $1 2>/dev/null | grep -o '[0-9a-f]*$'; }
WD(){ scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1; }
WDFILE=/tmp/.fm6000_wd_rearmer.pid

if [ "${1:-}" = "disarm" ]; then
    [ -f "$WDFILE" ] && kill "$(cat $WDFILE)" 2>/dev/null; rm -f "$WDFILE"
    scdreg 0x0120 0x0 >/dev/null 2>&1; echo "watchdog disarmed ($(scdreg 0x0120))"; exit 0
fi

echo "== arm SCD watchdog + background re-armer (30s powercycle safety) =="
( while :; do WD; sleep 10; done ) & echo $! > "$WDFILE"
sleep 1; echo "  wd=$(scdreg 0x0120 | grep -o '0x[0-9a-f]*$')"

echo "== stage payload from /mnt/flash -> /tmp =="
for f in ucode_l2.raw ucode_tail.raw fpdma_probe fm6000dma.ko; do
    [ -f /tmp/$f ] || cp /mnt/flash/$f /tmp/$f 2>/dev/null
    [ -f /tmp/$f ] || { echo "  MISSING /tmp/$f (stage it on /mnt/flash first)"; exit 1; }
done
chmod +x /tmp/fpdma_probe

[ -e /sys/bus/pci/devices/$B/vendor ] || { echo "FM6000 not enumerated - run init BIST bring-up first"; exit 1; }
echo "  FM6000 $(cat /sys/bus/pci/devices/$B/vendor):$(cat /sys/bus/pci/devices/$B/device)  GLORT_CAM0=0x$(R 0x0E000)"

echo "== load L2 microcode (fast) + danger-zone tail (paced) =="
V=$(pcicfg $B 0x04 | grep -o '0x[0-9a-f]*$'); pcicfg $B 0x04 $(printf '0x%x' $(( V | 0x6 ))) >/dev/null 2>&1; WD
for cmd in 1 2 3; do
    fm6000reg $B 0x1C022 $cmd >/dev/null 2>&1
    n=0; while [ $n -lt 50 ] && [ $(( 0x$(R 0x1C022) & 0x10 )) -eq 0 ]; do sleep 0.1 2>/dev/null || sleep 1; n=$((n+1)); done
    WD
done
fm6000reg $B 0x00009 0x0 >/dev/null 2>&1; sleep 1; WD
fm6000load $B /tmp/ucode_l2.raw >/dev/null 2>&1; WD
echo "  L2 loaded: GLORT_CAM0=0x$(R 0x0E000) (want 0x007fffff)"
n=0
while read a v; do
    n=$((n+1)); fm6000reg $B 0x$a 0x$v >/dev/null 2>&1
    [ $((n % 500)) -eq 0 ] && WD
done < /tmp/ucode_tail.raw
WD
echo "  tail paced: $n lines  L2F_PROFILE0=0x$(R 0x1A1000) (want 0x0008180a)"

echo "== verify catch-all DMASK: GLORT_CAM0 + RAM0 + L2F_256[1] =="
CAM0=0x$(R 0x0E000); RAM0=0x$(R 0x0E800)
D10=0x$(R 0x1A0004); D11=0x$(R 0x1A0005)
echo "  CAM0=$CAM0 RAM0=$RAM0  L2F_256[1]={w0=$D10, w1=$D11} (want {0x1,0x100})"
if [ "$D10" != "0x1" ]; then
    echo "  microcode did NOT set L2F_256[1] — writing it (CPU bit0 + Et1 bit40); L2F_256 is BIST-parity-safe"
    fm6000reg $B 0x1A0004 0x00000001 >/dev/null 2>&1   # w0: CPU port bit0
    fm6000reg $B 0x1A0005 0x00000100 >/dev/null 2>&1   # w1: Et1 bit40
    fm6000reg $B 0x1A0006 0x00000000 >/dev/null 2>&1   # w2
    WD; echo "  now L2F_256[1]={w0=0x$(R 0x1A0004), w1=0x$(R 0x1A0005)}"
fi

echo "== bring up DMA rings + inject sweep (non-destructive) =="
lsmod 2>/dev/null | grep -q fm6000dma || insmod /tmp/fm6000dma.ko 2>/dev/null || modprobe fm6000dma 2>/dev/null
[ -e /dev/fm6000dma ] || echo "  WARN: /dev/fm6000dma absent (kmod load failed?)"
for spec in "0xff00" "0x0001" "0xff00 swap"; do
    echo "--- fpdma_probe tx $spec ---"
    /tmp/fpdma_probe tx $spec 2>&1 | grep -iE 'ENUMERAT|tx desc|tx_reclaim|total RX frames|\*\*\* RX|FAILED|queued'
    WD
done
echo "== DONE. Watchdog STILL ARMED. Re-run 'fm6000-punt-inject.sh disarm' when finished. =="
