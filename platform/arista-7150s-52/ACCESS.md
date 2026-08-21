# Arista 7150S-52 — getting into the switch

Defaults for a freshly flashed EdgeNOS image. Nothing here is a secret and
nothing is site-specific; change both on anything exposed.

## Default credentials

| | |
|---|---|
| user | `root` |
| password | `arista` |

This matches the platform's factory default deliberately, so there is one fewer
thing to look up on a bench. **Change it.**

```sh
passwd                                    # interactive
# or install a key (survives only until reboot unless placed on flash):
mkdir -p /root/.ssh && cat /mnt/flash/authorized_keys >> /root/.ssh/authorized_keys
```

The root filesystem is a RAM initramfs: anything written outside `/mnt/flash`
is gone at the next boot. `/mnt/flash` is the only persistent storage.

## Default management address

The management port (`eth0`, the RJ45 marked MGMT) comes up **static**:

| | |
|---|---|
| address | `192.168.1.1/24` |
| gateway | none |

Static rather than DHCP on purpose — the switch is then always reachable at a
known place on a bench with no dependency on a DHCP server. (A DHCP build was
tried and removed: `tg3` brings this link up ~27 s into boot, after init has
already run, so the client broadcast into a dead link and left the port with no
address at all.)

### Changing it permanently

Create `/mnt/flash/mgmt.conf` — it is read at every boot and is not part of the
image:

```sh
cat > /mnt/flash/mgmt.conf <<'CONF'
MGMT_IP=198.51.100.10/24
MGMT_GW=198.51.100.1
CONF
sync
```

| variable | meaning |
|---|---|
| `MGMT_IP` | address with prefix length, e.g. `198.51.100.10/24` |
| `MGMT_GW` | optional default route |
| `MGMT_ROUTE` | optional CIDR pinned to `eth0` — see the warning below |

### Changing it right now, without a reboot

```sh
ip addr flush dev eth0
ip addr add 198.51.100.10/24 dev eth0
ip route add default via 198.51.100.1
```

### ⚠ If management dies the moment the dataplane comes up

Once `ospfd` starts it installs routes learned through the front-panel ports, and
one of them can be **more specific than your default route**. Replies to your SSH
session then leave by a front-panel port and never come back — the switch looks
dead while being perfectly healthy.

Set `MGMT_ROUTE` to the network you manage the switch from. It is pinned to
`eth0` at metric 5 so it wins:

```
MGMT_IP=198.51.100.10/24
MGMT_GW=198.51.100.1
MGMT_ROUTE=203.0.113.0/24
```

Check with `ip route get <your-workstation>` — it must say `dev eth0`.

## Serial console

9600 8N1, and it needs no login — the image drops straight to a root shell. It
works regardless of any network configuration, so it is the recovery path if you
lock yourself out.

## Recovering a bad image

`boot-config` on flash selects the image, and EdgeNOS rewrites it back to the
vendor OS at every boot, so a bad image self-recovers on the next power cycle.
To pick one by hand from the console:

```sh
echo SWI=flash:/<image>.swi > /mnt/flash/boot-config && sync && reboot -f
```

From Aboot (Ctrl-C during early boot) the same file can be edited before any
EdgeNOS code runs.
