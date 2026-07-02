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
/* Virtual slice 2 -> physical slice 8, which the Cumulus capture proves is
 * lookup-enabled (FP_SLICE_ENABLE) with field-select F2=1 = DstIP at F2[96..127].
 * The FP_TCAM is virtual-slice indexed, 256 entries/slice, so VS2 = idx 512..767 —
 * empty in the capture (no trap collision). (cumulus_replicate's "idx 1024 = VS6"
 * was wrong: idx 1024 is VS4 -> phys0, which has NO lookup enable, so entries there
 * never matched.) */
#define ACL_IDX_BASE  512       /* VS2 -> phys slice 8 (DstIP, lookup-enabled) */
#define ACL_MAX       64        /* 512..575 — within VS2 */

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

static void acl_program_one(int unit, int idx, uint32_t dstip, uint32_t dstmask, int deny)
{
    FP_TCAMm_t t;
    FP_GLOBAL_MASK_TCAMm_t g;
    FP_POLICY_TABLEm_t p;
    uint32_t f2[4]  = {0};
    uint32_t f2m[4] = {0};

    f2[3]  = dstip;             /* DstIP at F2 bits 96..127 (VS6 selcode 1) */
    f2m[3] = dstmask;

    FP_TCAMm_CLR(t);
    FP_TCAMm_VALIDf_SET(t, 3);  /* single-wide valid (as the VS6 OSPF trap) */
    FP_TCAMm_F2f_SET(t, f2);
    FP_TCAMm_F2_MASKf_SET(t, f2m);
    (void)WRITE_FP_TCAMm(unit, idx, t);

    FP_GLOBAL_MASK_TCAMm_CLR(g);
    FP_GLOBAL_MASK_TCAMm_VALIDf_SET(g, 1);      /* match any ingress port (v1) */
    (void)WRITE_FP_GLOBAL_MASK_TCAMm(unit, idx, g);

    FP_POLICY_TABLEm_CLR(p);
    if (deny) {                                 /* permit = no action, packet proceeds */
        FP_POLICY_TABLEm_G_DROPf_SET(p, 1);
        FP_POLICY_TABLEm_Y_DROPf_SET(p, 1);
        FP_POLICY_TABLEm_R_DROPf_SET(p, 1);
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
        uint32_t f2[4] = {0};
        int valid = -1;
        FP_TCAMm_CLR(t);
        if (cdk_xgs_mem_read(ACL_UNIT, FP_TCAMm, acl_idx_used[i], t.v, 15) >= 0) {
            valid = FP_TCAMm_VALIDf_GET(t);
            FP_TCAMm_F2f_GET(t, f2);
        }
        FP_COUNTER_TABLEm_CLR(c); pkts = 0xffffffff;
        if (cdk_xgs_mem_read(ACL_UNIT, FP_COUNTER_TABLEm, acl_idx_used[i], c.v, 3) >= 0)
            pkts = c.v[0] & 0x1fffffff;
        acl_log("diag idx=%d readback valid=%d f2[3]=0x%08x counter=%u",
                acl_idx_used[i], valid, f2[3], pkts);
    }
}

/* Invalidate every ACL entry we programmed (VALID=0), so a reload rebuilds cleanly. */
void edged_acl_reset(void)
{
    FP_TCAMm_t t;
    int i;
    FP_TCAMm_CLR(t);            /* VALID=0 -> entry disabled */
    for (i = 0; i < acl_n; i++)
        (void)WRITE_FP_TCAMm(ACL_UNIT, acl_idx_used[i], t);
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
    for (p = 0; p < 64; p++) {
        PORT_TABm_t pt;
        PORT_TABm_CLR(pt);
        if (cdk_xgs_mem_read(ACL_UNIT, PORT_TABm, p, pt.v, 10) < 0) continue;
        PORT_TABm_FILTER_ENABLEf_SET(pt, 1);
        if (WRITE_PORT_TABm(ACL_UNIT, p, pt) >= 0) n++;
    }
    acl_log("PORT_TAB.FILTER_ENABLE set on %d ports (per-port IFP enable)", n);
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
    {   /* Re-assert Cumulus's FP_SLICE_ENABLE in case it didn't stick / was overwritten
         * (0x000f33ff = lookup enabled on phys slices 2,3,6,7,8,9). */
        FP_SLICE_ENABLEr_t se;
        FP_SLICE_ENABLEr_CLR(se);
        FP_SLICE_ENABLEr_SET(se, 0x000f33ff);
        (void)WRITE_FP_SLICE_ENABLEr(ACL_UNIT, se);
    }
    acl_enable_port_filter();               /* the missing per-port IFP enable */
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
