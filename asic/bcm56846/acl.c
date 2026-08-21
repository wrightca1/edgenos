/*
 * acl.c — EdgeNOS operator ACLs on the BCM56846 Field Processor (AS5610 / edged).
 *
 * Phase 2a: destination-IP deny/permit. Programmed by hand into the FP (no SDK),
 * reusing the single-wide VS6 slice that cumulus_replicate.c already stands up and
 * enables — physical slice 4, FPF2 selcode 1, which places the 32-bit DstIP at the
 * F2 field (bits 96..127, first octet at the MSB). The OSPF 224/8 trap lives at
 * entry 0 (idx 1024); ACL entries go at 1025+ (lower index = higher precedence, so
 * the OSPF trap and lower-seq rules win). deny -> FP_POLICY *_DROP=1; permit -> a
 * matching entry with no action (packet proceeds), giving permit-over-deny by seq.
 *
 * v1 matches DESTINATION IP only. Rules that also constrain protocol / source /
 * L4 ports can't be expressed on this slice yet (the src/proto/L4 key-bit layout is
 * not mapped), so they are SKIPPED with a warning rather than over-matching.
 * Shares the /etc/edged/acls.conf format + the edgenos acl CLI with the 4610.
 * See docs/acl-design.md.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <arpa/inet.h>

#include "edged.h"
#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_chip.h>
#include <cdk/arch/xgs_reg.h>

#define ACL_UNIT      0
/* FP_TCAM is PHYSICAL-slice indexed (256 entries/slice): idx/256 = physical slice.
 * The IFP iterates VIRTUAL slices; virtual slice N's entries live in physical slice
 * FP_SLICE_MAP[N]. Confirmed live via the SDK oracle: FP_SLICE_MAP has VS6 -> phys 4,
 * and VS6 is the DstIP slice (FP_PORT_FIELD_SEL SLICE6_F2=1, single-wide, lookup-
 * enabled). So DstIP-key entries MUST sit in physical slice 4 = idx 1024..1279.
 *
 * The old base 512 was wrong: idx 512 = physical slice 2 = VS0 (map), which has
 * LOOKUP_ENABLE=0 — so entries there were never looked up (counter stayed 0, the
 * long-standing "IFP does no lookup" bug). cumulus_replicate's OSPF trap already
 * correctly uses idx 1024 (VS6 entry 0); ACL entries follow at 1025+. */
/* DOUBLE-WIDE: primary entries in physical slice 6 (= virtual slice 8, the DstIp
 * double-wide slice cumulus_replicate pairs with 9 via SLICE9_8_PAIRING); the paired
 * secondary is written at idx+256 (physical slice 7). Cumulus put its entry at 1555
 * (slice6 offset 19); we start at 1537 and cap at 64 so primary 1537..1600 stays in
 * slice 6 and secondary 1793..1856 stays in slice 7. */
/* SINGLE-WIDE physical slice 4 (FP_TCAM idx 512..767). PROVEN in silicon 2026-07-09:
 * an entry here IS consulted (a match-any drop killed 100% of traffic), whereas the old
 * double-wide slice-6/8 placement was never consulted. Trident+ slice geometry is
 * non-uniform (phys 0-3 = 128 entries, 4-9 = 256); slice 4 is single-wide, unpaired, and
 * we enable its FP_LOOKUP_ENABLE bit. See docs/acl-5610-double-wide-fp.md. */
#define ACL_IDX_BASE  512
#define ACL_MAX       64        /* 1537..1600 primary / 1793..1856 secondary */

static int acl_idx_used[ACL_MAX];
static int acl_n = 0;

/* edged's syslog is buried under DMA-timeout spam in the journal — mirror ACL
 * activity to a dedicated file for visibility. */
static void acl_log(const char *fmt, ...)
{
    FILE *lf = fopen("/tmp/edged-acl.log", "a");
    va_list ap;
    if (!lf) return;
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);
    fputc('\n', lf);
    fclose(lf);
}

