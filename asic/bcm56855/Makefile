# Build the SDK proof of concept.
#
# The OpenBCM tree is an EXTERNAL dependency and is never vendored into this
# repository -- its licence permits distributing binaries of the Code inside our
# own application but "in no event source code versions of the Code"
# (third_party/openbcm/NOTICE.md). Point OPENBCM_SDK at your own copy.
#
#   make OPENBCM_SDK=/path/to/sdk-6.5.24
#
# Use 6.5.24 or older: 6.5.27 lists BCM_56860_A0 in DEPRECATED_CHIP_NAMES and
# compiles our chip out entirely.
#
# Build the SDK libraries first:
#   cd $(OPENBCM_SDK)/systems/linux/user/x86-64-fc28
#   PATH=<gcc-wrapper-appending--Wno-error>:$$PATH BCM_56860_A0=1 make -j8 bcm

# INCLUDE_RCPU IS MANDATORY and is not cosmetic. soc_cm_device_vectors_t has
# a conditional `rcpu_trans_ptr_t *rcpu_tp` (cmtypes.h:75) sitting BEFORE
# config_var_get, so the flag changes the struct from 224 to 232 bytes and
# shifts every field after it by 8. The SDK libraries here were built with it
# -- soc_cm_get_rcpu_cfg is defined in libsoccommon.a and lives under
# #ifdef INCLUDE_RCPU in cm.c -- so an application compiled WITHOUT it writes
# every vector one slot early:
#
#   our .config_var_get   (24) -> library's rcpu_tp
#   our .interrupt_connect(32) -> library's config_var_get
#
# The SDK then called shim_interrupt_connect() expecting config_var_get. That
# returns int -1, which as a char* is 0xffffffff, and _shr_ctoi (util.c:823)
# dereferenced it immediately -- the SIGSEGV at 0xffffffff that cost six boots.
OPENBCM_SDK ?= /home/smiley/edgecore/OpenBCM/sdk-6.5.24
PLATFORM    ?= x86-64-fc28
LIBDIR      := $(OPENBCM_SDK)/build/unix-user/$(PLATFORM)

# The full define set is taken from the SDK's own build command line, not
# hand-picked. Hand-picking is what produced the INCLUDE_RCPU bug (8-byte
# struct shift) and then a NULL deref from soc_mem_t values that did not match
# the library's, because BCM_ALL_CHIPS was missing.
include sdk-defines.mk

CFLAGS  := -O2 -g -Wall $(SDK_DEFINES) \
           -DPHYMOD_SUPPORT -DPORTMOD_SUPPORT \
           -I$(OPENBCM_SDK)/include -I$(OPENBCM_SDK) \
           -I$(OPENBCM_SDK)/libs/phymod/include \
           -I$(OPENBCM_SDK)/systems

# liblubde.a is the SDK's OWN user BDE -- the one that talks to
# /dev/linux-user-bde and that this whole exercise replaces. It also defines
# sal_dma_alloc/sal_dma_free, so linking it collides with our shim. Drop it.
SDK_LIBS := $(filter-out $(LIBDIR)/liblubde.a,$(wildcard $(LIBDIR)/*.a))

# The chip drivers register through tables the linker cannot see are needed, so
# they must be pulled in whole; everything else links on demand. Listing them
# explicitly rather than --whole-archive'ing all 172 archives keeps the binary
# from dragging in the diag shell and every other chip family.
WHOLE := $(LIBDIR)/libsoc_esw.a $(LIBDIR)/libsoc_trident2.a \
         $(LIBDIR)/libsoc_trident2p.a $(LIBDIR)/libtrident2.a \
         $(LIBDIR)/libtrident2plus.a $(LIBDIR)/libsoc_mcm.a

LIBS := -Wl,--whole-archive $(WHOLE) -Wl,--no-whole-archive \
        -Wl,--start-group $(SDK_LIBS) -Wl,--end-group \
        -lpthread -lrt -lm -ldl

# EdgeNOS's initrd is busybox with no glibc to speak of, so the binary that
# actually runs on the switch has to be static -- same reason scdreset is.
# STATIC=1 is the one that gets shipped; the dynamic build is for checking the
# link resolves without waiting on a 40 MB copy.
ifeq ($(STATIC),1)
LDMODE := -static
# Do NOT --whole-archive libpthread here. In a modern glibc, libpthread.a is
# mostly stubs that also live in libc.a, so pulling it in whole produces
# "multiple definition of __nptl_setxid_error" and friends. Plain -lpthread
# (already in LIBS) resolves what the SAL actually calls.
endif

sdkpoc: sdkpoc.c bde_shim.c
	$(CC) $(CFLAGS) $(LDMODE) -o $@ sdkpoc.c bde_shim.c $(LIBS)

clean:
	rm -f sdkpoc

.PHONY: clean
