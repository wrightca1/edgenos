#!/usr/bin/env python3
"""smoke-test.py — boot an EdgeNOS x86_64 disk image under qemu and verify it came up.

Stdlib only (no pexpect): drives the serial console over a TCP socket. Checks:
  * reaches a login prompt, root/edgenos logs in
  * `edgenos version` reports the platform
  * ma1 + the expected ge* ports exist (port naming) and ma1 got a DHCP lease
  * zebra/ospfd/bgpd are running, vtysh answers
  * a marker file survives a reboot (overlay persistence)
  * optional --uefi: same checks booting through OVMF
Exit 0 on success. Used by CI and by humans: tools/qemu/smoke-test.py <image.qcow2> [-n 4] [--uefi]
"""
import argparse, os, socket, subprocess, sys, time, re, shutil, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


class Console:
    def __init__(self, port, timeout=240):
        self.buf = b""
        deadline = time.time() + 30
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        self.s.settimeout(1)
        self.timeout = timeout

    def expect(self, pattern, timeout=None):
        rx = re.compile(pattern.encode() if isinstance(pattern, str) else pattern, re.S)
        deadline = time.time() + (timeout or self.timeout)
        while time.time() < deadline:
            m = rx.search(self.buf)
            if m:
                out = self.buf[:m.end()]
                self.buf = self.buf[m.end():]
                return out.decode(errors="replace")
            try:
                d = self.s.recv(4096)
                if not d:
                    time.sleep(0.2)
                    continue
                self.buf += d
                sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
            except socket.timeout:
                pass
        raise TimeoutError(f"timeout waiting for {pattern!r}; last output: {self.buf[-400:]!r}")

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07\x1b]*(\x07|\x1b\\\\)|\r")

    def cmd(self, line, timeout=60):
        """Run a shell line at the root prompt, return (rc, output) — output between a start
        marker and an end marker, ANSI/OSC sequences stripped, terminal echo excluded."""
        tag = "%d" % int(time.time() * 1000)
        start, end = f"__S{tag}__", f"__E{tag}__"
        self.send(f"echo {start}; {line}; echo {end}$?")
        out = self.expect(end + r"(\d+)", timeout)
        out = self.ANSI.sub("", out)
        rc = int(re.search(end + r"(\d+)", out).group(1))
        # the last start-marker occurrence on its own line is the real one (earlier ones are echo)
        m = None
        for m in re.finditer(re.escape(start) + r"\n", out):
            pass
        body = out[m.end():] if m else out
        body = body.rsplit(end, 1)[0]
        return rc, body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("-n", "--nics", type=int, default=3, help="total NICs (1 mgmt + N-1 front-panel)")
    ap.add_argument("--uefi", action="store_true")
    ap.add_argument("--port", type=int, default=4555)
    ap.add_argument("--keep", action="store_true", help="don't use -snapshot (persist changes to the image)")
    a = ap.parse_args()

    run = os.path.join(HERE, "run-edgenos.sh")
    cmd = [run, a.image, "-n", str(a.nics), "--serial", f"tcp:{a.port}", "--ssh-port", "0", "--name", f"smoke{a.port}"]
    if not a.keep:
        cmd.append("--snapshot")
    if a.uefi:
        cmd.append("--uefi")
    # TAP NICs need root; for the smoke test use plain user-mode dummies instead (we only test naming)
    env = dict(os.environ, EDGENOS_NO_TAP="1")
    print("+", " ".join(cmd))
    qemu = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    ok = False
    try:
        c = Console(a.port)
        c.expect(r"login:", 180)
        print("\n[smoke] login prompt reached")
        c.send("root"); c.expect("Password:", 20); c.send("edgenos")
        c.expect(r"# ", 30)
        c.send("stty cols 200; export PS1='EDGENOS# ' TERM=dumb"); c.expect("EDGENOS# ", 10)

        rc, out = c.cmd("edgenos version")
        assert rc == 0 and "x86_64-kvm_x86_64-r0" in out, f"edgenos version: {out}"
        print("[smoke] version OK")

        want = ["ma1"] + [f"ge{i}" for i in range(a.nics - 1)]
        rc, out = c.cmd("ls -1 /sys/class/net | cat")
        have = out.split()
        missing = [p for p in want if p not in have]
        assert not missing, f"ports missing: {missing} (have {have})"
        print(f"[smoke] ports OK: {want}")

        rc, out = c.cmd("for i in $(seq 1 40); do ip -4 -o addr show ma1 | grep -q inet && break; sleep 1; done; ip -4 -o addr show ma1")
        assert "inet " in out, f"ma1 has no DHCP address: {out}"
        print("[smoke] ma1 DHCP OK")

        rc, out = c.cmd("for i in $(seq 1 30); do pgrep -x zebra-x86_64 >/dev/null && pgrep -x bgpd-x86_64 >/dev/null && break; sleep 1; done; pgrep -a -f -- '-x86_64 -f' | sed 's/.*opt.edgenos.//' | cut -d' ' -f1 | sort | tr '\\n' ' '")
        for d in ("zebra", "ospfd", "ospf6d", "bgpd"):
            assert d + "-x86_64" in out, f"{d} not running: {out}"
        print("[smoke] quagga daemons OK")

        rc, out = c.cmd("vtysh -c 'show version' 2>&1 | head -2")
        assert rc == 0 and "Quagga" in out, f"vtysh: {out}"
        print("[smoke] vtysh OK")

        rc, out = c.cmd("edgenos platform show 2>&1 | head -30")
        assert rc == 0 and "x86_64-kvm_x86_64-r0" in out, f"platform show: {out}"
        print("[smoke] platform class OK")

        rc, out = c.cmd("echo persist-$$ > /etc/edgenos/smoke-marker; cat /etc/edgenos/smoke-marker; sync")
        marker = out.strip().splitlines()[-1]
        c.send("reboot")
        c.expect(r"login:", 180)
        print("\n[smoke] rebooted")
        c.send("root"); c.expect("Password:", 20); c.send("edgenos"); c.expect(r"# ", 30)
        c.send("export PS1='EDGENOS# ' TERM=dumb"); c.expect("EDGENOS# ", 10)
        rc, out = c.cmd("cat /etc/edgenos/smoke-marker")
        assert marker in out, f"persistence failed: {out}"
        print("[smoke] persistence OK")
        c.send("poweroff")
        time.sleep(3)
        ok = True
    finally:
        qemu.terminate()
        try:
            qemu.wait(10)
        except subprocess.TimeoutExpired:
            qemu.kill()
        if not ok:
            err = qemu.stderr.read().decode(errors="replace")[-2000:]
            print("\n[smoke] qemu stderr:", err)
    print("\n[smoke] ALL OK" if ok else "\n[smoke] FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
