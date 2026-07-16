/*
 * acl.c — EdgeNOS operator ACLs on the BCM56846 Field Processor (AS5610 / edged).
 *
 * Phase 2b: destination-IP deny/permit. Programmed by hand into the FP (no SDK) as a
 * DOUBLE-WIDE group on physical slices 4+5, replicating Cumulus 2.5.0's proven transit
 * dst-IP ACL byte-for-byte (captured from this exact silicon — see
 * CUMULUS_TRANSIT_ACL_RECIPE.md). Every prior single-wide attempt wrote a byte-perfect
 * slice-4 entry the IFP never consulted: a dst-IP key (offset 110, width 32) does not
 * fit one slice — the group MUST be double-wide (slices 4+5 paired via SLICE5_4_PAIRING).
 *
 * Each rule => a primary FP_TCAM[512+n] (slice 4, the DstIp/IpType key, spliced from the
 * captured raw KEY/MASK template) + a secondary FP_TCAM[768+n] (slice 5, VALID=3) +
 * FP_GM_FIELDS + FP_GLOBAL_MASK_TCAM for both halves. deny -> FP_POLICY *_DROP=1 +
 * COPY_TO_CPU=3; permit -> no action (packet proceeds), giving permit-over-deny by seq
 * (lower seq -> lower index -> higher FP precedence). The slice pairing/selcodes/enable
 * are set once per load by acl_setup_doublewide().
 *
 * v1 matches DESTINATION IP only. Rules that also constrain protocol / source / L4 ports
 * are SKIPPED with a warning rather than over-matching. Shares the /etc/edged/acls.conf
 * format + the edgenos acl CLI with the 4610. See docs/acl-design.md.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "edged.h"
#include <cdk/chip/bcm56840_a0_defs.h>
#include <cdk/arch/xgs_chip.h>
#include <cdk/arch/xgs_reg.h>

#define ACL_UNIT      0
/* FP_TCAM index layout (Trident+, NON-uniform slice geometry): phys slices 0-3 hold 128
 * entries each (idx 0..511), slices 4-9 hold 256 each. So:
 *   physical slice 4 = idx 512..767   (double-wide PRIMARY   half)
 *   physical slice 5 = idx 768..1023  (double-wide SECONDARY half, = primary idx + 256)
 * Cumulus placed its entry at slice-4 offset 19 (idx 531) + slice-5 offset 19 (idx 787).
 * We start at offset 0 and cap at 64: primary 512..575, secondary 768..831. Lower index
 * = higher FP precedence, so lower-seq rules win. The captured register-diff proved the
 * absolute slice numbers are just allocator output — what matters is the double-wide
 * STRUCTURE (pairing + both-half selcodes + both LOOKUP-enable bits). */
#define ACL_IDX_BASE  512
#define ACL_MAX       64        /* primary 512..575 / secondary 768..831 */

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

/* "10.1.1.0/24" / "any" -> dst ip (host order, first octet = MSB) + mask. -1 = bad. */
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

/* Raw FP_TCAM KEY/MASK template captured VERBATIM from Cumulus 2.5.0's working
 * double-wide transit dst-IP deny (FP_TCAM[531], EID 0x7e, dst 10.101.101.2/32).
 * These are the PHYSICAL fp_tcam KEY (bits 2..235) and MASK (bits 236..469) field
 * values — copying them reproduces Cumulus's entry bit-for-bit, sidestepping the
 * logical-DATA/MASK vs raw-X/Y DltaCam ambiguity that defeated every prior attempt.
 * KEY holds only the DstIp @ offset 110; MASK holds the DstIp care-mask @110 PLUS the
 * FIXED_MASK (IpType=IPv4) bits at the top. We write the template, then splice the
 * per-rule DstIp into F2 (word[2] = KEY offset 110); the FIXED/IpType bits ride along.
 * See CUMULUS_TRANSIT_ACL_RECIPE.md. LSW-first uint32[8]. */
static const uint32_t ACL_KEY_TMPL[8]  =
    { 0x00000000, 0x00000000, 0x00000000, 0x59408000, 0x00000299, 0x00000000, 0x00000000, 0x00000000 };
static const uint32_t ACL_MASK_TMPL[8] =
    { 0x00000000, 0x00000000, 0x00000000, 0xffffc000, 0x00003fff, 0x00000000, 0x80000000, 0x00000003 };

