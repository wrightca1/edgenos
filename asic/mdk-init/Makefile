#
# mdk-init Makefile - BCM56846 ASIC initialization tool
#
# Based on OpenMDK examples/linux-user/Makefile.
# Builds CDK/BMD/PHY/LIBBDE static libraries and links mdk-init.
#
# Usage:
#   make CROSS_COMPILE=powerpc-linux-gnu- MDK=../openmdk BLDDIR=/tmp/build
#

ifndef MDK
MDK = $(CURDIR)/../openmdk
endif

ifndef CDK
CDK = $(MDK)/cdk
export CDK
endif

ifndef BMD
BMD = $(MDK)/bmd
export BMD
endif

ifndef PHY
PHY = $(MDK)/phy
export PHY
endif

ifndef LIBBDE
LIBBDE = $(MDK)/libbde
export LIBBDE
endif

################################################################################
# Build tools
################################################################################

COMPILER = gcc
LINKER   = ld
ARCHIVE  = ar

export CC = $(CROSS_COMPILE)$(COMPILER)
export LD = $(CROSS_COMPILE)$(LINKER)
export AR = $(CROSS_COMPILE)$(ARCHIVE)

CFLAGS += -fPIC

MAKE = make
ECHO = echo

ifndef BLDDIR
BLDDIR = $(CURDIR)/build
endif

ifndef DSTDIR
DSTDIR = $(BLDDIR)
endif

################################################################################
# Compiler and linker options
################################################################################

# Include paths
CPPFLAGS += -I$(CDK)/include -I$(BMD)/include -I$(PHY)/include -I$(CURDIR) -I$(LIBBDE)/include

# Use CDK custom config file and system C library
CPPFLAGS += -DCDK_INCLUDE_CUSTOM_CONFIG -DUSE_SYSTEM_LIBC
CPPFLAGS += -DBMD_CONFIG_INCLUDE_DMA=1
CPPFLAGS += -DBMD_SYS_USLEEP=_usleep -DPHY_SYS_USLEEP=_usleep
# DMA allocation functions (implemented in switchd's bde_interface.c at link time)
CPPFLAGS += '-DBMD_SYS_DMA_ALLOC_COHERENT=_bde_dma_alloc'
CPPFLAGS += '-DBMD_SYS_DMA_FREE_COHERENT=_bde_dma_free'
CPPFLAGS += '-DBMD_SYS_DMA_CACHE_FLUSH(addr,len)='
CPPFLAGS += '-DBMD_SYS_DMA_CACHE_INVAL(addr,len)='

# P2020 big-endian PCI
CPPFLAGS += -DSYS_BE_PIO=1 -DSYS_BE_PACKET=1 -DSYS_BE_OTHER=1

CFLAGS += -Wall -O2

################################################################################
# Configure CDK/BMD/PHY/LIBBDE library builds
################################################################################

export CDK_BLDDIR   = $(BLDDIR)/cdk
export CDK_CFLAGS   = $(CFLAGS)
export CDK_CPPFLAGS = $(CPPFLAGS)

export BMD_BLDDIR   = $(BLDDIR)/bmd
export BMD_CFLAGS   = $(CFLAGS)
export BMD_CPPFLAGS = $(CPPFLAGS)

export PHY_BLDDIR   = $(BLDDIR)/phy
export PHY_CFLAGS   = $(CFLAGS)
export PHY_CPPFLAGS = $(CPPFLAGS)

export LIBBDE_BLDDIR   = $(BLDDIR)/libbde
export LIBBDE_CFLAGS   = $(CFLAGS)
export LIBBDE_CPPFLAGS = $(CPPFLAGS)

# Import library lists from each component
include $(CDK)/targets.mk
CDK_BLIBS = $(addprefix $(CDK_BLDDIR)/,$(CDK_LIBS))

include $(BMD)/targets.mk
BMD_BLIBS = $(addprefix $(BMD_BLDDIR)/,$(BMD_LIBS))

include $(PHY)/targets.mk
PHY_BLIBS = $(addprefix $(PHY_BLDDIR)/,$(PHY_LIBS))

include $(LIBBDE)/targets.mk
LIBBDE_BLIBS = $(addprefix $(LIBBDE_BLDDIR)/,$(LIBBDE_LIBS))

################################################################################
# Application
################################################################################

OBJS = mdk-init.o
BOBJS = $(addprefix $(BLDDIR)/,$(OBJS))
APPL = $(DSTDIR)/mdk-init

################################################################################
# Make rules
################################################################################

.PHONY: all cdk bmd phy libbde clean cleanall

all: cdk phy bmd libbde $(APPL)

cdk:
	$(MAKE) -C $(CDK)

bmd:
	$(MAKE) -C $(BMD)

phy:
	$(MAKE) -C $(PHY)

libbde:
	$(MAKE) -C $(LIBBDE)

$(APPL): cdk bmd phy libbde $(BOBJS) $(LIBBDE_BLIBS) $(PHY_BLIBS) $(BMD_BLIBS) $(CDK_BLIBS)
	$(CC) $(BOBJS) $(BMD_BLIBS) $(LIBBDE_BLIBS) $(PHY_BLIBS) $(CDK_BLIBS) $(LDFLAGS) -o $@

$(BLDDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	$(MAKE) -C $(CDK) clean
	$(MAKE) -C $(BMD) clean
	$(MAKE) -C $(PHY) clean
	$(MAKE) -C $(LIBBDE) clean
	rm -f $(APPL) $(BOBJS)

cleanall: clean
	$(MAKE) -C $(CDK) cleanpkgs
	$(MAKE) -C $(BMD) cleanpkgs
	$(MAKE) -C $(PHY) cleanpkgs
	$(MAKE) -C $(LIBBDE) cleanpkgs
