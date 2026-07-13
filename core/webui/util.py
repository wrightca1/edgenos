"""Helpers for the EdgeNOS web UI — stdlib only (the switches' Python is minimal)."""
import html
import json
import socket
import subprocess


def run(cmd, timeout=10):
    """Run a command, return combined stdout/stderr text (never raises)."""
    if not isinstance(cmd, (list, tuple)):
        return "(error: cmd must be a list, not a string)"
    try:
        p = subprocess.run(cmd, shell=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
        return p.stdout.decode("utf-8", "replace")
    except Exception as e:                                  # noqa: BLE001
        return "(error: %s)" % e


def run_rc(cmd, timeout=20):
    """Run a command, return (returncode, output)."""
    if not isinstance(cmd, (list, tuple)):
        return 1, "(error: cmd must be a list, not a string)"
    try:
        p = subprocess.run(cmd, shell=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except Exception as e:                                  # noqa: BLE001
        return 1, "(error: %s)" % e


def json_cmd(cmd, timeout=10):
    """Run a command expected to emit JSON; return the parsed object or None."""
    try:
        return json.loads(run(cmd, timeout))
    except Exception:                                       # noqa: BLE001
        return None


def h(s):
    return html.escape("" if s is None else str(s))


def vty(port, password, commands, connect_timeout=2, idle=0.4, deadline=6):
    """Talk to a Quagga vty (zebra=2601, ospfd=2604, bgpd=2605) over telnet.
    Sends the password, `terminal length 0`, then each command; returns the text.
    Uses the vty so config changes apply live WITHOUT restarting the daemon.
    Bounded: a drain returns as soon as the vty goes idle for `idle` seconds (short
    recv timeout, so idle is detected fast), with an overall `deadline` hard cap —
    a slow/unresponsive vty can never hang the UI."""
    import time
    start = time.time()
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=connect_timeout)
    except OSError as e:
        return None, "vty %d unreachable: %s" % (port, e)
    s.settimeout(idle)
    buf = b""

    def drain():
        nonlocal buf
        last = time.time()
        while (time.time() - start) < deadline:
            try:
                d = s.recv(4096)
                if not d:
                    break
                buf += d
                last = time.time()
            except socket.timeout:
                if time.time() - last >= idle:
                    break

    drain()
    if password:
        s.sendall((password + "\n").encode()); drain()
    s.sendall(b"terminal length 0\n"); drain()
    out = []
    for c in commands:
        marker_before = len(buf)
        s.sendall((c + "\n").encode())
        drain()
        out.append(buf[marker_before:].decode("latin1"))
    try:
        s.sendall(b"exit\n"); s.close()
    except OSError:
        pass
    return out, None


def vty_available(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        s.close()
        return True
    except OSError:
        return False