/*
 * Program ONE double-wide dst-IP ACL entry, matching the captured Cumulus recipe:
 *   primary   FP_TCAM[idx]         (physical slice 4) — the DstIp/IpType key
 *   secondary FP_TCAM[idx+256]     (physical slice 5) — VALID=3, completes the pair
 *   FP_GM_FIELDS[idx]/[idx+256]    — the global-mask overlay (edged omitted this before)
 *   FP_GLOBAL_MASK_TCAM[idx]/[idx+256] — VALID=1, match-ANY ingress port
 *   FP_POLICY_TABLE[idx]           — G/Y/R_DROP + COPY_TO_CPU=3 (SwitchToCpuCancel)
 * The slice pairing/selcodes/enable are set once per load by acl_setup_doublewide().
 * edged's old SINGLE-WIDE slice-4 entry was never consulted: a dst-IP key at offset 110
 * doesn't fit a single slice — the group MUST be double-wide (slices 4+5 paired).
 */
static void acl_program_one(int unit, int idx, uint32_t dstip, uint32_t dstmask, int deny)
{
    FP_TCAMm_t t;
    FP_GM_FIELDSm_t gf;
    FP_GLOBAL_MASK_TCAMm_t g;
    FP_POLICY_TABLEm_t p;
    int sec = idx + 256;                        /* paired secondary in physical slice 7 */

    /* ── PRIMARY entry (physical slice 6) — verbatim field layout from the Cumulus
     * capture FP_TCAM[1536]: DstIp lives in the TOP 32 bits of the 128-bit F2
     * field (F2[127:96] = LSW-first word[3]); FIXED_MASK=0x380; the paired half
     * (PAIRING_F2/PAIRING_FIXED_MASK=0x700) carries the same DstIp. ── */
    FP_TCAMm_CLR(t);
    FP_TCAMm_VALIDf_SET(t, 3);
    {
        uint32_t f2[4]  = { 0, 0, 0, dstip };
        uint32_t f2m[4] = { 0, 0, 0, dstmask };
        FP_TCAMm_F2f_SET(t, f2);
        FP_TCAMm_F2_MASKf_SET(t, f2m);
        FP_TCAMm_PAIRING_F2f_SET(t, f2);
        FP_TCAMm_PAIRING_F2_MASKf_SET(t, f2m);
    }
    FP_TCAMm_FIXED_MASKf_SET(t, 0x380);
    FP_TCAMm_PAIRING_FIXED_MASKf_SET(t, 0x700);
    (void)WRITE_FP_TCAMm(unit, idx, t);

    /* ── SECONDARY entry (physical slice 7) — Cumulus FP_TCAM[1792]: VALID=3 plus
     * the fixed F3 mask the paired slice carries (F3_MASK=PAIRING_F3_MASK=
     * 0x07fff80000). ── */
    FP_TCAMm_CLR(t);
    FP_TCAMm_VALIDf_SET(t, 3);
    {
        uint32_t f3m[4] = { 0xfff80000, 0x00000007, 0, 0 };   /* 0x07fff80000 */
        FP_TCAMm_F3_MASKf_SET(t, f3m);
        FP_TCAMm_PAIRING_F3_MASKf_SET(t, f3m);
    }
    (void)WRITE_FP_TCAMm(unit, sec, t);

    /* ── FP_GM_FIELDS overlay — the memory edged never wrote (double-wide requires it).
     * Primary: VALID=1, MASK=0x1ffffffffe (37-bit), KEY=0.  Secondary: VALID=1 only.
     * Values verbatim from Cumulus FP_GM_FIELDS[531]/[787]. ── */
    FP_GM_FIELDSm_CLR(gf);
    FP_GM_FIELDSm_VALIDf_SET(gf, 1);
    { uint32_t gm[2] = { 0xfffffffe, 0x0000001f };      /* 0x1ffffffffe (Cumulus value) */
      FP_GM_FIELDSm_MASKf_SET(gf, gm); }
    (void)WRITE_FP_GM_FIELDSm(unit, idx, gf);
    FP_GM_FIELDSm_CLR(gf);
    FP_GM_FIELDSm_VALIDf_SET(gf, 1);
    (void)WRITE_FP_GM_FIELDSm(unit, sec, gf);

    /* ── FP_GLOBAL_MASK_TCAM ingress-port gate — Cumulus FP_GLOBAL_MASK_TCAM[1536]
     * VERBATIM: IPBM=0x00001fffffffffffff (all 53 front ports set), IPBM_MASK=
     * 0x02001fffffffffffff (care ports 0-52 + bit57). edged's earlier IPBM=0
     * (don't-care) did NOT match — the port gate needs the real bitmap. Secondary
     * [1792] = VALID=1 only. ── */
    FP_GLOBAL_MASK_TCAMm_CLR(g);
    FP_GLOBAL_MASK_TCAMm_VALIDf_SET(g, 1);
    {
        uint32_t ipbm[2]  = { 0xffffffff, 0x001fffff };   /* bits 0-52          */
        uint32_t ipbmm[2] = { 0xffffffff, 0x021fffff };   /* bits 0-52 + bit57  */
        FP_GLOBAL_MASK_TCAMm_IPBMf_SET(g, ipbm);
        FP_GLOBAL_MASK_TCAMm_IPBM_MASKf_SET(g, ipbmm);
    }
    (void)WRITE_FP_GLOBAL_MASK_TCAMm(unit, idx, g);
    FP_GLOBAL_MASK_TCAMm_CLR(g);
    FP_GLOBAL_MASK_TCAMm_VALIDf_SET(g, 1);
    (void)WRITE_FP_GLOBAL_MASK_TCAMm(unit, sec, g);

    FP_POLICY_TABLEm_CLR(p);
    if (deny == 1) {                            /* permit = no action, packet proceeds */
        FP_POLICY_TABLEm_G_DROPf_SET(p, 1);
        FP_POLICY_TABLEm_Y_DROPf_SET(p, 1);
        FP_POLICY_TABLEm_R_DROPf_SET(p, 1);
        /* COPY_TO_CPU=3 = SwitchToCpuCancel, exactly as Cumulus/switchd programs a
         * hardware drop (captured FP_POLICY: G/Y/R_DROP=1 + COPY_TO_CPU=3). */
        FP_POLICY_TABLEm_G_COPY_TO_CPUf_SET(p, 3);
        FP_POLICY_TABLEm_Y_COPY_TO_CPUf_SET(p, 3);
        FP_POLICY_TABLEm_R_COPY_TO_CPUf_SET(p, 3);
    } else if (deny == 2) {                     /* DIAGNOSTIC "copy": mirror match to CPU,
         * no drop. A matched transit packet (HW-forwarded swp1->swp2) that ALSO appears on
         * the swp1 CPU tap = proof the FP LOOKUP fired (independent of the broken counter and
         * of whether the drop action works). COPY_TO_CPU=1 = copy (not =3 cancel). */
        FP_POLICY_TABLEm_G_COPY_TO_CPUf_SET(p, 1);
        FP_POLICY_TABLEm_Y_COPY_TO_CPUf_SET(p, 1);
        FP_POLICY_TABLEm_R_COPY_TO_CPUf_SET(p, 1);
    }
    FP_POLICY_TABLEm_COUNTER_MODEf_SET(p, 7);   /* count all colors: a match signal */
    FP_POLICY_TABLEm_COUNTER_INDEXf_SET(p, idx & 0x7f);   /* 7-bit field (was wrapping) */
    (void)WRITE_FP_POLICY_TABLEm(unit, idx, p);
    acl_log("prog idx=%d f2[3]=0x%08x/0x%08x action=%s", idx, dstip, dstmask,
            deny == 1 ? "DENY" : deny == 2 ? "COPY2CPU" : "permit");
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
    acl_log("diag FP_SLICE_ENABLE(runtime)=0x%08x (want enable+lookup bits for slices 4&5: "
            "0x%08x)", sev, (1u<<4)|(1u<<5)|(1u<<14)|(1u<<15));

    /* FP-STAGE REGDIFF: read the FP/ingress-pipeline registers that gate whether the
     * IFP lookup runs, compare to the working-Cumulus values (from dump_soc.txt). A GAP
     * (ours=0) on FP_TCAM_BLK_SEL / FP_GM_TCAM_BLK_SEL etc. = the missing stage init. */
    {
        static const struct { uint32_t addr, cval; const char *name; } fpr[] = {
            { 0x0d180d20, 0x00000fff, "FP_TCAM_BLK_SEL" },
            { 0x0d180d21, 0x00000fff, "FP_GM_TCAM_BLK_SEL" },
            { 0x0d180601, 0x000e33ff, "FP_SLICE_ENABLE" },
            { 0x01180602, 0x000001ff, "ING_CONFIG_2" },
            { 0x01180600, 0x2080300e, "ING_CONFIG_64(lo)" },
            { 0x0f180665, 0x0000000c, "SW2_FP_DST_ACTION_CONTROL" },
            { 0x04180620, 0x000000ff, "VFP_SLICE_CONTROL" },
            { 0x04180621, 0x00000003, "VFP_KEY_CONTROL" },
            { 0x04180636, 0x0000e4e4, "VFP_SLICE_MAP" },
        };
        unsigned k;
        for (k = 0; k < sizeof(fpr)/sizeof(fpr[0]); k++) {
            uint32_t v = 0xdeadbeef;
            cdk_xgs_reg32_read(ACL_UNIT, fpr[k].addr, &v);
            acl_log("diag FPREG %-26s ours=0x%08x cumulus=0x%08x %s",
                    fpr[k].name, v, fpr[k].cval,
                    v == fpr[k].cval ? "OK" : (v == 0 ? "**GAP**" : "**DIFF**"));
        }
    }

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
                acl_log("diag INGRESS port=%d FILTER_EN=%d PFS S4[F1=%u F2=%u F3=%u] "
                        "S5[F1=%u F2=%u F3=%u] PAIR5_4=%u", prt, fe,
                        FP_PORT_FIELD_SELm_SLICE4_F1f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE4_F2f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE4_F3f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE5_F1f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE5_F2f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE5_F3f_GET(fs),
                        FP_PORT_FIELD_SELm_SLICE5_4_PAIRINGf_GET(fs));
            } else {
                acl_log("diag INGRESS port=%d FILTER_EN=%d PFS read FAILED", prt, fe);
            }
        }
    }

    l3_fwd_diag();   /* HW-L3-forward state: egress gates + Nexus neighbor entry */
}

