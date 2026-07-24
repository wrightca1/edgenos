#!/bin/sh
# Blocker B fix + FM6000 core-voltage read (reads-only, all in-kernel -> not gated).
# Chl822X(0x70)=accel#0 busId3, UCD90160(0x4e)=accel#0 busId5. Both need t=3 (1000ms)
# bus timeout (heavy PMBus clock-stretch); our driver defaults t=1 (35ms) => they time out.
NO=$(ls /sys/bus/pci/drivers/scd/0000:*/new_object 2>/dev/null | head -1)
DIR=$(dirname "$NO")
echo "=== register accel#0 (if needed) ==="
grep -rlq "master 0 bus 3" /sys/class/i2c-dev/i2c-*/name 2>/dev/null || { echo "smbus_master 0x8000 0 8" > "$NO"; sleep 1; }
CHL=""; UCD=""
for a in /sys/class/i2c-dev/i2c-*; do n=$(cat "$a/name" 2>/dev/null); b=$(basename "$a"|sed s/i2c-//)
  case "$n" in *"master 0 bus 3") CHL=$b;; *"master 0 bus 5") UCD=$b;; esac
done
echo "  Chl822X=/dev/i2c-$CHL  UCD=/dev/i2c-$UCD"
[ -z "$CHL$UCD" ] && { echo "  ERROR: accel#0 buses not found"; exit 1; }
echo "=== apply 1000ms (t=3) timeout tweak: <i2cN> <addr> <t> <datr> <datw> <ed> ==="
[ -n "$CHL" ] && { echo "$CHL 0x70 3 3 3 0" > "$DIR/smbus_tweaks" 2>&1 && echo "  Chl822X i2c-$CHL 0x70 -> t=3 ok"; }
[ -n "$UCD" ] && { echo "$UCD 0x4e 3 3 3 0" > "$DIR/smbus_tweaks" 2>&1 && echo "  UCD    i2c-$UCD 0x4e -> t=3 ok"; }
sleep 1
vout(){ # $1=i2cN $2=addr  -> prints volts using VOUT_MODE(0x20) exp + READ_VOUT(0x8b)
  m=$(i2cget -y $1 $2 0x20 b 2>/dev/null); [ -z "$m" ] && { echo "NO-ACK"; return; }
  e=$(( m & 0x1f )); [ $e -ge 16 ] && e=$(( e - 32 ))
  r=$(i2cget -y $1 $2 0x8b w 2>/dev/null); [ -z "$r" ] && { echo "ACK-but-no-VOUT"; return; }
  awk "BEGIN{printf \"%.4f V (mode=%d exp=%d raw=%d)\", $r*(2^($e)), $m, $e, $r}"
}
echo "=== FM6000 core voltage (the last untested hypothesis) ==="
echo "  Chl822X 0x70 Vout       = $(vout $CHL 0x70)"
echo "  --- UCD90160 rails ---"
for pg in 8 9 10; do i2cset -y $UCD 0x4e 0x00 $pg 2>/dev/null; echo "  UCD page $pg Vout           = $(vout $UCD 0x4e)"; done
echo "=== AltaVdd target (prefdl) ==="
( genprefdl 2>/dev/null; cat /etc/prefdl 2>/dev/null ) | grep -io "AltaVdd[^,}]*" | head -2 || echo "  (not found)"
echo "=== VERDICT: is core < 1.10V (under-volt) or ~1.2V (voltage theory dead)? ==="
