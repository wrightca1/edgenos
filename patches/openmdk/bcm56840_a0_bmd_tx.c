#include <bmd_config.h>
#if CDK_CONFIG_INCLUDE_BCM56840_A0 == 1

/*
 * 
 *
 * This software is governed by the Broadcom Switch APIs license.
 * This license is set out in https://raw.githubusercontent.com/Broadcom-Network-Switching-Software/OpenMDK/master/Legal/LICENSE file.
 * 
 * Copyright 2007-2020 Broadcom Inc. All rights reserved.
 */

#include <bmd/bmd.h>
#include <bmd/bmd_dma.h>

#include <bmdi/arch/xgs_dma.h>

#include <cdk/cdk_assert.h>
#include <cdk/cdk_debug.h>
#include <cdk/cdk_higig_defs.h>

#include <cdk/chip/bcm56840_a0_defs.h>

#include "bcm56840_a0_bmd.h"
#include "bcm56840_a0_internal.h"

#if BMD_CONFIG_INCLUDE_DMA == 1

static void
_dcb_init(int unit, TX_DCB_t *dcb, const bmd_pkt_t *pkt)
{
    uint32_t *sob;

    TX_DCB_CLR(*dcb); 

    if (pkt->port >= 0) {
        /* Enable stream-of-bytes module header */
        TX_DCB_HGf_SET(*dcb, 1); 

        /* Fill out stream-of-bytes module header */
        sob = TX_DCB_MODULE_HEADERf_PTR(*dcb);
        sob[0] = 0xff000000;
        sob[1] = 0x00000100; /* unicast */
        sob[2] = P2L(unit, pkt->port);
    }
}

#endif

int
bcm56840_a0_bmd_tx(int unit, const bmd_pkt_t *pkt)
{
#if BMD_CONFIG_INCLUDE_DMA == 1
    TX_DCB_t *dcb;
    dma_addr_t bdcb;
    int hdr_size, hdr_offset;
    int rv = CDK_E_NONE;

    BMD_CHECK_UNIT(unit);

    if (pkt->port >= 0) {
        if (BMD_PORT_VALID(unit, pkt->port)) {
            /* Cumulus 2.5 proved this chassis links up end-to-end. Our
             * MII_STAT-based link probe reads 0x0109 (no bit 2 / no link)
             * even though the physical link IS up — the Warpcore's MII
             * status register isn't being populated by our PHY init path,
             * so BMD_PST_LINK_UP never gets set, and bmd_tx silently
             * drops every userspace-originated frame. Bypass the gate. */
            (void)BMD_PORT_STATUS(unit, pkt->port);
        } else {
            return CDK_E_PORT;
        }
    }
    /* pkt->port < 0: VLAN-directed mode (Cumulus service VID scheme).
     * Frame already 802.1Q-tagged with a VID whose only untagged member
     * is the target front-panel port, so chip's L2 forwarding does the
     * routing and strips the tag on egress.  Skip the HiGig SOB path. */

    /* Check for valid physical bus address */
    CDK_ASSERT(pkt->baddr);

    /* Allocate DMA descriptors from DMA memory pool */
    dcb = bmd_dma_alloc_coherent(unit, 2 * sizeof(*dcb), &bdcb);
    if (dcb == NULL) {
        return CDK_E_MEMORY;
    }

    if (pkt->port < 0) {
        /* Single non-chained DCB, no HG SOB. */
        _dcb_init(unit, &dcb[0], pkt);   /* TX_DCB_CLR only since port<0 */
        TX_DCB_ADDRf_SET(dcb[0], pkt->baddr);
        TX_DCB_BYTE_COUNTf_SET(dcb[0], pkt->size);
        TX_DCB_SGf_SET(dcb[0], 0);
        TX_DCB_CHAINf_SET(dcb[0], 0);

        BMD_DMA_CACHE_FLUSH(dcb, sizeof(*dcb));
        bmd_xgs_dma_tx_start(unit, bdcb);

        if (bmd_xgs_dma_tx_poll(unit, BMD_CONFIG_DMA_MAX_POLLS) < 0) {
            rv = CDK_E_TIMEOUT;
        }
        BMD_DMA_CACHE_INVAL(dcb, sizeof(*dcb));
        bmd_dma_free_coherent(unit, 2 * sizeof(*dcb), dcb, bdcb);
        return rv;
    }

    /* Optionally strip VLAN tag (HG mode only). */
    hdr_offset = 16;
    hdr_size = 16;
    if (BMD_PORT_PROPERTIES(unit, pkt->port) & BMD_PORT_HG) {
#if BMD_CONFIG_INCLUDE_HIGIG == 1
        /* Always strip VLAN tag if HiGig packet */
        if (pkt->data[0] == CDK_HIGIG_SOF) {
            hdr_offset += CDK_HIGIG_SIZE;
            hdr_size += (CDK_HIGIG_SIZE - 4);
        } else if (pkt->data[0] == CDK_HIGIG2_SOF) {
            hdr_offset += CDK_HIGIG2_SIZE;
            hdr_size += (CDK_HIGIG2_SIZE - 4);
        }
#endif
    } else if (pkt->flags & BMD_PKT_F_UNTAGGED) {
        hdr_size = 12;
    }

    /* Set up first DMA descriptor */
    _dcb_init(unit, &dcb[0], pkt);
    TX_DCB_ADDRf_SET(dcb[0], pkt->baddr);
    TX_DCB_BYTE_COUNTf_SET(dcb[0], hdr_size);
    TX_DCB_SGf_SET(dcb[0], 1);
    TX_DCB_CHAINf_SET(dcb[0], 1);

    /* Set up second DMA descriptor */
    _dcb_init(unit, &dcb[1], pkt);
    TX_DCB_ADDRf_SET(dcb[1], pkt->baddr + hdr_offset);
    TX_DCB_BYTE_COUNTf_SET(dcb[1], pkt->size - hdr_offset);

    /* Start DMA */
    BMD_DMA_CACHE_FLUSH(dcb, 2 * sizeof(*dcb));
    bmd_xgs_dma_tx_start(unit, bdcb);

    /* Poll for DMA completion */
    if (bmd_xgs_dma_tx_poll(unit, BMD_CONFIG_DMA_MAX_POLLS) < 0) {
        rv = CDK_E_TIMEOUT;
    }
    BMD_DMA_CACHE_INVAL(dcb, 2 * sizeof(*dcb));
    bmd_xgsd_dump_tx_dcbs(unit, (uint32_t *)dcb, 2,
                         CDK_BYTES2WORDS(TX_DCB_SIZE), CDK_HIGIG2_WSIZE);

    /* Free DMA descriptor */
    bmd_dma_free_coherent(unit, 2 * sizeof(*dcb), dcb, bdcb);

    return rv;
#else
    return CDK_E_UNAVAIL;
#endif
}
#endif /* CDK_CONFIG_INCLUDE_BCM56840_A0 */
