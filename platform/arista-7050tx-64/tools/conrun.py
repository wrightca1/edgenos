#!/usr/bin/env python3
"""conrun.py <tty> <cmd>... -- run commands on an EOS console in EITHER state.

conexpect.py assumes the console is sitting at a login prompt. It is not always:
a previous session leaves it logged in at "localhost#", and conexpect then waits
for "login:" that never comes and reports "never reached a CLI prompt" -- which
reads like a dead console rather than an already-open one.

Single open, discard the PL2303 first-open burst, then probe what is actually
there and adapt.
"""
import os, sys, termios, time

tty, cmds = sys.argv[1], sys.argv[2:]
fd = os.open(tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
t = termios.tcgetattr(fd)
t[0] = t[1] = t[3] = 0
t[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
t[4] = t[5] = termios.B9600
t[6] = list(t[6]); t[6][termios.VMIN] = 0; t[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, t)

buf = bytearray()
def drain(sec):
    end = time.time() + sec
    while time.time() < end:
        try:
            d = os.read(fd, 4096)
            if d: buf.extend(d); sys.stdout.write(d.decode("utf-8","replace")); sys.stdout.flush()
        except BlockingIOError: pass
        time.sleep(0.02)

def send(s):
    os.write(fd, s.encode() + b"\r"); time.sleep(0.5)

drain(3.0)                      # first-open burst
buf.clear()
send("")
drain(3.0)
tail = bytes(buf[-200:]).decode("utf-8","replace")

if "login:" in tail:
    send("admin"); drain(2.0)
    if "assword" in bytes(buf[-120:]).decode("utf-8","replace"):
        send("arista"); drain(3.0)
send("enable"); drain(1.5)
send("terminal length 0"); drain(1.5)

for c in cmds:
    send(c)
    drain(8.0)
os.close(fd)
