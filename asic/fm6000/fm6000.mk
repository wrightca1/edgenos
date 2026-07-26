# EdgeNOS ASIC fragment: FM6000 "Alta" (Intel/Fulcrum) — clean-room FocalPoint.
#
# Unlike asic/bcm56846 (which links Broadcom's OpenMDK static libs), the FM6000
# datapath is a clean-room reimplementation: no vendor SDK to link. Register
# access is a userspace BAR0 mmap (x86_64, no MMIO barriers); only packet DMA
# needs kernel/VFIO backing (see fpdma.h struct fpdma_backing).
#
# Provenance of every constant: notes/analysis/phase7g-fm6000-bringup-recovered.md
# and edgenos/FPDMA.md in the arista RE repo. Proprietary payloads (microcode
# .raw, SPICO blob) are NOT vendored — staged on-box, loaded by path at runtime.

FM6000_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

FM6000_SRCS := \
  $(FM6000_DIR)fm6000_hw.c \
  $(FM6000_DIR)fm6000_ucode.c \
  $(FM6000_DIR)fm6000_boot.c \
  $(FM6000_DIR)fm6000_l2.c \
  $(FM6000_DIR)fpdma.c \
  $(FM6000_DIR)fpdma_kmod.c \
  $(FM6000_DIR)fpdma_vfio.c

FM6000_CFLAGS := -I$(FM6000_DIR) -std=gnu11 -D_GNU_SOURCE
# No vendor SDK, no special endianness flags (x86_64 LE, native MMIO).
# DMA backing is VFIO (fpdma_vfio.c) — needs kernel uapi headers (linux/vfio.h).

FM6000_OBJS := $(FM6000_SRCS:.c=.o)

# Consumed by the edged link for the x86_64/fm6000 target.
ASIC_SRCS   += $(FM6000_SRCS)
ASIC_CFLAGS += $(FM6000_CFLAGS)

# Standalone bring-up/punt diagnostic (not linked into edged).
$(FM6000_DIR)fm6000_bringup: $(FM6000_SRCS) $(FM6000_DIR)fm6000_bringup.c
	$(CC) $(FM6000_CFLAGS) -Wall -Wextra $^ -o $@

# Standalone CPU-punt L2 programmer (register-only; run before fpdma_probe tx).
$(FM6000_DIR)fm6000_l2_probe: $(FM6000_DIR)fm6000_hw.c $(FM6000_DIR)fm6000_l2.c \
                              $(FM6000_DIR)fm6000_l2_probe.c
	$(CC) $(FM6000_CFLAGS) -Wall -Wextra $^ -o $@
