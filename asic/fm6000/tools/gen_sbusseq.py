#!/usr/bin/env python3
# gen_sbusseq.py - emit fm6000_sbusseq.c, the SerDes SBus bring-up sequence.
#
# SBus is the largest single block left in the residual: 3,721 writes, 23% of it,
# and all of them land on three addresses -- SBUS_CFG, SBUS_REQUEST and
# SBUS_COMMAND. It is a serial bus being driven, not a table being filled, which
# is why no write-once generator could ever cover it.
#
# THE TRANSACTION
#
#   SBUS_REQUEST  <data>        payload, only when non-zero
#   SBUS_COMMAND  0             idle/clear
#   SBUS_COMMAND  01.DD.RR.VV   execute: device DD, register RR, value VV
#
# 1,239 transactions, and they use only THIRTEEN distinct (device, register)
# pairs. Devices 0x20, 0x21, 0x22; registers fd, fe, 01-04, 45, 49.
#
# THE PROGRAM
#
#   1. one setup transaction        21.fe = 0a, REQ=4
#   2. 84 x an eight-transaction toggle on device 0x22:
#          reg 01,02,03,04, each written 00 then 0f
#      no parameter varies across the 84 iterations -- it is a pulse train
#   3. a short preamble on 21.fd / 22.fd
#   4. ~50 x a nine-transaction unit that alternates between two targets:
#          21.fd=01 / 21.fd=02 REQ=20 / 21.fd=03 REQ=45|49 /
#          21.fd=0c REQ=18 / 21.fd=0c REQ=08 /
#          22.fd=01 / 22.fd=00 / 22.fd=02 / 21.{45|49}=2a REQ=0e|16
#
# ⚠ SBUS IS A HANDSHAKE, NOT THREE WRITES. fm6000_fullreplay does NOT replay
# these addresses literally -- it interprets them:
#
#     if (a == 0xF002) { pend = v; continue; }          /* stash, no write */
#     if (a == 0xF001) { if (!v) continue; sbus(v, pend); continue; }
#
# and sbus() writes the three words then POLLS SBUS_COMMAND until busy (bit 25)
# clears, up to 200,000 reads. A first version of this generator emitted the
# captured writes faithfully with no poll, and took Et2 down on 5 boots out of 5
# (PORT_STATUS 0x0815, LANE_STATUS 0) while the file-driven replay kept it up 4
# times in 5. Issuing the next transaction before the previous one completes
# corrupts it, and SerDes bring-up is where that shows.
#
# Reproducing a captured TRACE is not the same as reproducing the BEHAVIOUR that
# produced it. The trace contains writes the vendor's own replay engine
# deliberately does not perform.
#
# ⚠ WHAT IS AND IS NOT UNDERSTOOD. The STRUCTURE is recovered and exact -- the
# loop bounds, the alternation, and the fact that 84 iterations carry no varying
# parameter. Two device numbers are named by docs/EDGENOS-7150.md (was PORT3-BRINGUP): SBus device
# 0x45 is Et2's EPL16 lane and 0x49 is Et1's, which is why the ~50-iteration unit
# alternates between them. The rest of the register SEMANTICS are not established:
# the SDK's SBus field table names registers 0x00-0x35 (sbus_rx_data_gate,
# sbus_tx_output_en_cntl, ...) but these devices' register space does not line up
# with it, and 0xfd / 0xfe are outside it entirely. So this is an honest
# transcription of a PROGRAM rather than of a data table -- better than 3,721
# literals, short of authorship from intent. Do not describe it as the latter.
#
# Usage: gen_sbusseq.py <stream with the SBus writes> > fm6000_sbusseq.c
# SPDX-License-Identifier: GPL-2.0-or-later
import sys, collections

SBUS_CFG, SBUS_COMMAND, SBUS_REQUEST = 0x00f000, 0x00f001, 0x00f002

def read(path):
    o = []
    for line in open(path):
        f = line.split()
        if len(f) == 2:
            try: o.append((int(f[0], 16), int(f[1], 16)))
            except ValueError: pass
    return o

def compress(steps, maxp):
    """Collapse a step list by SEQUENCE periodicity, not run length.

    The 84-iteration toggle alternates (01=00, 01=0f, 02=00, ...), so
    consecutive steps differ and plain run-length encoding collapses almost
    nothing -- it produced 1,182 units for 1,239 transactions, which is
    transcription with extra ceremony. Finding the repeating BLOCK turns those
    672 transactions into one 8-step unit with a repeat count."""
    out, i = [], 0
    while i < len(steps):
        best_p, best_n = 1, 1
        for per in range(1, min(maxp, len(steps) - i) + 1):
            unit = steps[i:i + per]
            n = 1
            while steps[i + n * per:i + (n + 1) * per] == unit: n += 1
            if n * per > best_n * best_p: best_p, best_n = per, n
        out.append([steps[i:i + best_p], best_n])
        i += best_p * best_n
    return out


