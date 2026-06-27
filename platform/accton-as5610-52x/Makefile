# EdgeNOS board build: Accton AS5610-52X -> edged datapath daemon.
#
# Composes the datapath from the three unified seams:
#   core/datapath   (arch/ASIC-agnostic framework)
#   asic/bcm56846   (Trident+ chip glue: BDE, CDK, cumulus_replicate)
#   this board dir  (port map + LED layout)
#
# Mirrors the proven object list + flags from newnos/asic/edged/Makefile.
# REQUIRES the PowerPC cross-toolchain and the built OpenMDK libs — build inside
# the EdgeNOS cross-build container (see ../../MIGRATION.md). Not buildable on a
# host without powerpc-linux-gnu-gcc.

EDGENOS_ROOT ?= $(abspath $(CURDIR)/../..)
CORE_DIR := $(EDGENOS_ROOT)/core/datapath
ASIC_DIR := $(EDGENOS_ROOT)/asic/bcm56846
PLAT_DIR := $(CURDIR)

include $(EDGENOS_ROOT)/arch/powerpc/toolchain.mk
include $(ASIC_DIR)/sdk.mk

INCLUDES := -I$(CORE_DIR) -I$(ASIC_DIR) -I$(PLAT_DIR)
VPATH    := $(CORE_DIR):$(ASIC_DIR):$(PLAT_DIR)

# Source composition (== the proven SRCS list, now split by seam)
CORE_SRCS := edged.c packet_io.c netlink.c l2.c l3.c vlan.c datapath.c
ASIC_SRCS := bde_interface.c cumulus_replicate.c
PLAT_SRCS := portmap.c led.c
SRCS := $(CORE_SRCS) $(ASIC_SRCS) $(PLAT_SRCS)
OBJS := $(SRCS:.c=.o)

OUT    ?= $(EDGENOS_ROOT)/output/edged
TARGET := edged

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SDK_LIBS) $(LDFLAGS)
	@mkdir -p $(dir $(OUT)) && cp $@ $(OUT) && echo "edged -> $(OUT)"

%.o: %.c
	$(CC) $(CFLAGS) $(SDK_CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
