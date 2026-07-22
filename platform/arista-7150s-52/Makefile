# EdgeNOS board build: Arista DCS-7150S-52 -> edged-7150 datapath daemon.
#
# Composes the FM6000 datapath from the seams:
#   core/datapath   (only asic_ops.h — the ASIC backend seam; NOT the BMD-coupled
#                    l2/l3/vlan/packet_io, which are Broadcom-specific today)
#   asic/fm6000     (clean-room FocalPoint driver + asic_ops backend)
#   this board dir  (minimal TUN-bridged daemon main)
#
# NATIVE x86_64 build (the 7150 host is x86_64) — no cross toolchain, no vendor
# SDK to link. Buildable on any x86_64 host with kernel uapi headers (linux/vfio.h).

EDGENOS_ROOT ?= $(abspath $(CURDIR)/../..)
CORE_DIR := $(EDGENOS_ROOT)/core/datapath
ASIC_DIR := $(EDGENOS_ROOT)/asic/fm6000
PLAT_DIR := $(CURDIR)

include $(EDGENOS_ROOT)/arch/x86_64/toolchain.mk

INCLUDES := -I$(CORE_DIR) -I$(ASIC_DIR) -I$(PLAT_DIR)
VPATH    := $(ASIC_DIR):$(PLAT_DIR)

ASIC_SRCS := fm6000_hw.c fm6000_ucode.c fm6000_boot.c fpdma.c fpdma_kmod.c fpdma_vfio.c fm6000_edged.c
PLAT_SRCS := edged.c
SRCS := $(ASIC_SRCS) $(PLAT_SRCS)
OBJS := $(SRCS:.c=.o)

OUT    ?= $(EDGENOS_ROOT)/output/edged-7150
TARGET := edged-7150

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@mkdir -p $(dir $(OUT)) && cp $@ $(OUT) && echo "edged-7150 -> $(OUT)"

%.o: %.c
	$(CC) $(CFLAGS) -std=gnu11 -D_GNU_SOURCE $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