/* Invalidate every ACL entry we programmed (VALID=0), so a reload rebuilds cleanly. */
void edged_acl_reset(void)
{
    FP_TCAMm_t t;
    int i;
    FP_TCAMm_CLR(t);            /* VALID=0 -> entry disabled */
    for (i = 0; i < acl_n; i++) {
        int idx = acl_idx_used[i];
        (void)WRITE_FP_TCAMm(ACL_UNIT, idx, t);         /* primary (slice 4)   */
        (void)WRITE_FP_TCAMm(ACL_UNIT, idx + 256, t);   /* secondary (slice 5) */
    }
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
    /* NOTE: ING_CONFIG_2=0x1ff (a REGDIFF gap the SDK sets for BCM56846) was tried here
     * and BROKE box->Nexus forwarding (its USE_VLAN_ING_PORT_BITMAP bit needs subsystems
     * edged's partial init lacks) WITHOUT enabling the IFP match. Reverted — not the gate. */
    acl_log("IFP pipeline enable: IFP_BYPASS=0 + ING_EN_EFILTER_BITMAP=all-ports");
}

/* Stand up the DOUBLE-WIDE dst-IP group on slices 4+5 — the exact control surface the
 * Cumulus register-diff proved (only 3 registers change): FP_PORT_FIELD_SEL (per port:
 * both-half selcodes + SLICE5_4_PAIRING=1), FP_SLICE_KEY_CONTROL (secondary DstClass),
 * FP_SLICE_ENABLE (enable + LOOKUP-enable BOTH slices). RMW everywhere so the trap slices
 * cumulus_replicate set up are preserved. Selcodes verbatim from the capture:
 *   slice4 (primary):   F1=5  F2=1  F3=7
 *   slice5 (secondary): F1=0xc F2=5 F3=0xa , SLICE_5_DST_CLASS_ID_SEL=1 */
