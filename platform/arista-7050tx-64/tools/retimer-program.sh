#!/bin/busybox sh
# retimer-program.sh -- program the DS100KR800 QSFP retimer with the board's
# own settings, on EdgeNOS.
#
# The retimer sits between the QSFP cages and the ASIC SerDes on the last QSFP
# ports. Out of reset but unprogrammed it passes nothing, which is why
# `phy diag xe60 dsc` reads afe_det=0 while the far end is demonstrably
# transmitting (docs/PORT-LAYER-INVESTIGATION.md).
#
# ⚠ THE BOARD'S TUNING VALUES ARE NOT IN THIS FILE, DELIBERATELY.
#
# They come from `/etc/fdl` on the switch (`ds100KrData`), which is Arista
# Confidential. The offsets below are from the TI DS100KR800 datasheet
# (SNLS340E) Table 6 and are public; the values are the board vendor's and are
# not ours to publish. So they are read at runtime from a file the operator
# generates from their own switch:
#
#     tools/fdl-extract.sh > /etc/edgenos/retimer.conf
#
# Format, one key per line:
#     pin_control    <byte>     # FDL pinControl
#     disable_crc    <byte>     # FDL disableCrc
#     vod_low        <byte>     # outputAmplitude, channels 0-3
#     vod_high       <byte>     # outputAmplitude, channels 4-7
#     dem_low        <byte>     # txDeEmphasis, channels 0-3
#     dem_high       <byte>     # txDeEmphasis, channels 4-7
#     eq             <byte>     # rxEqualization, all channels
#
# ⚠ With no file, this REFUSES TO PROGRAM rather than guessing. An unprogrammed
# retimer passes nothing, which is a dead 40G port -- annoying and obvious. A
# wrongly programmed one is a marginal link that works until it does not.
#
set -u

BUS="${1:?usage: retimer-program.sh <i2c-bus> [--program]}"
ADDR=0x58
CONF="${EDGENOS_RETIMER_CONF:-/etc/edgenos/retimer.conf}"

if [ ! -r "$CONF" ]; then
    echo "** no $CONF -- refusing to program the retimer."
    echo "   Generate it on this switch with tools/fdl-extract.sh; the values"
    echo "   are the board's own, from its FDL, and are not shipped."
    [ "${2:-}" = "--program" ] && exit 1
fi
get() { sed -n "s/^$1[[:space:]]\\+//p" "$CONF" 2>/dev/null | head -1; }
PIN_CONTROL=$(get pin_control); DISABLE_CRC=$(get disable_crc)
VOD_LOW=$(get vod_low);         VOD_HIGH=$(get vod_high)
DEM_LOW=$(get dem_low);         DEM_HIGH=$(get dem_high)
EQ_ALL=$(get eq)
DO_WRITE="${2:-}"

# EQ base per channel; VOD = base+1, DEM = base+2.
CH_BASE="0x0F 0x16 0x1D 0x24 0x2C 0x33 0x3A 0x41"

# ⚠ busybox i2cget/i2cset parse the register argument as HEX. A computed
# decimal offset is therefore read at the WRONG address -- $((0x0F+1)) yields
# "16", which busybox reads as 0x16, not 0x10. Every computed address has to be
# formatted back to 0x.. form. The report-only pass caught this: VOD/DEM came
# back one channel out while EQ, the only literal address, was right.
hx() { printf "0x%02x" "$1"; }
rd() { busybox i2cget -y -f "$BUS" $ADDR "$1" 2>/dev/null; }
wr() { busybox i2cset -y -f "$BUS" $ADDR "$1" "$2" 2>&1; }

echo "=== DS100KR800 at $ADDR on i2c-$BUS ==="
echo "before:"
printf "  0x01 PWDN=%s  0x06 SlaveRegCtl=%s  0x08 PinCtl=%s\n" \
    "$(rd 0x01)" "$(rd 0x06)" "$(rd 0x08)"
i=0
for b in $CH_BASE; do
    printf "  ch%d base=%s  EQ=%s VOD=%s DEM=%s\n" "$i" "$b" \
        "$(rd $b)" "$(rd $(hx $((b+1))))" "$(rd $(hx $((b+2))))"
    i=$((i+1))
done

if [ "$DO_WRITE" != "--program" ]; then
    echo; echo "report only; pass --program to write"
    exit 0
fi

echo
echo "=== programming ==="
# Order matters: enable register control BEFORE touching per-channel VOD/DEM/EQ,
# and take the pins out of the picture, or the writes are ignored in pin mode.
echo "  0x06 = $DISABLE_CRC  (slave register control enable)"
wr 0x06 0x18
echo "  0x08 = $PIN_CONTROL  (override SD_TH + DEM pin control)"
wr 0x08 0x42
echo "  0x01 = 0x00  (all 8 channels powered ON -- FDL channelPowerDown=0)"
wr 0x01 0x00

i=0
for b in $CH_BASE; do
    # FDL: rxEqualization 0 for every channel; outputAmplitude 40 on ch0-3 and
    # 41 on ch4-7; txDeEmphasis 1 on ch0-3 and 0 on ch4-7.
    if [ "$i" -lt 4 ]; then VOD=$VOD_LOW; DEM=$DEM_LOW; else VOD=$VOD_HIGH; DEM=$DEM_HIGH; fi
    wr "$b"              0x00   >/dev/null   # rxEqualization
    wr "$(hx $((b+1)))" "$VOD"  >/dev/null   # outputAmplitude
    wr "$(hx $((b+2)))" "$DEM"  >/dev/null   # txDeEmphasis
    i=$((i+1))
done
echo "  per-channel EQ=$EQ_ALL, VOD=$VOD_LOW/$VOD_HIGH, DEM=$DEM_LOW/$DEM_HIGH written"

echo
echo "=== after ==="
printf "  0x01 PWDN=%s  0x06 SlaveRegCtl=%s  0x08 PinCtl=%s\n" \
    "$(rd 0x01)" "$(rd 0x06)" "$(rd 0x08)"
i=0
for b in $CH_BASE; do
    printf "  ch%d EQ=%s VOD=%s DEM=%s\n" "$i" \
        "$(rd $b)" "$(rd $(hx $((b+1))))" "$(rd $(hx $((b+2))))"
    i=$((i+1))
done
