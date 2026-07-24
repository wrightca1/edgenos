#!/bin/sh
# dllla-check.sh - THE decisive FM6000-enumeration diagnostic for M1 (phase31).
# Run AFTER fm6000-up.sh has released the Alta reset + programmed the refclk.
# Reads DLLLA (Data Link Layer Link Active) on the RS780 root port 00:04.0 - the one
# bit that says whether the FM6000 is driving its PCIe link:
#   DLLLA=1 -> chip IS up, enum gap is PCI-resource (bridge window) -> try `dllla-check.sh fix`
#   DLLLA=0 -> chip NOT driving PCIe -> a chip-boot problem, not enumeration.
# EOS does this enumerate step in the kernel via `pcielw`; M1 has none, so we diagnose + fix by hand.
# Read-only by default. `dllla-check.sh fix` attempts the PCI-resource fix (widen bridge window + rescan).
DIR=$(cd "$(dirname "$0")" && pwd)
BR=0000:00:04.0            # RS780 root port above the FM6000
EP=0000:02:00.0            # FM6000 endpoint (8086:155b)
PCICFG="$DIR/pcicfg"; SCDREG="$DIR/scdreg"
[ -x "$PCICFG" ] || PCICFG=pcicfg
[ -x "$SCDREG" ] || SCDREG=scdreg

echo "===== FM6000 enumeration diagnosis (phase31 DLLLA test) ====="
echo "--- 1. SCD Alta reset state (0x4000 resetGpo1; bits 1,2 should be CLEAR = out of reset) ---"
R=$("$SCDREG" 0x4000 2>/dev/null); echo "  $R"
case "$R" in *=*) v=${R##*=}; b1=$(( (v>>1)&1 )); b2=$(( (v>>2)&1 ));
  echo "  Alta-reset(bit1)=$b1  SOL(bit2)=$b2  $( [ "$b1" = 0 ] && echo '(Alta OUT of reset - good)' || echo '(Alta HELD in reset - fm6000-up.sh did not run / re-assert)')";;
esac

echo "--- 2. root port 00:04.0 link status (THE decisive read) ---"
"$PCICFG" "$BR" link 2>&1 | sed 's/^/  /'

echo "--- 3. is the FM6000 endpoint present? ---"
if [ -e "/sys/bus/pci/devices/$EP" ]; then
  echo "  $EP PRESENT: $(cat /sys/bus/pci/devices/$EP/vendor 2>/dev/null):$(cat /sys/bus/pci/devices/$EP/device 2>/dev/null)  (already enumerated!)"
else
  echo "  $EP ABSENT (not enumerated)"
fi

echo "--- 4. bridge memory window (for the resource-fix path) ---"
echo "  cfg 0x1c (IO base/limit)        = $("$PCICFG" "$BR" 0x1c 2>/dev/null | sed 's/.*= //')"
echo "  cfg 0x20 (Mem base/limit)       = $("$PCICFG" "$BR" 0x20 2>/dev/null | sed 's/.*= //')   [b31:20=base<<20, b15:4=limit|0xfffff]"
echo "  cfg 0x24 (PrefMem base/limit lo)= $("$PCICFG" "$BR" 0x24 2>/dev/null | sed 's/.*= //')"
echo "  EOS assigned the FM6000 BAR0 at 0xe2000000-0xe3ffffff (32MB). Window must cover that."

echo "===== VERDICT / NEXT ====="
DL=$("$PCICFG" "$BR" link 2>/dev/null | awk -F'DLLLA=|->' '/VERDICT/{gsub(/ /,"",$2);print $2}')
if [ "$DL" = "1" ]; then
  echo "  DLLLA=1: FM6000 IS driving PCIe. Enumeration gap = PCI resource (bridge window / no pcielw)."
  echo "  Fixes: boot M1 with 'pci=realloc pci=hpmemsize=32M', OR run: $0 fix   (widen window + rescan)"
else
  echo "  DLLLA=0: FM6000 is NOT driving PCIe -> chip-boot problem (clock/voltage/reset timing), not enum."
  echo "  pcielw/rescan/window-resize will NOT help until the link trains. Focus on chip bring-up."
fi

# ---- optional PCI-resource fix (only meaningful if DLLLA=1 && endpoint absent) ----
if [ "$1" = "fix" ]; then
  echo "===== FIX ATTEMPT: widen bridge mem window to cover 0xe2000000-0xe3ffffff + rescan ====="
  if [ "$DL" != "1" ]; then echo "  SKIP: DLLLA!=1, link not up - fix cannot help."; exit 0; fi
  if [ -e "/sys/bus/pci/devices/$EP" ]; then echo "  SKIP: endpoint already present."; exit 0; fi
  # Mem Base/Limit dword (cfg 0x20): base=0xe200 (0xe2000000>>16), limit=0xe3f0 (top of 0xe3ffffff)
  # layout: [31:20]=base>>20 in top bits, [15:4]=limit>>20. Encode base=0xe20,limit=0xe3f -> 0xe3f0e200.
  echo "  before: $("$PCICFG" "$BR" 0x20 2>/dev/null | sed 's/.*= //')"
  "$PCICFG" "$BR" 0x20 0xe3f0e200
  # ensure bridge mem-space + bus-master enabled (cmd reg 0x04 bits 1,2)
  CMD=$("$PCICFG" "$BR" 0x04 2>/dev/null | sed 's/.*= //'); echo "  bridge cmd=$CMD (want mem+busmaster)"
  echo "  rescanning..."; echo 1 > /sys/bus/pci/rescan 2>/dev/null; sleep 2
  if [ -e "/sys/bus/pci/devices/$EP" ]; then
    echo "  SUCCESS: $EP now present! BAR0=$(cat /sys/bus/pci/devices/$EP/resource 2>/dev/null | head -1)"
  else
    echo "  still absent. Next: try boot param pci=realloc, or inspect dmesg for BAR-assign failure:"
    dmesg 2>/dev/null | grep -iE '0000:02:00|0000:00:04|bar|no space' | tail -8 | sed 's/^/    /'
  fi
fi
