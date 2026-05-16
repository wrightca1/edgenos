#!/usr/bin/env python3
"""
Generate C header files containing per-table row data extracted from
Cumulus's captured `dump_socmem_diff.txt`.

These tables are loaded into the chip at boot by `cumulus_replicate.c`
to fix four documented chip→CPU drop causes (see decoded.md/14_*.md):

  * L2_USER_ENTRY     — 63 protocol-MAC CPU-trap entries
  * EGR_VLAN          — 53 service-VID egress rows (VLAN 1 + 3301..3352)
  * EGR_VLAN_STG      — egress STG-1 forwarding state
  * FP_TCAM           — 100 chip-side CPU-trap TCAM rows
  * FP_POLICY_TABLE   — 100 matching policy rows

Run from the asic/edged directory:
    ./scripts/gen_cumulus_tables.py

Outputs to ./generated/cumulus_<table>.h
"""
import os, re, sys

CAPTURE = "/home/smiley/edgecore/edgecore-5610-reverse-engineering" \
          "/cumulus_baseline_2013_run2/streamed_20260513_162341/soc" \
          "/dump_socmem_diff.txt"

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "generated")
os.makedirs(OUT, exist_ok=True)


def parse_fields(s):
    """Parse 'A=1,B=0x20,...' into dict (trailing commas ok, hex preserved)."""
    out = {}
    for kv in s.split(","):
        kv = kv.strip()
        if not kv or "=" not in kv: continue
        k, v = kv.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def parse_table(name):
    """Yield (index, fields-dict) for every '<name>.<scope>[idx]: <...>' row."""
    pat = re.compile(rf"^{re.escape(name)}\.\w+\[(\d+)\]:\s*<([^>]*)>")
    with open(CAPTURE, encoding="latin-1") as f:
        for line in f:
            m = pat.match(line)
            if not m: continue
            yield int(m.group(1)), parse_fields(m.group(2))


def fmt_val(v):
    """C-compatible integer literal — preserves hex format."""
    return v if v.startswith("0x") else v


