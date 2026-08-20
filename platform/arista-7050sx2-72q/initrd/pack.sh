#!/bin/bash
# Pack the initrd tree and VERIFY IT COMPLETELY.
#
# On 2026-08-11 a pack silently truncated: the scratchpad tmpfs hit 100% and
# gzip produced a 31MB image instead of 62MB, dropping bin/sdkpoc-kg. The
# check in use only cmp'd ONE file, which happened to sit before the cut, so
# it passed. Verify the entry COUNT and every regular file, and stage to disk
# rather than tmpfs.
set -u
W=${1:?tree}; OUT=${2:?output}
cd "$W" || exit 1
find . | cpio -o -H newc 2>/dev/null | gzip -1 > "$OUT" || { echo "PACK FAILED"; exit 1; }

want=$(find . -type f | wc -l)
got=$(gzip -dc "$OUT" 2>/dev/null | cpio -t 2>/dev/null | wc -l)
echo "packed $(stat -c%s "$OUT") bytes"
bad=0
while IFS= read -r f; do
    rel=${f#./}
    if ! gzip -dc "$OUT" 2>/dev/null | cpio --to-stdout -i "$rel" 2>/dev/null | cmp -s - "$f"; then
        echo "  ** MISMATCH/MISSING: $rel"; bad=$((bad+1))
    fi
done < <(find . -type f)
echo "entries: $got listed, $want files on disk, $bad bad"
[ "$bad" -eq 0 ] || { echo "VERIFY FAILED"; exit 1; }
echo "VERIFIED OK"
