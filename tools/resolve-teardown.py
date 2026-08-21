#!/usr/bin/env python3
"""resolve-teardown.py <fine-DARK.log> <fwd-executed.txt>

Turn a dark-boot in-replay trace into the actual writes that tore Et2's link down.

The trace samples Et2 every 1k ops through 163,840..212,992. Et2 reaches
LANE_STATUS=0x0940 (block lock) and then loses it; the writes between the last
locked sample and the first unlocked one are the suspects.

⚠ Op numbers index the file the replay ACTUALLY executed -- a generated variant,
not /mnt/flash/fwd4.txt. Indexing the wrong file once gave 1,024 MMIO writes and
zero SBus against an mmio counter showing 811, which is how the mistake was
caught. This takes the executed file explicitly for that reason.
"""
import sys, re
from collections import Counter

if len(sys.argv) < 3:
    sys.exit(__doc__)
trace, executed = sys.argv[1], sys.argv[2]

samples = []
for ln in open(trace):
    m = re.search(r"^\s*(\d+) ops .*et2=0x([0-9a-f]+)/([0-9a-f]+)", ln)
    if m:
        samples.append((int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16)))
if not samples:
    sys.exit("no samples parsed")

locked = [s for s in samples if s[2] == 0x940]
if not locked:
    sys.exit("this trace never reached block lock -- not a teardown boot")
last_locked = max(s[0] for s in locked)
after = [s for s in samples if s[0] > last_locked and s[2] != 0x940]
if not after:
    print(f"link held to the end (last locked at op {last_locked}) -- this is a GOOD boot")
    sys.exit(0)
first_lost = min(s[0] for s in after)
print(f"link LOCKED through op {last_locked}")
print(f"link LOST by       op {first_lost}")
print(f"suspect window: {first_lost - last_locked} ops\n")

n = 0; win = []
for ln in open(executed):
    p = ln.split()
    if len(p) != 2:
        continue
    try:
        a = int(p[0], 16); v = int(p[1], 16)
    except ValueError:
        continue
    n += 1
    if last_locked < n <= first_lost:
        win.append((n, a, v))
print(f"writes in window: {len(win)}   (executed file has {n} parsed ops)")

sb = [w for w in win if w[1] in (0xF001, 0xF002, 0xF003)]
mm = [w for w in win if w[1] not in (0xF001, 0xF002, 0xF003)]
print(f"  MMIO {len(mm)}   SBus-register {len(sb)}\n")

print("  MMIO regions touched (addr >> 12):")
for a, c in Counter(w[1] >> 12 for w in mm).most_common(12):
    print(f"    0x{a:04x}xxx : {c}")

devs = Counter()
for _, a, v in sb:
    if a == 0xF001 and v:
        devs[(v >> 8) & 0xff] += 1
if devs:
    print("\n  SBus devices addressed:")
    tags = {0x45: "Et2 SerDes", 0x49: "Et1 SerDes", 0x4a: "Et3 SerDes",
            0xfd: "SPICO bcast", 0xfe: "bus controller"}
    for d, c in devs.most_common():
        print(f"    dev 0x{d:02x}: {c}  {tags.get(d,'')}")

# Anything touching Et2's own EPL block is the prime suspect
et2 = [w for w in mm if 0xe4000 <= w[1] <= 0xe43ff]
print(f"\n  ★ writes into Et2's EPL block (0xe4000-0xe43ff): {len(et2)}")
for n_, a, v in et2[:20]:
    print(f"    op {n_}  0x{a:05x} <- 0x{v:08x}")
