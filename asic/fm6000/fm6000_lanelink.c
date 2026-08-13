/* fm6000_lanelink.c - bring up ONE EPL lane's SerDes, parameterised by front-panel port.
 *
 * fm6000_linkup.c replays a captured Et1 bring-up window verbatim: every SBus op
 * hardcoded to device 0x49, every EPL address to 0xe38xx (EPL14 lane 0), and 316
 * L2L-sweeper writes plus forwarding-table writes for logical id 0x3ee mixed in.
 * It brings up port 1 and nothing else, which is why port 3 has never linked
 * under EdgeNOS -- nothing in the boot touches lane 1 at all.
 *
 * The op table is the LIVE Ethernet3 down->up capture, segmented to just this
 * lane: 79 EPL-lane MMIO writes, 43 SBus ops to the lane's SerDes, and the 46
 * SPICO ops that belong to it. Segmentation uses the SPICO broadcast's reg-0x03
 * payload, which names the device the following block targets -- that is how the
 * 66 SPICO ops for other ports in the same window are excluded.
 *
 * It previously used fm6000_linkup's COLD Et1 window transposed to lane 1. That
 * ran cleanly on hardware and left the lane dark: the cold window holds 52 EPL
 * writes, 21 lane SBus ops and 16 SPICO ops, where a live bring-up does 79/43/46.
 * Parameterising the wrong sequence faithfully still gives the wrong sequence.
 *
 * Everything port-dependent is still derived, not transcribed:
 *
 *   MMIO address  = EPL_BASE + 0x400*epl + 0x80*lane + offset
 *   SBus device   = see sbus_dev() below
 *   TX equalisation = FM6000_SERDES_PORTS[].pre / .post, patched into
 *                     SERDES_TX_CFG TxOutputEqPre[14:12] / TxOutputEqPost[11:8]
 *
 * The equalisation derivation is not a guess. The captured lane-0 sequence writes
 * SERDES_TX_CFG = 0xc0000581, and port 1's table entry is pre=0 post=5. Port 3's
 * entry is pre=1 post=5, which yields 0xc0001581 -- exactly the value measured on
 * a live EOS chip whose lane 1 was up. The one value that differs between the two
 * lanes falls straight out of the tuning table we already had.
 *
 * A second table follows the link ops: DFE[], the RX adaptation procedure, taken
 * from a LIVE capture of one fm6000StartSerDesDfeTuning() call on EOS rather than
 * from a boot window. See the comment on DFE[] and docs/PORT3-BRINGUP.md. Pass
 * -l to run the link ops alone, which is what this tool did before.
 *
 * ⚠ PROVENANCE. The 89-op sequence is derived from an EOS capture, the same class
 * of artifact as fwd4.txt, and it was already in this tree inside fm6000_linkup.c.
 * This does not make it clean -- it makes it smaller and parameterised instead of
 * transcribed once per port. Generating it from first principles is still open.
 * DFE[] is the same class of artifact and carries the same debt.
 *
 *   fm6000_lanelink [-n] [-l] [-b <bdf>] [-d <us>] <front-panel-port>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "fm6000_serdes_ports.h"

#define PIN     0x1C021u
#define SB_CMD  0x0F001u
#define SB_REQ  0x0F002u
#define EPL_BASE 0xE0000u        /* word address; EPL n lane l = +0x400*n +0x80*l */

#define SERDES_TX_CFG_OFF 0x3a   /* TxOutputEqPost[11:8], TxOutputEqPre[14:12] */
#define SEQ_DEV  0x4au           /* device the captured table was recorded from */
#define SPICO_BC 0xfdu           /* SPICO broadcast; its reg 3 payload names the target */

enum { OP_MMIO, OP_SBUS };
struct op { uint8_t kind; uint8_t off_or_sbop; uint32_t val; uint8_t reg; uint8_t dev; };