/* "<mgmt-net-host>/24" / "any" -> dst ip (host order, first octet = MSB) + mask. -1 = bad. */
static int acl_cidr(const char *tok, uint32_t *ip, uint32_t *mask)
{
    char buf[48], *slash;
    int bits = 32;
    struct in_addr a;
    if (!strcmp(tok, "any") || !strcmp(tok, "0.0.0.0/0")) { *ip = 0; *mask = 0; return 0; }
    strncpy(buf, tok, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    slash = strchr(buf, '/'); if (slash) { *slash = 0; bits = atoi(slash + 1); }
    if (inet_pton(AF_INET, buf, &a) != 1) return -1;
    *ip = ntohl(a.s_addr);      /* f2[3] wants first octet at the MSB */
    *mask = bits <= 0 ? 0 : (bits >= 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32 - bits)));
    return 0;
}

static void acl_program_one(int unit, int idx, uint32_t dstip, uint32_t dstmask, int deny)
{
    FP_TCAMm_t t;
    FP_GLOBAL_MASK_TCAMm_t g;
    FP_POLICY_TABLEm_t p;
    uint32_t f2[4]  = {0};
    uint32_t f2m[4] = {0};
    int w;

    /*
     * FP_TCAM is a DltaCam: the hardware stores entries in X/Y form, NOT plain
     * DATA/MASK. The SDK's soc_mem_write applies _soc_mem_tcam_dm_to_xy() before
     * writing; the CDK's raw field-set does not. Without the transform the chip
     * decodes our DATA/MASK as X/Y and the mask comes out garbage
     * (decoded_mask = key | ~stored_mask), so no rule ever matches — verified on
     * the live chip via the SDK: our old entry read back F2_MASK=<ip>ffff.. .
     *
     * 40nm (Trident+) encode:  K0 = mask & key  -> KEY (F2) field
     *                          K1 = ~mask | key -> MASK (F2_MASK) field
     * DstIP sits in F2 word 3 (bits 96..127); words 0..2 are don't-care.
     */
    {
        /* DstIp sits in F2 WORD[2] (bits 64..95), per the live Cumulus capture
         * (FP_TCAM[1555]: F2=0x00000000<ip>0000000000000000) — NOT word[3] which
         * edged used before. docs/cumulus-acl-fp-recipe.md. */
        uint32_t dm_key[4]  = { 0, 0, dstip, 0 };
        uint32_t dm_mask[4] = { 0, 0, dstmask, 0 };
        for (w = 0; w < 4; w++) {
            f2[w]  =  dm_mask[w] & dm_key[w];   /* K0 */
            f2m[w] = ~dm_mask[w] | dm_key[w];   /* K1 */
        }
    }

    /*
     * DOUBLE-WIDE group (Cumulus/switchd captured recipe): the match lives in the
     * PRIMARY slice entry (this idx, physical slice 6 = virtual slice 8, paired
     * with slice 9 by cumulus_replicate SLICE9_8_PAIRING=1); a paired SECONDARY
     * entry at idx+256 (physical slice 7) completes the pair. The match qualifiers
     * are mirrored into the PAIRING_* fields. edged's old single-wide entry never
     * matched — this is the fix.
     */
    /* SINGLE-WIDE entry in physical slice 4 (proven consulted in silicon). One FP_TCAM
     * line, no paired secondary, no PAIRING_* fields, no FP_GM_FIELDS overlay (all of
     * those are double-wide-only). DstIp lives in F2 word[2] (F2f bits 64-95 = KEY bit
     * 110 = single-wide selcode-1 offset f2_offset(46)+64, SDK-derived + Cumulus-proven).
     * FIXED=0x100/care 0x300 selects IpType=IPv4. F1/F3/F4 masks MUST be don't-care
     * (all-ones), else a raw-zero DltaCam field decodes to "must==0" and blocks the match
     * (the bug that hid the whole feature — see docs/acl-5610-double-wide-fp.md). */
    FP_TCAMm_CLR(t);
    FP_TCAMm_VALIDf_SET(t, 3);
    FP_TCAMm_F2f_SET(t, f2);                        /* DstIp @ F2 word[2] (match-all: f2m=~0) */
    FP_TCAMm_F2_MASKf_SET(t, f2m);
    /* Every unused qualifier (F1/F3/F4/FIXED) MUST be don't-care = raw MASK(Y) all-ones,
     * else a raw-zero DltaCam field decodes to "must==0" and blocks the match. FIXED is
     * left don't-care (not IpType-constrained): its CDK field overlaps PAIRING_FIXED and
     * the single-field write skews the decode; a dst-IP only exists on IP packets anyway,
     * so not gating on IpType is harmless and matches the proven match-any config. */
    {
        uint32_t allone39[2] = { 0xffffffffu, 0x0000007fu }; /* 39-bit all-ones = don't-care */
        FP_TCAMm_F1_MASKf_SET(t, allone39);
        FP_TCAMm_F3_MASKf_SET(t, allone39);
        FP_TCAMm_F4_MASKf_SET(t, 0x7fu);                     /* 7-bit all-ones */
        /* FIXED_MASK and PAIRING_FIXED_MASK overlap the same TCAM bits; set BOTH all-ones
         * so the FIXED (IpType/Stage) field decodes to a clean don't-care (setting one
         * alone leaves an overlapped bit cared, over-constraining the entry). */
        FP_TCAMm_FIXED_MASKf_SET(t, 0x1ffffu);               /* 17-bit all-ones */
        FP_TCAMm_PAIRING_FIXED_MASKf_SET(t, 0x7ffffu);       /* 19-bit all-ones */
    }
    (void)WRITE_FP_TCAMm(unit, idx, t);

    /* FP_GLOBAL_MASK_TCAM per-entry ingress-port gate (single index for single-wide).
     * Trident requires VALID=1 even when IPBM/IPBM_MASK are 0 (triumph/field.c:3303-3350).
     * IPBM=0/IPBM_MASK=0/VALID=1 = match ANY ingress port (SDK default). Per-port `apply`
     * scoping would write a real IPBM bitmap here (needs IFP_GM_LOGIC_TO_PHYS_MAP remap). */
    FP_GLOBAL_MASK_TCAMm_CLR(g);
    FP_GLOBAL_MASK_TCAMm_VALIDf_SET(g, 1);
    (void)WRITE_FP_GLOBAL_MASK_TCAMm(unit, idx, g);

    FP_POLICY_TABLEm_CLR(p);
    if (deny) {                                 /* permit = no action, packet proceeds */
        FP_POLICY_TABLEm_G_DROPf_SET(p, 1);
        FP_POLICY_TABLEm_Y_DROPf_SET(p, 1);
        FP_POLICY_TABLEm_R_DROPf_SET(p, 1);
        /* COPY_TO_CPU=3 = SwitchToCpuCancel, exactly as Cumulus/switchd programs a
         * hardware drop (captured FP_POLICY: G/Y/R_DROP=1 + COPY_TO_CPU=3). */
        FP_POLICY_TABLEm_G_COPY_TO_CPUf_SET(p, 3);
        FP_POLICY_TABLEm_Y_COPY_TO_CPUf_SET(p, 3);
        FP_POLICY_TABLEm_R_COPY_TO_CPUf_SET(p, 3);
    }
    FP_POLICY_TABLEm_COUNTER_MODEf_SET(p, 7);   /* count all colors: a match signal */
    FP_POLICY_TABLEm_COUNTER_INDEXf_SET(p, idx);
    (void)WRITE_FP_POLICY_TABLEm(unit, idx, p);
    acl_log("prog idx=%d f2[3]=0x%08x/0x%08x %s", idx, dstip, dstmask, deny ? "DENY" : "permit");
}

