# switchd Makefile
# Builds the EdgeNOS switch daemon with OpenMDK integration

CROSS_COMPILE ?= powerpc-linux-gnu-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -O2 -g
# Static linking avoids glibc version mismatch between cross-compiler
# (Ubuntu glibc 2.35) and rootfs (Buildroot glibc, version varies).
# switchd needs GLIBC_2.34 symbols (__libc_start_main, __stack_chk_fail)
# which may not exist in Buildroot's glibc.
LDFLAGS = -static -lpthread -lrt

TOPDIR ?= $(shell cd ../.. && pwd)
OPENMDK ?= $(TOPDIR)/asic/openmdk

# OpenMDK SDK build directory (where libraries were built by mdk-init's make)
SDK_BLDDIR ?= $(TOPDIR)/output/sdk

# OpenMDK include paths
SDK_CFLAGS = -I$(OPENMDK)/cdk/include \
             -I$(OPENMDK)/bmd/include \
             -I$(OPENMDK)/phy/include \
             -I$(OPENMDK)/libbde/include \
             -I$(CURDIR)

# CDK custom config and system libc
SDK_CFLAGS += -DCDK_INCLUDE_CUSTOM_CONFIG -DUSE_SYSTEM_LIBC
SDK_CFLAGS += -DBMD_CONFIG_INCLUDE_DMA=1
SDK_CFLAGS += -DBMD_SYS_USLEEP=_usleep -DPHY_SYS_USLEEP=_usleep
SDK_CFLAGS += -DSYS_BE_PIO=1 -DSYS_BE_PACKET=1 -DSYS_BE_OTHER=1
# DMA allocation functions (implemented in bde_interface.c)
SDK_CFLAGS += '-DBMD_SYS_DMA_ALLOC_COHERENT=_bde_dma_alloc'
SDK_CFLAGS += '-DBMD_SYS_DMA_FREE_COHERENT=_bde_dma_free'
# DMA cache control (not needed - our DMA pool is uncached/coherent)
SDK_CFLAGS += '-DBMD_SYS_DMA_CACHE_FLUSH(addr,len)='
SDK_CFLAGS += '-DBMD_SYS_DMA_CACHE_INVAL(addr,len)='

# OpenMDK static libraries (link order matters)
SDK_LIBS = $(SDK_BLDDIR)/bmd/libbmdshell.a \
           $(SDK_BLDDIR)/bmd/libbmdapi.a \
           $(SDK_BLDDIR)/bmd/libbmdpkgsrc.a \
           $(SDK_BLDDIR)/bmd/libbmdshared.a \
           $(SDK_BLDDIR)/libbde/libbdeshared.a \
           $(SDK_BLDDIR)/phy/libphypkgsrc.a \
           $(SDK_BLDDIR)/phy/libphygeneric.a \
           $(SDK_BLDDIR)/phy/libphyutil.a \
           $(SDK_BLDDIR)/phy/libphysym.a \
           $(SDK_BLDDIR)/cdk/libcdkshell.a \
           $(SDK_BLDDIR)/cdk/libcdkmain.a \
           $(SDK_BLDDIR)/cdk/libcdkpkgsrc.a \
           $(SDK_BLDDIR)/cdk/libcdkshared.a \
           $(SDK_BLDDIR)/cdk/libcdksym.a \
           $(SDK_BLDDIR)/cdk/libcdklibc.a \
           $(SDK_BLDDIR)/cdk/libcdkdsym.a

SRCS = switchd.c \
       portmap.c \
       packet_io.c \
       netlink.c \
       l2.c \
       l3.c \
       vlan.c \
       bde_interface.c

OBJS = $(SRCS:.c=.o)
TARGET = switchd

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SDK_LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $(SDK_CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

# Dependencies
switchd.o: switchd.h portmap.h packet_io.h netlink.h l2.h l3.h vlan.h
portmap.o: switchd.h portmap.h
packet_io.o: switchd.h packet_io.h portmap.h
netlink.o: switchd.h netlink.h portmap.h l2.h l3.h
l2.o: switchd.h l2.h portmap.h
l3.o: switchd.h l3.h portmap.h
vlan.o: switchd.h vlan.h
bde_interface.o: switchd.h cdk_custom_config.h
