# Generated from the OpenBCM build's own command line -- do NOT hand-edit.
#
# The application MUST be compiled with the same defines as the SDK
# libraries. Headers are shared, so a define that is set for the library
# and not for us silently changes struct layouts and enum values:
#   INCLUDE_RCPU  shifted soc_cm_device_vectors_t by 8 bytes and made the
#                 SDK call interrupt_connect where it wanted config_var_get
#   BCM_ALL_CHIPS decides which chips' memories are in soc_mem_t, so every
#                 memory ID and NUM_SOC_MEM differ without it
#
# __KERNEL__ and USE_LINUX_BDE_MMAP are REMOVED: the line this was captured
# from builds the kernel BDE modules, and __KERNEL__ makes sal/core/libc.h
# pull in linux/ctype.h, which does not exist in a user-space build.
#
# Regenerate: build the SDK and capture the line containing BCM_ALL_CHIPS,
# then extract the -D flags. See docs/SDK-LOCAL-PATCH.md.
SDK_DEFINES := \
    -DPTRS_ARE_64BITS \
    -DLONGS_ARE_64BITS \
    -DPHYS_ADDRS_ARE_64BITS \
    -DNDEBUG \
    -DCONFIG_AS_CFI=1 \
    -DCONFIG_AS_CFI_SIGNAL_FRAME=1 \
    -DVENDOR_CALHOUN \
    -DVENDOR_GAMMA \
    -DVENDOR_BROADCOM \
    -DSAL_SPL_LOCK_ON_IRQ \
    -DSYS_BE_PIO=0 \
    -DSYS_BE_PACKET=0 \
    -DSYS_BE_OTHER=0 \
    -DLE_HOST=1 \
    -DBCM_PLATFORM_STRING=\"X86\" \
    -DSAL_BDE_DMA_MEM_DEFAULT=32 \
    -DNO_FILEIO \
    -DNO_CTRL_C \
    -DNO_MEMTUNE \
    -DLINUX \
    -DBCM_ALL_CHIPS \
    -DINCLUDE_BFD \
    -DINCLUDE_CES \
    -DINCLUDE_CHASSIS \
    -DINCLUDE_CUSTOMER \
    -DINCLUDE_I2C \
    -DINCLUDE_L3 \
    -DINCLUDE_MEM_SCAN \
    -DINCLUDE_PSTATS \
    -DINCLUDE_PTP \
    -DINCLUDE_RCPU \
    -DINCLUDE_TCB \
    -DINCLUDE_TEST \
    -DBCM_RPC_SUPPORT \
    -DBCM_ESW_SUPPORT \
    -DBCM_TOMAHAWK3_SUPPORT \
    -DINCLUDE_LIB_CPUDB \
    -DINCLUDE_LIB_CPUTRANS \
    -DINCLUDE_LIB_DISCOVER \
    -DINCLUDE_LIB_STKTASK \
    -DDISCOVER_APP_DATA_BOARDID \
    -DBCM_API_VERBOSE_LOGGING=0 \
    -DINCLUDE_PHY_522X \
    -DINCLUDE_PHY_54XX \
    -DINCLUDE_PHY_5464 \
    -DINCLUDE_PHY_5421S \
    -DINCLUDE_PHY_5482 \
    -DINCLUDE_PHY_54616 \
    -DINCLUDE_PHY_54680 \
    -DINCLUDE_PHY_54680E \
    -DINCLUDE_PHY_52681E \
    -DINCLUDE_PHY_54880E \
    -DINCLUDE_PHY_54682 \
    -DINCLUDE_PHY_54684 \
    -DINCLUDE_PHY_54640 \
    -DINCLUDE_PHY_54640E \
    -DINCLUDE_PHY_54880 \
    -DINCLUDE_PHY_SERDES \
    -DINCLUDE_PHY_SIMUL \
    -DINCLUDE_PHY_8703 \
    -DINCLUDE_PHY_8705 \
    -DINCLUDE_PHY_8706 \
    -DINCLUDE_PHY_8072 \
    -DINCLUDE_PHY_8040 \
    -DINCLUDE_PHY_8481 \
    -DINCLUDE_PHY_8750 \
    -DINCLUDE_PHY_8729 \
    -DINCLUDE_PHY_84740 \
    -DINCLUDE_PHY_84756 \
    -DINCLUDE_PHY_54380 \
    -DINCLUDE_PHY_542XX \
    -DINCLUDE_PHY_84334 \
    -DINCLUDE_PHY_84728 \
    -DINCLUDE_PHY_84749 \
    -DINCLUDE_PHY_84328 \
    -DINCLUDE_PHY_84793 \
    -DINCLUDE_PHY_82328 \
    -DINCLUDE_PHY_82381 \
    -DINCLUDE_PHY_82780 \
    -DINCLUDE_PHY_82764 \
    -DINCLUDE_PHY_EGPHY28 \
    -DINCLUDE_PHY_82864 \
    -DINCLUDE_PHY_82109 \
    -DINCLUDE_PHY_EGPHY16 \
    -DINCLUDE_PHY_8806X \
    -DINCLUDE_LONGREACH \
    -DPHYMOD_SUPPORT \
    -DPHYMOD_TIER1_SUPPORT \
    -DPHYMOD_INCLUDE_CUSTOM_CONFIG \
    -DPHYMOD_DIAG \
    -DCPRIMOD_DIAG \
    -DCPRIMOD_SUPPORT \
    -DCPRI_DIAG_SUPPORT \
    -DCPRIMOD_CPRI_FALCON_SUPPORT \
    -DPORTMOD_DIAG \
    -DPORTMOD_SUPPORT \
    -DCANCUN_SUPPORT \
    -DSW_AUTONEG_SUPPORT \
    -DPCIEPHY_SUPPORT \
    -DPCIEPHY_DIAG_SUPPORT \
    -DBROADCOM_DEBUG