/* Dump each ACL entry's FP match counter to the file-log (wired to SIGUSR1). A
 * non-zero count means the entry MATCHED — distinguishing "didn't match" (key/slice
 * wrong) from "matched but didn't drop" (drop/action wrong). */
void edged_acl_diag(void)
{
    FP_COUNTER_TABLEm_t c;
    uint32_t pkts, sev = 0;
    int i;

    cdk_xgs_reg32_read(ACL_UNIT, FP_SLICE_ENABLEr, &sev);
    acl_log("diag FP_SLICE_ENABLE(runtime)=0x%08x (want lookup bit for phys8)", sev);

    for (i = 0; i < acl_n; i++) {
        FP_TCAMm_t t;
        FP_GLOBAL_MASK_TCAMm_t g;
        FP_POLICY_TABLEm_t p;
        uint32_t f2[4] = {0}, ipbm[3] = {0}, ipbmm[3] = {0};
        int valid = -1, gvalid = -1;
        uint32_t cidx = 0xffffffff, cmode = 0xffffffff, gdrop = 0xffffffff;
        int idx = acl_idx_used[i];

        FP_TCAMm_CLR(t);
        if (cdk_xgs_mem_read(ACL_UNIT, FP_TCAMm, idx, t.v, 15) >= 0) {
            valid = FP_TCAMm_VALIDf_GET(t);
            FP_TCAMm_F2f_GET(t, f2);
        }
        /* Gate readback: did the FP_GLOBAL_MASK_TCAM write land? raw words + decoded. */
        FP_GLOBAL_MASK_TCAMm_CLR(g);
        if (cdk_xgs_mem_read(ACL_UNIT, FP_GLOBAL_MASK_TCAMm, idx, g.v, 5) >= 0) {
            gvalid = FP_GLOBAL_MASK_TCAMm_VALIDf_GET(g);
            FP_GLOBAL_MASK_TCAMm_IPBMf_GET(g, ipbm);
            FP_GLOBAL_MASK_TCAMm_IPBM_MASKf_GET(g, ipbmm);
            acl_log("diag idx=%d GMASK raw=[%08x %08x %08x %08x %08x] valid=%d "
                    "IPBM=%08x.%08x.%08x IPBM_MASK=%08x.%08x.%08x",
                    idx, g.v[0], g.v[1], g.v[2], g.v[3], g.v[4], gvalid,
                    ipbm[2], ipbm[1], ipbm[0], ipbmm[2], ipbmm[1], ipbmm[0]);
        }
        /* Policy readback: where does the counter actually land (COUNTER_INDEX)? */
        FP_POLICY_TABLEm_CLR(p);
        if (cdk_xgs_mem_read(ACL_UNIT, FP_POLICY_TABLEm, idx, p.v, 15) >= 0) {
            cidx  = FP_POLICY_TABLEm_COUNTER_INDEXf_GET(p);
            cmode = FP_POLICY_TABLEm_COUNTER_MODEf_GET(p);
            gdrop = FP_POLICY_TABLEm_G_DROPf_GET(p);
        }
        FP_COUNTER_TABLEm_CLR(c); pkts = 0xffffffff;
        if (cdk_xgs_mem_read(ACL_UNIT, FP_COUNTER_TABLEm, idx, c.v, 3) >= 0)
            pkts = c.v[0] & 0x1fffffff;
        acl_log("diag idx=%d valid=%d dstip(f2[2])=0x%08x COUNTER_INDEX=%u MODE=%u "
                "G_DROP=%u counter[idx]=%u", idx, valid, f2[2], cidx, cmode, gdrop, pkts);
    }

    /* Decisive scan: hits may land at a COUNTER_INDEX != our TCAM idx. Sweep the whole
     * FP_COUNTER_TABLE and report EVERY non-zero counter, so a match anywhere is visible
     * regardless of index confusion. FP_COUNTER_TABLE has 2 counters/entry (G/Y/R share). */
    {
        int scanned = 0, hits = 0;
        for (i = 0; i <= 2047; i++) {
            uint32_t v0, v1;
            FP_COUNTER_TABLEm_CLR(c);
            if (cdk_xgs_mem_read(ACL_UNIT, FP_COUNTER_TABLEm, i, c.v, 3) < 0) continue;
            scanned++;
            v0 = c.v[0] & 0x1fffffff;
            v1 = ((c.v[0] >> 29) | (c.v[1] << 3)) & 0x1fffffff;
            if (v0 || v1) { acl_log("diag COUNTER_SCAN nonzero idx=%d c0=%u c1=%u", i, v0, v1); hits++; }
        }
        acl_log("diag COUNTER_SCAN done: scanned=%d nonzero=%d", scanned, hits);
    }

    /* Ingress-port state: is the field-select + per-port IFP filter actually LIVE on the
     * ports the traffic enters (swp1=chip 65, swp2=chip 66)? If PFS reads all-zero the
     * selcodes never reached the port; if FILTER_ENABLE=0 the IFP skips the port. */
    {
        int prt, pp;
        int ports[2] = { 65, 66 };
        for (pp = 0; pp < 2; pp++) {
            FP_PORT_FIELD_SELm_t fs;
            PORT_TABm_t pt;
            int fe = -1;
            prt = ports[pp];
            FP_PORT_FIELD_SELm_CLR(fs);
            PORT_TABm_CLR(pt);
            if (cdk_xgs_mem_read(ACL_UNIT, PORT_TABm, prt, pt.v, 10) >= 0)
                fe = PORT_TABm_FILTER_ENABLEf_GET(pt);
            if (cdk_xgs_mem_read(ACL_UNIT, FP_PORT_FIELD_SELm, prt, fs.v, 6) >= 0) {
                acl_log("diag INGRESS port=%d FILTER_EN=%d PFS S6.F2=%u S7.F2=%u S8.F2=%u "
                        "S9.F2=%u PAIR7_6=%u PAIR9_8=%u", prt, fe,
                        FP_PORT_FIELD_SELm_SLICE6_F2f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE7_F2f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE8_F2f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE9_F2f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE7_6_PAIRINGf_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE9_8_PAIRINGf_GET(fs));
            } else {
                acl_log("diag INGRESS port=%d FILTER_EN=%d PFS read FAILED", prt, fe);
            }
        }
    }
}

