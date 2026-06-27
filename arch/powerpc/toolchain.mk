# EdgeNOS arch fragment: PowerPC (Freescale e500v2 / P2020).
# Toolchain + link policy shared by anything built for this CPU.
CROSS_COMPILE ?= powerpc-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -Wall -Wextra -O2 -g
# Static link: the cross-compiler's glibc (Ubuntu 2.35) and the target rootfs glibc
# (Buildroot, varies) differ; edged needs symbols that may be absent in the target.
LDFLAGS ?= -static -lpthread -lrt
