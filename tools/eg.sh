#!/bin/sh
# eg.sh -- run a command on the 7150 under test (the "edge" box).
#
# Recreated 2026-08-20: this and p5.sh are referenced by transit-test.sh,
# a4-driver.sh, a4-slice-sweep.sh, a4-leaveout.sh and et2-demux-test.sh, but
# were missing from the tree, which made every one of those harnesses unrunnable.
#
# The 7150 has no authorized_keys (RAM rootfs, so anything added by hand dies at
# reboot) and dropbear runs without -s, so password auth is what works. See
# the edgenos-hardware-access note: root/arista, sshpass required.
exec sshpass -p "${EG_PASS:-arista}" ssh \
    -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o PubkeyAuthentication=no -o PreferredAuthentications=password,keyboard-interactive \
    -o LogLevel=ERROR "root@${EG_HOST:-10.1.1.77}" "$@"
