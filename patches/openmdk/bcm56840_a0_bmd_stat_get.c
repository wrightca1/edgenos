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

#include <cdk/cdk_device.h>
#include <cdk/cdk_error.h>
#include <cdk/chip/bcm56840_a0_defs.h>

#include "bcm56840_a0_bmd.h"
#include "bcm56840_a0_internal.h"

int
bcm56840_a0_bmd_stat_get(int unit, int port, bmd_stat_t stat, bmd_counter_t *counter)
{
    int ioerr = 0;
    int lport;
    RRPKTr_t rrpkt;
    RDBGC0r_t rdbgc0;
    RDBGC3r_t rdbgc3;
    RDBGC4r_t rdbgc4;
    RDBGC5r_t rdbgc5;
    RDBGC6r_t rdbgc6;
    RIPC4r_t  ripc4;
    TPKTr_t tpkt;
    TBYTr_t tbyt;
    TFCSr_t tfcs;
    TFRGr_t tfrg;
    TOVRr_t tovr;
    TUFLr_t tufl;
    TERRr_t terr;
    RPKTr_t rpkt;
    RBYTr_t rbyt;
    RFCSr_t rfcs;
    RJBRr_t rjbr;
    ROVRr_t rovr;
    RFRGr_t rfrg;

    BMD_CHECK_UNIT(unit);
    BMD_CHECK_PORT(unit, port);

    CDK_MEMSET(counter, 0, sizeof(*counter));

    /* Only support lower 32-bits of counters */
    if (port != CMIC_PORT) {
        switch (stat) {
        case bmdStatTxPackets:
            ioerr += READ_TPKTr(unit, port, &tpkt);
            counter->v[0] += TPKTr_GET(tpkt, 0);
            break;
        case bmdStatTxBytes:
            ioerr += READ_TBYTr(unit, port, &tbyt);
            counter->v[0] += TBYTr_GET(tbyt, 0);
            break;
        case bmdStatTxErrors:
            ioerr += READ_TFRGr(unit, port, &tfrg);
            counter->v[0] += TFRGr_GET(tfrg, 0);
            ioerr += READ_TFCSr(unit, port, &tfcs);
            counter->v[0] += TFCSr_GET(tfcs, 0);
            ioerr += READ_TOVRr(unit, port, &tovr);
            counter->v[0] += TOVRr_GET(tovr, 0);
            ioerr += READ_TUFLr(unit, port, &tufl);
            counter->v[0] += TUFLr_GET(tufl, 0);
            ioerr += READ_TERRr(unit, port, &terr);
            counter->v[0] += TERRr_GET(terr, 0);
            break;
        case bmdStatRxPackets:
            ioerr += READ_RPKTr(unit, port, &rpkt);
            counter->v[0] += RPKTr_GET(rpkt, 0);
            break;
        case bmdStatRxBytes:
            ioerr += READ_RBYTr(unit, port, &rbyt);
            counter->v[0] += RBYTr_GET(rbyt, 0);
            break;
        case bmdStatRxErrors:
            ioerr += READ_RFCSr(unit, port, &rfcs);
            counter->v[0] += RFCSr_GET(rfcs, 0);
            ioerr += READ_RJBRr(unit, port, &rjbr);
            counter->v[0] += RJBRr_GET(rjbr, 0);
            ioerr += READ_ROVRr(unit, port, &rovr);
            counter->v[0] += ROVRr_GET(rovr, 0);
            ioerr += READ_RFRGr(unit, port, &rfrg);
            counter->v[0] += RFRGr_GET(rfrg, 0);
            ioerr += READ_RRPKTr(unit, port, &rrpkt);
            counter->v[0] += RRPKTr_GET(rrpkt, 0);
            break;
        default:
            break;
        }
    }

    lport = P2L(unit, port);

    /* Non-MAC counters */
    switch (stat) {
    case bmdStatRxDrops:
        ioerr += READ_RDBGC0r(unit, lport, &rdbgc0);
        counter->v[0] += RDBGC0r_GET(rdbgc0);
        break;
    case bmdStatRxIpDrop:
        ioerr += READ_RDBGC3r(unit, lport, &rdbgc3);
        counter->v[0] += RDBGC3r_GET(rdbgc3);
        break;
    case bmdStatRxDiscard:
        ioerr += READ_RDBGC4r(unit, lport, &rdbgc4);
        counter->v[0] += RDBGC4r_GET(rdbgc4);
        break;
    case bmdStatRxFilterDrop:
        ioerr += READ_RDBGC5r(unit, lport, &rdbgc5);
        counter->v[0] += RDBGC5r_GET(rdbgc5);
        break;
    case bmdStatRxGenDrop:
        ioerr += READ_RDBGC6r(unit, lport, &rdbgc6);
        counter->v[0] += RDBGC6r_GET(rdbgc6);
        break;
    case bmdStatRxPuntToCpu:
        ioerr += READ_RIPC4r(unit, lport, &ripc4);
        counter->v[0] += RIPC4r_GET(ripc4);
        break;
    default:
        break;
    }

    return ioerr ? CDK_E_IO : CDK_E_NONE;
}

#endif /* CDK_CONFIG_INCLUDE_BCM56840_A0 */