/* Invalidate every ACL entry we programmed (VALID=0), so a reload rebuilds cleanly. */
void edged_acl_reset(void)
{
    FP_TCAMm_t t;
    int i;
    FP_TCAMm_CLR(t);            /* VALID=0 -> entry disabled */
    for (i = 0; i < acl_n; i++)
        (void)WRITE_FP_TCAMm(ACL_UNIT, acl_idx_used[i], t);          /* single-wide: one line */
    acl_n = 0;
}

#define ACL_MAX_RULES 128
#define ACL_MAX_BINDS 32

struct acl_rule { char name[32]; char dst[48]; int seq, deny, supported; };

/* Per-port IFP enable — PORT_TAB.FILTER_ENABLE. The SDK sets this per port at init;
 * cumulus_replicate never does and OpenMDK's bmd_init doesn't either, so the IFP never
 * looks up ANY port's packets (even a match-any entry reads counter=0). RMW so other
 * port config is preserved. Found by diffing the OpenBCM SDK field/port init. */
static void acl_enable_port_filter(void)
{
    int p, n = 0;
    /* THE BUG: this loop stopped at p<64, but the AS5610 uplink ports ingress on chip
     * ports 65/66 (PORT_TABm_MAX=66) — so FILTER_ENABLE was NEVER set on the ports the
     * traffic actually enters, and the IFP silently skipped every packet on them (global
     * "no lookup" for ALL slices — the whole project's wall). Cover every port. */
    for (p = 0; p <= PORT_TABm_MAX; p++) {
        PORT_TABm_t pt;
        PORT_TABm_CLR(pt);
        if (cdk_xgs_mem_read(ACL_UNIT, PORT_TABm, p, pt.v, 10) < 0) continue;
        PORT_TABm_FILTER_ENABLEf_SET(pt, 1);
        if (WRITE_PORT_TABm(ACL_UNIT, p, pt) >= 0) n++;
    }
    acl_log("PORT_TAB.FILTER_ENABLE set on %d ports (per-port IFP enable)", n);
}

