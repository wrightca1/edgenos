#!/usr/bin/env bash
# Verify the builder images provide exactly what the build scripts invoke.
#
# This is deliberately not a "does the image exist" check. Each builder is exercised
# with the same tools the real build uses, and the cross toolchains actually compile
# and are checked for the right target architecture -- an ARM toolchain that quietly
# produces x86 objects would sail past a `command -v` test.
#
# Usage: docker/verify-images.sh
set -uo pipefail
export DOCKER_HOST=${DOCKER_HOST:-unix:///run/user/$(id -u)/docker.sock}

# See docker/README.md: on the LXC build host containers must use host networking.
NET=${NETWORK:-host}
rc=0

say()  { printf '%-58s %s\n' "$1" "$2"; }
ok()   { say "$1" "OK"; }
fail() { say "$1" "FAIL"; rc=1; }

have() { # image, binary
  if docker run --rm --network "$NET" "$1" sh -c "command -v $2 >/dev/null 2>&1"; then
    ok "$1: $2"
  else
    fail "$1: $2"
  fi
}

# cross-compiles a trivial C file and asserts the ELF machine type
cross() { # image, cc, expected-substring-of-`file`
  local out
  out=$(docker run --rm --network "$NET" "$1" sh -c "
        printf 'int main(void){return 0;}\n' > /tmp/t.c
        $2 -o /tmp/t /tmp/t.c 2>&1 && file /tmp/t" 2>&1)
  if printf '%s' "$out" | grep -q "$3"; then
    ok "$1: $2 -> $3"
  else
    fail "$1: $2 -> $3"
    printf '    got: %s\n' "$(printf '%s' "$out" | tail -1)"
  fi
}

echo "=== edgenos/builder9:1.8-rootless (ARM / ONL) ==="
B=edgenos/builder9:1.8-rootless
for t in arm-linux-gnueabihf-gcc arm-linux-gnueabihf-strip dtc mkimage python2 zip unzip make file; do
  have "$B" "$t"
done
cross "$B" arm-linux-gnueabihf-gcc "ARM"
# build-onie-installer-4610.sh runs ONL's mkshar/pyfit under python2
docker run --rm --network "$NET" "$B" python2 -c "print('x')" >/dev/null 2>&1 \
  && ok "$B: python2 executes" || fail "$B: python2 executes"
# the FIT builds compile a .dts -> .dtb with dtc
docker run --rm --network "$NET" "$B" sh -c \
  'printf "/dts-v1/;\n/ { model=\"t\"; };\n" > /tmp/a.dts && dtc -I dts -O dtb /tmp/a.dts -o /tmp/a.dtb' \
  >/dev/null 2>&1 && ok "$B: dtc compiles a dts" || fail "$B: dtc compiles a dts"

echo
echo "=== sdk5610build:1 (PowerPC / OpenBCM SDK) ==="
S=sdk5610build:1
for t in powerpc-linux-gnu-gcc powerpc-linux-gnu-g++ make perl file; do have "$S" "$t"; done
cross "$S" powerpc-linux-gnu-gcc "PowerPC"

echo
echo "=== edgenos-builder (PowerPC / Quagga control plane) ==="
Q=edgenos-builder
for t in powerpc-linux-gnu-gcc make gawk wget; do have "$Q" "$t"; done
cross "$Q" powerpc-linux-gnu-gcc "PowerPC"
# Quagga's configure breaks under mawk; make sure awk really is GNU awk
docker run --rm --network "$NET" "$Q" sh -c 'awk --version 2>/dev/null | head -1 | grep -qi "GNU Awk"' \
  && ok "$Q: awk is GNU awk" || fail "$Q: awk is GNU awk"
# it links -static, so the PPC static libc must be present
docker run --rm --network "$NET" "$Q" sh -c \
  'printf "int main(void){return 0;}\n" > /tmp/t.c && powerpc-linux-gnu-gcc -static -o /tmp/t /tmp/t.c' \
  >/dev/null 2>&1 && ok "$Q: static PPC link works" || fail "$Q: static PPC link works"

echo
[ $rc -eq 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $rc
