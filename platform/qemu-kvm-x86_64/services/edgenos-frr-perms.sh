#!/bin/sh
# edgenos-frr-perms.sh: normalise /etc/frr ownership before FRR starts. Package overlays install
# config files as root:root; FRR's non-root vtysh model wants /etc/frr frr:frrvty (0750, files 0640)
# so members of frrvty (the admin account) can run vtysh.
[ -d /etc/frr ] || exit 0
chown frr:frrvty /etc/frr 2>/dev/null
chmod 0750 /etc/frr 2>/dev/null
for f in /etc/frr/*; do
    [ -f "$f" ] || continue
    chown frr:frrvty "$f" 2>/dev/null
    chmod 0640 "$f" 2>/dev/null
done
exit 0