/* IFP global-mask logical->physical port map. Cumulus programs this per port; OpenMDK
 * and cumulus_replicate never did (the register is abbreviated IFP_GM_LOGIC_TO_PHYS_MAP
 * in the CDK, easy to miss). With it all-zero the IFP global-mask lookup can't resolve
 * the ingress port, so NO FP entry ever matches on any port. Values verbatim from the
 * Cumulus SOC register diff (dump_soc_diff.txt), indexed by SOC port. */
static void acl_gm_map_init(void)
{
    static const uint32_t gm[66] = {
        0x00,0x41,0x42,0x43,0x44,0x45,0x46,0x47,
        0x48,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,
        0x0c,0x0d,0x0e,0x0f,0x10,0x12,0x11,0x14,
        0x13,0x16,0x15,0x18,0x17,0x19,0x1a,0x1b,
        0x1c,0x1d,0x1e,0x1f,0x20,0x21,0x22,0x23,
        0x24,0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,
        0x2c,0x31,0x2d,0x3d,0x39,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x49,
    };
    int port;
    for (port = 0; port < 66; port++) {
        IFP_GM_LOGIC_TO_PHYS_MAPr_t r;
        IFP_GM_LOGIC_TO_PHYS_MAPr_CLR(r);
        IFP_GM_LOGIC_TO_PHYS_MAPr_PHYSICAL_PORT_NUMf_SET(r, gm[port]);
        (void)WRITE_IFP_GM_LOGIC_TO_PHYS_MAPr(ACL_UNIT, port, r);
    }
    acl_log("IFP_GM_LOGIC_TO_PHYS_MAP written for 66 ports (the missing GM port map)");
}