static void acl_setup_doublewide(void)
{
    int p, n = 0;

    /* FP_PORT_FIELD_SEL per ingress port: selcodes for both halves + the pairing bit.
     * edged addresses this memory by chip port (same as PORT_TAB/FILTER_ENABLE above),
     * so cover every port 0..MAX — a transit packet may ingress on any front port. */
    for (p = 0; p <= FP_PORT_FIELD_SELm_MAX; p++) {
        FP_PORT_FIELD_SELm_t fs;
        FP_PORT_FIELD_SELm_CLR(fs);
        if (cdk_xgs_mem_read(ACL_UNIT, FP_PORT_FIELD_SELm, p, fs.v, 6) < 0) continue;
        /* The user dst-IP ingress drop group is GID 3 = virtual slices 8,9
         * (-> physical slices 6,7 via FP_SLICE_MAP), verified in the ACL capture
         * (static_port_field_sel.txt). Selcodes verbatim: slice8 F1=5/F2=1/F3=7,
         * slice9 F1=0xc/F2=5/F3=0xa, SLICE9_8_PAIRING=1. */
        FP_PORT_FIELD_SELm_SLICE8_F1f_SET(fs, 5);
        FP_PORT_FIELD_SELm_SLICE8_F2f_SET(fs, 1);
        FP_PORT_FIELD_SELm_SLICE8_F3f_SET(fs, 7);
        FP_PORT_FIELD_SELm_SLICE9_F1f_SET(fs, 0xc);
        FP_PORT_FIELD_SELm_SLICE9_F2f_SET(fs, 5);
        FP_PORT_FIELD_SELm_SLICE9_F3f_SET(fs, 0xa);
        FP_PORT_FIELD_SELm_SLICE9_8_PAIRINGf_SET(fs, 1);
        if (WRITE_FP_PORT_FIELD_SELm(ACL_UNIT, p, fs) >= 0) n++;
    }

    /* FP_SLICE_KEY_CONTROL[0]: DstClass select for the secondary slice 9 (RMW). */
    {
        FP_SLICE_KEY_CONTROLm_t kc;
        FP_SLICE_KEY_CONTROLm_CLR(kc);
        if (cdk_xgs_mem_read(ACL_UNIT, FP_SLICE_KEY_CONTROLm, 0, kc.v, 4) >= 0) {
            FP_SLICE_KEY_CONTROLm_SLICE_9_DST_CLASS_ID_SELf_SET(kc, 1);
            (void)WRITE_FP_SLICE_KEY_CONTROLm(ACL_UNIT, 0, kc);
        }
    }

    /* FP_SLICE_ENABLE: enable + LOOKUP-enable BOTH slices 4 and 5 (RMW-OR). Two distinct
     * bit-fields: SLICE_ENABLE_SLICE_n = bit n, LOOKUP_ENABLE_SLICE_n = bit 10+n. The
     * register-diff showed LOOKUP-enable is the bit that decides consult-vs-ignore; a
     * paired slice with its LOOKUP bit clear is written but never looked up. */
    {
        FP_SLICE_ENABLEr_t se;
        uint32_t v = 0;
        cdk_xgs_reg32_read(ACL_UNIT, FP_SLICE_ENABLEr, &v);   /* for the log below */
        v = 0x000e33ff;   /* Cumulus EXACT: SLICE_ENABLE all + LOOKUP_ENABLE 2,3,7,8,9
                           * (clears the stray LOOKUP_SLICE_6 bit that read back as 0xf33ff) */
        FP_SLICE_ENABLEr_CLR(se);
        FP_SLICE_ENABLEr_SET(se, v);
        (void)WRITE_FP_SLICE_ENABLEr(ACL_UNIT, se);
        acl_log("doublewide: FP_SLICE_ENABLE=0x%08x, PFS pairing on %d ports", v, n);
    }
}

