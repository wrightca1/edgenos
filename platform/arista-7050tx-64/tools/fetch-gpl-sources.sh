#!/bin/bash
# fetch-gpl-sources.sh -- assemble the COMPLETE CORRESPONDING SOURCE for every
# GPL/LGPL binary the EdgeNOS image ships for the Arista 7050TX-64.
#
# We distribute an image containing binaries we did not write: FRR, glibc and its
# library closure, and Arista's GPL scd/scd-hwmon kernel modules. Conveying those
# obliges us to convey their source too. The image ships the licence texts and a
# written offer; this script is what makes that offer real.
#
# It is driven by the generated BILL-OF-MATERIALS rather than a hand-kept list,
# so it cannot silently miss a library that got added to the closure.
#
# Usage:  tools/fetch-gpl-sources.sh [outdir]     default build/gpl-sources
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/build/gpl-sources}"
BOM="$REPO/build/frr-root/usr/share/doc/edgenos/BILL-OF-MATERIALS"
IMG="debian:bookworm"

# Arista's GPL kernel modules. The scd.ko and scd-hwmon.ko we ship are built
# from this tree; record the exact commit so the source matches the binary.
SCD_REPO="https://github.com/aristanetworks/sonic.git"
SCD_COMMIT="${EDGENOS_SCD_COMMIT:-d88a654}"

say() { printf '\033[1;36mgpl-sources:\033[0m %s\n' "$*"; }

[ -f "$BOM" ] || { echo "** no bill of materials -- run tools/build-frr.sh first"; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT"

# Source packages, from the BOM. Column 3 is the SOURCE package, which is what
# apt-get source wants -- asking for the binary package name silently fetches
# the wrong thing for anything built as part of a larger source tree.
SRCS=$(awk 'NR>5 && NF {print $3}' "$BOM" | sort -u | tr '\n' ' ')
say "source packages: $SRCS"

docker run --rm --network=host -v "$OUT:/out" "$IMG" bash -euc "
    export DEBIAN_FRONTEND=noninteractive
    echo 'APT::Sandbox::User \"root\";' > /etc/apt/apt.conf.d/99-no-sandbox
    sed -i 's/^Types: deb\$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources 2>/dev/null || true
    grep -q '^deb-src' /etc/apt/sources.list 2>/dev/null || \
        echo 'deb-src http://deb.debian.org/debian bookworm main' >> /etc/apt/sources.list
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends dpkg-dev >/dev/null
    cd /out
    for s in $SRCS; do
        echo \"  fetching \$s\"
        apt-get source -q --download-only \"\$s\" 2>/dev/null || echo \"  ** \$s FAILED\"
    done
    chmod -R a+rX /out
"

say "fetching Arista GPL kernel modules at $SCD_COMMIT"
git clone -q "$SCD_REPO" "$OUT/arista-sonic" 2>/dev/null || true
if [ -d "$OUT/arista-sonic" ]; then
    git -C "$OUT/arista-sonic" checkout -q "$SCD_COMMIT" 2>/dev/null \
        || say "** could not check out $SCD_COMMIT -- recording HEAD instead"
    git -C "$OUT/arista-sonic" rev-parse HEAD > "$OUT/arista-sonic.commit"
fi

# Our own kernel configuration is already in the tree; the kernel source itself
# is stock upstream, recorded by version so it can be fetched from kernel.org.
grep -m1 '^# Linux.*Kernel Configuration' "$REPO/build/kernel/.config" 2>/dev/null \
    > "$OUT/kernel-version.txt" || true

say "collected $(find "$OUT" -maxdepth 1 -name '*.tar.*' | wc -l) source archives into $OUT"
say "total $(du -sh "$OUT" 2>/dev/null | cut -f1)"