/* soc_mem_clear(FP_GLOBAL_MASK_TCAM) — the SDK's _soc_trident_misc_init does exactly
 * this at soc_init time ("Must clear FP_GLOBAL_MASK_TCAM after port-to-pipe mappings are
 * initialized", src/soc/esw/trident.c). OpenMDK's bmd_init never does. Without the memory
 * being initialized, the IFP global-mask lookup hits uninitialized entries and NO FP rule
 * ever matches (match-any reads counter=0). This is an OPERATION, not a register value —
 * which is why register replication never reproduced it. Init all 2048 entries to 0. */
static void acl_clear_global_mask(void)
{
    int i;
    /* The full SDK init: _bcm_field_tr_hw_clear() (src/bcm/esw/triumph/field.c) clears
     * every ingress-FP memory + _soc_trident_misc_init clears FP_GLOBAL_MASK_TCAM. OpenMDK
     * inits NONE of these, so the FP pipeline reads uninitialized (bad-parity) entries and
     * no lookup ever fires. Init them all (typed CLR+WRITE handle the word counts). */
    { FP_TCAMm_t e;             FP_TCAMm_CLR(e);             for (i=0;i<=2047;i++) (void)WRITE_FP_TCAMm(ACL_UNIT,i,e); }
    { FP_GLOBAL_MASK_TCAMm_t e; FP_GLOBAL_MASK_TCAMm_CLR(e); for (i=0;i<=2047;i++) (void)WRITE_FP_GLOBAL_MASK_TCAMm(ACL_UNIT,i,e); }
    { FP_POLICY_TABLEm_t e;     FP_POLICY_TABLEm_CLR(e);     for (i=0;i<=2047;i++) (void)WRITE_FP_POLICY_TABLEm(ACL_UNIT,i,e); }
    { FP_METER_TABLEm_t e;      FP_METER_TABLEm_CLR(e);      for (i=0;i<=2047;i++) (void)WRITE_FP_METER_TABLEm(ACL_UNIT,i,e); }
    { FP_COUNTER_TABLEm_t e;    FP_COUNTER_TABLEm_CLR(e);    for (i=0;i<=2047;i++) (void)WRITE_FP_COUNTER_TABLEm(ACL_UNIT,i,e); }
    { FP_UDF_TCAMm_t e;         FP_UDF_TCAMm_CLR(e);         for (i=0;i<=511;i++)  (void)WRITE_FP_UDF_TCAMm(ACL_UNIT,i,e); }
    { FP_UDF_OFFSETm_t e;       FP_UDF_OFFSETm_CLR(e);       for (i=0;i<=511;i++)  (void)WRITE_FP_UDF_OFFSETm(ACL_UNIT,i,e); }
    { FP_RANGE_CHECKm_t e;      FP_RANGE_CHECKm_CLR(e);      for (i=0;i<=31;i++)   (void)WRITE_FP_RANGE_CHECKm(ACL_UNIT,i,e); }
    acl_log("FP mem init: TCAM/GMASK/POLICY/METER/COUNTER/UDF_TCAM/UDF_OFFSET/RANGE_CHECK");
}

