# How Cumulus/switchd programs an ACL in the BCM56846 FP — the recipe edged must follow

Captured live from Cumulus Linux 2.5.0 on the AS5610 (2026-07-05) by loading ACLs via
`cl-acltool -i` and reading switchd's SDK programming with `bcmcmd "fp show"` + raw table
dumps. **This is the authoritative answer to edged's long-standing "IFP does no lookup" bug.**
Raw capture data: `edgecore/cumulus-acl-capture-20260705/` (fp show per ACL type + raw
FP_TCAM/POLICY/METER + static config + Cumulus config.bcm/rc.soc).

## The core finding: ACLs are a DOUBLE-WIDE group, split across a slice pair

switchd programs every IPv4 ACL into **GID 3** — an **Ingress, double-wide** field group
spanning **virtual slices 8+9** (→ physical slices **6+7** via FP_SLICE_MAP). Each rule is
**one logical entry split across the pair**:

| | Virtual slice | Physical slice | TCAM idx (this capture) | Holds |
|---|---|---|---|---|
| **Primary** (part 0) | 8 | 6 | 1555 | the **match** qualifiers |
| **Secondary** (part 1) | 9 | 7 | 1811 (=1555+256) | paired half (`VALID=3`, empty key); action via FP_POLICY |

edged failed because it wrote a **single-wide** entry with the action in the *same* slice and
used the global-mask TCAM for the port. The real structure is the above.

## Field-select for the group (FP_PORT_FIELD_SEL, primary slice)
`selcodes[0]: FPF1=5, FPF2=1, FPF3=7` → key = {InPorts, Stage, RangeCheck, StageIngress,
IpType, L2CacheHit, SrcIp, DstIp, DSCP}. (edged already sets F1=5/F2=1/F3=7 on virt slice 8 in
`cumulus_replicate.c` — the field-select was right; the entry *structure* was wrong.)
`selcodes[1]` (secondary): FPF1=12, FPF2=5, FPF3=10 (SrcMac/DstMac/L4 ports/proto/etc.).

## The primary entry — raw FP_TCAM[1555]
```
VALID=3
F2      = 0x00000000 0a1f1f1f 00000000 00000000   <- DstIp (10.31.31.31) in F2 word[2] (bits 64-95), NOT word[3]
F2_MASK = 0x00000000 ffffffff 00000000 00000000   <- clean 0xffffffff over the IP word
PAIRING_F2      = 0x00000000 0a1f1f1f 00000000 00000000   <- pairing mirror of F2
PAIRING_F2_MASK = 0x00000000 ffffffff 00000000 00000000
FIXED_MASK = 0x380 , PAIRING_FIXED_MASK = 0x700          <- IpType/Stage fixed bits
```
Key qualifier offsets in the full key (from `fp show`, for building any match):
| Qualifier | Offset | Width |
|---|---|---|
| L4DstPort | 5 | 16 |
| DstIp | 110 | 32 |
| SrcIp | 142 | 32 |
| IpType | 222 | 4 |
| InPorts | (port bitmap, DATA/MASK per ingress port) | 64 |

Note the **InPorts qualifier is IN the TCAM key** (DATA = the ingress-port bitmap, e.g. bit 4
for swp4), masked per rule — this is how switchd scopes a rule to a port, *not* the
FP_GLOBAL_MASK_TCAM that edged used.

## The action — FP_POLICY_TABLE (drop)
```
G_DROP=1, Y_DROP=1, R_DROP=1,  G/Y/R_COPY_TO_CPU=3 (=SwitchToCpuCancel),  COUNTER_MODE=7
```
(`permit`/ACCEPT = same entry minus the DROP bits. `POLICE`/rate-limit adds
METER_PAIR_MODE + METER_PAIR_INDEX + a FP_METER_TABLE entry.)

## Slice map (live, matches edged's chip)
VS0→phys0, VS1→1, VS2→4, VS3→5, VS4→2, VS5→3, VS6→8, VS7→9, **VS8→6, VS9→7**. Each VS also has
a VIRTUAL_SLICE_GROUP id. FP_TCAM is physical-indexed: idx/256 = physical slice.

## ACL types captured (all offloaded except MAC)
dst-IP, src-IP, L4-dport, ip-proto, **IPv6** (GID 2, SrcIp6/DstIp6), **rate-limit** (meter),
**permit**. MAC/ebtables did NOT offload in this test (follow-up).

## What edged's acl.c must change (the fix)
1. Allocate a **double-wide** slice pair (8+9 → phys 6+7), not a single slice.
2. Write the **match** (DstIp/SrcIp/InPorts/IpType) in the **primary** slice entry (VALID=3),
   with DstIp in F2 **word[2]** (bits 64-95), F2_MASK 0xffffffff there.
3. Write the **paired secondary** entry at idx+256 (VALID=3, empty key) to complete the pair.
4. Put the port scope in the **InPorts qualifier** in the key, not the global-mask TCAM.
5. Action in **FP_POLICY_TABLE**: G/Y/R_DROP=1, COPY_TO_CPU=3, COUNTER_MODE=7.
