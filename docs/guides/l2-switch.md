# How-to: L2-switch mode for a set of ports

By default every front port on an EdgeNOS switch is its **own L3 interface** — give
it an IP and traffic between ports is routed. Sometimes you instead want a group of
ports to behave like a plain unmanaged switch: bridge them into one L2 broadcast
domain so they MAC-learn and forward directly to each other in hardware, with no
routing in between.

That's an **L2 group**. You pick a VLAN id and a set of ports; those ports are pulled
off their per-port L3 service VLANs and onto the shared VLAN. It applies **live** (no
reboot) and persists across reboots.

Works the same on both switches; only the port names differ:

| Switch | Datapath daemon | Port names | Config file |
|--------|-----------------|------------|-------------|
| AS5610-52X | `edged` | `swp1`..`swp52` | `/etc/edged/l2-groups.conf` |
| AS4610-54T | `bcmd`  | `ge1`..`ge48`, `xe0`.. | `/etc/bcmd/l2-groups.conf` |

---

## Option A — the CLI (the quickest "lever")

```sh
# bridge three ports into one L2 switch on VLAN 100
edgenos l2 set 100 swp1 swp2 swp3        # (5610)
edgenos l2 set 100 ge1 ge2 ge3           # (4610)

# see what's grouped
edgenos l2 show

# remove one group, or all groups (ports go back to being L3 interfaces)
edgenos l2 del 100
edgenos l2 clear
```

`edgenos l2` auto-detects the datapath daemon, writes its `l2-groups.conf`, and signals
it (`SIGHUP`) to re-apply **immediately** — no restart, no traffic hit on other ports.

## Option B — the web UI

Open the management web UI and choose **L2 Switch** in the sidebar. Tick the ports you
want bridged, enter a VLAN id, and click **Bridge selected ports**. The current groups
are listed with **remove** buttons, and **Clear all** puts every port back to L3. (The
page only appears when a datapath daemon that supports L2 groups is running.)

## Option C — the config file directly

Edit the daemon's `l2-groups.conf` — one group per line, `<vid> <port> <port> ...`:

```
# /etc/edged/l2-groups.conf   (5610)
# bridge ports 1-3, and separately ports 10-11
100 swp1 swp2 swp3
200 swp10 swp11
```

Then apply it live with `pkill -HUP edged` (or `pkill -HUP bcmd` on the 4610) — or just
reboot; it's read at startup.

---

## Verifying

```sh
edgenos l2 show
```
and in the datapath log you'll see each port being moved, e.g.:
```
L2 group 100: + swp2 (lane 5, moved off service VID 3302)
L2 group VLAN 100: 3/3 member ports active
```
A quick functional check: put two hosts on two ports of the same group (no IPs needed)
— they can reach each other directly; a host on a port outside the group cannot.

## Notes

- **VLAN id** must be `2`–`4094` and outside the reserved per-port service range.
- A grouped port is removed from its per-port L3 service VLAN, so don't put an OSPF/L3
  uplink into an L2 group unless you mean to stop routing on it. `edgenos l2 clear`
  (or `del`) restores the port to L3.
- **Persistence:** the config file lives on the read-write overlay, so groups survive
  reboots. On the AS4610, stage the persistent copy via the platform's config-overlay
  if you're baking it into an image.
- **Isolation:** different groups (and the remaining L3 ports) are isolated from each
  other — each group is its own broadcast domain.
