#!/usr/bin/env python3
# leddance.py - Drive AS5610-52X front-panel LEDs via the BCM56840 CMIC
# LED processors. Loads a passthrough bytecode program into both LEDUP0 and
# LEDUP1 and writes patterns to data RAM.
#
# Usage:
#   leddance.py status                 - show CTRL of both LED processors
#   leddance.py save FILE              - save current programs+state
#   leddance.py restore FILE           - restore programs+state
#   leddance.py load PROG0 PROG1       - load .hex programs into LEDUP0 and LEDUP1
#   leddance.py bit N                  - light LED slot N (0..63 LEDUP0, 64..127 LEDUP1)
#   leddance.py raw HEX0 HEX1          - write 8 bytes of hex to each data RAM (16 hex chars each)
#   leddance.py dance                  - Knight-Rider sweep until Ctrl-C

import ctypes
import fcntl
import os
import struct
import sys
import time

# /dev/linux-kernel-bde ioctl interface (from newnos/asic/bde/linux-kernel-bde.c).
# struct bde_reg_io { u32 dev; u32 addr; u32 val; }  -> 12 bytes
#
# PowerPC ioctl encoding (3-bit dir, 13-bit size, 8-bit type, 8-bit nr):
#   _IOC(dir, type, nr, size) = (dir << 29) | (size << 16) | (type << 8) | nr
#   _IOC_WRITE      = 4
#   _IOC_READ|WRITE = 6
BDE_DEV = "/dev/linux-kernel-bde"
BDE_IOC_TYPE = ord('b')
# REG_READ/WRITE take BAR0 offsets and auto-route through PAXB sub-window 7
# for offsets >= 0x1000 (CMICm). All our LED registers fit this path.
BDE_IOC_REG_READ    = (6 << 29) | (12 << 16) | (BDE_IOC_TYPE << 8) | 1
BDE_IOC_REG_WRITE   = (4 << 29) | (12 << 16) | (BDE_IOC_TYPE << 8) | 2

# CMICm AXI offsets for BCM56840 LED processors (from cdk/.../bcm56840_a0_defs.h).
# Each 32-bit register stores one byte payload; arrays step by 4.
LEDUP0 = {
    "ctrl":           0x1000,
    "scan_assembly":  0x1008,
    "port_remap":     0x0d00,  # 9 entries
    "data_ram":       0x1400,  # 256 entries
    "program_ram":    0x1800,  # 256 entries
}
LEDUP1 = {
    "ctrl":           0x2000,
    "scan_assembly":  0x2008,
    "port_remap":     0x0e00,
    "data_ram":       0x2400,
    "program_ram":    0x2800,
}

PROG_SIZE = 256
DATA_SIZE = 256

class BDE:
    def __init__(self, path=BDE_DEV, dev=0):
        self.fd = os.open(path, os.O_RDWR)
        self.dev = dev

    def close(self):
        os.close(self.fd)

    def iproc_read(self, addr):
        buf = struct.pack("III", self.dev, addr, 0)
        ret = fcntl.ioctl(self.fd, BDE_IOC_REG_READ, buf, True)
        _, _, val = struct.unpack("III", ret)
        return val

    def iproc_write(self, addr, val):
        buf = struct.pack("III", self.dev, addr, val & 0xFFFFFFFF)
        fcntl.ioctl(self.fd, BDE_IOC_REG_WRITE, buf, False)

# --- LED processor helpers ------------------------------------------------

def stop(bde, lp):
    ctrl = bde.iproc_read(lp["ctrl"])
    bde.iproc_write(lp["ctrl"], ctrl & ~0x1)

def start(bde, lp):
    ctrl = bde.iproc_read(lp["ctrl"])
    bde.iproc_write(lp["ctrl"], ctrl | 0x1)

def read_ram(bde, base, n):
    return bytes(bde.iproc_read(base + i*4) & 0xFF for i in range(n))

