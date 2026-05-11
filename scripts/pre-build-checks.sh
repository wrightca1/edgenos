#!/bin/bash
# pre-build-checks.sh - Sanity-check the codebase against past-regression rules.
#
# Run before every build (top-level Makefile hooks this from `image:`).
# Exits non-zero on any violation so CI/local builds can never produce a
# .bin that silently reverts a hard-won fix.
#
# Each rule cites the memory/document where the underlying lesson was
# captured, so a future developer who hits a check failure knows why the
# rule exists.

set -e

TOPDIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$TOPDIR"

FAIL=0
say()  { echo "  [check] $*"; }
fail() { echo "  [FAIL]  $*"; FAIL=$((FAIL + 1)); }
pass() { echo "  [ ok ]  $*"; }

echo "==> EdgeNOS pre-build sanity checks"

# 1. IND_40BITIF must use bit 15, never bit 6.
#    Source: memory/project_session_20260509 — wrong bit broke SerDes link.
say "IND_40BITIF bit-15 (asic/edged/portmap.c)"
if grep -nE 'misc3_val[[:space:]]*\|=[[:space:]]*\(1[[:space:]]*<<[[:space:]]*6\)' \
        asic/edged/portmap.c >/dev/null; then
    fail "portmap.c still ORs (1<<6) into misc3_val — IND_40BITIF must be bit 15"
elif grep -nE 'misc3_val[[:space:]]*\|=[[:space:]]*\(1[[:space:]]*<<[[:space:]]*15\)' \
        asic/edged/portmap.c >/dev/null; then
    pass "IND_40BITIF=(1<<15) present"
else
    fail "portmap.c missing IND_40BITIF assignment — refusing to build"
fi

# 2. PAXB sub-window 7 remap must be present in linux-kernel-bde.c.
#    Source: memory/project_subwindow_fix — CMICm regs above 0x8000 need this.
say "PAXB sub-window 7 remap (asic/bde/linux-kernel-bde.c)"
if grep -q 'PAXB_REMAP_SUBWIN[[:space:]]*7' asic/bde/linux-kernel-bde.c \
   && grep -q 'PAXB_IMAP0_BASE' asic/bde/linux-kernel-bde.c; then
    pass "IMAP0_7 dynamic remap wired"
else
    fail "BDE missing PAXB sub-window 7 remap path"
fi

# 3. iProc init must use PCI config space writes (not BAR0 MMIO).
#    Source: memory/feedback_cumulus_bde_pci_config — BAR0 writes don't persist.
say "iProc init via PCI config space (asic/bde/linux-kernel-bde.c)"
if grep -q 'pci_write_config_dword' asic/bde/linux-kernel-bde.c; then
    pass "pci_write_config_dword used for iProc PAXB writes"
else
    fail "BDE not using PCI config space for iProc — IMAP/OARR writes will be lost"
fi

# 4. CPLD writes must never exceed offset 0x1F.
#    Source: memory/feedback_cpld_addr_wrap — CPLD wraps; writes above 0x1F
#    can hit reset-sensitive registers and brick the board.
say "CPLD writes ≤ 0x1F (platform/cpld/)"
if grep -nE 'cpld_write[[:alpha:]_]*\([^,]+,[[:space:]]*0x[2-9a-fA-F][0-9a-fA-F]' \
        platform/cpld/*.c 2>/dev/null; then
    fail "CPLD write to offset >0x1F detected — refusing to build"
else
    pass "No CPLD writes above 0x1F"
fi

# 5. Docker builds must include --no-cache when source changed.
#    Source: memory/feedback_docker_cache — stale COPY layer ate hours.
say "Docker build flag discipline (build.sh, docker-build.sh)"
if [ -f build.sh ]; then
    if grep -q -- '--no-cache' build.sh && grep -q -- '--network host' build.sh; then
        pass "build.sh uses --no-cache --network host"
    else
        # Only a warning — not every build of build.sh is a code-change build.
        echo "  [warn]  build.sh missing --no-cache or --network host; ok for cached re-runs"
    fi
fi

# 6. Top-level Makefile must not reference the old switchd target.
#    Source: this session's rename of switchd → edged.
say "switchd → edged rename (Makefile, services, scripts)"
LEFTOVER=$(grep -rln '\bswitchd\b' \
    --include=Makefile --include='*.sh' --include='*.service' \
    --include='*.conf' \
    --exclude-dir=asic/openmdk --exclude-dir=asic/opennsl-init \
    --exclude-dir=asic/opennsl-examples --exclude-dir=.git \
    --exclude-dir=output --exclude-dir=build \
    --exclude=ONIE_ISSUES.md 2>/dev/null \
    | xargs -r grep -l 'Cumulus' -L 2>/dev/null || true)
if [ -n "$LEFTOVER" ]; then
    echo "$LEFTOVER" | sed 's/^/      - /'
    fail "stale 'switchd' references found in build/runtime files (above)"
else
    pass "no stale switchd references in build/runtime files"
fi

# 7. CDR reset must be present in DS100DF410 init path.
#    Source: memory/project_cdr_reset_breakthrough — without cdr_rst, retimers
#    pass garbage; links never come up.
say "DS100DF410 CDR reset wired (platform/retimer/, config/rootfs/overlay/)"
if grep -rqE 'cdr_rst|cdr_reset' platform/retimer/ \
        config/rootfs/overlay/usr/sbin/ 2>/dev/null; then
    pass "CDR reset path present"
else
    fail "no cdr_rst / cdr_reset reference found — retimers will not initialize"
fi

echo
if [ "$FAIL" -gt 0 ]; then
    echo "==> $FAIL check(s) FAILED. Build aborted."
    exit 1
fi
echo "==> All pre-build checks passed."
