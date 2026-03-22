#!/bin/bash
# setup-toolchain.sh - Verify and install cross-compilation toolchain
set -e

REQUIRED_PKGS=(
    gcc-powerpc-linux-gnu
    binutils-powerpc-linux-gnu
    u-boot-tools
    device-tree-compiler
    squashfs-tools
    qemu-user-static
    bc
    flex
    bison
    openssl-devel
    elfutils-libelf-devel
    wget
    cpio
    rsync
    perl
    python3
)

echo "Checking required packages..."

MISSING=()
for pkg in "${REQUIRED_PKGS[@]}"; do
    if ! rpm -q "$pkg" &>/dev/null; then
        MISSING+=("$pkg")
    fi
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo "Missing packages: ${MISSING[*]}"
    echo "Installing..."
    sudo dnf install -y "${MISSING[@]}"
else
    echo "All required packages installed."
fi

# Verify cross-compiler works
echo -n "Cross-compiler: "
powerpc-linux-gnu-gcc --version | head -1

# Verify we can produce PPC binaries
TMPDIR=$(mktemp -d)
cat > "$TMPDIR/test.c" << 'EOF'
#include <stdio.h>
int main() { printf("EdgeNOS PPC test OK\n"); return 0; }
EOF

powerpc-linux-gnu-gcc -o "$TMPDIR/test" "$TMPDIR/test.c" -static
file "$TMPDIR/test" | grep -q "PowerPC" && echo "Cross-compile test: OK" || {
    echo "ERROR: Cross-compile test failed"
    exit 1
}

# Test with qemu-user if available
if command -v qemu-ppc-static &>/dev/null; then
    OUTPUT=$(qemu-ppc-static "$TMPDIR/test" 2>/dev/null || true)
    if [ "$OUTPUT" = "EdgeNOS PPC test OK" ]; then
        echo "QEMU user-mode test: OK"
    else
        echo "QEMU user-mode test: SKIPPED (may need binfmt setup)"
    fi
fi

rm -rf "$TMPDIR"
echo "Toolchain ready."