def write_ram(bde, base, data, n=None):
    if n is None:
        n = len(data)
    for i in range(n):
        bde.iproc_write(base + i*4, data[i] if i < len(data) else 0)

def load_program(bde, lp, prog):
    stop(bde, lp)
    write_ram(bde, lp["program_ram"], prog, PROG_SIZE)
    # Clear data RAM 0x80..0xFF (per Broadcom convention)
    for off in range(0x80, DATA_SIZE):
        bde.iproc_write(lp["data_ram"] + off*4, 0)
    start(bde, lp)

def parse_hex_program(text):
    """Parse Broadcom .hex format (whitespace-separated bytes) into a 256-byte buffer."""
    nums = []
    for tok in text.split():
        try:
            nums.append(int(tok, 16))
        except ValueError:
            pass
    if len(nums) > PROG_SIZE:
        nums = nums[:PROG_SIZE]
    return bytes(nums + [0] * (PROG_SIZE - len(nums)))

# --- Subcommands ----------------------------------------------------------

def cmd_status(args):
    bde = BDE()
    for name, lp in (("LEDUP0", LEDUP0), ("LEDUP1", LEDUP1)):
        ctrl = bde.iproc_read(lp["ctrl"])
        scan = bde.iproc_read(lp["scan_assembly"])
        en = ctrl & 1
        intra = (ctrl >> 1) & 7
        start_d = (ctrl >> 4) & 0xF
        prog0 = read_ram(bde, lp["program_ram"], 16)
        data_a0 = read_ram(bde, lp["data_ram"] + 0xA0*4, 8)
        print(f"{name}: ctrl=0x{ctrl:08x} en={en} intra_delay={intra} start_delay={start_d}")
        print(f"        scan_assembly_st_addr=0x{scan:02x}")
        print(f"        program[0..15] = {prog0.hex(' ')}")
        print(f"        data[0xA0..0xA7] = {data_a0.hex(' ')}")
    bde.close()

def cmd_save(args):
    path = args[0]
    bde = BDE()
    state = {}
    for name, lp in (("LEDUP0", LEDUP0), ("LEDUP1", LEDUP1)):
        state[name] = {
            "ctrl": bde.iproc_read(lp["ctrl"]),
            "scan_assembly": bde.iproc_read(lp["scan_assembly"]),
            "program": read_ram(bde, lp["program_ram"], PROG_SIZE).hex(),
            "data": read_ram(bde, lp["data_ram"], DATA_SIZE).hex(),
            "port_remap": [bde.iproc_read(lp["port_remap"] + i*4) for i in range(9)],
        }
    bde.close()
    import json
    with open(path, "w") as f:
        json.dump(state, f, indent=2)
    print(f"Saved state to {path}")

def cmd_restore(args):
    path = args[0]
    import json
    with open(path) as f:
        state = json.load(f)
    bde = BDE()
    for name, lp in (("LEDUP0", LEDUP0), ("LEDUP1", LEDUP1)):
        s = state[name]
        stop(bde, lp)
        write_ram(bde, lp["program_ram"], bytes.fromhex(s["program"]), PROG_SIZE)
        write_ram(bde, lp["data_ram"], bytes.fromhex(s["data"]), DATA_SIZE)
        for i, v in enumerate(s["port_remap"]):
            bde.iproc_write(lp["port_remap"] + i*4, v)
        bde.iproc_write(lp["scan_assembly"], s["scan_assembly"])
        bde.iproc_write(lp["ctrl"], s["ctrl"])
    bde.close()
    print(f"Restored state from {path}")

def cmd_load(args):
    p0_path, p1_path = args[0], args[1]
    with open(p0_path) as f: prog0 = parse_hex_program(f.read())
    with open(p1_path) as f: prog1 = parse_hex_program(f.read())
    bde = BDE()
    load_program(bde, LEDUP0, prog0)
    load_program(bde, LEDUP1, prog1)
    bde.close()
    print(f"Loaded {p0_path} into LEDUP0 and {p1_path} into LEDUP1")

