#!/bin/sh
# fm6000-rx-state.sh - read the CPU-frame RX-capture state (phase50 diagnosis).
#
# Fixed-function counters/state, no STATS_AR config needed. Run before AND after a
# single inject and compare — tells you whether a frame reached the CPU DMA/FIBM.
#
#  FIBM block (WORD 0x05000): the in-band-mgmt CPU frame interface.
#  PCIe DMA ring (BYTE 0x5000 == WORD 0x1400): the fpdma path our punt uses.
#    byte 0x5004=COMMAND -> word 0x1401 ; byte 0x5008=STATUS -> word 0x1402 ;
#    byte 0x5060=DMA_CFG -> word 0x1418 (== the golden 0x37, confirms the mapping).
# SPDX-License-Identifier: GPL-2.0-or-later
B=0000:02:00.0
R(){ fm6000reg $B $1 2>/dev/null | grep -o '[0-9a-f]*$'; }

echo "== PCIe DMA ring engine (word 0x1400 region) =="
echo "  COMMAND(0x1401)=0x$(R 0x1401)  STATUS(0x1402)=0x$(R 0x1402)  [RxState=bits5:3, TxState=2:0; golden run=0x12]"
echo "  DMA_CFG(0x1418)=0x$(R 0x1418) (want 0x37)  cur_ptr(0x140E)=0x$(R 0x140E)"
echo "  RX_BD_BASE(0x1404/5)=0x$(R 0x1405)$(R 0x1404)  RX_BD_END(0x1406/7)=0x$(R 0x1407)$(R 0x1406)"

echo "== FIBM CPU-frame interface (word 0x05000) =="
echo "  CFG(0x5000)=0x$(R 0x5000)  SGLORT(0x5001)=0x$(R 0x5001)"
echo "  REQUEST_CTR(0x5008)=0x$(R 0x5008)  DROP_CTR(0x5009)=0x$(R 0x5009)  RESPONSE_CTR(0x500A)=0x$(R 0x500A)"
echo "  INTR_CTR_0(0x500B)=0x$(R 0x500B)  INTR_CTR_1(0x500C)=0x$(R 0x500C)"
echo "== compare before/after inject: a moved DMA cur_ptr or FIBM REQUEST => a frame reached the CPU path =="