int edged_acl_load(const char *path)
{
    static struct acl_rule rules[ACL_MAX_RULES];
    static char applied[ACL_MAX_BINDS][32];
    FILE *f = fopen(path, "r");
    char line[256];
    int nr = 0, nb = 0, i, j, done = 0, skipped = 0;
    int fp_mode, fp_n = 0;

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
            rules[nr].deny = !strcmp(act, "deny") ? 1 : (!strcmp(act, "copy") ? 2 : 0);
            strncpy(rules[nr].dst, ds, sizeof(rules[nr].dst) - 1);
            proto_any = (!strcmp(pr, "ip") || !strcmp(pr, "any") || !strcmp(pr, "all"));
            src_any   = (!strcmp(sr, "any") || !strcmp(sr, "0.0.0.0/0"));
            rules[nr].supported = (proto_any && src_any && !has_l4 && !strchr(ds, ':'));
            nr++;
        }
    }
    fclose(f);

    /* ── Program applied rules via the L3 datapath (NOT the FP) ──────────────────
     * The IFP/FP lookup is not armed by OpenMDK's init on this chip (exhaustively
     * confirmed 2026-07-14 — every FP register+memory matches Cumulus yet the lookup
     * never fires). The L3 forwarding path works, so a dst-IP deny is programmed as
     * an L3 DST_DISCARD entry (l3_v4_deny_add): the chip's L3 lookup drops routed
     * traffic to the dst. This touches NEITHER the FP nor the L2_USER_ENTRY CPU-punt,
     * so it can't break forwarding or the control plane. deny -> a DST_DISCARD entry;
     * permit -> nothing (permit is the default; a more-specific /32 permit would win
     * by L3 longest-prefix-match). Scope: L3-routed/transit traffic, global ingress.
     * The FP double-wide machinery (acl_program_one/acl_setup_doublewide/...) is kept
     * for if/when the IFP is ever armed, but is not called on this path. */
    /* FP silicon-drop path (gated on /etc/edged/acl_fp): program each dst-IP deny
     * as a double-wide Ingress Field Processor Drop entry (slices 4+5) — the way
     * Cumulus does it (act=Drop @ DstIp). Un-armed pre-coherent-set; the coherent
     * soc_init set may now arm the FP lookup (as it armed the L3 lookup). Arm the
     * full recipe once, then one entry per deny. L3 hybrid runs alongside so the
     * ACL still works if the FP doesn't fire; rdbgc1 tells them apart (FP -> chip
     * drop / rdbgc1 climbs; hybrid -> CPU punt / rdbgc1=0). */
    fp_mode = (access("/etc/edged/acl_fp", F_OK) == 0);
    l3_v4_deny_reset();
    if (fp_mode) {
        edged_acl_reset();          /* invalidate any prior FP entries */
        acl_gm_map_init();          /* IFP global-mask logical->phys port map */
        /* NOTE: NOT calling acl_clear_global_mask() — it wipes cumulus_replicate's
         * FP_TCAM/FP_POLICY traps and breaks forwarding (100% loss). The
         * FP_GLOBAL_MASK_TCAM entries we need are written per-entry below. */
        acl_enable_port_filter();   /* PORT_TAB.FILTER_ENABLE all ports */
        acl_ifp_pipeline_enable();  /* IFP_BYPASS=0 + ING_EN_EFILTER_BITMAP */
        acl_setup_doublewide();     /* slices 8+9 pairing/selcodes (GID 3 recipe) */
        acl_log("FP mode ON: double-wide IFP dst-IP drop armed (slices 8/9)");
    }
    while (1) {
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
                if (rules[best].deny == 1) {                    /* deny -> L3 DST_DISCARD */
                    if (l3_v4_deny_add(ip, mask) == 0) done++;
                    if (fp_mode && fp_n < ACL_MAX) {            /* + FP silicon drop */
                        int idx = 1536 + fp_n;   /* physical slice 6 (virtual 8) */
                        acl_program_one(ACL_UNIT, idx, ip, mask, 1);
                        acl_idx_used[acl_n++] = idx;
                        fp_n++;
                    }
                } else if (rules[best].deny == 2 && fp_mode && fp_n < ACL_MAX) {
                    /* DIAGNOSTIC "copy": program an FP COPY_TO_CPU entry (mask=0
                     * => match-any IPv4). If its FP_COUNTER increments (or copies
                     * hit the CPU), the IFP stage IS running -> the dst-IP drop's
                     * key is the issue; if it stays 0, the whole IFP stage is dead. */
                    int idx = 1536 + fp_n;
                    acl_program_one(ACL_UNIT, idx, ip, mask, 2);
                    acl_idx_used[acl_n++] = idx;
                    fp_n++;
                    done++;
                } else {                                        /* permit: default */
                    acl_log("rule %s seq %d: permit (default) — no L3 entry needed",
                            rules[best].name, rules[best].seq);
                }
            }
        } else {
            syslog(LOG_WARNING, "ACL %s seq %d: 5610 v1 matches dst-IP only "
                   "(proto/src/L4 unsupported) — skipped", rules[best].name, rules[best].seq);
            skipped++;
        }
        rules[best].seq = -1;                                    /* mark taken */
    }
    syslog(LOG_INFO, "ACL: installed %d L3 dst-IP den%s from %s (%d skipped)",
           done, done == 1 ? "y" : "ies", path, skipped);
    acl_log("load %s: %d rules, %d applied-bindings -> %d L3 denies, %d skipped",
            path, nr, nb, done, skipped);
    return done;
}
