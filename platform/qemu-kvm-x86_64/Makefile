# EdgeNOS board build: QEMU/KVM x86_64 virtual switch -> edged-vswitch datapath daemon.
#
# Composes the software datapath from the seams:
#   core/datapath   (only asic_ops.h — the ASIC backend seam)
#   asic/vswitch    (L2 learning switch over AF_PACKET + asic_ops backend)
#   this board dir  (the TAP-bridged daemon main — same loop as the Arista 7150's)
#
# NATIVE x86_64 build. For a binary that matches the Buildroot base byte-for-byte
# (and is static anyway), pass CROSS_COMPILE=output/br-x86_64/host/bin/x86_64-buildroot-linux-gnu-

EDGENOS_ROOT ?= $(abspath $(CURDIR)/../..)
CORE_DIR := $(EDGENOS_ROOT)/core/datapath
ASIC_DIR := $(EDGENOS_ROOT)/asic/vswitch
PLAT_DIR := $(CURDIR)

include $(EDGENOS_ROOT)/arch/x86_64/toolchain.mk

INCLUDES := -I$(CORE_DIR) -I$(ASIC_DIR) -I$(PLAT_DIR)
VPATH    := $(ASIC_DIR):$(PLAT_DIR)

ASIC_SRCS := vswitch.c
PLAT_SRCS := edged.c
SRCS := $(ASIC_SRCS) $(PLAT_SRCS)
OBJS := $(SRCS:.c=.o)

OUT    ?= $(EDGENOS_ROOT)/output/edged-vswitch
TARGET := edged-vswitch

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@mkdir -p $(dir $(OUT)) && cp $@ $(OUT) && echo "edged-vswitch -> $(OUT)"

%.o: %.c
	$(CC) $(CFLAGS) -std=gnu11 -D_GNU_SOURCE $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
