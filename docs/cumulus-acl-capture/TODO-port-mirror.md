# TODO: capture SPAN / ERSPAN (port mirror) + MAC ACLs on Cumulus — follow-up

These did NOT offload in the 2026-07-05 capture and are saved for a return visit.

## SPAN (local port mirror) / ERSPAN (remote mirror)
Rule tried (did not create an FP entry): `-A FORWARD --in-interface swp4 -j SPAN --dport swp2`
Likely reason it didn't offload: the mirror **analyzer/destination port must be configured
and UP** first, and switchd needs a mirror destination set up. On a real switch swp2 was
admin-up-no-carrier.

### To capture next time (on Cumulus, box reflashed back):
1. Bring the mirror-dest port fully up (ideally with link): `ip link set swp2 up` (+ a cabled
   port, e.g. use swp4↔4610 as the *source* and another live port as dest).
2. SPAN: `[iptables]  -A FORWARD --in-interface swp4 -j SPAN --dport swp2` then `cl-acltool -i`.
3. ERSPAN (remote, encapsulated to an IP): `-A FORWARD --in-interface swp4 -j ERSPAN
   --src-ip 12.0.0.1 --dst-ip 12.0.0.2 --ttl 64`.
4. Capture: `bcmcmd "fp show"` — expect the action to be `act=MirrorIngress` (or a
   mirror-to-port), and in FP_POLICY the `MTP_INDEX0..3` (mirror-to-port index) fields set.
   Also dump the mirror destination setup: `bcmcmd "mirror show"` and the `MIRROR` /
   `IM_MTP_INDEX` tables. ERSPAN also programs an egress encap (tunnel) — capture EGR tables.

## MAC / ebtables ACLs
Rule tried (did not offload): `[ebtables]  -A FORWARD -d 00:11:22:33:44:55 -j DROP`
Man page says ebtables IS supported on swp ports. Retry with: an ethertype match
(`-p IPv4`/`-p 0x0800`) and/or source MAC, and check it lands in an ingress group with
`DstMac`/`SrcMac`/`EtherType` qualifiers (selcodes[1] of GID 3 has DstMac/SrcMac/EtherType).
Verify the ebtables rule reaches the kernel (`ebtables -t filter -L`) first.

## How to get back on Cumulus
Reflash procedure: `edgecore/edgecore-5610-reverse-engineering/CUMULUS_INSTALL_RUNBOOK.md`
(+ the 2026-07-05 learnings: ONIE 2017 needs fdisk/mkfs.ext2/fw_setenv symlinked to busybox
OR skip_disk_format=y; and set U-Boot **bootsource=flashboot** so it boots the installed
Cumulus instead of falling to ONIE). License: clock to 2013-10-01 + cp /home/smiley/license.txt
to /etc/cumulus/.license.txt + `service switchd start`. Login cumulus / CumulusLinux!.