static const struct op SEQ[] = {
	{ OP_MMIO, 0x02, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x04, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07fffffeu, 0x00, 0x00 },
	{ OP_MMIO, 0x37, 0x018c0002u, 0x00, 0x00 },
	{ OP_SBUS, 0x22, 0x00000000u, 0x1f, 0x4a },
	{ OP_SBUS, 0x21, 0x00000008u, 0x2a, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x1f, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x2b, 0x4a },
	{ OP_SBUS, 0x21, 0x00000010u, 0x17, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000010u, 0x17, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x0d, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x26, 0x4a },
	{ OP_SBUS, 0x21, 0x00000010u, 0x17, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x0d, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x26, 0x4a },
	{ OP_SBUS, 0x21, 0x00000010u, 0x17, 0x4a },
	{ OP_MMIO, 0x3c, 0x000001eeu, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x003a0281u, 0x00, 0x00 },
	{ OP_MMIO, 0x41, 0x00000020u, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fdfu, 0x00, 0x00 },
	{ OP_SBUS, 0x22, 0x00000000u, 0x24, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x20, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x21, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x22, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x23, 0x4a },
	{ OP_MMIO, 0x37, 0x010c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002a0280u, 0x00, 0x00 },
	{ OP_MMIO, 0x3a, 0xc0001580u, 0x00, 0x00 },
	{ OP_MMIO, 0x3b, 0x00000883u, 0x00, 0x00 },
	{ OP_MMIO, 0x37, 0x000c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fdfu, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fffu, 0x00, 0x00 },
	{ OP_MMIO, 0x41, 0x00003fffu, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fdfu, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002f8280u, 0x00, 0x00 },
	{ OP_MMIO, 0x37, 0x018c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002f8280u, 0x00, 0x00 },
	{ OP_MMIO, 0x3a, 0xc0001580u, 0x00, 0x00 },
	{ OP_MMIO, 0x3b, 0x00000883u, 0x00, 0x00 },
	{ OP_MMIO, 0x37, 0x000c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002f8280u, 0x00, 0x00 },
	{ OP_MMIO, 0x3a, 0xc0001580u, 0x00, 0x00 },
	{ OP_MMIO, 0x3b, 0x00000883u, 0x00, 0x00 },
	{ OP_SBUS, 0x21, 0x0000001fu, 0x01, 0x4a },
	{ OP_SBUS, 0x21, 0x0000003fu, 0x02, 0x4a },
	{ OP_MMIO, 0x39, 0x002a0280u, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002f8280u, 0x00, 0x00 },
	{ OP_MMIO, 0x28, 0x00000000u, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002f8280u, 0x00, 0x00 },
	{ OP_MMIO, 0x3a, 0xc0001580u, 0x00, 0x00 },
	{ OP_MMIO, 0x3b, 0x00000883u, 0x00, 0x00 },
	{ OP_MMIO, 0x10, 0x2000033cu, 0x00, 0x00 },
	{ OP_MMIO, 0x11, 0x400003e0u, 0x00, 0x00 },
	{ OP_MMIO, 0x12, 0x00002414u, 0x00, 0x00 },
	{ OP_MMIO, 0x13, 0x00001841u, 0x00, 0x00 },
	{ OP_MMIO, 0x10, 0x2000033cu, 0x00, 0x00 },
	{ OP_MMIO, 0x11, 0x400003e0u, 0x00, 0x00 },
	{ OP_MMIO, 0x12, 0x00002414u, 0x00, 0x00 },
	{ OP_MMIO, 0x13, 0x00001841u, 0x00, 0x00 },
	{ OP_MMIO, 0x10, 0x2000033cu, 0x00, 0x00 },
	{ OP_MMIO, 0x11, 0x400003e0u, 0x00, 0x00 },
	{ OP_MMIO, 0x12, 0x00002414u, 0x00, 0x00 },
	{ OP_MMIO, 0x13, 0x00001841u, 0x00, 0x00 },
	{ OP_MMIO, 0x41, 0x00000020u, 0x00, 0x00 },
	{ OP_MMIO, 0x37, 0x018c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x41, 0x000000e0u, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003f1fu, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x003f8281u, 0x00, 0x00 },
	{ OP_MMIO, 0x3a, 0xc0001581u, 0x00, 0x00 },
	{ OP_MMIO, 0x3b, 0x00000883u, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003f5fu, 0x00, 0x00 },
	{ OP_MMIO, 0x41, 0x00003f5fu, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003f1fu, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fbfu, 0x00, 0x00 },
	{ OP_MMIO, 0x41, 0x00003fbfu, 0x00, 0x00 },
	{ OP_SBUS, 0x20, 0x00000000u, 0x00, 0x4a },
	{ OP_SBUS, 0x21, 0x00000010u, 0x17, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x06, 0x4a },
	{ OP_MMIO, 0x3a, 0xc0001581u, 0x00, 0x00 },
	{ OP_MMIO, 0x3b, 0x00000c83u, 0x00, 0x00 },
	{ OP_MMIO, 0x37, 0x000c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x04, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07fffffeu, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x003a0281u, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003f9fu, 0x00, 0x00 },
	{ OP_SBUS, 0x22, 0x00000000u, 0x1f, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x24, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x20, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x21, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x22, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x23, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x24, 0x4a },
	{ OP_MMIO, 0x37, 0x008c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x3c, 0x000001e2u, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002a0281u, 0x00, 0x00 },
	{ OP_SBUS, 0x21, 0x00000000u, 0x17, 0x4a },
	{ OP_SBUS, 0x21, 0x0000000eu, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000002u, 0x2b, 0x4a },
	{ OP_MMIO, 0x02, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x04, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fbfu, 0x00, 0x00 },
	{ OP_MMIO, 0x41, 0x00003fbfu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07fffffeu, 0x00, 0x00 },
	{ OP_SBUS, 0x22, 0x00000000u, 0x1f, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x24, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x25, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x26, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x27, 0x4a },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x1f, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x21, 0x00000020u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_MMIO, 0x37, 0x000c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fbfu, 0x00, 0x00 },
	{ OP_MMIO, 0x3c, 0x000001feu, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002a0281u, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x04, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07fffffeu, 0x00, 0x00 },
	{ OP_SBUS, 0x21, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x21, 0x00000020u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x00000016u, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x21, 0x00000020u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000000eu, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x21, 0x00000020u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x00000016u, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x21, 0x00000020u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000000eu, 0x2a, 0x4a },
};
#define NSEQ ((int)(sizeof SEQ / sizeof SEQ[0]))

