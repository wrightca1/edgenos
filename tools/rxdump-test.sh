#!/bin/bash
# rxdump-test.sh - does the ASIC tag punted frames, or does portd only think it doesn't?
#
# The discriminator for the untagged-punt finding. portd reports "TAP<-ASIC (no
# tag)" and delivers every frame to port 0, which makes et2 receive-dead. But an
# earlier fm6000_rxdump capture of the SAME frame type (OSPF/IPv6 multicast,
# DMAC 33:33:00:00:00:05) showed an 8-byte F64 tag at offset 12, with
# MOD_TX_PORT_TAG[0] reading 2 in both cases.
#
#   tag present in rxdump  -> the ASIC tags; the bug is in portd's parsing
#   tag absent in rxdump   -> the ASIC is not tagging; MOD_TX_PORT_TAG is not
#                             sufficient and the cause is upstream of portd
#
# rxdump and portd contend for the same punt ring, so this needs a boot where
# portd has NOT been started -- i.e. edgenos-up.sh must not run. Et2 does not
# need to be up: OSPF hellos arrive on Et1, which links every boot.
set -u
S="$(cd "$(dirname "$0")" && pwd)"
SW="$S/sw.sh"; EG="$S/eg.sh"
IMG=flash:/edgenos-7150-0.3.0-alpha10.swi
edge(){ timeout 60 "$EG" "$@" 2>/dev/null; }
eos(){  timeout 60 "$SW" "$@" 2>/dev/null; }

echo "=== reboot to EOS ==="
if edge 'echo ok' | grep -q ok; then edge 'sync; (sleep 1; reboot -f) >/dev/null 2>&1 &' >/dev/null
else eos 'bash sync' >/dev/null; eos 'reload now' >/dev/null; fi
sleep 45
t=0; until ping -c1 -W2 10.1.1.77 >/dev/null 2>&1 || [ $t -ge 40 ]; do sleep 15; t=$((t+1)); done
sleep 60
eos 'show version | include Uptime' | grep -q Uptime || { echo "no EOS"; exit 1; }

echo "=== arm alpha10 and boot ==="
eos 'configure' "boot system $IMG" 'end' >/dev/null
eos 'bash sync' >/dev/null; eos 'reload now' >/dev/null
sleep 40
t=0; until edge 'echo ok' | grep -q ok || [ $t -ge 40 ]; do sleep 15; t=$((t+1)); done
t=0; until edge 'grep -q "FULLSEQ DONE" /var/log/fm6000-fullseq && echo d' | grep -q d || [ $t -ge 40 ]; do sleep 20; t=$((t+1)); done

echo "=== state (portd must NOT be running) ==="
edge 'pidof fm6000_portd >/dev/null && echo "⛔ portd IS running" || echo "portd not running, good"
      echo -n "Et1: "; /mnt/flash/fmdump 0xe3800 1
      echo -n "Et2: "; /mnt/flash/fmdump 0xe4000 1
      echo -n "MOD_TX_PORT_TAG[0]: "; /mnt/flash/fmdump 0x15f280 1'

echo "=== load DMA module only, then rxdump for 20s ==="
edge 'insmod /lib/modules/6.12.0/extra/fm6000dma.ko 2>/dev/null
      /usr/bin/fm6000_rxdump 20 2>&1 | head -30'
