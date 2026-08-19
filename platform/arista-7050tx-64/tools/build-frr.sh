#!/bin/bash
# build-frr.sh -- assemble stock FRR plus the shared libraries it needs, for the
# EdgeNOS initrd on the 7050TX-64.
#
# WHY THIS EXISTS, AND WHAT IT CHANGES
#
# The initrd had NO libc at all -- /lib held only `modules` -- so every binary
# had to be fully static. That is why sdkpoc needs STATIC=1, and it is why this
# project first shipped a static Quagga 1.2.4 when FRR was what was actually
# asked for. Static-linking modern FRR means static libyang2, pcre2, json-c and
# c-ares first; the constraint had started dictating which software the box was
# allowed to run.
#
# So the constraint goes. This script puts a real glibc in the image and runs
# STOCK FRR, unmodified, from Debian's own package.
#
# ⚠ The binaries and the libc MUST come from the same container. Mixing a
# distro's FRR with a different glibc is exactly the version skew that makes a
# dynamically linked initrd miserable, and the failure is a runtime loader error
# on the switch rather than a build error here.
#
# Usage:  tools/build-frr.sh [outdir]      default build/frr-root
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/build/frr-root}"
IMG="debian:bookworm"

# ⚠ --network=host is required on this build host. Rootless Docker cannot write
# net.ipv4.ip_unprivileged_port_start when it sets up a container network
# namespace, and the run dies with "reopen fd 8: permission denied". The usual
# workaround here is --network=none, but apt needs the network, so host
# networking is the one that works for both.

say() { printf '\033[1;36mbuild-frr:\033[0m %s\n' "$*"; }

rm -rf "$OUT"
mkdir -p "$OUT"

say "collecting FRR and its libraries from $IMG"