/* RX adaptation (DFE tuning), segmented from the LIVE capture of one
 * fm6000StartSerDesDfeTuning(0,69,0) call on EOS -- see docs/PORT3-BRINGUP.md.
 * Same segmentation rules as SEQ: 29 EPL lane writes, 35 SBus ops to the lane's
 * SerDes, 30 SPICO ops claimed by the reg-0x03 payload rule. The 46 SPICO ops
 * and 6 SBus ops for lanes 0x45/0x49 in the same window are excluded.
 *
 * The capture was disarmed mid-block, so its last two ops -- a SPICO interrupt
 * opened with reg 0x01/0x02 and never given its reg-0x03 target -- are dropped
 * rather than replayed half-formed. The table ends on the last complete block.
 *
 * ⚠ The SPICO firmware performs the adaptation; these ops start it and poll it.
 * The polls are replayed as fixed reads, so this does NOT wait for convergence
 * the way fm6000CheckSerDesDfeTuningState does -- it issues the procedure. And
 * it is inert on an image built without SPICO (the fibre-only C1 option), since
 * there is then no firmware to run it. Neither point is yet tested on hardware. */
static const struct op DFE[] = {
	{ OP_SBUS, 0x21, 0x00000000u, 0x17, 0x4a },
	{ OP_SBUS, 0x21, 0x0000000au, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000002u, 0x2b, 0x4a },
	{ OP_MMIO, 0x02, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x04, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07fffffeu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x04, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07fffffeu, 0x00, 0x00 },
	{ OP_SBUS, 0x22, 0x00000000u, 0x1f, 0x4a },
	{ OP_SBUS, 0x21, 0x00000008u, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x2b, 0x4a },
	{ OP_SBUS, 0x21, 0x00000010u, 0x17, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x0d, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x26, 0x4a },
	{ OP_SBUS, 0x21, 0x00000010u, 0x17, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x24, 0x4a },
	{ OP_MMIO, 0x37, 0x000c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x3c, 0x000001eeu, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x003a0281u, 0x00, 0x00 },
	{ OP_MMIO, 0x41, 0x00000020u, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fdfu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x04, 0x07ffffffu, 0x00, 0x00 },
	{ OP_SBUS, 0x22, 0x00000000u, 0x1f, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x24, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x20, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x21, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x22, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x23, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x24, 0x4a },
	{ OP_MMIO, 0x37, 0x008c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x3c, 0x000001e2u, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002a0281u, 0x00, 0x00 },
	{ OP_SBUS, 0x21, 0x00000000u, 0x17, 0x4a },
	{ OP_SBUS, 0x21, 0x0000000eu, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000002u, 0x2b, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x24, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x20, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x21, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x22, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x23, 0x4a },
	{ OP_MMIO, 0x02, 0x07fffffeu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x04, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fffu, 0x00, 0x00 },
	{ OP_MMIO, 0x41, 0x00003fffu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07fffffeu, 0x00, 0x00 },
	{ OP_SBUS, 0x22, 0x00000000u, 0x1f, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x24, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x25, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x26, 0x4a },
	{ OP_SBUS, 0x22, 0x00000000u, 0x27, 0x4a },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x1f, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x21, 0x00000020u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_MMIO, 0x37, 0x000c0002u, 0x00, 0x00 },
	{ OP_MMIO, 0x40, 0x00003fffu, 0x00, 0x00 },
	{ OP_MMIO, 0x3c, 0x000001feu, 0x00, 0x00 },
	{ OP_MMIO, 0x39, 0x002a0281u, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x04, 0x07ffffffu, 0x00, 0x00 },
	{ OP_MMIO, 0x02, 0x07fffffeu, 0x00, 0x00 },
	{ OP_SBUS, 0x21, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x21, 0x00000020u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x00000016u, 0x2a, 0x4a },
	{ OP_SBUS, 0x21, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x21, 0x00000020u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000004au, 0x03, 0xfd },
	{ OP_SBUS, 0x21, 0x00000018u, 0x0c, 0xfd },
	{ OP_SBUS, 0x21, 0x00000008u, 0x0c, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x01, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x00, 0xfd },
	{ OP_SBUS, 0x22, 0x00000000u, 0x02, 0xfd },
	{ OP_SBUS, 0x21, 0x0000000eu, 0x2a, 0x4a },
};
#define NDFE ((int)(sizeof DFE / sizeof DFE[0]))

