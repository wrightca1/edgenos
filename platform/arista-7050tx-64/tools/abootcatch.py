#!/usr/bin/env python3
"""abootcatch.py <tty> -- catch the Aboot prompt on reboot, then optionally
run commands there.

The Control-C window at the Aboot banner is short, and a fixed-timer sender
misses it. This watches the stream for the banner and only then starts sending
Control-C, repeatedly, until a prompt appears. It never sends Control-C before
the banner, so it cannot disturb a running EOS.

Modes:
  --catch            wait for a reboot, grab the Aboot prompt, sit there
  --send "CMD"       assume we are ALREADY at a prompt; send CMD and log the reply
                     (repeatable, sent in order)

Everything read is echoed to stdout and appended to --log, so a run is always
reviewable afterwards even if the match logic gets it wrong.

Read-only by default: with neither --catch nor --send it just listens.
"""
import argparse
import os
import re
import sys
import termios
import time

ap = argparse.ArgumentParser()
ap.add_argument("tty")
ap.add_argument("-b", "--baud", type=int, default=9600)
ap.add_argument("--catch", action="store_true",
                help="wait for the Aboot banner and interrupt into the shell")
ap.add_argument("--from-start", action="store_true",
                help="send Control-C from the moment we start, without waiting "
                     "for the banner -- use only when a reboot is already on its "
                     "way, since it types into whatever is listening")
ap.add_argument("--send", action="append", default=[],
                help="command to send once at a prompt (repeatable)")
ap.add_argument("--timeout", type=float, default=420.0,
                help="overall seconds to wait for the banner")
ap.add_argument("--settle", type=float, default=20.0,
                help="seconds to keep reading after the last action")
ap.add_argument("--log", default="/tmp/abootcatch.log")
a = ap.parse_args()

BAUDS = {9600: termios.B9600, 115200: termios.B115200}
fd = os.open(a.tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
t = termios.tcgetattr(fd)
t[0] = t[1] = t[3] = 0
t[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
t[4] = t[5] = BAUDS[a.baud]
t[6] = list(t[6])
t[6][termios.VMIN] = 0
t[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, t)

log = bytearray()
logf = open(a.log, "ab")


def pump(seconds):
    """Read for `seconds`, echoing and logging. Returns text read in this call."""
    got = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        try:
            d = os.read(fd, 4096)
            if d:
                got.extend(d)
                log.extend(d)
                logf.write(d)
                logf.flush()
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()
                continue
        except BlockingIOError:
            pass
        time.sleep(0.02)
    return got.decode("utf-8", "replace")


def tail(n=3000):
    return log[-n:].decode("utf-8", "replace")


# The Aboot shell prompt. Observed forms vary by Aboot version, so accept a
# bare "Aboot#" as well as a generic shell prompt at line start.
PROMPT = re.compile(r"(Aboot[^\n]*#|\n#\s*$|\n[a-zA-Z0-9_.-]+#\s*$)")
BANNER = re.compile(r"Aboot\s+[\d.]+-|Press\s+Control-C")

if a.catch:
    print("\n=== abootcatch: waiting up to %.0fs for the Aboot banner ===" % a.timeout,
          flush=True)
    end = time.time() + a.timeout
    seen_banner = False

    # ⚠ Detecting the banner and only THEN sending Control-C loses the race.
    # Aboot's window is about two seconds and the banner is the first thing it
    # prints, so by the time 0.5 s of polling has noticed it and the first ^C is
    # on a 9600-baud wire, Aboot has already committed to booting -- observed
    # 2026-08-18: banner seen, 7 ^C sent, and the box booted EOS regardless,
    # echoing the ^C at the login prompt a minute later. --from-start removes
    # the race by sending ^C continuously from before the banner exists.
    if a.from_start:
        print("\n=== abootcatch: --from-start, sending Control-C continuously ===",
              flush=True)
        seen_banner = True

    while time.time() < end:
        if seen_banner:
            os.write(fd, b"\x03")
            pump(0.1)
        else:
            pump(0.5)
        if not seen_banner and BANNER.search(tail(4000)):
            seen_banner = True
            print("\n=== abootcatch: BANNER SEEN -- sending Control-C ===", flush=True)
        if seen_banner and PROMPT.search(tail(1500)):
            print("\n=== abootcatch: AT A PROMPT ===", flush=True)
            break
    else:
        print("\n=== abootcatch: TIMED OUT waiting for the banner ===", flush=True)
    # Nudge once so the prompt is freshly printed in the log.
    os.write(fd, b"\r")
    pump(3.0)

for cmd in a.send:
    print("\n=== abootcatch: SEND: %s ===" % cmd, flush=True)
    os.write(fd, cmd.encode() + b"\r")
    pump(a.settle)

if not a.catch and not a.send:
    print("\n=== abootcatch: listen-only for %.0fs ===" % a.settle, flush=True)
    pump(a.settle)

print("\n=== abootcatch: done, %d bytes logged to %s ===" % (len(log), a.log),
      flush=True)
