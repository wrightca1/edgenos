#!/bin/sh
# fm6000-pcie-init.sh - bring the FM6000 PCIe endpoint up to enumeration, WORKING/live-proven 2026-07-25.
#
# THE breakthrough: the FM6000 SPI-ROM boot loads config + locks the main PLL but STOPS SHORT of putting the
# chip in NORMAL OPERATING MODE and setting up the PCIe SerDes. EOS's FocalPointV2 (fm6000PrebootSwitch +
# fmPlatformSetupPCIe) finishes it PRE-enumeration over the sideband. We replicate it over the FM6000
# management I2C slave (SCD master0/bus2 = i2c-10, addr 0x40 - responds only while the chip is out of reset).
#
# All values are LITERAL (no read-modify-write): the slave dies once the chip leaves scan mode, so the whole
# sequence is written blind in one shot and verified via PCI enumeration on the root port.
#
# Prereqs: FM6000 out of reset (SCD 0x4000=0x100, done by fm6000-up.sh) and accel#0 registered so i2c-10 exists.
# Result (live-proven): 02:00.0 appears as 8086:155b, DLLLA=1, Gen1 x4, BAR0=32M @ 0xe2000000 (== EOS).
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
A=0x40
# FM6000 mgmt I2C slave is on SCD "master 0 bus 2" (accel#0 must be registered: smbus_master 0x8000 0 8).
BUS=""
for d in /sys/class/i2c-dev/i2c-*; do
	case "$(cat "$d/name" 2>/dev/null)" in *"master 0 bus 2") BUS=$(basename "$d"|sed s/i2c-//);; esac
done
[ -n "$BUS" ] || { echo "[pcie-init] ERROR: FM6000 slave bus 'master 0 bus 2' not found - register accel#0 (smbus_master 0x8000 0 8)"; exit 1; }
echo "[pcie-init] FM6000 mgmt slave on i2c-$BUS addr $A"
hx(){ printf '0x%02x' $1; }
WR(){ a=$1; d=$2
  i2ctransfer -y $BUS w7@$A $(hx $(((a>>16)&255))) $(hx $(((a>>8)&255))) $(hx $((a&255))) \
    $(hx $(((d>>24)&255))) $(hx $(((d>>16)&255))) $(hx $(((d>>8)&255))) $(hx $((d&255))) >/dev/null 2>&1; }
RD(){ i2ctransfer -y $BUS w3@$A $(hx $((($1>>16)&255))) $(hx $((($1>>8)&255))) $(hx $(($1&255))) r4@$A 2>/dev/null | tr -d ' ' | sed 's/0x//g'; }

# wait for the FM6000 SPI-ROM boot to finish (BOOT_CTRL 0x1C022 bit5 = EepromLoadDone) before touching it
n=0; while [ $n -lt 20 ]; do v=$(RD 0x1C022); v=${v:-0}; [ $(( 0x$v & 0x20 )) -ne 0 ] && break; sleep 1; n=$((n+1)); done
echo "[pcie-init] boot complete after ${n}s (BOOT_CTRL=0x$v; want bit5 EepromLoadDone set)"

echo "[pcie-init] normal operating mode (scan chain)"
WR 0x1C022 0x0                    # BOOT_CTRL clear
WR 0x1C03A 0x88800000; sleep 1    # SCAN_CONFIG_DATA_IN staging
WR 0x1C03A 0x88008000; sleep 1
WR 0x1C03A 0x80000040
WR 0x1C03B 0xFFFFFFFF             # SCAN_CHAIN_DATA_IN = normal operating mode (datasheet Table 4-1 Step 5)
echo "[pcie-init] enable core DLLs"
WR 0x1C045 0x3; sleep 1           # DLL_CTRL hi enable -> PLL_STAT 0x03 -> 0x0F (Locked1/2 + DllLocked1/2)
echo "[pcie-init] fmPlatformSetupPCIe (JSS release + SBus init + SerDes lanes on)"
sleep 1
WR 0x00009 0x17                  # SOFT_RESET: release JSS (bit3), keep PCIe (bit0) - SBus/SerDes dead until JSS out
WR 0x0F000 0x0                   # SBUS_CFG out of reset
sleep 1
WR 0x00004 0x1
WR 0x00009 0x16                  # release PCIe (bit0) too
WR 0x0F002 0x4                   # SBus kick (dev 0xFE reg 0x0A = 0x4)
WR 0x0F001 0x0
WR 0x0F001 0x121FE0A             # SBUS_COMMAND execute
WR 0x01400 0x0                   # PCI_ENDIANISM
WR 0x01002 0x2000000             # PCI_CFG_1
WR 0x01418 0x35
WR 0x0140C 0xFFFFFFFF
WR 0x0140D 0xFFFFFFFF
WR 0x01435 0xF121F34             # *** PCI_SERDES_CTRL_1: TxOutputEn + RefSel = turn the PCIe lanes ON ***
WR 0x1C002 0x3FFF
WR 0x0141D 0x1C01F               # PCI_CORE_CTRL_1 (incl bit16 core enable)
sleep 2                           # SerDes PLL lock + link train
echo "[pcie-init] rescan + verify"
echo 1 > /sys/bus/pci/rescan 2>/dev/null; sleep 2
pcicfg 0000:00:04.0 retrain 2>/dev/null >/dev/null; sleep 2
pcicfg 0000:00:04.0 link 2>/dev/null | grep -oE 'DLLLA.b13.=[01].*'
if [ -e /sys/bus/pci/devices/0000:02:00.0/vendor ]; then
	echo "[pcie-init] *** FM6000 ENUMERATED: $(cat /sys/bus/pci/devices/0000:02:00.0/vendor):$(cat /sys/bus/pci/devices/0000:02:00.0/device) BAR0=$(cat /sys/bus/pci/devices/0000:02:00.0/resource | head -1) ***"
else
	echo "[pcie-init] FM6000 still absent - check DLLLA above; re-reset (Si5338 reprogram + 0x4010<=0x6) and retry"
fi
