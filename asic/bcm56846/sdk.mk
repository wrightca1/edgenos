# EdgeNOS ASIC fragment: BCM56846 (Trident+) — OpenMDK SDK integration.
# Mirrors the proven flags from newnos/asic/edged/Makefile. Anything linking the
# BCM56846 datapath includes this.
#
# OpenMDK is shared at the repo top level (sibling of edgenos/), not vendored here.
OPENMDK   ?= $(EDGENOS_ROOT)/../OpenMDK
# Where mdk-init builds the static libs (gitignored output). Populate via the SDK
# build step before linking edged (see MIGRATION.md).
SDK_BLDDIR ?= $(EDGENOS_ROOT)/output/sdk

SDK_CFLAGS := -I$(OPENMDK)/cdk/include -I$(OPENMDK)/bmd/include \
              -I$(OPENMDK)/phy/include -I$(OPENMDK)/libbde/include
SDK_CFLAGS += -DCDK_INCLUDE_CUSTOM_CONFIG -DUSE_SYSTEM_LIBC
SDK_CFLAGS += -DBMD_CONFIG_INCLUDE_DMA=1
SDK_CFLAGS += -DBMD_SYS_USLEEP=_usleep -DPHY_SYS_USLEEP=_usleep
# PPC P2020 big-endian PIO (see arch/powerpc quirks); CDK byte-swaps natively.
SDK_CFLAGS += -DSYS_BE_PIO=1 -DSYS_BE_PACKET=1 -DSYS_BE_OTHER=1
# DMA alloc/free implemented in asic/bcm56846/bde_interface.c
SDK_CFLAGS += '-DBMD_SYS_DMA_ALLOC_COHERENT=_bde_dma_alloc'
SDK_CFLAGS += '-DBMD_SYS_DMA_FREE_COHERENT=_bde_dma_free'
# DMA pool is uncached/coherent — no cache ops needed.
SDK_CFLAGS += '-DBMD_SYS_DMA_CACHE_FLUSH(addr,len)='
SDK_CFLAGS += '-DBMD_SYS_DMA_CACHE_INVAL(addr,len)='

# OpenMDK static libraries (link order matters).
SDK_LIBS := \
  $(SDK_BLDDIR)/bmd/libbmdshell.a   $(SDK_BLDDIR)/bmd/libbmdapi.a \
  $(SDK_BLDDIR)/bmd/libbmdpkgsrc.a  $(SDK_BLDDIR)/bmd/libbmdshared.a \
  $(SDK_BLDDIR)/libbde/libbdeshared.a \
  $(SDK_BLDDIR)/phy/libphypkgsrc.a  $(SDK_BLDDIR)/phy/libphygeneric.a \
  $(SDK_BLDDIR)/phy/libphyutil.a    $(SDK_BLDDIR)/phy/libphysym.a \
  $(SDK_BLDDIR)/cdk/libcdkshell.a   $(SDK_BLDDIR)/cdk/libcdkmain.a \
  $(SDK_BLDDIR)/cdk/libcdkpkgsrc.a  $(SDK_BLDDIR)/cdk/libcdkshared.a \
  $(SDK_BLDDIR)/cdk/libcdksym.a     $(SDK_BLDDIR)/cdk/libcdklibc.a \
  $(SDK_BLDDIR)/cdk/libcdkdsym.a
