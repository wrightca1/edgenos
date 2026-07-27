#!/bin/sh
# fm6000-punt-inject.sh - CORRECTED CPU-punt byte-mover (arista notes phase45).
#
# Corrected model: the microcode does NOT set a catch-all GLORT or the L2F_256
# DMASK (EOS software does). GLORT_CAM (1024 entries, HIGHEST-match wins, entry 0
# is a HW-forced catch-all), GLORT_RAM and L2F_256 are left parity-invalid by the
# microcode, so a lookup that reads them faults -> the config wedge. Fix
# (datasheet step-12): CRM Memory-Set init those tables, THEN point the always-on
# entry-0 catch-all at a CPU-port DMASK. F64 tag goes in the BD (edgenos d814994).
#
# Phases (mode arg limits how far it goes; default 'crm' = stop after CRM validate):
#   ucode : BIST(done by init) -> load L2 + noMOD-tail microcode
#   crm   : + CRM Memory-Set init GLORT_CAM/RAM + L2F_256  (validate, no config)
#   cfg   : + program entry-0 catch-all: GLORT_RAM[0]->DMaskBaseIdx=1, L2F_256[1]=CPU
#   full  : + insmod DMA kmod, inject special-delivery sweep, poll RX
#
# Prereqs streamed to /tmp: ucode_l2.raw ucode_tail_nomod.raw fm6000_crm
#   (+ for full: fpdma_probe fm6000dma.ko). External WD re-armer MUST be running
#   (a held-open SSH petting scdreg 0x0120 0xC0000BB8 every 5s) - a script-internal
#   subshell re-armer does NOT pet reliably over SSH (phase45). We also arm once here.
# SPDX-License-Identifier: GPL-2.0-or-later
B=0000:02:00.0
MODE="${1:-crm}"
PATH=/tmp:/usr/bin:$PATH; export PATH
R(){ fm6000reg $B $1 2>/dev/null | grep -o '[0-9a-f]*$'; }
WD(){ scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1; }

echo "== fm6000-punt-inject mode=$MODE (external WD re-armer expected) =="
WD; echo "  wd=$(scdreg 0x0120 | grep -o '0x[0-9a-f]*$')"

for f in ucode_l2.raw ucode_tail_nomod.raw fm6000_crm; do
    [ -f /tmp/$f ] || { echo "  MISSING /tmp/$f"; exit 1; }
done
chmod +x /tmp/fm6000_crm 2>/dev/null
[ -e /sys/bus/pci/devices/$B/vendor ] || { echo "FM6000 not enumerated"; exit 1; }
echo "  FM6000 $(cat /sys/bus/pci/devices/$B/vendor):$(cat /sys/bus/pci/devices/$B/device)"

echo "== [ucode] boot-ctrl + MSB release + L2 + noMOD-tail =="
V=$(pcicfg $B 0x04 | grep -o '0x[0-9a-f]*$'); pcicfg $B 0x04 $(printf '0x%x' $(( V | 0x6 ))) >/dev/null 2>&1; WD
for cmd in 1 2 3; do
    fm6000reg $B 0x1C022 $cmd >/dev/null 2>&1
    n=0; while [ $n -lt 50 ] && [ $(( 0x$(R 0x1C022) & 0x10 )) -eq 0 ]; do sleep 0.1 2>/dev/null || sleep 1; n=$((n+1)); done
done
fm6000reg $B 0x00009 0x0 >/dev/null 2>&1; sleep 1; WD
fm6000load $B /tmp/ucode_l2.raw >/dev/null 2>&1; WD
n=0; while read a v; do n=$((n+1)); fm6000reg $B 0x$a 0x$v >/dev/null 2>&1; [ $((n%500)) -eq 0 ] && WD; done < /tmp/ucode_tail_nomod.raw; WD
echo "  loaded L2+noMOD ($n tail lines)  GLORT_CAM0=0x$(R 0x0E000) (expect 0x37a74ed0; ucode does NOT set catch-all)  L2F_PROFILE0=0x$(R 0x1A1000) (want 0x0008180a)"
[ "$MODE" = ucode ] && { echo "== stop after ucode =="; exit 0; }

echo "== [crm] CRM Memory-Set init GLORT_CAM/RAM + L2F_256 (parity-valid) =="
WD
/tmp/fm6000_crm 2>&1 | sed 's/^/  /'
crmrc=$?
WD
echo "  post-CRM: GLORT_CAM0=0x$(R 0x0E000) GLORT_RAM0=0x$(R 0x0E800) L2F_256[1]={0x$(R 0x1A0004),0x$(R 0x1A0005)}"
[ $crmrc -ne 0 ] && { echo "  CRM FAILED (rc=$crmrc) - not proceeding to config"; exit 1; }
[ "$MODE" = crm ] && { echo "== stop after CRM validate (chip alive = CRM works) =="; exit 0; }

echo "== [cfg] program entry-0 catch-all -> CPU-port DMASK (golden-matched) =="
# GLORT_RAM[0]: DMaskBaseIdx=1 (golden RAM0=0x01c00004). Entry 0 is the HW-forced
# catch-all, so any (non-zero) DGLORT resolves here.  L2F_256[1] = CPU port bit0.
fm6000reg $B 0x0E800 0x01c00004 >/dev/null 2>&1     # GLORT_RAM[0] w0
fm6000reg $B 0x0E801 0x00000000 >/dev/null 2>&1     # GLORT_RAM[0] w1
fm6000reg $B 0x1A0004 0x00000001 >/dev/null 2>&1    # L2F_256[1] w0 = CPU port bit0
fm6000reg $B 0x1A0005 0x00000000 >/dev/null 2>&1    # L2F_256[1] w1
fm6000reg $B 0x1A0006 0x00000000 >/dev/null 2>&1    # L2F_256[1] w2
WD
echo "  GLORT_RAM0=0x$(R 0x0E800) L2F_256[1]={0x$(R 0x1A0004),0x$(R 0x1A0005)}  CAM0=0x$(R 0x0E000)"
[ "$MODE" = cfg ] && { echo "== stop after cfg (chip alive = config didn't wedge) =="; exit 0; }

echo "== [full] DMA rings + inject special-delivery sweep =="
for f in fpdma_probe fm6000dma.ko; do [ -f /tmp/$f ] || { echo "  MISSING /tmp/$f for full"; exit 1; }; done
chmod +x /tmp/fpdma_probe
lsmod 2>/dev/null | grep -q fm6000dma || insmod /tmp/fm6000dma.ko 2>/dev/null || modprobe fm6000dma 2>/dev/null
for spec in "0xff00" "0x0001" "0xff00 swap"; do
    echo "--- fpdma_probe tx $spec ---"
    /tmp/fpdma_probe tx $spec 2>&1 | grep -iE 'ENUMERAT|tx desc|tx_reclaim|total RX frames|\*\*\* RX|FAILED|queued'
    WD
done
echo "== DONE (mode=full). Stop the external WD re-armer + disarm when finished. =="
