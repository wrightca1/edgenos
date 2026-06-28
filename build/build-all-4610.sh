#!/usr/bin/env bash
# Full from-source AS4610 pipeline, in dependency order, so the image can NEVER
# ship a stale .epk (the build-hygiene bug that bit us once: imgbuild composes
# whatever .epk exist in output/packages — if a component's source changed but its
# .epk wasn't rebuilt, the image silently carries old content).
#
# Order: datapath binary -> (re)build every component .epk from spec -> compose
# image -> wrap ONIE installer.  Run from the edgenos/ repo root.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SR=$(cd "$HERE/.." && pwd)
cd "$HERE"
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1750000000}"
PLAT=arm-accton-as4610-54-r0
A=armhf; S=bcm56340

echo "== 1. datapath: bcmd from source =="
bash build/build-bcmd.sh

echo "== 2. (re)build every AS4610 component .epk from current source =="
for sp in bcmd linux-kernel-bde linux-user-bde linux-bcm-knet platform-svc; do
  ./bin/edgenos pkg build "packaging/specs/as4610-54/$sp.yml" --source-root "$SR" --arch $A --asic $S
done
./bin/edgenos pkg build packaging/specs/as4610-54/quagga.yml --source-root "$SR" --arch $A --asic any

echo "== 3. compose image =="
./bin/edgenos build "$PLAT" --source-root "$SR"

echo "== 4. wrap ONIE installer =="
bash build/build-onie-installer-4610.sh

echo "== done: output/images/onie-installer-EdgeNOS-*-$PLAT =="