def main():
    # Every SBus-block address, not a hand-picked three. The first attempt
    # filtered on {CFG, COMMAND, REQUEST} and silently dropped two writes to
    # 0x00f004, which showed up as a divergence 2,021 writes in.
    rows = [w for w in read(sys.argv[1]) if 0x00f000 <= w[0] <= 0x00f00f]
    # One ordered step list. A CFG write is its own step kind: hoisting them to
    # the front produced a stream that matched for exactly one write and then
    # diverged, because the second CFG write happens PART WAY THROUGH.
    #   kind 0: a raw write, {addr, value}
    #   kind 1: a transaction, REQUEST=payload, COMMAND=0, COMMAND=cmd
    # ⚠ SBus programming is NOT one lump. In the executed stream these writes
    # form TWELVE separate runs, interleaved with the rest of the bring-up --
    # the SerDes is brought up alongside the ports. A generator that emitted all
    # 3,721 writes in one go could never be scheduled, because no single point in
    # the stream is the right place for them. So the output is SEGMENTED: -s N
    # emits run N only, and the schedule places each one where it belongs.
    runs, cur, prev = [], [], None
    for a, v in read(sys.argv[1]):
        inb = 0x00f000 <= a <= 0x00f00f
        if inb:
            if prev is False and cur: runs.append(cur); cur = []
            cur.append((a, v))
        prev = inb
    if cur: runs.append(cur)

    segs = []
    for run in runs:
        steps, pend = [], 0
        for a, v in run:
            if a not in (SBUS_REQUEST, SBUS_COMMAND): steps.append((0, a, v))
            elif a == SBUS_REQUEST: pend = v
            elif a == SBUS_COMMAND and v != 0:
                steps.append((1, pend, v)); pend = 0
        segs.append(steps)
    steps = [x for seg in segs for x in seg]
    # Compress by SEQUENCE periodicity, not run length. The 84-iteration toggle
    # alternates (01=00, 01=0f, 02=00, ...), so consecutive transactions differ
    # and plain run-length encoding collapses almost nothing -- it produced 1,182
    # steps for 1,239 transactions, which is transcription with extra ceremony.
    # Finding the repeating BLOCK turns those 672 transactions into one 8-step
    # unit with a repeat count.
    MAXP = 64
    rle, seg_units = [], []
    for seg in segs:
        u0 = len(rle)
        rle.extend(compress(seg, MAXP))
        seg_units.append((u0, len(rle) - u0))

    out = sys.stdout.write
    out("/* fm6000_sbusseq.c - SerDes SBus bring-up sequence.\n"
        " * GENERATED by asic/fm6000/tools/gen_sbusseq.py -- do not edit by hand.\n"
        " *\n"
        " * %d transactions on three registers, expressed as %d program steps.\n"
        " * Each step is {payload, command, repeat}: write SBUS_REQUEST when the\n"
        " * payload is non-zero, then SBUS_COMMAND 0, then the command word\n"
        " * 01.DD.RR.VV (device, register, value).\n"
        " *\n"
        " * ⚠ The structure is recovered exactly; the register SEMANTICS are not.\n"
        " * See gen_sbusseq.py for what that means.\n"
        " * SPDX-License-Identifier: GPL-2.0-or-later\n */\n"
        % (len(steps), len(rle)))
    out("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <fcntl.h>\n#include <unistd.h>\n#include <stdint.h>\n"
        "#include <sys/mman.h>\n\n")
    out("#define SBUS_CFG      0x%06xu\n#define SBUS_COMMAND  0x%06xu\n"
        "#define SBUS_REQUEST  0x%06xu\n\n" % (SBUS_CFG, SBUS_COMMAND, SBUS_REQUEST))
    out("static volatile uint32_t *M;\n\n")
    flat, units = [], []
    for unit, n in rle:
        units.append((len(flat), len(unit), n))
        flat.extend(unit)
    out("/* distinct steps: {kind, a, b};  kind 0 = raw write a=b,\n"
        " * kind 1 = transaction with payload a and command b */\n"
        "static const uint32_t T[][3] = {\n")
    for kind, payload, cmd in flat:
        out("\t{ %d, 0x%08xu, 0x%08xu },\n" % (kind, payload, cmd))
    out("};\n\n/* the program: {first transaction, length, repeat count} */\n"
        "static const uint16_t U[][3] = {\n")
    for off, ln, n in units:
        out("\t{ %4d, %2d, %4d },\n" % (off, ln, n))
    out("};\n\n/* segments: {first unit, unit count} -- one per interleaved run */\n"
        "static const uint16_t SEG[][2] = {\n")
    for u0, n in seg_units:
        out("\t{ %3d, %3d },\n" % (u0, n))
    out("};\n\n")
    out('''static int dry, list_only, nw, seg = -1;   /* -s N: emit run N only */
static long sbus_done, sbus_timeout;

/* Wait for the SBus to finish the transaction just issued: busy is bit 25 of
 * SBUS_COMMAND. This mirrors fm6000_fullreplay's sbus(), and it is not optional
 * -- without it Et2 went down on 5 boots out of 5. 0xffffffff means the chip is
 * off the bus, in which case there is nothing to wait for. */
static void sbus_wait(void)
{
\tlong i;
\tif (dry || list_only) return;
\tfor (i = 0; i < 200000; i++) {
\t\tuint32_t s = M[SBUS_COMMAND];
\t\t__sync_synchronize();
\t\tif (s == 0xffffffffu) return;
\t\tif (!(s & (1u << 25))) { sbus_done++; return; }
\t}
\tsbus_timeout++;
}

static void emit(uint32_t addr, uint32_t val)
{
\tif (list_only)   { printf("%08x\\n", addr); return; }
\tif (dry)         { printf("%08x %08x\\n", addr, val); nw++; return; }
\tM[addr] = val; __sync_synchronize(); nw++;
}

int main(int argc, char **argv)
{
\tconst char *bdf = "0000:02:00.0";
\tint i; size_t u, r, t;

\tfor (i = 1; i < argc; i++) {
\t\tif      (!strcmp(argv[i], "-n")) dry = 1;
\t\telse if (!strcmp(argv[i], "-a")) list_only = 1;
\t\telse if (!strcmp(argv[i], "-b") && i + 1 < argc) bdf = argv[++i];
\t\telse if (!strcmp(argv[i], "-s") && i + 1 < argc) seg = atoi(argv[++i]);
\t\telse { fprintf(stderr, "usage: %s [-n|-a] [-b bdf]\\n", argv[0]); return 2; }
\t}

\tif (list_only) {
\t\t/* Three addresses, each written many times. -a exists so make_residual
\t\t * can subtract what a generator covers; claiming these three is correct
\t\t * ONLY because this generator reproduces every write to them. */
\t\tsize_t k;
\t\tuint32_t seen = 0;
\t\tfor (k = 0; k < sizeof(T) / sizeof(T[0]); k++)
\t\t\tif (T[k][0] == 0 && !(seen & (1u << (T[k][1] & 0xf)))) {
\t\t\t\tseen |= 1u << (T[k][1] & 0xf);
\t\t\t\tprintf("%08x\\n", T[k][1]);
\t\t\t}
\t\tprintf("%08x\\n%08x\\n", SBUS_REQUEST, SBUS_COMMAND);
\t\treturn 0;
\t}

\tif (!dry) {
\t\tchar path[256];
\t\tint fd;
\t\tsnprintf(path, sizeof path, "/sys/bus/pci/devices/%s/resource0", bdf);
\t\tfd = open(path, O_RDWR | O_SYNC);
\t\tif (fd < 0) { perror("open resource0"); return 1; }
\t\tM = mmap(NULL, 32u * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
\t\tif (M == MAP_FAILED) { perror("mmap"); return 1; }
\t}

\t{
\t\tsize_t ufirst = 0, ulast = sizeof(U) / sizeof(U[0]);
\t\tif (seg >= 0) {
\t\t\tif ((size_t)seg >= sizeof(SEG) / sizeof(SEG[0])) {
\t\t\t\tfprintf(stderr, "fm6000_sbusseq: no segment %d (have %zu)\\n",
\t\t\t\t        seg, sizeof(SEG) / sizeof(SEG[0]));
\t\t\t\treturn 2;
\t\t\t}
\t\t\tufirst = SEG[seg][0];
\t\t\tulast  = (size_t)(SEG[seg][0] + SEG[seg][1]);
\t\t}
\tfor (u = ufirst; u < ulast; u++)
\t\tfor (r = 0; r < U[u][2]; r++)
\t\t\tfor (t = U[u][0]; t < (size_t)(U[u][0] + U[u][1]); t++) {
\t\t\t\t/* REQUEST is written even when the payload is zero: the
\t\t\t\t * captured stream does, and a stale non-zero payload from
\t\t\t\t * the previous transaction would otherwise be reused. */
\t\t\t\tif (T[t][0] == 0) { emit(T[t][1], T[t][2]); continue; }
\t\t\t\temit(SBUS_REQUEST, T[t][1]);
\t\t\t\temit(SBUS_COMMAND, 0);
\t\t\t\temit(SBUS_COMMAND, T[t][2]);
\t\t\t\tsbus_wait();
\t\t\t}

\t}
\tif (!dry) fprintf(stderr, "fm6000_sbusseq: %d writes, %ld transactions acked, "
\t                  "%ld timed out\\n", nw, sbus_done, sbus_timeout);
\treturn 0;
}
''')

if __name__ == "__main__":
    main()
