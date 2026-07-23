#!/bin/sh
# Experiment C - FM6000 bring-up: write-protect clear + core-voltage margin + Link-Disable clear
#                + reset re-pulse + rescan.  Run interactively on M1 after boot.
# Uses: scdreg, i2cget/i2cset, pcicfg, awk (busybox).  See notes/analysis/phase27.
BR=0000:00:04.0
FMPATH=/sys/bus/pci/devices/0000:02:00.0
CHLADDR=0x70
fm(){ [ -d "$FMPATH" ] && echo UP || echo DOWN; }
scdv(){ scdreg "$1" 2>/dev/null | grep -o '0x[0-9a-f]*$'; }

echo "=================== EXPERIMENT C ==================="
echo "baseline: FM6000=$(fm)"

echo "--- [1] SCD write-protect 0x1000 ---"
echo "  0x1000 = $(scdv 0x1000)   (clearing to un-gate control writes + restore reboot)"
scdreg 0x1000 0x00000000 2>/dev/null
echo "  0x1000 after clear = $(scdv 0x1000)"
echo "  write-path test: 0x5010 <= 0x1 ..."; scdreg 0x5010 0x1 >/dev/null 2>&1
echo "    0x5010 = $(scdv 0x5010)  (0x1 => writes work / un-gated; 0x0 => still gated, need boot-script path)"

echo "--- [2] register SMBus accel#0 (Chl822X bus3 / UCD bus5) ---"
NO=$(ls /sys/bus/pci/drivers/scd/0000:*/new_object 2>/dev/null | head -1)
if ! grep -rq "master 0 bus 3" /sys/class/i2c-adapter/i2c-*/name 2>/dev/null; then
  echo "smbus_master 0x8000 0 8" > "$NO" 2>/dev/null && echo "  accel#0 registered" || echo "  WARN register failed"
  sleep 1
fi
CHL=""; UCD=""
for a in /sys/class/i2c-adapter/i2c-*; do
  n=$(cat "$a/name" 2>/dev/null); b=$(basename "$a" | sed 's/i2c-//')
  case "$n" in *"master 0 bus 3") CHL=$b;; *"master 0 bus 5") UCD=$b;; esac
done
echo "  Chl822X=/dev/i2c-$CHL  UCD=/dev/i2c-$UCD"
[ -z "$CHL" ] && { echo "  ERROR no Chl822X bus - aborting"; exit 1; }

echo "--- [3] read FM6000 core voltage + AltaVdd target ---"
MODE=$(i2cget -y $CHL $CHLADDR 0x20 b 2>/dev/null)
EXP=$(( MODE & 0x1f )); [ $EXP -ge 16 ] && EXP=$(( EXP - 32 ))
RAW=$(i2cget -y $CHL $CHLADDR 0x8b w 2>/dev/null)
VNOW=$(awk "BEGIN{printf \"%.4f\", $RAW*(2^($EXP))}")
echo "  VOUT_MODE=$MODE exp=$EXP  READ_VOUT=$RAW  ->  Vcore = $VNOW V"
ALTAVDD=$( (genprefdl 2>/dev/null || cat /etc/prefdl 2>/dev/null) | grep -io "AltaVdd['\"]*:[0-9.]*" | grep -o '[0-9.]*$' | head -1)
[ -z "$ALTAVDD" ] && ALTAVDD=1.2
echo "  AltaVdd target (prefdl) = $ALTAVDD V   (Vcore $VNOW < 1.10 => under-volted)"

echo "--- [4] margin core to $ALTAVDD V (try PMBus VOUT_COMMAND 0x21 first) ---"
CODE=$(awk "BEGIN{printf \"%d\", $ALTAVDD*(2^(-($EXP)))+0.5}")
echo "  target code = $CODE (0x$(printf %x $CODE))"
i2cset -y $CHL $CHLADDR 0x21 $CODE w 2>/dev/null
sleep 1
RAW2=$(i2cget -y $CHL $CHLADDR 0x8b w 2>/dev/null)
V2=$(awk "BEGIN{printf \"%.4f\", $RAW2*(2^($EXP))}")
echo "  after VOUT_COMMAND: Vcore = $V2 V  (if unchanged, use the 0xD3/D4/D5 VID window - dump 0x08-0x3F)"

echo "--- [5] ensure bridge Link-Disable clear ---"
pcicfg $BR linkctl

echo "--- [6] re-pulse FM6000 reset so it re-boots at $ALTAVDD V ---"
echo "  assert (0x4000<=0x2): "; scdreg 0x4000 0x2 >/dev/null 2>&1; echo "    0x4000=$(scdv 0x4000)"
sleep 1
echo "  (voltage now $(awk "BEGIN{printf \"%.3f\", $(i2cget -y $CHL $CHLADDR 0x8b w 2>/dev/null)*(2^($EXP))}") V while held)"
echo "  deassert (0x4010<=0x6): "; scdreg 0x4010 0x6 >/dev/null 2>&1; echo "    0x4000=$(scdv 0x4000)"

echo "--- [7] rescan (+retrain if needed) ---"
sleep 1; echo 1 > /sys/bus/pci/rescan 2>/dev/null; sleep 2
if [ "$(fm)" = DOWN ]; then
  echo "  not yet - retrain kick (set/clear Link-Disable)"
  CAP=$(pcicfg $BR linkctl | grep -o 'LinkControl@0x[0-9a-f]*' | grep -o '0x[0-9a-f]*')
  LCV=$(pcicfg $BR $CAP | grep -o '0x[0-9a-f]*$')
  pcicfg $BR $CAP $(printf '0x%x' $(( LCV | 0x10 ))) >/dev/null; sleep 1
  pcicfg $BR $CAP $(printf '0x%x' $(( LCV & ~0x10 ))) >/dev/null; sleep 2
  echo 1 > /sys/bus/pci/rescan 2>/dev/null; sleep 2
fi

echo "=================== RESULT ==================="
if [ "$(fm)" = UP ]; then
  echo ">>> FM6000 ENUMERATED: $(cat $FMPATH/vendor):$(cat $FMPATH/device) <<<"
else
  echo "FM6000 still absent. Vcore=$V2 V. Next: VID-window voltage set, or refclk check, or boot-script order."
fi
echo "mgmt eth0: $(ip -o link show eth0 >/dev/null 2>&1 && echo OK || echo GONE)"
