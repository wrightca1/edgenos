"""EdgeNOS platform class for the Accton AS4610-54T (ONL OnlPlatform analog).

Helix4 (BCM56340) / ARM iProc. Datapath = bcmd (OpenBCM). The 6.1 own-build keeps
the datapath under /opt/edgenos (modules included), loaded with the params from
bcmd-prep.sh; this class encodes that declaratively.
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (_HERE, os.path.abspath(os.path.join(_HERE, "..", "..", "core", "platform"))):
    if _p not in sys.path:
        sys.path.insert(0, _p)
from base import EdgeNOSPlatformBase, PortConfig_48x1_4x10   # noqa: E402


class EdgeNOSPlatform_arm_accton_as4610_54_r0(EdgeNOSPlatformBase, PortConfig_48x1_4x10):
    PLATFORM = "arm-accton-as4610-54-r0"          # == switch DB key
    MODEL = "AS4610-54"
    SYS_OBJECT_ID = ".4610.54"

    # 6.1 own-build keeps datapath .ko under /opt/edgenos (not /lib/modules).
    MODULE_DIRS = ["opt/edgenos"]
    DRIVERS = [
        ("linux-kernel-bde", "dmasize=8M"),       # iProc CMICd; smaller DMA pool than the 5610
        ("linux-user-bde", ""),
        ("linux-bcm-knet", "default_mtu=1600"),   # KNET RX/TX punt (match far-side MTU)
    ]
    # ONLP platform layer for this board comes from ONL (onlp component).
    INIT_SCRIPTS = []                             # bcmd.service runs bcmd-prep.sh at start

    def fan_count(self):
        return 2

    def psu_count(self):
        return 2
