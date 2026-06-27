/*
 * CDK custom configuration for Linux userspace.
 * Maps CDK functions to system libc.
 * Based on OpenMDK examples/linux-user/cdk_custom_config.h
 */

#ifndef __CDK_CUSTOM_CONFIG_H__
#define __CDK_CUSTOM_CONFIG_H__

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Use system type definitions */
#define CDK_CONFIG_DEFINE_SIZE_T                0
#define CDK_CONFIG_DEFINE_UINT8_T               0
#define CDK_CONFIG_DEFINE_UINT16_T              0
#define CDK_CONFIG_DEFINE_UINT32_T              0
#define CDK_CONFIG_DEFINE_PRIu32                0
#define CDK_CONFIG_DEFINE_PRIx32                0

/* Map CDK functions to system libc */
#define CDK_ABORT                               abort
#define CDK_PRINTF                              printf
#define CDK_VPRINTF                             vprintf
#define CDK_SPRINTF                             sprintf
#define CDK_VSPRINTF                            vsprintf
#define CDK_ATOI                                atoi
#define CDK_STRNCHR                             strnchr
#define CDK_STRCPY                              strcpy
#define CDK_STRNCPY                             strncpy
#define CDK_STRLEN                              strlen
#define CDK_STRCHR                              strchr
#define CDK_STRRCHR                             strrchr
#define CDK_STRCMP                               strcmp
#define CDK_MEMCMP                              memcmp
#define CDK_MEMSET                              memset
#define CDK_MEMCPY                              memcpy
#define CDK_STRUPR                              strupr
#define CDK_TOUPPER                             toupper
#define CDK_STRCAT                              strcat

/*
 * XGSD CMC (CMIC Management Controller) selection.
 * BCM56846 iProc uses CMC 0 for the PCI host interface.
 * Without this, CDK_XGSD_CMC_OFFSET adds an uninitialized
 * offset to all CMICm register addresses, causing DMA
 * registers to be accessed at the wrong AXI address.
 */
#define CDK_XGSD_CMC                            0

#endif /* __CDK_CUSTOM_CONFIG_H__ */
