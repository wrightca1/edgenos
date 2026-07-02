# EdgeNOS ACLs — Design & Implementation Plan

Add a **configurable ACL feature**: operator-defined permit / deny / rate-limit rules programmed
into the switch silicon (the Field Processor / ACL engine) on both switches, through one unified
CLI + config + HAL — mirroring the existing `l2-groups` and L3-sync patterns.

Today the only silicon ACL work is `asic/bcm56846/cumulus_replicate.c` (fixed Cumulus control-plane
trap rules, drops forced off). This makes ACLs a real, user-driven feature.

---

## 1. User-facing model

An **ACL** is a named, ordered list of **rules**; each rule is a **match** + an **action**.
Lowest sequence number = highest priority. Unmatched traffic follows normal forwarding
(**implicit permit** — chosen so adding an ACL can't silently black-hole a box).

| | v1 (first cut) | later |
|---|---|---|
| **Match** | ingress port(s), src/dst IPv4+IPv6, IP protocol, L4 src/dst port | MAC, VLAN, DSCP, TCP flags, ICMP type |
| **Action** | `permit`, `deny` (drop), implicit `count` | `rate-limit` (meter), `copy-to-cpu`, `redirect`, `set-dscp` |
| **Bind** | ingress, per front port (or `all`) | egress, per-VLAN |

---

## 2. CLI — `edgenos acl` (mirrors `edgenos l2`)

```
edgenos acl show
edgenos acl set   <name> <seq> permit|deny <proto> <src-cidr> <dst-cidr> [dport N] [sport N]
edgenos acl del   <name> [<seq>]
edgenos acl apply <name> <port>...      # bind to ingress ports
edgenos acl unapply <name>
edgenos acl clear
```

Auto-detects the datapath daemon (`edged`/`bcmd`), writes its `acls.conf`, and `SIGHUP`s it for a
**live** re-program — same mechanism as `edgenos l2`.

---

## 3. Config file — `/etc/{edged,bcmd}/acls.conf`

```
# <name> <seq> <action> <proto> <src-cidr> <dst-cidr> [dport N] [sport N]
mgmt-protect 10 deny   tcp  any          10.14.1.0/24  dport 23
mgmt-protect 20 permit ip   any          any
# bind:  <name> apply <port>...
mgmt-protect apply ge25 ge26
```

Loaded at boot and on `SIGHUP`, exactly like `l2-groups.conf`.

---

## 4. Architecture (fits the core/ + asic/ split)

```
   acls.conf ─▶ core/datapath/acl.c  (portable rule model + parser + HAL seam)
                        │  acl_program(unit, ruleset, ports)
        ┌───────────────┴────────────────┐
        ▼                                 ▼
 asic/bcm56846  (raw FP via SCHAN)   asic/bcm56340  (OpenBCM bcm_field SDK)
   the HARD backend                    the EASY backend
```

- **`core/datapath/acl.c` / `acl.h`** — chip-independent: parse config, hold the ordered ruleset,
  diff on SIGHUP, call the ASIC backend. Same shape as the L2/L3 code already split this way.
- Two backends behind one interface, picked at build/platform time.

---

## 5. Silicon backends

### AS4610 (bcmd / BCM56340) — SDK, straightforward
```
 bcm_field_group_create(unit, qset, prio, &grp)     # qset = InPort, SrcIp[6], DstIp[6], IpProtocol, L4DstPort…
 per rule:
   bcm_field_entry_create(unit, grp, &e)
   bcm_field_qualify_SrcIp / DstIp / L4DstPort / IpProtocol / InPort(...)
   bcm_field_action_add(e, bcmFieldActionDrop | CopyToCpu | ...)
   bcm_field_entry_prio_set(e, seq); bcm_field_entry_install(e)
 rate-limit: bcm_field_meter_* / bcm_policer_*
```
The SDK manages slices, field selection, and TCAM packing. **Low risk, moderate effort.**

### AS5610 (edged / BCM56846) — raw FP, no SDK
No SDK — program the Field Processor directly over SCHAN, as `cumulus_replicate.c` already does:
- **`FP_PORT_FIELD_SEL` / slice config** — which packet fields form the lookup key (we already have
  the Cumulus-captured selection as a starting point).
- **`FP_TCAM`** — key + mask per rule (built from the match fields).
- **`FP_POLICY_TABLE`** — action per rule (drop / copy-to-CPU / meter pointer).
- **`FP_METER_TABLE`** — for rate-limit.
- Plus slice allocation + priority/ordering by hand.

**High risk, large effort** — a wrong FP write can drop all traffic (cumulus_replicate forces
`*_DROP=0` for exactly this reason). Needs read-back verification and careful testing.

---

## 6. Phased plan

1. **Phase 1 — model + CLI + 4610 backend.** `permit`/`deny`/`count`, v4+v6 5-tuple, ingress
   per-port. Quickest path to a working feature; validate on the live AS4610.
2. **Phase 2 — 5610 raw-FP backend.** Same rule model, programmed into the FP by hand (reusing the
   cumulus_replicate field-selection knowledge). The hard part.
3. **Phase 3 — actions++**: rate-limit (meters), copy-to-cpu, more match fields, egress ACLs.
4. **Phase 4 — Web UI module** (rule table + apply, capability-driven like the OSPF/L2 pages).

---

## 7. Risks & decisions to lock

- **Lockout safety.** A `deny` mistake could break front-port forwarding. Mgmt survives (separate
  NIC), but consider implicit-permit (chosen), a `commit-confirm` timeout, or a dry-run.
- **FP resource limits.** Slices + TCAM entries are finite; validate counts and fail cleanly.
- **Precedence.** Define `seq → hardware priority` mapping explicitly and identically on both chips.
- **Atomic live apply.** On SIGHUP, don't leave a half-programmed FP — build then swap.
- **v6 keys are double-wide** in the FP (as with L3_ENTRY_IPV6) — the 5610 backend must account for it.

---

## 8. Effort snapshot

| Phase | Platform | Risk | Effort |
|-------|----------|------|--------|
| 1 (model+CLI+4610) | 4610 SDK | low | moderate |
| 2 (5610 FP) | 5610 raw | **high** | large |
| 3 (meters/actions) | both | med | incremental |
| 4 (webui) | both | low | small |

**Recommended start:** Phase 1 on the AS4610 (live + SDK does the heavy lifting), which also nails
down the portable model + CLI + config that Phase 2 reuses.

---

## 9. Status

**Phase 1 — DONE and hardware-verified on the AS4610 (bcmd).**
- `edgenos acl set|apply|del|unapply|show|clear` (core/cli/edgenos) → `/etc/bcmd/acls.conf`, SIGHUP → live re-program.
- `bcmd` ingress FP backend (`bcm_field`): v4/v6 groups, InPorts + src/dst + proto + L4 ports, `permit`/`deny`, `seq → priority`.
- Verified live (5610 → 4610 xe0): baseline 0% → `deny icmp` 100% (dropped in silicon) → `clear` 0% (restored); `permit` seq 5 overrides `deny` seq 10 (0%); an ACL with no `apply` line is inert.

**Gotcha found:** `bcm_field_group_flush` does **not** uninstall entries from the TCAM on the BCM56340 —
a bare flush left stale drops in place (clear looked applied but traffic stayed blocked). Fix: track
each installed entry and `bcm_field_entry_remove` + `bcm_field_entry_destroy` on reset.

**Phase 2a — IN PROGRESS on the AS5610 (edged, raw FP).** `asic/bcm56846/acl.c` parses the same
`acls.conf` and hand-programs the FP for **destination-IP** deny/permit, reusing the VS6 single-wide
DstIP slice that `cumulus_replicate.c` stands up (OSPF trap at entry 0 / idx 1024; ACL entries at
1025+). Wired into boot + SIGHUP; shares the `edgenos acl` CLI + config with the 4610. Rules needing
src/proto/L4 are skipped (that slice-key layout isn't mapped yet).

- **Pipeline proven** on the live 5610: CLI → `acls.conf` → SIGHUP → edged, and the FP entry is
  programmed with the correct key/action (file-log `/tmp/edged-acl.log`:
  `prog idx=1025 f2[3]=0x0a656601/0xffffffff DENY`).
- **Not yet dropping:** the deny didn't drop a ping to the switch's *own* IP — that dest is punted to
  the CPU by MY_STATION independent of the IFP drop, so the hand-rolled drop needs to suppress the
  local-CPU punt (the 4610 SDK does this for free). **Next:** add an FP match counter (like the VS6
  trap) for observability, the FP entry is written correctly (read-back confirms) but the VS6 slice does **no IFP
  lookup** — counter stays 0 for the ACL entry *and* the pre-existing OSPF trap in that slice. Enabling
  physical-slice-4 lookup didn't fix it. **Blocker:** the FP slice-lookup config (virtual-vs-physical
  slice indexing, slice size, which slice actually matches) needs the real config off a working Cumulus
  box (`bcmcmd fp show` / reg diff) or the BCM56846 FP register spec — not blind bit-guessing.

**Then:** meters/rate-limit + copy-to-cpu actions, and the Web UI ACL module.
