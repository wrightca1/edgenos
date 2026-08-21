# EdgeNOS arch fragment: ARM hard-float (Broadcom iProc, Cortex-A9 / ARMv7-A).
# Toolchain + link policy shared by anything built for this CPU.
# Declared in switchdb/arch/armhf.yml; this is the code that acts on it.
CROSS_COMPILE ?= arm-linux-gnueabihf-
CC      := $(CROSS_COMPILE)gcc
# -mcpu/-mfpu/-mfloat-abi mirror switchdb/arch/armhf.yml's cflags. The
# arm-linux-gnueabihf toolchain already defaults to hard-float VFP; stating them
# keeps the ABI explicit and independent of the distro toolchain's defaults.
CFLAGS  ?= -Wall -Wextra -O2 -g -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard
# Same rationale as arch/powerpc: the cross-compiler's glibc is newer than the
# target rootfs', so anything linked here is linked static.
#
# NOTE: bcmd is NOT linked with these flags -- it is built and linked by the
# OpenBCM SDK's own makefile (see build/build-bcmd.sh), which only takes
# CROSS_COMPILE from here. LDFLAGS/CFLAGS apply to future armhf components that
# build against this fragment the way edged does on PowerPC.
LDFLAGS ?= -static -lpthread -lrt

# Lets non-make consumers single-source a value instead of hardcoding it:
#   CROSS_COMPILE=$(make -sf arch/armhf/toolchain.mk print-CROSS_COMPILE)
# A pattern rule never becomes the default goal, so including this fragment from
# a board Makefile cannot hijack its `all` target.
print-%:
	@echo '$($*)'
