#!/bin/sh
B=0000:02:00.0
R(){ fm6000reg $B "$1" 2>/dev/null | grep -o '[0-9a-f]*$'; }
# WATCHDOG FIRST + petter
scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1
( while :; do scdreg 0x0120 0xC0000BB8 >/dev/null 2>&1; sleep 4; done ) & PET=$!
trap 'kill $PET 2>/dev/null; scdreg 0x0120 0x0 >/dev/null 2>&1' EXIT INT TERM
echo "[CR] START PIN=$(R 0x1c021)"; cd /tmp
wget -q -O /tmp/fm6000_coldreplay http://<mgmt-net-host>:8000/fm6000_coldreplay && chmod +x /tmp/fm6000_coldreplay
[ "$(R 0x1c021)" = "00000208" ] || { echo "[CR] NOT clean"; exit 1; }
# NO pre-BM-march: let the coldreplay's own trace-faithful BIST init all memory (as EOS does)
echo "[CR] no-pre-bist PIN=$(R 0x1c021)"
echo "---- VERBATIM cold-trace replay (unbuffered, checkpoints every 2 ops) ----"
# raw stream, NO pipe (so checkpoints reach SSH before any wedge)
/tmp/fm6000_coldreplay $B
echo "[CR] after replay PIN=$(R 0x1c021)"
kill $PET 2>/dev/null; scdreg 0x0120 0x0 >/dev/null 2>&1; echo "[CR] DONE"