static volatile uint32_t *M;
static unsigned DLY = 20;
static int dry;
static int link_only;

static void wr(uint32_t w, uint32_t v)
{
	if (dry) { printf("    %06x <- %08x\n", w, v); return; }
	M[w] = v; __sync_synchronize(); if (DLY) usleep(DLY);
}
static uint32_t rd(uint32_t w) { uint32_t v = M[w]; __sync_synchronize(); return v; }

static int sbus(uint32_t cmd, uint32_t data)
{
	long i;
	if (dry) { printf("    SBUS cmd=%08x data=%08x\n", cmd, data); return 0; }
	wr(SB_REQ, data); wr(SB_CMD, 0); wr(SB_CMD, cmd);
	for (i = 0; i < 2000000L; i++) {
		uint32_t s = rd(SB_CMD);
		if (s == 0xffffffffu) return -2;
		if (!(s & (1u << 25))) return (int)((s >> 26) & 7);
	}
	return -3;
}

/* Observed SerDes SBus device addresses. Lane step is +1, confirmed by capturing
 * Ethernet3 down->up with fmPlatformTraceRegOps armed: EPL14 lane 1 is 0x4a.
 * The EPL step is inferred from two points only (EPL14 lane0 = 0x49, EPL16
 * lane0 = 0x45), so anything outside those is flagged rather than trusted. */
static int sbus_dev(unsigned epl, unsigned lane, int *observed)
{
	*observed = (epl == 14 && lane <= 1) || (epl == 16 && lane == 0);
	return 0x49 + (int)lane - 2 * ((int)epl - 14);
}

static const struct fm6000_serdes_port *find_port(unsigned intf)
{
	size_t i, n = sizeof FM6000_SERDES_PORTS / sizeof FM6000_SERDES_PORTS[0];
	for (i = 0; i < n; i++)
		if (FM6000_SERDES_PORTS[i].intf == intf) return &FM6000_SERDES_PORTS[i];
	return NULL;
}

/* TxOutputEqPre[14:12] and TxOutputEqPost[11:8] come from the port's tuning row. */
static uint32_t patch_tx_cfg(uint32_t v, const struct fm6000_serdes_port *p)
{
	v &= ~((0x7u << 12) | (0xfu << 8));
	v |= ((uint32_t)(p->pre & 0x7) << 12) | ((uint32_t)(p->post & 0xf) << 8);
	return v;
}

