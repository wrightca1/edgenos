#!/bin/sh
# Addresses are deliberately not baked in: this tree is published, and the lab
# topology is not ours to publish. Export the host, or put it in a local
# (gitignored) env file:  export EG_HOST=<addr>
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
    -o LogLevel=ERROR "root@${EG_HOST:?set EG_HOST to the 7150 under test}" "$@"
