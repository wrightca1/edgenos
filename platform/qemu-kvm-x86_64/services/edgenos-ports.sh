#!/bin/sh
# edgenos-ports.sh — name the VM's NICs the EdgeNOS way.
#
#   1st PCI NIC            -> ma1        (management; DHCP via systemd-networkd)
#   2nd, 3rd, ... PCI NIC  -> ge0, ge1…  (front-panel ports, PCI order, admin-up, no IP)
#
# PCI order is the order EVE-NG / containerlab / qemu attach the NICs, so "ge0" is the
# first link in the topology. Works for virtio-net, e1000/e1000e and vmxnet3 alike.
# Idempotent: re-running renames nothing that is already right. Writes the inventory to
# /run/edgenos/ports (one "pci name" per line) for the platform class / operators.
RUN=/run/edgenos
mkdir -p "$RUN"
log() { logger -t edgenos-ports "$*" 2>/dev/null; echo "edgenos-ports: $*"; }

# Datapath mode (/etc/edgenos/datapath): "none" (default) -> the kernel forwards and the
# front-panel NICs are ge<N>; "vswitch" -> the NICs are pge<N>, owned by edged-vswitch,
# and the control plane sees the daemon's cpu0 TAP instead.
MODE=$(cat /etc/edgenos/datapath 2>/dev/null | tr -d '[:space:]')
case "$MODE" in vswitch) PFX=pge ;; *) MODE=none; PFX=ge ;; esac

# pci-address-of <netdev>  (walk the device path: virtio NICs sit one level below the PCI dev)
pci_of() {
    p=$(readlink -f "/sys/class/net/$1/device" 2>/dev/null) || return 1
    echo "$p" | tr '/' '\n' | grep -E '^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]$' | tail -1
}

# inventory: "pci current-name" sorted by PCI address
inv=$(for d in /sys/class/net/*; do
          n=$(basename "$d")
          [ -e "$d/device" ] || continue          # skip lo, bridges, veth, tunnels
          pci=$(pci_of "$n") || continue
          echo "$pci $n"
      done | sort)

: > "$RUN/ports.tmp"
idx=0
echo "$inv" | while read -r pci cur; do
    [ -n "$cur" ] || continue
    if [ "$idx" -eq 0 ]; then want=ma1; else want="$PFX$((idx - 1))"; fi
    idx=$((idx + 1))
    if [ "$cur" != "$want" ]; then
        # if another device currently holds the wanted name, park it first
        if [ -e "/sys/class/net/$want" ]; then
            ip link set "$want" down 2>/dev/null
            ip link set "$want" name "tmp$$$idx" 2>/dev/null
        fi
        ip link set "$cur" down 2>/dev/null
        if ip link set "$cur" name "$want" 2>/dev/null; then
            log "$cur ($pci) -> $want"
        else
            log "WARNING: could not rename $cur -> $want"
            want=$cur
        fi
    fi
    echo "$pci $want" >> "$RUN/ports.tmp"
done
mv "$RUN/ports.tmp" "$RUN/ports"

# front-panel ports: admin-up (like a real switch), no addresses (zebra/operator own those).
# virtio-net reports "Speed: Unknown" - Linux 802.3ad then gives the port LACP key 0 and never
# aggregates it (no LACPDUs), so give every front-panel port a real speed/duplex first (the
# platform's ge ports are 1G). Harmless on e1000/vmxnet3, which already report a speed.
for d in /sys/class/net/$PFX[0-9]*; do
    [ -e "$d" ] || continue
    p=$(basename "$d")
    if ethtool "$p" 2>/dev/null | grep -q "Speed: Unknown"; then
        ethtool -s "$p" speed 1000 duplex full autoneg off 2>/dev/null || true
    fi
    ip link set "$p" up 2>/dev/null
done
echo "$MODE" > "$RUN/datapath"
n=$(grep -c " $PFX" "$RUN/ports" 2>/dev/null || echo 0)
log "done: mode=$MODE mgmt=ma1, $n front-panel port(s) ($PFX*)"
exit 0
