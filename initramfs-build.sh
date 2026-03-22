#!/bin/bash
# Build minimal initramfs for open-nos-as5610 (PPC32).
# Uses nos-init.c (static C init) instead of a shell script.
# Produces: initramfs.cpio.gz
#
# Requires: powerpc-linux-gnu-gcc (or Docker) to compile nos-init.c

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${SCRIPT_DIR}/initramfs.cpio.gz"
ROOT="${SCRIPT_DIR}/root"

rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,dev,proc,sys,newroot}

# Compile static C init (nos-init.c → /init in initramfs)
INIT_SRC="${SCRIPT_DIR}/nos-init.c"
INIT_BIN="${ROOT}/init"

if [ -f "$INIT_SRC" ]; then
	if command -v powerpc-linux-gnu-gcc &>/dev/null; then
		echo "Compiling nos-init.c with powerpc-linux-gnu-gcc..."
		powerpc-linux-gnu-gcc -static -Os -o "$INIT_BIN" "$INIT_SRC"
		chmod +x "$INIT_BIN"
	elif command -v docker &>/dev/null; then
		echo "Compiling nos-init.c via Docker (debian:bookworm)..."
		docker run --rm \
			-v "$SCRIPT_DIR:/work" -w /work \
			-e DEBIAN_FRONTEND=noninteractive \
			debian:bookworm bash -c \
			'apt-get update -qq && apt-get install -y -qq gcc-powerpc-linux-gnu >/dev/null 2>&1 && powerpc-linux-gnu-gcc -static -Os -o root/init nos-init.c && chmod +x root/init'
	else
		echo "ERROR: need powerpc-linux-gnu-gcc or Docker to compile nos-init.c"
		exit 1
	fi
else
	echo "ERROR: nos-init.c not found at $INIT_SRC"
	exit 1
fi

( cd "$ROOT" && find . | cpio -o -H newc ) | gzip -9 > "$OUT"
echo "Built: $OUT ($(du -sh "$OUT" | cut -f1))"
