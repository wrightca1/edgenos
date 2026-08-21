#!/bin/sh
# fm6000-read-golden-serdes.sh — read serdes-68 (Et1, EPL14/lane0) GOLDEN register state on a
# WARM EOS where Et1 is UP. These are the exact values the cold bring-up must reproduce — especially
# SERDES_CFG (0xe3834/35, PowerDown+RefSel) and PCS_10GBASER_CFG (0xe3825) which coldlink.sh never wrote.
#
# Run on EOS (mgmt <switch>) as root:  bash fm6000-read-golden-serdes.sh
# Reads FM6000 BAR via the EOS oracle (sudo python2 mmap of resource0). BDF 02:00.0.
# SPDX-License-Identifier: GPL-2.0-or-later
BDF=0000:02:00.0
py() { sudo python2 -c "
import mmap,struct
f=open('/sys/bus/pci/devices/$BDF/resource0','r+b')
m=mmap.mmap(f.fileno(),32*1024*1024)
def rd(w): return struct.unpack('<I',m[w*4:w*4+4])[0]
$1"; }

echo "=== serdes 68 / EPL14 lane0 GOLDEN state (Et1 must be UP) ==="
py "
regs=[('PORT_STATUS',0xe3800),('MAC_CFG0',0xe3810),('MAC_CFG1',0xe3811),('MAC_CFG2',0xe3812),('MAC_CFG3',0xe3813),
('PCS_10GBASER_CFG',0xe3825),('PCS_10GBASER_RX_STATUS',0xe3826),('PCS_10GBASER_TX_STATUS',0xe3827),
('SERDES_CFG_lo',0xe3834),('SERDES_CFG_hi',0xe3835),('LANE_CFG',0xe3837),('LANE_STATUS',0xe3838),
('SERDES_RX_CFG',0xe3839),('SERDES_TX_CFG_lo',0xe383a),('SERDES_TX_CFG_hi',0xe383b),
('SIGNAL_DETECT',0xe383c),('SERDES_STATUS_lo',0xe383e),('SERDES_STATUS_hi',0xe383f),
('SERDES_IM',0xe3840),('SERDES_IP',0xe3841)]
for n,w in regs: print('  %-24s 0x%05x = 0x%08x' % (n,w,rd(w)))
hi=rd(0xe383f)
print('  --> TxRdy(bit5)=%d RxRdy(bit6)=%d' % ((hi>>5)&1,(hi>>6)&1))
cfg=(rd(0xe3835)<<32)|rd(0xe3834)
print('  --> SERDES_CFG: PowerDown[31:30]=%d RefSel[11:6]=0x%x KrTrainingEn[33]=%d NearLoopback[28]=%d' %
  ((cfg>>30)&3,(cfg>>6)&0x3f,(cfg>>33)&1,(cfg>>28)&1))
"
echo "=== also read serdes-68 ETH regs 00,17,1d,1f,22,26,36,3b,0d via EOS fpV2ConfigSerdes/debug if available ==="
echo "(these are SBus dev 0x49 regs; capture with the SDK's dbg read while up)"
