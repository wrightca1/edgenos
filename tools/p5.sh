#!/bin/sh
# Addresses are deliberately not baked in: this tree is published, and the lab
# topology is not ours to publish. Export the host, or put it in a local
# (gitignored) env file:  export P5_HOST=<addr>
# p5.sh -- run a command on the AS5610 peer.
#
# The peer is BOTH ends of the transit path: swp7 = 10.101.101.33 faces the
# 7150's et2 (ingress), swp6 = 10.101.101.25 faces et1 (egress). That is why
# transit-test.sh installs a /32 via swp7 -- without it the peer delivers
# locally and nothing ever touches the switch.
#
# Key auth (added 2026-08-20). Falls back to the documented root/as5610 password
# only if P5_PASS is set explicitly.
if [ -n "${P5_PASS:-}" ]; then
    exec sshpass -p "$P5_PASS" ssh \
        -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o PubkeyAuthentication=no -o LogLevel=ERROR "root@${P5_HOST:?set P5_HOST to the AS5610 peer}" "$@"
fi
exec ssh -o ConnectTimeout=10 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o BatchMode=yes -o LogLevel=ERROR "root@${P5_HOST:?set P5_HOST to the AS5610 peer}" "$@"
