# asic/vswitch — the software "ASIC" (no silicon)

An L2 learning switch over Linux netdevs (AF_PACKET, promiscuous), exposed through
`core/datapath/asic_ops.h` — the same backend seam the Arista 7150 (FM6000) uses, so the
same board daemon loop drives real silicon and this. Used by the QEMU/KVM x86_64 virtual
platform (`edged-vswitch`, see `platform/qemu-kvm-x86_64/edged.c`).

| asic_ops | here |
|---|---|
| `init()` | open every `pge<N>` netdev (or `EDGENOS_VSWITCH_PORTS`), promisc, link up, epoll |
| `port_set(port, en, speed)` | admin up/down the netdev (speed ignored: virtio) |
| `tx(frame)` | CPU → fabric: learn CPU MAC, lookup DA, forward or flood |
| `rx_poll(budget, cb)` | move frames port→port (learn/forward/flood), deliver punts to `cb` |
| `intr_fd()` | the epoll fd (readable when any port has frames) |
| `shutdown()` | close, print counters |

Model (M2, intentionally small): one L2 domain, MAC learning + 300 s ageing, unknown-
unicast/broadcast/multicast flooding, the CPU is port 64 in the MAC table (its MAC is
learnt from injected frames), hairpin drop. No VLANs / L3 offload yet: those grow behind
the seam. `kill -USR1 <edged-vswitch>` dumps ports, counters and the MAC table.

Mode switch on the virtual platform: `/etc/edgenos/datapath` = `none` (default: kernel
forwards on `ge*`) or `vswitch` (NICs become `pge*`, owned by the daemon; the control
plane sees `cpu0`). `edgenos-datapath-mode vswitch|none` writes it; takes effect at boot.
