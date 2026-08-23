# EdgeNOS arch fragment: x86_64 (AMD64). First little-endian / 64-bit / x86 target.
# The build host is x86_64, so this is a NATIVE build by default: no cross toolchain.
# (For byte-exact parity with the Buildroot base, point CROSS_COMPILE at
#  output/br-x86_64/host/bin/x86_64-buildroot-linux-gnu- — build/build-vm-image.sh does.)
CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -Wall -Wextra -O2 -g
# Static-link the datapath daemon so host/target glibc skew is a non-issue — same
# rationale as the powerpc fragment. Mgmt-plane userland can link dynamically.
LDFLAGS ?= -static -lpthread -lrt
