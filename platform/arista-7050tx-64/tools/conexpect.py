#!/usr/bin/env python3
"""conexpect.py <tty> -- log into an EOS console and run read-only commands.

Waits for each prompt instead of sending on a fixed timer, which is what a
ZTP-mode box needs: it interleaves %ZTP-5-DHCP_QUERY messages into the login
prompt and a timed sender lands its password in the middle of one.
"""
import sys, os, time, termios, argparse, re

ap = argparse.ArgumentParser()
ap.add_argument("tty")
ap.add_argument("-b", "--baud", type=int, default=9600)
ap.add_argument("-u", "--user", default="admin")
ap.add_argument("-c", "--cmds", default="show version")
# ⚠ This box DOES have a console password, unlike the assumption below. Sending
# an empty one gets "Login timed out" and a silent failure that reads like a
# dead console rather than a rejected login.
ap.add_argument("-p", "--password", default="")
a = ap.parse_args()

BAUDS = {9600: termios.B9600, 115200: termios.B115200}
fd = os.open(a.tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
t = termios.tcgetattr(fd)
t[0] = t[1] = t[3] = 0
t[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
t[4] = t[5] = BAUDS[a.baud]
t[6] = list(t[6]); t[6][termios.VMIN] = 0; t[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, t)

log = bytearray()

def expect(pattern, timeout=25.0):
    """Read until `pattern` matches the tail of the stream. Returns True/False."""
    rx = re.compile(pattern)
    end = time.time() + timeout
    while time.time() < end:
        try:
            d = os.read(fd, 4096)
            if d:
                log.extend(d)
                if rx.search(log[-4000:].decode("utf-8", "replace")):
                    return True
                continue
        except BlockingIOError:
            pass
        time.sleep(0.05)
    return False

def send(s):
    os.write(fd, s.encode() + b"\r")
    time.sleep(0.4)

# Wake the line and get to a login prompt.
send("")
if not expect(r"login:\s*$|login:\s*\S*$", 40):
    send("")
    expect(r"login:", 40)

send(a.user)
# EOS admin often has no password, but not always -- send whatever was given.
if expect(r"[Pp]assword:", 6):
    send(a.password)
if not expect(r"[>#]\s*$", 30):
    print("!! never reached a CLI prompt", file=sys.stderr)

send("enable")
expect(r"[>#]\s*$", 15)
send("terminal length 0")
expect(r"[>#]\s*$", 15)

for c in a.cmds.split("|"):
    send(c.strip())
    expect(r"[>#]\s*$", 45)

os.close(fd)
sys.stdout.write(log.decode("utf-8", "replace"))