def write_header(name, table_name, rows, fields, wide=None):
    """rows: [(idx, {field:val})];  wide = set of field names that need uint64_t."""
    wide = wide or set()
    path = os.path.join(OUT, f"cumulus_{name}.h")
    with open(path, "w") as f:
        f.write(f"/*\n * Auto-generated from Cumulus capture by\n"
                f" * asic/edged/scripts/gen_cumulus_tables.py — do not edit by hand.\n"
                f" *\n * Source: {CAPTURE}\n */\n")
        f.write(f"#ifndef _CUMULUS_{name.upper()}_H_\n#define _CUMULUS_{name.upper()}_H_\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"struct cumulus_{name}_row {{\n")
        f.write("    uint32_t index;\n")
        for fld in fields:
            ctype = "uint64_t" if fld in wide else "uint32_t"
            f.write(f"    {ctype} {fld.lower()};\n")
        f.write("};\n\n")
        f.write(f"static const struct cumulus_{name}_row cumulus_{name}_rows[] = {{\n")
        for idx, fdict in rows:
            vals = []
            for fld in fields:
                # Strip stray non-hex chars; values from capture are clean.
                vals.append(fmt_val(fdict.get(fld, "0")) + ("ULL" if fld in wide else ""))
            f.write(f"    {{ {idx}, " + ", ".join(vals) + " },\n")
        f.write("};\n\n")
        f.write(f"#define CUMULUS_{name.upper()}_COUNT "
                f"(sizeof(cumulus_{name}_rows)/sizeof(cumulus_{name}_rows[0]))\n\n")
        f.write("#endif\n")
    print(f"  wrote {path}  ({len(rows)} rows)")


def main():
    # --- L2_USER_ENTRY: protocol-MAC traps ---
    # MAC_ADDR is 48 bits; KEY is 61 bits — both need uint64_t.
    rows = list(parse_table("L2_USER_ENTRY"))
    write_header("l2_user_entry", "L2_USER_ENTRY", rows,
        ["VALID", "KEY_TYPE", "MAC_ADDR", "KEY",
         "L2_PROTOCOL_PKT", "DO_NOT_LEARN_MACSA",
         "CPU", "BPDU"],
        wide={"MAC_ADDR", "KEY"})

    # --- EGR_VLAN: service VLANs ---
    rows = list(parse_table("EGR_VLAN"))
    write_header("egr_vlan", "EGR_VLAN", rows,
        ["VALID", "STG",
         "PORT_BITMAP_W0", "PORT_BITMAP_W1", "PORT_BITMAP_W2",
         "UT_PORT_BITMAP_W0", "UT_PORT_BITMAP_W1", "UT_PORT_BITMAP_W2",
         "UT_BITMAP_W0", "UT_BITMAP_W1", "UT_BITMAP_W2"])

    # --- EGR_VLAN_STG: STG state ---
    rows = list(parse_table("EGR_VLAN_STG"))
    # 1 row with sp_tree_portN fields — extract dynamically
    if rows:
        all_fields = set()
        for _, d in rows: all_fields.update(d.keys())
        sp_fields = sorted([f for f in all_fields if f.startswith("SP_TREE_PORT")],
                           key=lambda x: int(x[len("SP_TREE_PORT"):]))
        write_header("egr_vlan_stg", "EGR_VLAN_STG", rows, sp_fields)

    # --- FP_TCAM: 100 ipipe0 trap-rule keys ---
    # TCAM keys are very wide (KEY/MASK can be 240+ bits).  We emit them as
    # hex strings; cumulus_replicate.c will use cdk_field_set on uint32 arrays.
    rows = list(parse_table("FP_TCAM"))
    if rows:
        # Two formats: PAIRING + non-PAIRING.  Capture both KEY+MASK.
        write_fp_tcam(rows)

    # --- FP_POLICY_TABLE ---
    # (No VALID — validity inherited from matching FP_TCAM row.)
    rows = list(parse_table("FP_POLICY_TABLE"))
    write_header("fp_policy_table", "FP_POLICY_TABLE", rows,
        ["Y_DROP", "Y_COPY_TO_CPU",
         "R_DROP", "R_COPY_TO_CPU",
         "G_DROP", "G_COPY_TO_CPU",
         "METER_PAIR_MODE_MODIFIER", "COUNTER_MODE"])


def hexword_split(s, words=8):
    """Split '0x...' into <words> uint32 words, little-end first."""
    if s.startswith("0x"): s = s[2:]
    # Pad LEFT to the requested width
    s = s.zfill(words * 8)
    out = []
    # walk from the LSB end (rightmost 8 chars first)
    for i in range(words):
        chunk = s[len(s) - (i + 1) * 8: len(s) - i * 8]
        out.append("0x" + chunk)
    return out


def write_fp_tcam(rows):
    path = os.path.join(OUT, "cumulus_fp_tcam.h")
    WORDS = 8  # 240-bit TCAM key fits in 8 uint32 words (256 bits)
    with open(path, "w") as f:
        f.write(f"/* Auto-generated from {CAPTURE} */\n"
                f"#ifndef _CUMULUS_FP_TCAM_H_\n#define _CUMULUS_FP_TCAM_H_\n"
                f"#include <stdint.h>\n\n"
                f"struct cumulus_fp_tcam_row {{\n"
                f"    uint32_t index;\n"
                f"    uint32_t valid;\n"
                f"    uint32_t key[{WORDS}];\n"
                f"    uint32_t mask[{WORDS}];\n"
                f"}};\n\n"
                f"static const struct cumulus_fp_tcam_row "
                f"cumulus_fp_tcam_rows[] = {{\n")
        for idx, d in rows:
            valid = d.get("VALID", "0")
            key = d.get("KEY", "0x0")
            mask = d.get("MASK", "0x0")
            kw = hexword_split(key, WORDS)
            mw = hexword_split(mask, WORDS)
            f.write(f"    {{ {idx}, {valid}, "
                    f"{{ {', '.join(kw)} }}, "
                    f"{{ {', '.join(mw)} }} }},\n")
        f.write("};\n\n"
                f"#define CUMULUS_FP_TCAM_COUNT "
                f"(sizeof(cumulus_fp_tcam_rows)/sizeof(cumulus_fp_tcam_rows[0]))\n\n"
                f"#endif\n")
    print(f"  wrote {path}  ({len(rows)} rows)")


if __name__ == "__main__":
    main()
