#!/bin/sh
# fm6000-pcie-bist-init.sh - pre-enum bring-up WITH cold-BIST memory-init.
#
# fm6000-pcie-init.sh + the recovered fm6000BistMemoryInit sequence inserted in the
# scan-mode window (after SPI-ROM boot, before normal-operating-mode), all over the
# FM6000 mgmt i2c slave (i2c "master 0 bus 2", addr 0x40). BIST marches every table
# RAM parity-valid so post-enum CPU writes to MCAST/MOD/L2F don't fault the block.
# BIST values from libFocalpointSDK.so @0x34bb94 (cross-checked vs the EOS capture).
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
A=0x40
BUS=""
for d in /sys/class/i2c-dev/i2c-*; do
  case "$(cat "$d/name" 2>/dev/null)" in *"master 0 bus 2") BUS=$(basename "$d"|sed s/i2c-//);; esac
done
[ -n "$BUS" ] || { echo "[pbi] ERROR: FM6000 slave 'master 0 bus 2' not found (register accel#0)"; exit 1; }
echo "[pbi] FM6000 mgmt slave on i2c-$BUS"
hx(){ printf '0x%02x' $1; }
WR(){ a=$1; d=$2
  i2ctransfer -y $BUS w7@$A $(hx $(((a>>16)&255))) $(hx $(((a>>8)&255))) $(hx $((a&255))) \
    $(hx $(((d>>24)&255))) $(hx $(((d>>16)&255))) $(hx $(((d>>8)&255))) $(hx $((d&255))) >/dev/null 2>&1; }
RD(){ i2ctransfer -y $BUS w3@$A $(hx $((($1>>16)&255))) $(hx $((($1>>8)&255))) $(hx $(($1&255))) r4@$A 2>/dev/null | tr -d ' ' | sed 's/0x//g'; }

# wait for SPI-ROM boot (BOOT_CTRL 0x1C022 bit5)
n=0; while [ $n -lt 20 ]; do v=$(RD 0x1C022); v=${v:-0}; [ $(( 0x$v & 0x20 )) -ne 0 ] && break; sleep 1; n=$((n+1)); done
echo "[pbi] boot done (BOOT_CTRL=0x$v)"

# ===== cold-BIST memory init (scan mode) =====
echo "[pbi] BIST: scan/PLL setup"
WR 0x1C022 0x0
WR 0x1C03A 0x00000063; sleep 1
WR 0x1C03A 0x80000063; sleep 1
WR 0x1C03A 0x88D55555; sleep 1
WR 0x1C03A 0x88009555; sleep 1
echo "[pbi] BIST: BM march table (0x1D080/0x1D708)"
WR 0x1D080 0x6529EDA9; WR 0x1D081 0x9B8ED9B1; WR 0x1D082 0xEFCA952B; WR 0x1D083 0x000FCA99
WR 0x1D708 0x6529EDA9; WR 0x1D709 0x9B8ED9B1; WR 0x1D70A 0xEFCA952B; WR 0x1D70B 0x000FCA99
echo "[pbi] BIST: per-block enables + config"
WR 0x1D210 0x200000; WR 0x1D290 0x200000; WR 0x1D310 0x200000; WR 0x1D390 0x200000
WR 0x1D400 0x200000; WR 0x1D480 0x200000; WR 0x1D500 0x200000; WR 0x1D580 0x200000; WR 0x1D600 0x200000
WR 0x1D218 0xB4; WR 0x1D298 0xB4; WR 0x1D318 0xB4; WR 0x1D398 0xB4
WR 0x1D241 0x4; WR 0x1D2C1 0x4; WR 0x1D261 0x4; WR 0x1D281 0x4; WR 0x1D2A1 0x4
WR 0x1D404 0xC; WR 0x1D484 0xC; WR 0x1D504 0xC; WR 0x1D584 0xC
WR 0x1D604 0x4
WR 0x1D440 0x1; WR 0x1D4C0 0x1; WR 0x1D4E0 0x1; WR 0x1D540 0x1; WR 0x1D5C0 0x1; WR 0x1D5E0 0x1; WR 0x1D640 0x1; WR 0x1D660 0x1
WR 0x1D409 0xFFF; WR 0x1D489 0x7FFF; WR 0x1D509 0x3FFF; WR 0x1D589 0xFFF; WR 0x1D609 0x3FF
WR 0x1D441 0x4; WR 0x1D4C1 0x4; WR 0x1D4E1 0x4; WR 0x1D541 0x4; WR 0x1D5C1 0x6; WR 0x1D5E1 0x6; WR 0x1D641 0xA; WR 0x1D661 0xA
WR 0x1D220 0x3; WR 0x1D2A0 0x3; WR 0x1D320 0x3; WR 0x1D3A0 0x3
WR 0x1D40B 0x0; WR 0x1D48B 0x2; WR 0x1D50B 0x2; WR 0x1D58B 0x2; WR 0x1D60B 0x0
echo "[pbi] BIST: fixed 6s march settle (i2c reads unreliable in scan mode)"
sleep 6
echo "[pbi] BIST done (BM_STATUS=0x$(RD 0x1D08E) result 0x1D70E=0x$(RD 0x1D70E))"

# ===== normal operating mode + PCIe SerDes (fm6000-pcie-init.sh tail) =====
echo "[pbi] normal operating mode"
WR 0x1C03A 0x88800000; sleep 1
WR 0x1C03A 0x88008000; sleep 1
WR 0x1C03A 0x80000040
WR 0x1C03B 0xFFFFFFFF
WR 0x1C045 0x3; sleep 1
echo "[pbi] fmPlatformSetupPCIe (SerDes lanes on)"
sleep 1
WR 0x00009 0x17
WR 0x0F000 0x0; sleep 1
WR 0x00004 0x1
WR 0x00009 0x16
WR 0x0F002 0x4
WR 0x0F001 0x0
WR 0x0F001 0x121FE0A
WR 0x01400 0x0
WR 0x01002 0x2000000
WR 0x01418 0x35
WR 0x0140C 0xFFFFFFFF
WR 0x0140D 0xFFFFFFFF
WR 0x01435 0xF121F34
WR 0x1C002 0x3FFF
WR 0x0141D 0x1C01F
sleep 2
echo "[pbi] rescan + verify"
echo 1 > /sys/bus/pci/rescan 2>/dev/null; sleep 2
pcicfg 0000:00:04.0 retrain 2>/dev/null >/dev/null; sleep 2
if [ -e /sys/bus/pci/devices/0000:02:00.0/vendor ]; then
  echo "[pbi] *** FM6000 ENUMERATED (BIST'd): $(cat /sys/bus/pci/devices/0000:02:00.0/vendor):$(cat /sys/bus/pci/devices/0000:02:00.0/device) ***"
else
  echo "[pbi] FM6000 absent - retry (Si5338 reprogram + 0x4010<=0x6)"
fi