# Everything happens inside the container: install FRR, then walk each binary's
# NEEDED list with ldd and copy the closure. Copying by ldd rather than by a
# hand-written list is the point -- a hand-written list is what goes stale.
docker run --rm --network=host -v "$OUT:/out" "$IMG" bash -euc '
    export DEBIAN_FRONTEND=noninteractive
    # ⚠ Rootless Docker again: apt drops privileges to the _apt user to fetch,
    # and setgroups is not permitted in this user namespace, so every download
    # dies with "Method http has died unexpectedly". Telling apt to stay root
    # is the fix; this is a throwaway build container.
    echo "APT::Sandbox::User \"root\";" > /etc/apt/apt.conf.d/99-no-sandbox
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends frr frr-pythontools >/dev/null

    mkdir -p /out/usr/lib/frr /out/usr/bin /out/lib64 /out/lib/x86_64-linux-gnu

    # The daemons we need, plus the tools that drive them.
    for b in /usr/lib/frr/zebra /usr/lib/frr/ospfd /usr/lib/frr/ospf6d \
             /usr/lib/frr/staticd /usr/lib/frr/watchfrr /usr/bin/vtysh; do
        [ -x "$b" ] || { echo "missing $b"; exit 1; }
        cp -a "$b" /out"$b"
    done

    # Library closure. ldd output lines look like "libfoo.so => /path (0x...)".
    for b in /out/usr/lib/frr/* /out/usr/bin/vtysh; do
        ldd "$b" 2>/dev/null | awk "/=> \//{print \$3} /ld-linux/{print \$1}"
    done | sort -u | while read -r lib; do
        [ -e "$lib" ] || continue
        mkdir -p /out"$(dirname "$lib")"
        cp -aL "$lib" /out"$lib"
    done

    # ⚠ Everything from here down is dlopen()ed at runtime, so ldd cannot see it
    # and the library closure above will not contain it. Each of these was a
    # runtime failure on the switch, not a build failure here.

    # libyang plugins, opened when FRR loads its YANG models.
    if [ -d /usr/lib/x86_64-linux-gnu/libyang2 ]; then
        cp -a /usr/lib/x86_64-linux-gnu/libyang2 /out/usr/lib/x86_64-linux-gnu/
    fi

    # PAM modules. Debian builds vtysh against PAM, and without the modules it
    # aborts with "Failure to initialize pam: Critical error - immediate abort"
    # -- so the daemons run fine and the tool you inspect them with does not.
    if [ -d /lib/x86_64-linux-gnu/security ]; then
        mkdir -p /out/lib/x86_64-linux-gnu/security
        cp -a /lib/x86_64-linux-gnu/security/pam_permit.so \
              /lib/x86_64-linux-gnu/security/pam_deny.so \
              /lib/x86_64-linux-gnu/security/pam_unix.so \
              /out/lib/x86_64-linux-gnu/security/ 2>/dev/null || true
    fi
    mkdir -p /out/etc/pam.d
    # The box has no user database beyond root, and vtysh access is already
    # gated by having a shell on the switch, so permit is the honest policy
    # rather than a stack that cannot succeed.
    # NB: no single quotes in here -- this whole block is already inside a
    # single-quoted argument to docker, and nesting them silently truncated an
    # earlier version of this write, leaving /etc/pam.d empty with no error.
    { echo "auth    sufficient pam_permit.so"
      echo "account sufficient pam_permit.so"
      echo "session sufficient pam_permit.so"; } > /out/etc/pam.d/frr
    # ...and its YANG models, which it reads from disk and refuses to start without.
    mkdir -p /out/usr/share
    cp -a /usr/share/yang /out/usr/share/ 2>/dev/null || true

    dpkg-query -W -f "FRR \${Version}\n" frr > /out/VERSION

    # ---- bill of materials -------------------------------------------------
    # Every file we ship, mapped back to the binary package that owns it and the
    # SOURCE package that produced it. This is the compliance record, and it is
    # generated rather than hand-written precisely so it cannot go stale as the
    # library closure changes.
    mkdir -p /out/usr/share/doc/edgenos
    BOM=/out/usr/share/doc/edgenos/BILL-OF-MATERIALS
    { echo "# Third-party binaries shipped in the EdgeNOS image for the"
      echo "# Arista 7050TX-64, and the source packages they came from."
      echo "# Generated by tools/build-frr.sh from $(cat /etc/debian_version) (bookworm)."
      echo "#"
      echo "# binary-package  version  source-package  licence"
      echo; } > $BOM
    find /out -type f \( -name "*.so*" -o -path "*/usr/lib/frr/*" -o -name vtysh \) \
        -printf "%p\n" | sed "s|^/out||" | sort -u | while read -r f; do
        pkg=$(dpkg -S "$f" 2>/dev/null | cut -d: -f1 | head -1)
        [ -n "$pkg" ] || continue
        echo "$pkg"
    done | sort -u | while read -r pkg; do
        ver=$(dpkg-query -W -f "\${Version}" "$pkg" 2>/dev/null)
        src=$(dpkg-query -W -f "\${source:Package}" "$pkg" 2>/dev/null)
        [ -n "$src" ] || src="$pkg"
        lic=$(sed -n "s/^License: //p" /usr/share/doc/"$pkg"/copyright 2>/dev/null | sort -u | paste -sd, -)
        printf "%-28s %-26s %-22s %s\n" "$pkg" "$ver" "$src" "${lic:-see copyright}" >> $BOM
    done

    # The copyright files themselves -- the licence texts we are obliged to
    # convey along with the binaries.
    mkdir -p /out/usr/share/doc/edgenos/copyright
    for pkg in $(awk "NR>5 {print \$1}" $BOM); do
        [ -f /usr/share/doc/"$pkg"/copyright ] && \
            cp /usr/share/doc/"$pkg"/copyright /out/usr/share/doc/edgenos/copyright/"$pkg".copyright
    done

    chmod -R a+rX /out
'

say "collected:"
printf '  FRR       %s\n' "$(cat "$OUT/VERSION")"
printf '  binaries  %s\n' "$(find "$OUT/usr/lib/frr" "$OUT/usr/bin" -type f 2>/dev/null | wc -l)"
printf '  libraries %s\n' "$(find "$OUT" -name '*.so*' -type f | wc -l)"
printf '  size      %s\n' "$(du -sh "$OUT" | cut -f1)"

# The loader is the one path that cannot be wrong: the kernel reads the
# interpreter path out of the ELF header verbatim, so it must exist at exactly
# that path in the image or every binary dies with "not found".
#
# ⚠ Do this on the HOST. The first version of this check ran readelf inside the
# container, which does not have readelf; INTERP came back empty, the test
# became `[ -e "$OUT" ]`, and it passed by testing the output directory. A check
# that cannot fail is worse than no check, because it reads as verification.
INTERP=$(readelf -l "$OUT/usr/lib/frr/zebra" | sed -n 's/.*interpreter: \(.*\)\]/\1/p')
[ -n "$INTERP" ] || { echo "** could not read the ELF interpreter"; exit 1; }
say "ELF interpreter is $INTERP"
[ -e "$OUT$INTERP" ] || { echo "** $INTERP missing from the tree"; exit 1; }

# Every NEEDED library must resolve inside the tree too, not just the loader.
MISSING=0
for b in "$OUT"/usr/lib/frr/* "$OUT"/usr/bin/vtysh; do
    for lib in $(readelf -d "$b" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p'); do
        find "$OUT" -name "$lib" -print -quit | grep -q . || {
            echo "** $(basename "$b") needs $lib, which is not in the tree"
            MISSING=1
        }
    done
done
[ "$MISSING" = 0 ] || exit 1
say "loader and every NEEDED library resolve inside the tree"
