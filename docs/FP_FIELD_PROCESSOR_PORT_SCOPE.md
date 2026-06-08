# Field Processor (FP) — what it is, and the scope to port it

Context: OSPF inbound multicast (224.0.0.5) never reaches the CPU on EdgeNOS. We
proved (via the OpenBCM SDK) the chip does **not** flood multicast to the CPU by
design; control traffic reaches the CPU only via explicit copy-to-CPU **traps**,
and for an IP-multicast-on-an-L3-port like OSPF the trap mechanism is the
**Field Processor**. This document explains what the FP is and scopes the work to
bring it up.

---

## Part 1 — What the Field Processor is

The Field Processor (FP) is the switch chip's **programmable packet classifier** —
the hardware ACL / policy engine. It's a set of TCAMs that match arbitrary packet
fields and, on a hit, apply actions (copy-to-CPU, drop, redirect, set CoS class,
police/rate-limit, count, mirror, …). Everything a NOS does with iptables-style
control-plane rules, ACLs, QoS classification, and CoPP is implemented in the FP.

### Stages
There are three FP stages, each a separate TCAM bank, applied at different points:

| Stage | When | Typical use |
|---|---|---|
| **VFP** (VLAN FP) | ingress, *before* L2/L3 lookup | VLAN assignment, pre-classification |
| **IFP** (Ingress FP) | ingress, *after* L2/L3 lookup | **control-plane traps (OSPF/BGP/…), ACLs, QoS** ← what we need |
| **EFP** (Egress FP) | egress | egress filtering/marking |

### IFP geometry on Trident+ (BCM56846, our chip)
- **9 slices**, **256 entries/slice** = 2,304 IFP rules.
- A **slice** is one TCAM block. Each slice produces a fixed-width **key** by
  extracting a chosen set of packet fields. *Which* fields a slice extracts is set
  by its **field selectors** (`FPF0/FPF1/FPF2/FPF3`, a.k.a. F1/F2/F3 in the
  register) — programmed in `FP_PORT_FIELD_SEL` (per ingress port, per slice).
- A **rule** = one TCAM entry: a `{KEY, MASK}` (in `FP_TCAM` + `FP_GLOBAL_MASK_TCAM`)
  plus an **action** (`FP_POLICY_TABLE`) and optional meter/counter
  (`FP_METER_TABLE`, `FP_COUNTER_TABLE`).

### The chip tables involved
| Table | Role |
|---|---|
| `FP_PORT_FIELD_SEL` | per-port: which field group (F1/F2/F3 selcode) each slice extracts |
| `FP_SLICE_MAP` | maps virtual→physical slices (priority ordering) |
| `FP_SLICE_KEY_CONTROL` | per-slice key sub-selectors (TOS/TTL/TCP/class) |
| `FP_GM_FIELDS` | global-mask field definitions (2048 entries) |
| `FP_GLOBAL_MASK_TCAM` | per-rule global key/mask + ingress-port-bitmap (IPBM) |
| `FP_TCAM` | per-rule key + mask (the match) |
| `FP_POLICY_TABLE` | per-rule action (copy-to-cpu, drop, redirect, …) |
| `FP_METER_TABLE` / `FP_COUNTER_TABLE` | rate-limit / stats |

### The SDK abstraction (and why it's non-trivial)
The OpenBCM SDK hides all that behind an API:
- **qset** (qualifier set): which fields you want to match (e.g. `IpProtocol`).
- **group**: a set of slices sharing a key layout, created for a qset at a priority.
- **entry**: a rule — `qualify_X(value, mask)` + `action_add(Y)`.
- **aset**: the action set.

The hard part is the **selcode engine**: `bcm_field_group_set()` auto-selects which
slice field-selector codes (`fpf0..fpf3`) satisfy the qset, and
`_bcm_field_trx_selcodes_install()` programs `FP_PORT_FIELD_SEL` accordingly. Then
each qualifier maps to a **bit offset within the slice key** — e.g. on Trident
(`src/bcm/esw/trident/field.c`) `bcmFieldQualifyIpProtocol` is in the **FPF2**
group at slice-key offset **`f2_offset(46) + 56 = 102`**, width 8. That slice-key
offset must still be translated to a **physical FP_TCAM key bit** by the key-
assembly logic — which is why raw-poking the tables by hand is error-prone.

### How OSPF maps onto this
The fix is a single IFP rule:
```
qualify IpProtocol == 89 (0x59)   →   action CopyToCpu
```
That's exactly what Cumulus's `00control_plane.rules` (`-p ospf -j SETCLASS class 7`)
compiles into. Today EdgeNOS writes `FP_TCAM` + `FP_POLICY` (replicated raw from a
Cumulus capture) but **never programs the slice infrastructure**
(`FP_PORT_FIELD_SEL`/`FP_SLICE_MAP`/…), so the chip builds keys in a different
layout than our entries expect and nothing ever matches.

---

## Part 2 — Why a port (not raw replication)

Two ways to get there; the port is the right long-term one:

1. **Raw replication** of Cumulus's captured FP state (slice config + ~2250 table
   entries + the 100 rules). Tractable but: the 100 `FP_POLICY` rows carry
   `DROP=1`, so a mis-replication **drops production traffic** on the one working
   switch (we already broke + recovered the datapath once via an EGR_MASK polarity
   mistake; FP DROP rules are worse — they persist across an edged restart). And it
   only gets us Cumulus's exact rule set, frozen.

