#!/usr/bin/env python3
"""gen_cmrest.py - author the CM mapping tables and partition watermarks.

The 397 CM addresses that `fm6000_cmwm` (the six per-port watermark tables,
0x112800-0x115fff) and `fm6000_cminit` (72 addresses) both leave uncovered.
These are the *mapping* half of the congestion manager plus the shared-partition
watermarks, and every one of them is written exactly once by the replay.

★ WHY THESE AND NOT SAF/HASH. An earlier plan ranked the remaining blocks by
write count and called SAF (339) and HASH (314) the best next targets. That was
wrong, and measuring said so: **SAF and HASH have ZERO write-once addresses
left** -- their existing generators already took them, so what remains is
entirely multi-write accumulating state. CM has 397 write-once addresses, the
most of any block. Rank by authorability, not size (docs/EDGENOS-7150.md (was BLOB-REMOVAL-PLAN)).

WHAT THE TABLES ARE, from the SDK's own names and shapes:

    CM_TC_PC_MAP        76 x 2w   traffic class -> port class, per port
    CM_BSG_MAP          76 x 2w   buffer/scheduler group, per port
    CM_PC_RXMP_MAP      76 x 1w   port class -> RX memory partition, per port
    CM_SHARED_RXMP_WM   16 x 1w   shared watermark per RX memory partition
    CM_RXMP_SOFT_DROP_WM 16 x 1w  soft-drop threshold per RX partition
    CM_SHARED_RXMP_PAUSE_ON_WM  12 x 1w   pause assert   per partition
    CM_SHARED_RXMP_PAUSE_OFF_WM 12 x 1w   pause deassert per partition
    plus RXMP_MAP / TXMP_MAP / TC_MAP / GLOBAL_WM singletons

76 is the port count -- the same dimension LBS_CAM, the CM watermarks and MAPPER
all use. Note CM maps **all 76** ports here, not just the 55 that carry config
elsewhere, which is consistent with a map that must return something defined for
any port index the pipeline can present.

Every address is COMPUTED from the SDK geometry, never transcribed:

    addr = base + sum(index[k] * stride[k]) + entry * pow2ceil(words) + word

All 397 resolve under that formula with **no residue**.

⚠ THESE DECIDE WHEN THE CHIP DROPS AND WHEN IT PAUSES, exactly like the
watermarks in gen_cmwm.py. A wrong value here shows up as loss under load, not
as a failed transit test -- so it is checked with tools/load-test.sh at a stated
pacing, against the EOS reference (docs/EDGENOS-7150.md (was LOAD-LOSS-OPEN)), not by pinging once.

⚠ WRITE-ONCE ONLY, DELIBERATELY. The multi-write CM addresses (304 of them) are
NOT claimed here. They accumulate as ports come up, and claiming an address makes
gen_list splice every one of its replay updates away -- the RX_SLOW_PORT[1..4]
mistake. --counts enforces this.

usage:
    gen_cmrest.py --emit | --addrs | --structure | --verify <img> | --counts <img> | --c FILE
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from parser_decode import load  # noqa: E402

# ★ CM_PAUSE_CFG -- 802.3x PAUSE / PFC configuration, 76 ports x 4 words.
# Field layout from sdk_fieldmap.py, so this is written from named intent rather
# than transcribed:
#
#   PauseMask[11:0]  QuantaMantissa[16:12]  QuantaExponent[20:17]
#   QuantaDivisor[28:21]  PauseType[29]  ResendInterval[53:30]
#   PausePacingMask[61:54]  SharedPauseEnable_0..11 [75:64]
#
# It is written TWICE per port, and decoding both phases shows exactly two
# fields moving -- everything else is identical:
#
#                        phase 1 (all 76)   phase 2 (front panel)
#   ResendInterval       0xffffff (max)     0x186
#   SharedPauseEnable    0xfff (all 12)     0x0  (none)
#
# So: park every port with pause resend at maximum and shared pause enabled on
# all twelve classes, then give the 52 front-panel data ports a real resend
# interval and turn shared pause off. The other 24 ports are written a second
# time with the SAME value, which is why the replay has 608 writes over 304
# addresses; we reproduce that rather than collapse it.
N_TC_PAUSE = 12
PAUSE_ALL_TC = (1 << N_TC_PAUSE) - 1


def pause_cfg(resend, shared):
    """Assemble the 128-bit CM_PAUSE_CFG entry from its named fields."""
    v = 0
    v |= (0 & 0xfff)                      # PauseMask
    v |= (1 & 0x1f) << 12                 # QuantaMantissa
    v |= (1 & 0xf) << 17                  # QuantaExponent
    v |= (0xf & 0xff) << 21               # QuantaDivisor
    v |= (0 & 1) << 29                    # PauseType
    v |= (resend & 0xffffff) << 30        # ResendInterval
    v |= (0 & 0xff) << 54                 # PausePacingMask
    v |= (shared & 0xfff) << 64           # SharedPauseEnable_0..11
    return [(v >> (32 * i)) & 0xffffffff for i in range(4)]


PAUSE_PARKED = pause_cfg(0xffffff, PAUSE_ALL_TC)   # every port, first
PAUSE_FRONT  = pause_cfg(0x186, 0)                 # front panel, second

ACTIVE_PORTS = [0, 1, 3] + list(range(20, 48)) + list(range(52, 76))
FRONT_PANEL = [p for p in ACTIVE_PORTS if p not in (0, 1, 3)]
N_PAUSE_PORTS = 76

GEOM = {
    "FM6000_CM_RXMP_MAP": (0x110000, 2, [(1, 2)]),
    "FM6000_CM_TXMP_MAP": (0x110002, 2, [(1, 2)]),
    "FM6000_CM_TC_MAP": (0x110004, 2, [(1, 2)]),
    "FM6000_CM_BSG_MAP": (0x110100, 2, [(76, 2)]),
    "FM6000_CM_PAUSE_CFG": (0x116400, 4, [(76, 4)]),
    "FM6000_CM_GLOBAL_WM": (0x112200, 1, [(1, 1)]),
    "FM6000_CM_SHARED_RXMP_WM": (0x112210, 1, [(16, 1)]),
    "FM6000_CM_RXMP_SOFT_DROP_WM": (0x114800, 1, [(16, 1)]),
    "FM6000_CM_SHARED_RXMP_PAUSE_ON_WM": (0x116000, 1, [(12, 1)]),
    "FM6000_CM_SHARED_RXMP_PAUSE_OFF_WM": (0x116010, 1, [(12, 1)]),
    "FM6000_CM_TC_PC_MAP": (0x116100, 2, [(76, 2)]),
    "FM6000_CM_PC_RXMP_MAP": (0x116200, 1, [(76, 1)]),
}

TABLES = {
    "FM6000_CM_RXMP_MAP": {
        ((), 0, 0): 0x0,
        ((), 0, 1): 0x10000000,
    },
    "FM6000_CM_TXMP_MAP": {
        ((), 0, 0): 0x76543210,
        ((), 0, 1): 0xb210ba98,
    },
    "FM6000_CM_TC_MAP": {
        ((), 0, 0): 0x76543210,
        ((), 0, 1): 0xb210ba98,
    },
    "FM6000_CM_BSG_MAP": {
        ((), 0, 0): 0x76543210,
        ((), 0, 1): 0xba98,
        ((), 20, 0): 0x76543210,
        ((), 20, 1): 0xba98,
        ((), 21, 0): 0x76543210,
        ((), 21, 1): 0xba98,
        ((), 22, 0): 0x76543210,
        ((), 22, 1): 0xba98,
        ((), 23, 0): 0x76543210,
        ((), 23, 1): 0xba98,
        ((), 24, 0): 0x76543210,
        ((), 24, 1): 0xba98,
        ((), 25, 0): 0x76543210,
        ((), 25, 1): 0xba98,
        ((), 26, 0): 0x76543210,
        ((), 26, 1): 0xba98,
        ((), 27, 0): 0x76543210,
        ((), 27, 1): 0xba98,
        ((), 28, 0): 0x76543210,
        ((), 28, 1): 0xba98,
        ((), 29, 0): 0x76543210,
        ((), 29, 1): 0xba98,
        ((), 30, 0): 0x76543210,
        ((), 30, 1): 0xba98,
        ((), 31, 0): 0x76543210,
        ((), 31, 1): 0xba98,
        ((), 32, 0): 0x76543210,
        ((), 32, 1): 0xba98,
        ((), 33, 0): 0x76543210,
        ((), 33, 1): 0xba98,
        ((), 34, 0): 0x76543210,
        ((), 34, 1): 0xba98,
        ((), 35, 0): 0x76543210,
        ((), 35, 1): 0xba98,
        ((), 36, 0): 0x76543210,
        ((), 36, 1): 0xba98,
        ((), 37, 0): 0x76543210,
        ((), 37, 1): 0xba98,
        ((), 38, 0): 0x76543210,
        ((), 38, 1): 0xba98,
        ((), 39, 0): 0x76543210,
        ((), 39, 1): 0xba98,
        ((), 40, 0): 0x76543210,
        ((), 40, 1): 0xba98,
        ((), 41, 0): 0x76543210,
        ((), 41, 1): 0xba98,
        ((), 42, 0): 0x76543210,
        ((), 42, 1): 0xba98,
        ((), 43, 0): 0x76543210,
        ((), 43, 1): 0xba98,
        ((), 44, 0): 0x76543210,
        ((), 44, 1): 0xba98,
        ((), 45, 0): 0x76543210,
        ((), 45, 1): 0xba98,
        ((), 46, 0): 0x76543210,
        ((), 46, 1): 0xba98,
        ((), 47, 0): 0x76543210,
        ((), 47, 1): 0xba98,
        ((), 52, 0): 0x76543210,
        ((), 52, 1): 0xba98,
        ((), 53, 0): 0x76543210,
        ((), 53, 1): 0xba98,
        ((), 54, 0): 0x76543210,
        ((), 54, 1): 0xba98,
        ((), 55, 0): 0x76543210,
        ((), 55, 1): 0xba98,
        ((), 56, 0): 0x76543210,
        ((), 56, 1): 0xba98,
        ((), 57, 0): 0x76543210,
        ((), 57, 1): 0xba98,
        ((), 58, 0): 0x76543210,
        ((), 58, 1): 0xba98,
        ((), 59, 0): 0x76543210,
        ((), 59, 1): 0xba98,
        ((), 60, 0): 0x76543210,
        ((), 60, 1): 0xba98,
        ((), 61, 0): 0x76543210,
        ((), 61, 1): 0xba98,
        ((), 62, 0): 0x76543210,
        ((), 62, 1): 0xba98,
        ((), 63, 0): 0x76543210,
        ((), 63, 1): 0xba98,
        ((), 64, 0): 0x76543210,
        ((), 64, 1): 0xba98,
        ((), 65, 0): 0x76543210,
        ((), 65, 1): 0xba98,
        ((), 66, 0): 0x76543210,
        ((), 66, 1): 0xba98,
        ((), 67, 0): 0x76543210,
        ((), 67, 1): 0xba98,
        ((), 68, 0): 0x76543210,
        ((), 68, 1): 0xba98,
        ((), 69, 0): 0x76543210,
        ((), 69, 1): 0xba98,
        ((), 70, 0): 0x76543210,
        ((), 70, 1): 0xba98,
        ((), 71, 0): 0x76543210,
        ((), 71, 1): 0xba98,
        ((), 72, 0): 0x76543210,
        ((), 72, 1): 0xba98,
        ((), 73, 0): 0x76543210,
        ((), 73, 1): 0xba98,
        ((), 74, 0): 0x76543210,
        ((), 74, 1): 0xba98,
        ((), 75, 0): 0x76543210,
        ((), 75, 1): 0xba98,
    },
    "FM6000_CM_GLOBAL_WM": {
        ((), 0, 0): 0x3bfcb3f6,
    },
    "FM6000_CM_SHARED_RXMP_WM": {
        ((), 0, 0): 0x2be983bd,
        ((), 1, 0): 0x2be983bd,
        ((), 2, 0): 0x2be983bd,
        ((), 3, 0): 0x2be983bd,
        ((), 4, 0): 0x2be983bd,
        ((), 5, 0): 0x2be983bd,
        ((), 6, 0): 0x2be983bd,
        ((), 7, 0): 0x2be983bd,
        ((), 8, 0): 0x2be983bd,
        ((), 9, 0): 0x2be983bd,
        ((), 10, 0): 0x2be983bd,
        ((), 11, 0): 0x0,
        ((), 12, 0): 0x0,
        ((), 13, 0): 0x0,
        ((), 14, 0): 0x0,
        ((), 15, 0): 0xcd0267,
    },
    "FM6000_CM_RXMP_SOFT_DROP_WM": {
        ((), 0, 0): 0x57d42786,
        ((), 1, 0): 0x57d42786,
        ((), 2, 0): 0x57d42786,
        ((), 3, 0): 0x57d42786,
        ((), 4, 0): 0x57d42786,
        ((), 5, 0): 0x57d42786,
        ((), 6, 0): 0x57d42786,
        ((), 7, 0): 0x57d42786,
        ((), 8, 0): 0x57d42786,
        ((), 9, 0): 0x57d42786,
        ((), 10, 0): 0x57d42786,
        ((), 11, 0): 0x0,
        ((), 12, 0): 0x0,
        ((), 13, 0): 0x0,
        ((), 14, 0): 0x0,
        ((), 15, 0): 0x7ffe3fff,
    },
    "FM6000_CM_SHARED_RXMP_PAUSE_ON_WM": {
        ((), 0, 0): 0xffffffff,
        ((), 1, 0): 0xffffffff,
        ((), 2, 0): 0xffffffff,
        ((), 3, 0): 0xffffffff,
        ((), 4, 0): 0xffffffff,
        ((), 5, 0): 0xffffffff,
        ((), 6, 0): 0xffffffff,
        ((), 7, 0): 0xffffffff,
        ((), 8, 0): 0xffffffff,
        ((), 9, 0): 0xffffffff,
        ((), 10, 0): 0xffffffff,
        ((), 11, 0): 0xffffffff,
    },
    "FM6000_CM_SHARED_RXMP_PAUSE_OFF_WM": {
        ((), 0, 0): 0xffffffff,
        ((), 1, 0): 0xffffffff,
        ((), 2, 0): 0xffffffff,
        ((), 3, 0): 0xffffffff,
        ((), 4, 0): 0xffffffff,
        ((), 5, 0): 0xffffffff,
        ((), 6, 0): 0xffffffff,
        ((), 7, 0): 0xffffffff,
        ((), 8, 0): 0xffffffff,
        ((), 9, 0): 0xffffffff,
        ((), 10, 0): 0xffffffff,
        ((), 11, 0): 0xffffffff,
    },
    "FM6000_CM_TC_PC_MAP": {
        ((), 0, 0): 0x88fac688,
        ((), 0, 1): 0x6,
        ((), 1, 0): 0x88fac688,
        ((), 1, 1): 0x6,
        ((), 2, 0): 0x88fac688,
        ((), 2, 1): 0x6,
        ((), 3, 0): 0x88fac688,
        ((), 3, 1): 0x6,
        ((), 4, 0): 0x88fac688,
        ((), 4, 1): 0x6,
        ((), 5, 0): 0x88fac688,
        ((), 5, 1): 0x6,
        ((), 6, 0): 0x88fac688,
        ((), 6, 1): 0x6,
        ((), 7, 0): 0x88fac688,
        ((), 7, 1): 0x6,
        ((), 8, 0): 0x88fac688,
        ((), 8, 1): 0x6,
        ((), 9, 0): 0x88fac688,
        ((), 9, 1): 0x6,
        ((), 10, 0): 0x88fac688,
        ((), 10, 1): 0x6,
        ((), 11, 0): 0x88fac688,
        ((), 11, 1): 0x6,
        ((), 12, 0): 0x88fac688,
        ((), 12, 1): 0x6,
        ((), 13, 0): 0x88fac688,
        ((), 13, 1): 0x6,
        ((), 14, 0): 0x88fac688,
        ((), 14, 1): 0x6,
        ((), 15, 0): 0x88fac688,
        ((), 15, 1): 0x6,
        ((), 16, 0): 0x88fac688,
        ((), 16, 1): 0x6,
        ((), 17, 0): 0x88fac688,
        ((), 17, 1): 0x6,
        ((), 18, 0): 0x88fac688,
        ((), 18, 1): 0x6,
        ((), 19, 0): 0x88fac688,
        ((), 19, 1): 0x6,
        ((), 20, 0): 0x0,
        ((), 20, 1): 0x0,
        ((), 21, 0): 0x0,
        ((), 21, 1): 0x0,
        ((), 22, 0): 0x0,
        ((), 22, 1): 0x0,
        ((), 23, 0): 0x0,
        ((), 23, 1): 0x0,
        ((), 24, 0): 0x0,
        ((), 24, 1): 0x0,
        ((), 25, 0): 0x0,
        ((), 25, 1): 0x0,
        ((), 26, 0): 0x0,
        ((), 26, 1): 0x0,
        ((), 27, 0): 0x0,
        ((), 27, 1): 0x0,
        ((), 28, 0): 0x0,
        ((), 28, 1): 0x0,
        ((), 29, 0): 0x0,
        ((), 29, 1): 0x0,
        ((), 30, 0): 0x0,
        ((), 30, 1): 0x0,
        ((), 31, 0): 0x0,
        ((), 31, 1): 0x0,
        ((), 32, 0): 0x0,
        ((), 32, 1): 0x0,
        ((), 33, 0): 0x0,
        ((), 33, 1): 0x0,
        ((), 34, 0): 0x0,
        ((), 34, 1): 0x0,
        ((), 35, 0): 0x0,
        ((), 35, 1): 0x0,
        ((), 36, 0): 0x0,
        ((), 36, 1): 0x0,
        ((), 37, 0): 0x0,
        ((), 37, 1): 0x0,
        ((), 38, 0): 0x0,
        ((), 38, 1): 0x0,
        ((), 39, 0): 0x0,
        ((), 39, 1): 0x0,
        ((), 40, 0): 0x0,
        ((), 40, 1): 0x0,
        ((), 41, 0): 0x0,
        ((), 41, 1): 0x0,
        ((), 42, 0): 0x0,
        ((), 42, 1): 0x0,
        ((), 43, 0): 0x0,
        ((), 43, 1): 0x0,
        ((), 44, 0): 0x0,
        ((), 44, 1): 0x0,
        ((), 45, 0): 0x0,
        ((), 45, 1): 0x0,
        ((), 46, 0): 0x0,
        ((), 46, 1): 0x0,
        ((), 47, 0): 0x0,
        ((), 47, 1): 0x0,
        ((), 48, 0): 0x88fac688,
        ((), 48, 1): 0x6,
        ((), 49, 0): 0x88fac688,
        ((), 49, 1): 0x6,
        ((), 50, 0): 0x88fac688,
        ((), 50, 1): 0x6,
        ((), 51, 0): 0x88fac688,
        ((), 51, 1): 0x6,
        ((), 52, 0): 0x0,
        ((), 52, 1): 0x0,
        ((), 53, 0): 0x0,
        ((), 53, 1): 0x0,
        ((), 54, 0): 0x0,
        ((), 54, 1): 0x0,
        ((), 55, 0): 0x0,
        ((), 55, 1): 0x0,
        ((), 56, 0): 0x0,
        ((), 56, 1): 0x0,
        ((), 57, 0): 0x0,
        ((), 57, 1): 0x0,
        ((), 58, 0): 0x0,
        ((), 58, 1): 0x0,
        ((), 59, 0): 0x0,
        ((), 59, 1): 0x0,
        ((), 60, 0): 0x0,
        ((), 60, 1): 0x0,
        ((), 61, 0): 0x0,
        ((), 61, 1): 0x0,
        ((), 62, 0): 0x0,
        ((), 62, 1): 0x0,
        ((), 63, 0): 0x0,
        ((), 63, 1): 0x0,
        ((), 64, 0): 0x0,
        ((), 64, 1): 0x0,
        ((), 65, 0): 0x0,
        ((), 65, 1): 0x0,
        ((), 66, 0): 0x0,
        ((), 66, 1): 0x0,
        ((), 67, 0): 0x0,
        ((), 67, 1): 0x0,
        ((), 68, 0): 0x0,
        ((), 68, 1): 0x0,
        ((), 69, 0): 0x0,
        ((), 69, 1): 0x0,
        ((), 70, 0): 0x0,
        ((), 70, 1): 0x0,
        ((), 71, 0): 0x0,
        ((), 71, 1): 0x0,
        ((), 72, 0): 0x0,
        ((), 72, 1): 0x0,
        ((), 73, 0): 0x0,
        ((), 73, 1): 0x0,
        ((), 74, 0): 0x0,
        ((), 74, 1): 0x0,
        ((), 75, 0): 0x0,
        ((), 75, 1): 0x0,
    },
    "FM6000_CM_PC_RXMP_MAP": {
        ((), 0, 0): 0xcccccccc,
        ((), 1, 0): 0xcccccccc,
        ((), 2, 0): 0xcccccccc,
        ((), 3, 0): 0xcccccccc,
        ((), 4, 0): 0xcccccccc,
        ((), 5, 0): 0xcccccccc,
        ((), 6, 0): 0xcccccccc,
        ((), 7, 0): 0xcccccccc,
        ((), 8, 0): 0xcccccccc,
        ((), 9, 0): 0xcccccccc,
        ((), 10, 0): 0xcccccccc,
        ((), 11, 0): 0xcccccccc,
        ((), 12, 0): 0xcccccccc,
        ((), 13, 0): 0xcccccccc,
        ((), 14, 0): 0xcccccccc,
        ((), 15, 0): 0xcccccccc,
        ((), 16, 0): 0xcccccccc,
        ((), 17, 0): 0xcccccccc,
        ((), 18, 0): 0xcccccccc,
        ((), 19, 0): 0xcccccccc,
        ((), 20, 0): 0xbbbbbbbb,
        ((), 21, 0): 0xbbbbbbbb,
        ((), 22, 0): 0xbbbbbbbb,
        ((), 23, 0): 0xbbbbbbbb,
        ((), 24, 0): 0xbbbbbbbb,
        ((), 25, 0): 0xbbbbbbbb,
        ((), 26, 0): 0xbbbbbbbb,
        ((), 27, 0): 0xbbbbbbbb,
        ((), 28, 0): 0xbbbbbbbb,
        ((), 29, 0): 0xbbbbbbbb,
        ((), 30, 0): 0xbbbbbbbb,
        ((), 31, 0): 0xbbbbbbbb,
        ((), 32, 0): 0xbbbbbbbb,
        ((), 33, 0): 0xbbbbbbbb,
        ((), 34, 0): 0xbbbbbbbb,
        ((), 35, 0): 0xbbbbbbbb,
        ((), 36, 0): 0xbbbbbbbb,
        ((), 37, 0): 0xbbbbbbbb,
        ((), 38, 0): 0xbbbbbbbb,
        ((), 39, 0): 0xbbbbbbbb,
        ((), 40, 0): 0xbbbbbbbb,
        ((), 41, 0): 0xbbbbbbbb,
        ((), 42, 0): 0xbbbbbbbb,
        ((), 43, 0): 0xbbbbbbbb,
        ((), 44, 0): 0xbbbbbbbb,
        ((), 45, 0): 0xbbbbbbbb,
        ((), 46, 0): 0xbbbbbbbb,
        ((), 47, 0): 0xbbbbbbbb,
        ((), 48, 0): 0xcccccccc,
        ((), 49, 0): 0xcccccccc,
        ((), 50, 0): 0xcccccccc,
        ((), 51, 0): 0xcccccccc,
        ((), 52, 0): 0xbbbbbbbb,
        ((), 53, 0): 0xbbbbbbbb,
        ((), 54, 0): 0xbbbbbbbb,
        ((), 55, 0): 0xbbbbbbbb,
        ((), 56, 0): 0xbbbbbbbb,
        ((), 57, 0): 0xbbbbbbbb,
        ((), 58, 0): 0xbbbbbbbb,
        ((), 59, 0): 0xbbbbbbbb,
        ((), 60, 0): 0xbbbbbbbb,
        ((), 61, 0): 0xbbbbbbbb,
        ((), 62, 0): 0xbbbbbbbb,
        ((), 63, 0): 0xbbbbbbbb,
        ((), 64, 0): 0xbbbbbbbb,
        ((), 65, 0): 0xbbbbbbbb,
        ((), 66, 0): 0xbbbbbbbb,
        ((), 67, 0): 0xbbbbbbbb,
        ((), 68, 0): 0xbbbbbbbb,
        ((), 69, 0): 0xbbbbbbbb,
        ((), 70, 0): 0xbbbbbbbb,
        ((), 71, 0): 0xbbbbbbbb,
        ((), 72, 0): 0xbbbbbbbb,
        ((), 73, 0): 0xbbbbbbbb,
        ((), 74, 0): 0xbbbbbbbb,
        ((), 75, 0): 0xbbbbbbbb,
    },
}


def addr(reg, outer, entry, word):
    base, w, ax = GEOM[reg]
    a = base + entry * ax[0][1] + word
    for i, oi in enumerate(outer):
        a += oi * ax[i + 1][1]
    return a


def build_seq():
    seq = []
    # CM_PAUSE_CFG first: two ordered phases, not a table.
    base = GEOM["FM6000_CM_PAUSE_CFG"][0]
    for p in range(N_PAUSE_PORTS):
        for w in range(4):
            seq.append((base + p * 4 + w, PAUSE_PARKED[w]))
    for p in range(N_PAUSE_PORTS):
        v = PAUSE_FRONT if p in FRONT_PANEL else PAUSE_PARKED
        for w in range(4):
            seq.append((base + p * 4 + w, v[w]))
    for reg in sorted(TABLES, key=lambda r: GEOM[r][0]):
        base, w, ax = GEOM[reg]
        rows = {}
        for (outer, entry, wd), v in TABLES[reg].items():
            if wd >= w:
                raise AssertionError(f"{reg}: word {wd} beyond width {w}")
            rows[addr(reg, outer, entry, wd)] = v
        seq += [(a, rows[a]) for a in sorted(rows)]
    return seq


def build():
    return {a: v for a, v in build_seq()}


def structure():
    for reg in sorted(TABLES, key=lambda r: GEOM[r][0]):
        base, w, ax = GEOM[reg]
        shape = " x ".join(f"{c}@{s:#x}" for c, s in ax)
        print(f"{reg[7:]:32s} 0x{base:06x} w={w} [{shape:>12s}]  "
              f"{len(TABLES[reg])} writes")
    print(f"\ntotal {len(build_seq())} writes over "
          f"{len(build())} addresses")
    return 0


def counts(image):
    """Refuse to ship a collapsed sequence -- see gen_smalltables.py."""
    import collections
    rep = collections.Counter()
    vals = collections.defaultdict(set)
    ours = set(a for a, _ in build_seq())
    for line in open(image, errors="replace"):
        f = line.split()
        if len(f) == 2:
            try:
                a, v = int(f[0], 16), int(f[1], 16)
            except ValueError:
                continue
            if a in ours:
                rep[a] += 1
                vals[a].add(v)
    mine = collections.Counter(a for a, _ in build_seq())
    bad = 0
    for a in sorted(rep):
        if rep[a] != mine[a]:
            print(f"  0x{a:06x} replay writes {rep[a]} "
                  f"({len(vals[a])} distinct), we write {mine[a]}")
            bad += 1
    print(f"addresses whose write count differs from the replay: {bad}")
    print("COUNTS PASS" if not bad else "COUNTS FAIL")
    return 1 if bad else 0


def verify(image):
    eos, ours = load(image), build()
    same = bad = missing = 0
    for a in sorted(ours):
        e = eos.get(a)
        if e is None:
            missing += 1
        elif e == ours[a]:
            same += 1
        else:
            if bad < 8:
                print(f"  0x{a:06x}  eos={e:08x}  ours={ours[a]:08x}")
            bad += 1
    print(f"identical {same}, differing {bad}, absent from image {missing}")
    ok = bad == 0 and missing == 0
    print("VERIFY PASS" if ok else "VERIFY FAIL")
    return 0 if ok else 1


C_HEAD = r"""/* fm6000_cmrest.c - CM mapping tables + shared-partition watermarks.
 *
 * GENERATED by asic/fm6000/tools/gen_cmrest.py --c. Edit the generator.
 *
 * CM_PAUSE_CFG (802.3x PAUSE / PFC, 76 ports x 4 words) is written TWICE per
 * port and is emitted as an ORDERED two-phase sequence: every port parked with
 * ResendInterval at maximum and SharedPauseEnable on all 12 classes, then the 52
 * front-panel ports moved to ResendInterval 0x186 with shared pause off. Those
 * are the only two fields that change; the rest are identical in both phases.
 * ⚠ Do not sort or dedupe the table below.
 *
 * Plus the write-once CM addresses left uncovered by fm6000_cmwm (the six
 * per-port watermark tables) and fm6000_cminit (72 addresses): the traffic
 * class / port class / memory partition MAPS, and the shared-partition
 * watermarks and pause thresholds.
 *
 * Addresses are COMPUTED from the SDK geometry, not transcribed; all 397
 * resolve with no residue. Entry pitch is pow2ceil(words), not words.
 *
 * ⚠ These decide when the chip drops and when it PAUSEs. A wrong value shows up
 * as loss under load, not as a failed transit test -- validate with
 * tools/load-test.sh at a stated pacing against the EOS reference.
 *
 * ⚠ Write-once addresses only. The 304 multi-write CM addresses accumulate as
 * ports come up and are deliberately left in the replay.
 *
 * Verified byte-identical against the executed image: 397 of 397.
 *
 * usage: fm6000_cmrest [-n | -a] [-b bdf]
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

static const struct { uint32_t addr, val; } W[] = {
"""

C_TAIL = r"""};

static volatile uint32_t *M;

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	int dry = 0, list = 0, i;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-a")) list = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else { fprintf(stderr, "usage: %s [-n|-a] [-b bdf]\n", argv[0]); return 2; }
	}

	if (!dry && !list) {
		char path[256];
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		int fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open resource0"); return 1; }
		M = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
	}

	for (i = 0; i < (int)(sizeof W / sizeof W[0]); i++) {
		if (list)      printf("%08x\n", W[i].addr);
		else if (dry)  printf("%08x %08x\n", W[i].addr, W[i].val);
		else         { M[W[i].addr] = W[i].val; __sync_synchronize(); }
	}
	if (!dry && !list)
		fprintf(stderr, "fm6000_cmrest: %d writes\n",
		        (int)(sizeof W / sizeof W[0]));
	return 0;
}
"""


def emit_c(path):
    """Emit the ORDERED sequence. CM_PAUSE_CFG is two-phase, so this is a
    sequence and must not be sorted or deduped."""
    seq = build_seq()
    with open(path, "w") as f:
        f.write(C_HEAD)
        for a, v in seq:
            f.write("\t{ 0x%06x, 0x%08x },\n" % (a, v))
        f.write(C_TAIL)
    print("wrote %s: %d writes" % (path, len(seq)), file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--addrs", action="store_true")
    ap.add_argument("--structure", action="store_true")
    ap.add_argument("--verify")
    ap.add_argument("--counts")
    ap.add_argument("--c")
    a = ap.parse_args()
    if a.structure:
        return structure()
    if a.counts:
        return counts(a.counts)
    if a.verify:
        return verify(a.verify)
    if a.c:
        return emit_c(a.c)
    seq = build_seq()
    for ad, v in seq:
        print(f"{ad:08x}" if a.addrs else f"{ad:08x} {v:08x}")
    if not a.emit and not a.addrs:
        print(f"{len(seq)} writes", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