/* Run one op table, retargeting every port-dependent field to this lane. */
static int run_seq(const char *what, const struct op *seq, int n,
		   uint32_t base, int dev, const struct fm6000_serdes_port *p)
{
	int i;

	printf("  %s: %d ops\n", what, n);
	for (i = 0; i < n; i++) {
		const struct op *o = &seq[i];
		if (o->kind == OP_MMIO) {
			uint32_t v = o->val;
			if (o->off_or_sbop == SERDES_TX_CFG_OFF) v = patch_tx_cfg(v, p);
			wr(base + o->off_or_sbop, v);
		} else {
			/* Two device classes, and they are NOT interchangeable:
			 *   0x49 -- the lane's own SerDes; retarget to this lane's device
			 *   0xfd -- the SPICO broadcast; stays 0xfd, but its reg 0x03 write
			 *           carries the TARGET device as DATA, so that is what moves.
			 * Retargeting the broadcast device instead of its payload silently
			 * corrupted 16 of the 89 ops until a full-sequence diff caught it.
			 * The payload rule is confirmed against the lane-1 capture, which
			 * writes dev=0xfd reg=0x03 data=0x4a. */
			uint32_t d = o->dev == SEQ_DEV ? (uint32_t)dev : o->dev;
			uint32_t data = o->val;
			if (o->dev == SPICO_BC && o->reg == 0x03u && data == SEQ_DEV)
				data = (uint32_t)dev;
			uint32_t cmd = ((uint32_t)o->off_or_sbop << 16) |
				       ((d & 0xff) << 8) | o->reg | (1u << 24);
			int r = sbus(cmd, data);
			if (r < 0) {
				fprintf(stderr, "  %s: SBus op %d failed rc=%d\n", what, i, r);
				return 1;
			}
		}
		if (!dry && (i & 7) == 0 && rd(PIN) != 0x208u) {
			fprintf(stderr, "  %s: chip went off-bus at op %d\n", what, i);
			return 1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *bdf = "0000:02:00.0";
	char path[256];
	int fd, i, observed, dev, rc = 0;
	long intf = -1;
	const struct fm6000_serdes_port *p;
	uint32_t base;

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-n")) dry = 1;
		else if (!strcmp(argv[i], "-l")) link_only = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) DLY = strtoul(argv[++i], NULL, 0);
		else if (argv[i][0] == '-') { fprintf(stderr,
			"usage: fm6000_lanelink [-n] [-l] [-b bdf] [-d us] <front-panel-port>\n"
			"  -l  link ops only, skip the DFE (RX adaptation) sequence\n"); return 2; }
		else intf = strtol(argv[i], NULL, 0);
	}
	if (intf < 0 || !(p = find_port((unsigned)intf))) {
		fprintf(stderr, "unknown front-panel port %ld\n", intf); return 2;
	}

	base = EPL_BASE + 0x400u * p->epl + 0x80u * p->lane;
	dev  = sbus_dev(p->epl, p->lane, &observed);

	printf("port %u: alta %u, EPL%u lane %u -> MMIO base 0x%05x, SBus dev 0x%02x%s\n",
	       p->intf, p->alta, p->epl, p->lane, base, dev,
	       observed ? "" : "  (EXTRAPOLATED -- not an observed mapping)");
	printf("  tx eq: pre=%u post=%u (drive=%u)\n", p->pre, p->post, p->drive);
	if (!observed && !dry)
		fprintf(stderr, "  refusing to drive an unobserved SBus mapping; use -n to inspect\n");

	if (!dry) {
		if (!observed) return 1;
		snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
		fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0) { perror("open"); return 1; }
		M = mmap(NULL, 32u*1024*1024, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
		if (M == MAP_FAILED) { perror("mmap"); return 1; }
		if (rd(PIN) != 0x208u) { fprintf(stderr, "chip off-bus (PIN=%08x)\n", rd(PIN)); return 1; }
	}

	rc = run_seq("link", SEQ, NSEQ, base, dev, p);

	/* RX adaptation follows the link ops, and only if they completed -- tuning a
	 * lane whose bring-up aborted would adapt to a link that is not there. */
	if (!rc && !link_only) {
		if (!dry) {
			uint32_t st = rd(base + 0x00);
			printf("  after link: PORT_STATUS = %08x  pcsRx = %08x\n",
			       st, rd(base + 0x26));
		}
		rc = run_seq("dfe", DFE, NDFE, base, dev, p);
	}

	if (!dry) {
		uint32_t st = rd(base + 0x00);
		printf("  PORT_STATUS = %08x  pcsRx = %08x\n", st, rd(base + 0x26));
	}
	printf("  %d ops %s\n", link_only ? NSEQ : NSEQ + NDFE,
	       dry ? "(dry run)" : (rc ? "-- ABORTED" : "done"));
	return rc;
}
