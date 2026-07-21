# EdgeNOS arch fragment: x86_64 (AMD64) — Arista 7150 (AMD RS780 / Family-10h host).
# First little-endian / 64-bit / x86 target. The build host is x86_64, so this is a
# NATIVE build: no cross toolchain.
CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -Wall -Wextra -O2 -g
# Static-link the datapath daemon (M2) so host/target glibc skew is a non-issue — same
# rationale as the powerpc fragment. Mgmt-plane userland (M0/M1) can link dynamically.
LDFLAGS ?= -static -lpthread -lrt
