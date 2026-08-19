# tools/

## Generators — run these, they are the supported path

| tool | what it does |
|---|---|
| `mkconfigbcm.sh <ip>` | reads the board's port map from your own switch (while it still runs the vendor OS) and writes a `config.bcm` |
| `mkpolarity.sh <ip>` | reads the SerDes polarity table the same way; `POLARITY=1 mkconfigbcm.sh` chains it |

Both are **read-only** — every command they issue is a show command or a register
read, and they are safe to run against a switch carrying live traffic.

`mkconfigbcm.sh` reproduces 314 of the 410 properties of a known-working
configuration, with the port map (78/78) and polarity (62/62) identical. The 96
it does not emit are `serdes_preemphasis_*`, which are not load-bearing — see
`../PROVENANCE.md`.

⚠ One safety note carried over from the bring-up: use `phy raw sbus`, never
`getreg`. A blind `getreg` sweep wedged the reference switch hard enough to need
a physical power cycle.

## Test harness — read these, do not expect them to run

`edgenos-2port-test.sh`, `edgenos-l3hw-test.sh` and `verify-ospf.sh` are the
scripts that produced the forwarding measurements quoted in the README. They are
included because a claim like "992 of 1000 packets, CPU counter flat" should come
with the thing that measured it.

They are **a lab harness, not a product**. They hardcode the addressing of the
bench they ran on (`10.101.101.40/29` to an AS5610, `10.101.101.56/29` to a
Nexus), fetch binaries over HTTP from a build host (`SERVE=` overrides it), and
assume a switch that can be rebooted at will. Read them for the method; adapt
them for anything else.