def cmd_bit(args):
    """Light a single bit in the chain. Slots 0..63 = LEDUP0, 64..127 = LEDUP1."""
    n = int(args[0])
    if not (0 <= n < 128):
        sys.exit(f"bit out of range: {n}")
    if n < 64:
        lp, slot = LEDUP0, n
    else:
        lp, slot = LEDUP1, n - 64
    byte_off = 0xA0 + (slot // 8)
    bit = slot & 7
    bde = BDE()
    # Clear all 8 data bytes on both processors first
    for proc in (LEDUP0, LEDUP1):
        for i in range(8):
            bde.iproc_write(proc["data_ram"] + (0xA0+i)*4, 0)
    bde.iproc_write(lp["data_ram"] + byte_off*4, 1 << bit)
    bde.close()
    print(f"Bit {n}: {'LEDUP0' if n<64 else 'LEDUP1'} byte 0x{byte_off:02x} bit {bit}")

def cmd_raw(args):
    h0, h1 = args[0], args[1]
    b0 = bytes.fromhex(h0); b1 = bytes.fromhex(h1)
    if len(b0) != 8 or len(b1) != 8:
        sys.exit("each hex pattern must be 16 hex chars (8 bytes)")
    bde = BDE()
    for i in range(8):
        bde.iproc_write(LEDUP0["data_ram"] + (0xA0+i)*4, b0[i])
        bde.iproc_write(LEDUP1["data_ram"] + (0xA0+i)*4, b1[i])
    bde.close()

# Front-panel-port → (processor, amber_bit_offset) table.
# Each port occupies 2 consecutive chain bits: amber first, green second.
# Source: cumulus/platforms/accton.py AcctonAS5610_52XSwitch.ports
# (extracted from extracted/2.5.1-powerpc).
PANEL_PORTS = {
    1: (1, 34), 2: (1, 32), 3: (1, 38), 4: (1, 36),
    5: (1, 62), 6: (1, 60), 7: (1, 58), 8: (1, 56),
    9: (0, 2), 10: (0, 0), 11: (0, 6), 12: (0, 4),
    13: (0, 50), 14: (0, 48), 15: (0, 54), 16: (0, 52),
    17: (0, 46), 18: (0, 44), 19: (0, 42), 20: (0, 40),
    21: (0, 62), 22: (0, 60), 23: (0, 58), 24: (0, 56),
    25: (0, 38), 26: (0, 36), 27: (0, 34), 28: (0, 32),
    29: (0, 30), 30: (0, 28), 31: (0, 26), 32: (0, 24),
    33: (0, 14), 34: (0, 8), 35: (0, 10), 36: (0, 12),
    37: (0, 22), 38: (0, 20), 39: (0, 18), 40: (0, 16),
    41: (1, 44), 42: (1, 42), 43: (1, 40), 44: (1, 46),
    45: (1, 52), 46: (1, 50), 47: (1, 48), 48: (1, 54),
    49: (1, 26), 50: (1, 24), 51: (1, 30), 52: (1, 28),
}
PANEL_PORT_LIST = sorted(PANEL_PORTS.keys())   # [1..52]

def write_chain(bde, bits128):
    """Write 128 chain bits (16 bytes: 8 for LEDUP0, then 8 for LEDUP1)."""
    for proc_idx, proc in enumerate((LEDUP0, LEDUP1)):
        base = proc["data_ram"]
        for i in range(8):
            byte = bits128[proc_idx*8 + i]
            bde.iproc_write(base + (0xA0+i)*4, byte)

def set_panel_port(bits128, panel_port, amber, green):
    """Set front-panel port (1..52) amber/green bits in a 16-byte buffer."""
    if panel_port not in PANEL_PORTS:
        return
    proc, amber_bit = PANEL_PORTS[panel_port]
    green_bit = amber_bit + 1
    for bit, on in ((amber_bit, amber), (green_bit, green)):
        byte_idx = proc*8 + (bit // 8)
        bit_idx = bit & 7
        if on:
            bits128[byte_idx] |= 1 << bit_idx
        else:
            bits128[byte_idx] &= ~(1 << bit_idx)

def cmd_dance(args):
    """Knight Rider in physical-panel order: green sweeps right, amber sweeps left."""
    delay = float(args[0]) if args else 0.05
    bde = BDE()
    ports = PANEL_PORT_LIST
    n = len(ports)
    g, a = 0, n - 1
    g_dir, a_dir = 1, -1
    try:
        while True:
            bits = bytearray(16)
            set_panel_port(bits, ports[g], amber=False, green=True)
            set_panel_port(bits, ports[a], amber=True, green=False)
            if g == a:
                set_panel_port(bits, ports[g], amber=True, green=True)
            write_chain(bde, bits)
            time.sleep(delay)
            g += g_dir
            a += a_dir
            if g >= n - 1 or g <= 0: g_dir = -g_dir; g = max(0, min(n-1, g))
            if a >= n - 1 or a <= 0: a_dir = -a_dir; a = max(0, min(n-1, a))
    except KeyboardInterrupt:
        pass
    finally:
        write_chain(bde, bytearray(16))
        bde.close()
        print("\nstopped")

def cmd_walk(args):
    """Walk each panel port 1..52 in order, lit yellow."""
    delay = float(args[0]) if args else 0.3
    bde = BDE()
    try:
        for port in PANEL_PORT_LIST:
            bits = bytearray(16)
            set_panel_port(bits, port, amber=True, green=True)
            write_chain(bde, bits)
            proc, abit = PANEL_PORTS[port]
            print(f"swp{port:<2d}  proc={proc} amber_bit={abit:2d} green_bit={abit+1:2d}", flush=True)
            time.sleep(delay)
    except KeyboardInterrupt:
        pass
    finally:
        write_chain(bde, bytearray(16))
        bde.close()

def cmd_swp(args):
    """Light one or more panel ports. Usage: swp <port> [color]  (color = green|amber|yellow)"""
    if not args:
        sys.exit("usage: swp <panel_port> [green|amber|yellow]")
    port = int(args[0])
    color = args[1] if len(args) > 1 else "yellow"
    a = color in ("amber", "yellow")
    g = color in ("green", "yellow")
    bde = BDE()
    bits = bytearray(16)
    set_panel_port(bits, port, amber=a, green=g)
    write_chain(bde, bits)
    bde.close()

# ---------- pattern primitives -----------------------------------------

def _color_for_blend(blend):
    # blend in [0..1]:  0 = green, 0.5 = yellow, 1 = amber
    g = blend < 0.66
    a = blend > 0.33
    return a, g

def pat_knight(t, n):
    """Two dots crossing. Returns dict {port_idx: (amber, green)}."""
    period = 80
    half = period // 2
    pos = t % period
    g_pos = pos if pos < half else (period - 1 - pos)
    a_pos = (n - 1 - g_pos)
    out = {g_pos: (False, True), a_pos: (True, False)}
    if g_pos == a_pos:
        out[g_pos] = (True, True)
    return out

def pat_wave(t, n):
    """Color wave rolling across panel: green→yellow→amber→off."""
    out = {}
    for i in range(n):
        phase = ((t + i*3) % 60) / 60.0
        if phase < 0.7:
            out[i] = _color_for_blend(phase / 0.7)
        # else: dark gap
    return out

def pat_trails(t, n):
    """Two dots with fading trails."""
    out = {}
    pos = t % (n * 2)
    if pos >= n:
        pos = (n*2 - 1) - pos
    # green trail behind moving dot
    for offset in range(6):
        i = pos - offset
        if 0 <= i < n:
            out[i] = (offset >= 3, True if offset < 4 else False)
    # amber dot from the other side
    pos2 = n - 1 - pos
    for offset in range(6):
        i = pos2 + offset
        if 0 <= i < n:
            existing = out.get(i, (False, False))
            out[i] = (True, existing[1] if offset < 3 else False)
    return out

def pat_sparkle(t, n):
    """Random ports flashing in random colors."""
    import random
    rng = random.Random(t // 4)
    out = {}
    k = 8 + rng.randint(0, 6)
    for _ in range(k):
        i = rng.randrange(n)
        c = rng.choice([(False, True), (True, False), (True, True)])
        out[i] = c
    return out

def pat_strobe(t, n):
    """Whole panel toggles colors."""
    phase = (t // 4) % 4
    if phase == 0:
        return {i: (False, True) for i in range(n)}
    if phase == 1:
        return {i: (True, False) for i in range(n)}
    if phase == 2:
        return {i: (True, True) for i in range(n)}
    return {}  # dark

def pat_chase(t, n):
    """Multiple chaser dots evenly spaced."""
    out = {}
    spacing = 8
    for k in range(n // spacing + 1):
        pos = (t + k*spacing) % n
        if k % 2 == 0:
            out[pos] = (False, True)
        else:
            out[pos] = (True, False)
    return out

PATTERNS = {
    "knight": pat_knight,
    "wave":   pat_wave,
    "trails": pat_trails,
    "sparkle": pat_sparkle,
    "strobe": pat_strobe,
    "chase":  pat_chase,
}

def render(bde, mapping):
    """Render a {port_index: (amber, green)} mapping (port_index = 0..51) to LEDs."""
    bits = bytearray(16)
    for idx, (a, g) in mapping.items():
        if 0 <= idx < len(PANEL_PORT_LIST):
            set_panel_port(bits, PANEL_PORT_LIST[idx], amber=a, green=g)
    write_chain(bde, bits)

def cmd_pattern(args):
    """Run a single named pattern. Usage: pattern <name> [delay]"""
    if not args or args[0] not in PATTERNS:
        sys.exit(f"usage: pattern <name> [delay]; names: {', '.join(PATTERNS)}")
    fn = PATTERNS[args[0]]
    delay = float(args[1]) if len(args) > 1 else 0.04
    bde = BDE()
    n = len(PANEL_PORT_LIST)
    t = 0
    try:
        while True:
            render(bde, fn(t, n))
            time.sleep(delay)
            t += 1
    except KeyboardInterrupt:
        pass
    finally:
        write_chain(bde, bytearray(16))
        bde.close()

def cmd_party(args):
    """Cycle through every pattern, ~6 seconds each, fast."""
    delay = float(args[0]) if args else 0.035
    seg_steps = int(float(args[1]) / delay) if len(args) > 1 else int(6.0 / delay)
    bde = BDE()
    n = len(PANEL_PORT_LIST)
    pats = list(PATTERNS.values())
    p = 0
    t = 0
    seg_t = 0
    try:
        while True:
            render(bde, pats[p](t, n))
            time.sleep(delay)
            t += 1
            seg_t += 1
            if seg_t >= seg_steps:
                seg_t = 0
                p = (p + 1) % len(pats)
                t = 0
    except KeyboardInterrupt:
        pass
    finally:
        write_chain(bde, bytearray(16))
        bde.close()

CMDS = {
    "status": cmd_status,
    "save": cmd_save,
    "restore": cmd_restore,
    "load": cmd_load,
    "bit": cmd_bit,
    "raw": cmd_raw,
    "dance": cmd_dance,
    "walk": cmd_walk,
    "swp": cmd_swp,
    "pattern": cmd_pattern,
    "party": cmd_party,
}

def main():
    if len(sys.argv) < 2 or sys.argv[1] not in CMDS:
        print(__doc__ if __doc__ else "see header for commands", file=sys.stderr)
        print("commands:", ", ".join(CMDS), file=sys.stderr)
        sys.exit(1)
    CMDS[sys.argv[1]](sys.argv[2:])

if __name__ == "__main__":
    main()
