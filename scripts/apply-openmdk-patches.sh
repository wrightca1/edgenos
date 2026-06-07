#!/bin/sh
# apply-openmdk-patches.sh — restore EdgeNOS's OpenMDK source modifications
# into the (gitignored, nested) asic/openmdk clone.
#
# asic/openmdk is a nested clone of Broadcom/OpenMDK and is gitignored by the
# parent repo, so our divergence from stock can't live in the parent's git as
# normal tracked files. Instead patches/openmdk/ holds canonical copies of every
# file we modified; this script copies them back into the openmdk tree at the
# correct PKG/ master locations.
#
# Run after a fresh clone of OpenMDK (base = stock 2.10.9, db9c678) and BEFORE
# build.sh / rebuild-edged-with-sdk.sh.
set -e

TOPDIR=$(cd "$(dirname "$0")/.." && pwd)
SRC="$TOPDIR/patches/openmdk"
OMK="$TOPDIR/asic/openmdk"

[ -d "$OMK" ] || { echo "ERROR: OpenMDK clone not found at $OMK"; exit 1; }
[ -d "$SRC" ] || { echo "ERROR: patch set not found at $SRC"; exit 1; }

# canonical file  ->  destination (relative to asic/openmdk)
# Keep this table in sync with patches/openmdk/README.md.
copy() {
  bn="$1"; dst="$2"
  [ -f "$SRC/$bn" ] || { echo "  MISSING in patch set: $bn"; return 1; }
  mkdir -p "$OMK/$dst"
  cp "$SRC/$bn" "$OMK/$dst/$bn"
  echo "  $bn -> $dst/"
}

echo "==> Applying EdgeNOS OpenMDK source modifications..."

# --- PHY: Warpcore SerDes (the 40G QSFP fix: fw_mode=0 + CL82 dual-block) ---
copy bcmi_warpcore_xgxs_drv.c phy/PKG/chip/bcmi_warpcore_xgxs
copy cumulus_wc_ucode.c       phy/PKG/chip/bcmi_warpcore_xgxs

# --- BMD: bcm56840_a0 datapath (port map, RX/TX DMA, STP, switching init) ---
for f in bcm56840_a0_bmd_attach.c \
         bcm56840_a0_bmd_port_mode_set.c \
         bcm56840_a0_bmd_port_stp_set.c \
         bcm56840_a0_bmd_rx.c \
         bcm56840_a0_bmd_stat_get.c \
         bcm56840_a0_bmd_switching_init.c \
         bcm56840_a0_bmd_tx.c ; do
  copy "$f" bmd/PKG/chip/bcm56840_a0
done

# --- BMD: arch + shared + headers ---
copy xgsd_dma.c           bmd/PKG/arch/xgsd
copy bmd_phy_staged_init.c bmd/shared
copy bmd.h                bmd/include/bmd

# --- CDK: MIIM (Warpcore block-select fix for regs > 0x8000) ---
copy xgs_miim.c           cdk/PKG/arch/xgs

echo "==> Done. OpenMDK tree now matches the working EdgeNOS datapath."
echo "    Next: ./build.sh image   (or scripts/rebuild-edged-with-sdk.sh)"