2. **Port the SDK field module** — bring the OpenBCM `bcm_field` engine (group /
   entry / qualifier / action / selcode / key-assembly) into edged. Larger, but it
   makes the FP *programmable*: OSPF, BGP CoPP, ACLs, QoS, storm control all become
   a few API calls, correctly, with no hand-built keys. This is the foundation a
   full-featured NOS needs anyway.

We now have the complete `OpenBCM/sdk-6.5.27` source (incl. `src/bcm`), so the port
is a guided effort, not reverse-engineering.

---

## Part 3 — Port scope (phased, each phase independently verifiable)

Guiding principle: **no `DROP` actions until the engine is trusted** — every phase
uses `CopyToCpu` only, so a wrong key just fails to match (no datapath regression).
Keep a known-good `edged` backed up on the box; ping the Nexus after every deploy;
remember FP-enable/`APPLY_*` bits persist across an edged restart, so recovery must
explicitly clear them (or reboot).

### Phase 0 — Harness (low risk, do first)
- Add an `edged` FP read-back diag (SIGUSR1 or `/tmp` trigger): dump
  `FP_PORT_FIELD_SEL[port]`, `FP_SLICE_MAP[0]`, `FP_SLICE_KEY_CONTROL[0]`, and a
  given `FP_TCAM`/`FP_POLICY` index.
- Extend the existing `/tmp/regdump.in` path to cover these memories so we can diff
  live-vs-Cumulus-SOCMEM field-by-field.
- **Exit criteria:** we can read every FP table we intend to write.

### Phase 1 — Port the device stage init (slice setup)
- From `src/bcm/esw/trident/field.c`: port `_field_trident_ingress_qualifiers_init`
  (the qualifier→offset table) and the IFP **stage/slice setup** that programs
  `FP_SLICE_MAP`, `FP_PORT_FIELD_SEL`, `FP_SLICE_KEY_CONTROL`, and the
  `FP_GM_FIELDS` defaults.
- Cross-check the resulting register values against the captured Cumulus SOCMEM
  (`dump_socmem.txt.gz`) — they should match field-for-field.
- **Exit criteria:** one IFP slice is configured for the **FPF2 (IP/L4)** group and
  reads back identical to Cumulus; ping still 0%.

### Phase 2 — Port the minimal selcode + key-assembly
- From `src/bcm/esw/field.c` + `field_common.c`: port the subset that, given a qset
  containing `IpProtocol`, (a) picks the FPF2 selcode, (b) computes the **physical
  FP_TCAM key bit** for the qualifier (translate slice-key `f2_offset+56` → physical
  key bit), and (c) builds `{KEY, MASK}`.
- Validate by reconstructing a **known captured rule** (e.g. the IGMP proto-2 or
  ICMP proto-1 rule) and confirming our computed key/mask matches the captured bytes.
- **Exit criteria:** we can produce a byte-exact `FP_TCAM` key for "IpProtocol == N".

### Phase 3 — The OSPF rule (the actual goal)
- Program one IFP entry: `FP_TCAM` key = IpProtocol 0x59 (+ the slice/IPBM bits),
  `FP_GLOBAL_MASK_TCAM` = permissive (all ingress ports), `FP_POLICY` =
  `COPY_TO_CPU` (CoS class 7, **no DROP**), `FP_METER` optional rate-limit.
- Enable the slice / `FP` for that entry.
- **Exit criteria:** `tcpdump -i swp1 ip proto ospf` shows the Nexus hellos at the
  CPU → `ospfd` adjacency reaches **Full** → routes install via the FIB→chip ECMP
  path we already have. Ping still 0%.

### Phase 4 — Generalize to the `bcm_field` API (full-featured)
- Wrap the ported engine in the OpenNSL-style API surface: `group_create(qset)`,
  `entry_create`, `qualify_*`, `action_add`, `group_install`.
- Re-implement the control plane as API calls: OSPF/BGP/BFD/IGMP/ICMP/ARP/DHCP CoPP
  (matching `00control_plane.rules`), each with a police meter.
- This unlocks ACLs and QoS as well.
- **Exit criteria:** the full `control_plane.rules` set is expressible and installed
  via the API (replacing the raw-replicated 100 rows), with meters.

---

## Part 4 — Files to port (from `OpenBCM/sdk-6.5.27`)
| File | What to take |
|---|---|
| `src/bcm/esw/trident/field.c` | device stage init, qualifier offset table, slice setup, `FP_SLICE_MAP` write |
| `src/bcm/esw/field.c` | generic group/entry/qualifier/action engine + selcode selection + key assembly (port the minimal subset) |
| `src/bcm/esw/field_common.c`, `field_reg_mem.c` | helpers, register/memory glue |
| `include/bcm/field.h`, `include/bcm_int/esw/field.h` | qualifier/action enums, structs |

## Cross-reference assets we already have
- **Cumulus SOCMEM/SOC dumps** — ground-truth values for every FP table.
- **The 100 captured `FP_TCAM`/`FP_POLICY` rows** (`asic/edged/generated/`) — to
  validate key construction against real rules.
- **Decompiled Cumulus `libopennsl`** (`edgecore-5610-reverse-engineering/.../ghidra-analysis/`,
  `FP_MMU_AND_REMAINING.md`) — the exact `bcm_field` call sequence for our chip.

## Risk register
- `FP_POLICY.DROP=1` and `APPLY_*`/FP-enable bits **persist across edged restart**
  (read-modify-write init preserves them). Recovery = explicit-write-0 or reboot.
- Keep `edged.goodbak` on the box; ping-verify each deploy; CopyToCpu-only until
  Phase 4. One physical switch — do not risk it on un-verified DROP rules.
