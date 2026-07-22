#!/bin/sh
# to-eos.sh - return to EOS by kexec (the reboot mechanism EOS itself uses; the
# hardware reset is broken on this AMD SB700 box). Mounts the boot flash, unpacks
# the EOS SWI's kernel+initrd, and kexecs into it with the EOS cmdline. This is
# what Aboot's boot0 does, minus Aboot.
#
# Root-caused in notes/analysis/phase16-reboot-research.md. Uses the static i386
# kexec from EOS (runs via CONFIG_IA32_EMULATION).
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
KEXEC=/usr/bin/kexec
FLASH=/mnt/flash
SWI="$FLASH/EOS-4.16.8M.swi"

echo "to-eos: kexec back into EOS..."
mkdir -p "$FLASH" /tmp/eos

# Mount the boot flash (FAT32). Boot DOM is /dev/sda1 (see block_flash cmdline).
for dev in /dev/sda1 /dev/sdb1 /dev/sda; do
	mount -t vfat -o ro "$dev" "$FLASH" 2>/dev/null && break
done
if [ ! -f "$SWI" ]; then
	echo "to-eos: EOS SWI not found ($SWI) — is the flash mounted?" >&2
	ls "$FLASH" 2>/dev/null | head
	exit 1
fi

unzip -oq "$SWI" linux-i386 initrd-i386 -d /tmp/eos || { echo "to-eos: unzip failed"; exit 1; }

# EOS kernel cmdline (from the live box; the flash/rootfs + platform params EOS
# needs). reboot=p is EOS's own (its reboot is kexec anyway).
APPEND="nmi_watchdog=panic tsc=reliable pcie_ports=native reboot=p SWI=flash:/EOS-4.16.8M.swi CONSOLESPEED=9600 console=ttyS0 block_drive=pci0000:00/0000:00:11.0/.*host./target.:0:0/.*$ net_ma1=pci0000:00/0000:00:14.6$ block_flash=pci0000:00/0000:00:12.[02]/usb.*$ dmamem=48M platform=raven"

# BUG69923: on Raven, ma1 loses its MAC mailbox across kexec — bring eth0 up (to
# call __tg3_set_mac_addr) then DOWN before kexec (it DMAs into RAM early).
ip link set eth0 up 2>/dev/null; sleep 1; ip link set eth0 down 2>/dev/null

sync
"$KEXEC" --load /tmp/eos/linux-i386 --initrd=/tmp/eos/initrd-i386 --append="$APPEND" \
	|| { echo "to-eos: kexec --load failed"; exit 1; }
echo "to-eos: kexec loaded; executing (should boot EOS, no BIOS POST)..."
sync
"$KEXEC" --exec
echo "to-eos: kexec --exec returned (unexpected)"