/* Pipeline-level IFP enables from _soc_trident_misc_init (src/soc/esw/trident.c), found by
 * statically tracing the SDK's soc-init FP setup. OpenMDK sets NEITHER, so the IFP stage
 * never runs regardless of slice/entry setup: (1) IFP_BYPASS_ENABLE must be 0 (else the FP
 * stage is bypassed); (2) ING_EN_EFILTER_BITMAP must include the ports (else the ingress
 * filter runs on no port). These gate the whole stage — the missing global switch. */
static void acl_ifp_pipeline_enable(void)
{
    {
        ING_BYPASS_CTRLr_t r;
        uint32_t val = 0;
        cdk_xgs_reg32_read(ACL_UNIT, ING_BYPASS_CTRLr, &val);
        ING_BYPASS_CTRLr_SET(r, val);
        ING_BYPASS_CTRLr_IFP_BYPASS_ENABLEf_SET(r, 0);   /* IFP stage NOT bypassed */
        (void)WRITE_ING_BYPASS_CTRLr(ACL_UNIT, r);
    }
    {
        ING_EN_EFILTER_BITMAPm_t e;
        uint32_t ones[3] = { 0xffffffff, 0xffffffff, 0xffffffff };
        ING_EN_EFILTER_BITMAPm_CLR(e);
        ING_EN_EFILTER_BITMAPm_BITMAPf_SET(e, ones);     /* filter enabled on all ports */
        (void)WRITE_ING_EN_EFILTER_BITMAPm(ACL_UNIT, 0, e);
    }
    acl_log("IFP pipeline enable: IFP_BYPASS=0 + ING_EN_EFILTER_BITMAP=all-ports");
}

