#!/bin/bash
# mkkernel.sh -- build the EdgeNOS kernel for the Arista DCS-7050TX-64.
#
# Builds OUT OF TREE (make O=...) into build/kernel, so the shared
# ~/kbuild/linux-6.12 source tree -- which the 7150 work also uses and has a
# working bzImage in -- is never modified. The source is mounted READ-ONLY so
# that is enforced rather than hoped for.
#
# The build runs inside the edgenos-kbuild:1 container, not natively. This host
# has no flex/bison/bc/libssl-dev, and the project's standing rule is to build
# in the container anyway: a native link is not evidence the artifact is right.
# --network=host is mandatory under this host's rootless Docker.
#
# Config = x86_64_defconfig + image/kernel/config-7050tx64, merged with the
# kernel's own scripts/kconfig/merge_config.sh so a typo in the fragment is a
# loud error rather than a silently dropped option.
#
# The result is verified against the fragment afterwards: every option we asked
# for must actually be set in the generated .config. merge_config.sh warns about
# overridden values but still exits 0, so the check is done here explicitly.
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# A PRISTINE tree. Not ~/kbuild/linux-6.12 -- that one holds the 7150's in-tree
# build, and `make O=` refuses to run against a tree that is not clean ("The
# source tree is not clean, please run 'make mrproper'"). mrproper there would
# destroy the other project's work, so we keep a second, never-built checkout.
SRC="${EDGENOS_KSRC:-/home/smiley/kbuild/linux-6.12-vanilla}"
FRAG="${EDGENOS_KFRAG:-$REPO/image/kernel/config-7050tx64}"
OBJ="${EDGENOS_KOBJ:-$REPO/build/kernel}"
JOBS="${JOBS:-$(nproc)}"
IMAGE="${EDGENOS_KBUILD_IMAGE:-edgenos-kbuild:1}"

die() { echo "mkkernel: FATAL: $*" >&2; exit 1; }
say() { echo "mkkernel: $*"; }

[ -f "$SRC/Makefile" ] || die "no kernel source at $SRC"
[ -f "$FRAG" ]         || die "no config fragment at $FRAG"
[ -x "$SRC/scripts/kconfig/merge_config.sh" ] || die "merge_config.sh missing"
command -v docker >/dev/null || die "docker not available"
docker image inspect "$IMAGE" >/dev/null 2>&1 || die "no container image $IMAGE"

mkdir -p "$OBJ" || die "cannot create $OBJ"

# Run a command inside the builder. Source read-only, objdir and fragment
# writable/readable as needed.
kb() {
    docker run --rm --network=host \
        -v "$SRC":/src:ro \
        -v "$OBJ":/obj \
        -v "$(dirname "$FRAG")":/frag:ro \
        -w /src "$IMAGE" bash -c "$1"
}

say "source   $SRC (read-only mount)"
say "fragment $FRAG"
say "objdir   $OBJ"
say "builder  $IMAGE"
say "jobs     $JOBS"

# --- config ----------------------------------------------------------------
say "generating x86_64_defconfig"
kb "make -s O=/obj x86_64_defconfig" >/dev/null || die "defconfig failed"

say "merging fragment"
kb "cd /obj && /src/scripts/kconfig/merge_config.sh -m .config /frag/$(basename "$FRAG")" \
    >/dev/null || die "merge_config failed"
kb "make -s O=/obj olddefconfig" >/dev/null || die "olddefconfig failed"

# --- verify the fragment actually took -------------------------------------
# merge_config warns and continues when an option gets overridden by a
# dependency, so check every line ourselves. This is the difference between
# "we asked for UIO" and "the kernel has UIO".
say "verifying every fragment option landed in .config"
bad=0
while IFS= read -r line; do
    case "$line" in
        \#*is\ not\ set)
            opt=$(echo "$line" | sed 's/^# \(CONFIG_[A-Z0-9_]*\) is not set$/\1/')
            if grep -q "^${opt}=" "$OBJ/.config"; then
                echo "  ** $opt should be UNSET but is $(grep "^${opt}=" "$OBJ/.config")"
                bad=$((bad+1))
            fi
            ;;
        CONFIG_*=*)
            opt=${line%%=*}
            if ! grep -qx -- "$line" "$OBJ/.config"; then
                got=$(grep "^${opt}=" "$OBJ/.config" || echo "<unset>")
                echo "  ** wanted '$line' but got '$got'"
                bad=$((bad+1))
            fi
            ;;
    esac
done < <(grep -E '^(CONFIG_|# CONFIG_.* is not set)' "$FRAG")
[ "$bad" -eq 0 ] || die "$bad config option(s) did not take"
say "config verified"

if [ "${1:-}" = "--config-only" ]; then
    say "--config-only: stopping before the build"
    say "  .config at $OBJ/.config"
    exit 0
fi

# --- build ------------------------------------------------------------------
say "building bzImage (this takes a while)"
kb "make O=/obj -j$JOBS bzImage" || die "bzImage build failed"

BZ="$OBJ/arch/x86/boot/bzImage"
[ -s "$BZ" ] || die "no bzImage produced"

say "building modules"
kb "make O=/obj -j$JOBS modules" || die "modules build failed"
kb "make -s O=/obj INSTALL_MOD_PATH=/obj/modroot modules_install" >/dev/null \
    || die "modules_install failed"

echo
say "built $BZ"
say "  size    $(stat -c%s "$BZ") bytes"
say "  md5     $(md5sum "$BZ" | cut -d' ' -f1)"
say "  release $(cat "$OBJ/include/config/kernel.release" 2>/dev/null)"
say "  modules $(find "$OBJ/modroot" -name '*.ko' 2>/dev/null | wc -l)"
echo
say "use it with:  EDGENOS_KERNEL=$BZ tools/mkswi.sh"
