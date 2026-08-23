#!/bin/bash
# run-edgenos.sh — boot an EdgeNOS x86_64 disk image under qemu for a quick look.
#
#   tools/qemu/run-edgenos.sh <image.qcow2> [-n NICS] [-m MB] [--uefi] [--serial tcp:PORT] [--snapshot] [--] [extra qemu args]
#
#   NIC 0 = ma1 on a QEMU user network (DHCP; ssh forwarded to localhost:2222)
#   NIC 1..N = ge0..geN-1 as TAP devices (tapN, script=no; wire them yourself) — or, with
#              --socket, as UDP socket links for a two-VM back-to-back test.
# Console on stdio (serial) unless --serial tcp:PORT (then: telnet localhost PORT).
set -euo pipefail
IMG=${1:?usage: $0 <image.qcow2> [options]}; shift
NICS=2; MEM=1024; UEFI=; SERIAL="mon:stdio"; SNAP=; SSHPORT=2222; NAME=edgenos
while [ $# -gt 0 ]; do
    case "$1" in
        -n) NICS=$2; shift 2;;
        -m) MEM=$2; shift 2;;
        --uefi) UEFI=1; shift;;
        --serial) SERIAL=$2; shift 2;;
        --snapshot) SNAP=1; shift;;
        --ssh-port) SSHPORT=$2; shift 2;;
        --name) NAME=$2; shift 2;;
        --) shift; break;;
        *) break;;
    esac
done
MGMT="user,id=m0"; [ "$SSHPORT" != 0 ] && MGMT="$MGMT,hostfwd=tcp::${SSHPORT}-:22"
ARGS=(-name "$NAME" -machine type=pc,accel=kvm -cpu host -m "$MEM" -smp 1 -nographic -rtc base=utc
      -drive "file=$IMG,if=virtio,format=qcow2${SNAP:+,snapshot=on}"
      -netdev "$MGMT" -device "virtio-net-pci,netdev=m0,mac=52:54:00:ed:9e:00")
for i in $(seq 0 $((NICS - 2))); do
    if [ -n "${EDGENOS_NO_TAP:-}" ]; then     # unprivileged (CI/smoke): isolated per-NIC user nets, just for naming
        ARGS+=(-netdev "user,id=p$i,restrict=on" -device "virtio-net-pci,netdev=p$i,mac=$(printf '52:54:00:ed:9e:%02x' $((i + 1)))")
    else
        ARGS+=(-netdev "tap,id=p$i,ifname=${NAME}-ge$i,script=no,downscript=no" -device "virtio-net-pci,netdev=p$i,mac=$(printf '52:54:00:ed:9e:%02x' $((i + 1)))")
    fi
done
case "$SERIAL" in
    tcp:*) ARGS+=(-serial "tcp::${SERIAL#tcp:},server,nowait" -monitor none) ;;
    *)     ARGS+=(-serial "$SERIAL") ;;
esac
if [ -n "$UEFI" ]; then
    for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd /usr/share/edk2/ovmf/OVMF_CODE.fd; do
        [ -f "$f" ] && { ARGS+=(-drive "if=pflash,format=raw,readonly=on,file=$f"); break; }
    done
fi
[ -r /dev/kvm ] || ARGS=("${ARGS[@]/accel=kvm/accel=tcg}")
exec qemu-system-x86_64 "${ARGS[@]}" "$@"