int edged_acl_load(const char *path)
{
    static struct acl_rule rules[ACL_MAX_RULES];
    static char applied[ACL_MAX_BINDS][32];
    FILE *f = fopen(path, "r");
    char line[256];
    int nr = 0, nb = 0, i, j, done = 0, skipped = 0;

    if (!f) { syslog(LOG_INFO, "ACL: no %s (none configured)", path); return 0; }
    while (fgets(line, sizeof(line), f)) {
        char *s = line, *name, *t2;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\0') continue;
        name = strtok(s, " \t\n"); if (!name) continue;
        t2 = strtok(NULL, " \t\n"); if (!t2) continue;
        if (!strcmp(t2, "apply")) {                 /* <name> apply <port>... (ports ignored in v1) */
            for (i = 0; i < nb; i++) if (!strcmp(applied[i], name)) break;
            if (i == nb && nb < ACL_MAX_BINDS) { strncpy(applied[nb], name, 31); applied[nb][31] = 0; nb++; }
        } else {                                    /* <name> <seq> <act> <proto> <src> <dst> [dport N]... */
            char *act, *pr, *sr, *ds, *k;
            int proto_any, src_any, has_l4 = 0;
            if (nr >= ACL_MAX_RULES) continue;
            act = strtok(NULL, " \t\n"); pr = strtok(NULL, " \t\n");
            sr  = strtok(NULL, " \t\n"); ds = strtok(NULL, " \t\n");
            if (!act || !pr || !sr || !ds) continue;
            while ((k = strtok(NULL, " \t\n")))
                if (!strcmp(k, "dport") || !strcmp(k, "sport")) has_l4 = 1;
            memset(&rules[nr], 0, sizeof(rules[nr]));
            strncpy(rules[nr].name, name, 31);
            rules[nr].seq  = atoi(t2);
            rules[nr].deny = !strcmp(act, "deny");
            strncpy(rules[nr].dst, ds, sizeof(rules[nr].dst) - 1);
            proto_any = (!strcmp(pr, "ip") || !strcmp(pr, "any") || !strcmp(pr, "all"));
            src_any   = (!strcmp(sr, "any") || !strcmp(sr, "0.0.0.0/0"));
            rules[nr].supported = (proto_any && src_any && !has_l4 && !strchr(ds, ':'));
            nr++;
        }
    }
    fclose(f);

    /* Program applied+supported rules in ascending seq order (lower seq -> lower
     * index -> higher FP precedence), skipping unsupported ones with a warning. */
    edged_acl_reset();
    {   /* FP_SLICE_ENABLE = 0x000f73ff: slices 0-9 enabled + FP_LOOKUP_ENABLE on phys
         * 2,3,6,7,8,9 (Cumulus set) PLUS bit 14 = FP_LOOKUP_ENABLE_SLICE_4 — our
         * single-wide ACL slice. Without bit 14 the slice-4 entries are never consulted. */
        FP_SLICE_ENABLEr_t se;
        FP_SLICE_ENABLEr_CLR(se);
        FP_SLICE_ENABLEr_SET(se, 0x000f73ff);
        (void)WRITE_FP_SLICE_ENABLEr(ACL_UNIT, se);
    }
    acl_enable_port_filter();               /* the missing per-port IFP enable */
    acl_gm_map_init();                      /* the missing IFP global-mask port map */
    acl_clear_global_mask();                /* the missing FP_GLOBAL_MASK_TCAM init */
    acl_ifp_pipeline_enable();              /* the missing IFP stage enable (bypass+filter) */

    /* ── SLICE GEOMETRY PROBE ────────────────────────────────────────────────────
     * If a rule named "probe" is applied, write a match-ALL count entry at base+50 of
     * every EVEN physical slice (0,2,4,6); their paired secondaries (base+256) land in
     * the odd slices (1,3,5,7), so no two probe entries collide. Under real IPv4
     * traffic the counter-scan (SIGUSR1) then shows WHICH physical slice the IFP
     * actually consults — resolving the virtual-vs-physical FP_TCAM indexing that has
     * blocked this feature for the whole project. Non-destructive (offset 50 clears the
     * low-offset traps; permit = no drop). */
    {
        int pi;
        for (pi = 0; pi < nr; pi++) if (!strcmp(rules[pi].name, "probe")) break;
        if (pi < nr) {
            int s;
            for (s = 0; s <= 6 && acl_n < ACL_MAX; s += 2) {
                int pidx = s * 256 + 50;
                acl_program_one(ACL_UNIT, pidx, 0, 0, 0);   /* match-all, count-only */
                acl_idx_used[acl_n++] = pidx;
            }
            acl_log("SLICE PROBE: match-all count entries at even-slice bases "
                    "50(s0)/562(s2)/1074(s4)/1586(s6) — scan counters to find live slice");
        }
    }

    while (acl_n < ACL_MAX) {
        int best = -1;
        for (i = 0; i < nr; i++) {
            if (rules[i].seq < 0) continue;                     /* already taken */
            for (j = 0; j < nb; j++) if (!strcmp(applied[j], rules[i].name)) break;
            if (j == nb) continue;                               /* ACL not applied: inert */
            if (best < 0 || rules[i].seq < rules[best].seq) best = i;
        }
        if (best < 0) break;
        if (rules[best].supported) {
            uint32_t ip, mask;
            if (acl_cidr(rules[best].dst, &ip, &mask) == 0) {
                int idx = ACL_IDX_BASE + acl_n;
                acl_program_one(ACL_UNIT, idx, ip, mask, rules[best].deny);
                acl_idx_used[acl_n++] = idx;
                done++;
            }
        } else {
            syslog(LOG_WARNING, "ACL %s seq %d: 5610 v1 matches dst-IP only "
                   "(proto/src/L4 unsupported) — skipped", rules[best].name, rules[best].seq);
            skipped++;
        }
        rules[best].seq = -1;                                    /* mark taken */
    }
    syslog(LOG_INFO, "ACL: installed %d FP entr%s from %s (%d skipped, dst-IP only)",
           done, done == 1 ? "y" : "ies", path, skipped);
    acl_log("load %s: %d rules, %d applied-bindings -> %d installed, %d skipped",
            path, nr, nb, done, skipped);
    return done;
}
