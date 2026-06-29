# EdgeNOS operator guides

Task-oriented how-tos for running an EdgeNOS switch. (For the system architecture see
[`../EDGENOS_SUMMARY.md`](../EDGENOS_SUMMARY.md); for what's planned see
[`../ROADMAP.md`](../ROADMAP.md).)

## Available

- **[L2-switch mode](l2-switch.md)** — bridge a set of ports into one L2 broadcast
  domain (dumb-switch behavior) via the CLI, web UI, or config file.

## Planned (stubs to fill in)

- **Interface IP addressing** — assign IPv4 to ports (CLI / web UI / `addrs.conf`).
- **OSPF** — bring up OSPF, read neighbors and learned routes, add networks live.
- **ECMP** — multipath routing and verifying it's hardware-forwarded.
- **Packages (`.epk`)** — install/remove components on a live switch; the modular web UI.
- **Web UI** — enabling it, the mgmt-only binding, and the capability-driven modules.
- **Install / upgrade** — flashing an ONIE image; switching a box between NOSes.

Each guide is one task, with the CLI, the web UI, and the underlying config file shown
side by side, and a "how to verify" section. Keep them copy-pasteable.
