#!/usr/bin/env python3
"""onie-test.py ISO INSTALLER.bin [--uefi] — end-to-end ONIE install test under qemu (KVM).
1. blank 2G disk, boot the ONIE recovery ISO (BIOS or OVMF), pick "Embed ONIE" from GRUB (serial)
2. ONIE reboots from disk into install mode; stop discovery, onie-nos-install http://10.0.2.2:PORT/INSTALLER
3. installer runs, reboots; expect the EdgeNOS GRUB + login prompt; log in, check edgenos version + ONIE menu entry
"""
import os, sys, re, socket, subprocess, threading, time, http.server, functools

iso, installer = sys.argv[1], sys.argv[2]
uefi = "--uefi" in sys.argv
PORT, SERIAL = 8765, 4570
work = os.path.abspath("onie-test-work"); os.makedirs(work, exist_ok=True)
os.system("pkill -f 'serial tcp::%d' 2>/dev/null" % SERIAL); time.sleep(1)   # no stale VM on our serial port
disk = os.path.join(work, "disk.qcow2")
subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", disk, "2G"], check=True)

# http server for the installer
srv_dir = os.path.dirname(os.path.abspath(installer))
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=srv_dir)
httpd = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), handler)
threading.Thread(target=httpd.serve_forever, daemon=True).start()

def qemu(cdrom):
    args = ["qemu-system-x86_64", "-machine", "type=pc,accel=kvm", "-cpu", "host", "-m", "1024", "-smp", "1",
            "-nographic", "-serial", f"tcp::{SERIAL},server,nowait", "-monitor", "none",
            "-drive", f"file={disk},if=virtio,format=qcow2",
            "-netdev", "user,id=m0", "-device", "virtio-net-pci,netdev=m0",
            "-boot", "order=dc" if cdrom else "order=c"]
    if cdrom:
        args += ["-cdrom", iso]
    if uefi:
        args += ["-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                 "-drive", f"if=pflash,format=raw,file={work}/OVMF_VARS.fd"]
        if not os.path.exists(f"{work}/OVMF_VARS.fd"):
            subprocess.run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd", f"{work}/OVMF_VARS.fd"], check=True)
    print("+", " ".join(args)); sys.stdout.flush()
    return subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

class Con:
    def __init__(self):
        deadline = time.time() + 30
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", SERIAL), timeout=5); break
            except OSError:
                if time.time() > deadline: raise
                time.sleep(0.5)
        self.s.settimeout(1); self.buf = b""
    def expect(self, pat, t=120):
        rx = re.compile(pat.encode(), re.S); end = time.time() + t
        while time.time() < end:
            m = rx.search(self.buf)
            if m:
                o = self.buf[:m.end()]; self.buf = self.buf[m.end():]; return o.decode(errors="replace")
            try:
                d = self.s.recv(4096)
                if d:
                    self.buf += d; sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
            except socket.timeout:
                pass
        print(f"\n[onie-test] TIMEOUT waiting for {pat!r}"); import atexit; os.system("pkill -f 'serial tcp::%d' 2>/dev/null" % SERIAL); raise SystemExit(1)
    def send(self, l): self.s.sendall((l + "\n").encode())
    def raw(self, b): self.s.sendall(b)

# --- phase 1: embed ONIE from the rescue shell (the ISO's default GRUB entry) ---
q = qemu(cdrom=True)
c = Con()
c.expect(r"Rescue mode detected|ONIE(-RECOVERY)?:/ #|Please press Enter to activate this console", 240)
time.sleep(2); c.send(""); time.sleep(1); c.send("")
c.expect(r"ONIE(-RECOVERY)?:/ #", 60)
c.send("onie-self-update -e file:///lib/onie/onie-updater")
out = c.expect(r"Success|Failure|failed", 900)
if "Success" not in out:
    print("\n[onie-test] ONIE embed FAILED"); q.terminate(); sys.exit(1)
# ONIE reboots after a successful update: wait until the VM comes back up (GRUB menu from the ISO again)
c.expect(r"highlighted entry will be executed|Booting `ONIE|GNU GRUB", 600)
time.sleep(2)
q.terminate(); q.wait(10)
print("\n[onie-test] ONIE embedded; booting from disk"); sys.stdout.flush()

# --- phase 2: NOS install ---
q = qemu(cdrom=False)
c = Con()
c.expect(r"ONIE(-RECOVERY)?:/ #|Please press Enter to activate this console", 300)
c.send(""); time.sleep(1); c.send("")
c.expect(r"ONIE(-RECOVERY)?:/ #", 60)
c.send("onie-stop; sleep 2; onie-sysinfo -p; echo MARK_PLAT")
c.expect(r"x86_64-kvm_x86_64-r0.*MARK_PLAT", 60)
name = os.path.basename(installer)
c.send(f"onie-nos-install http://10.0.2.2:{PORT}/{name}")
out = c.expect(r"install complete|ERROR:|Failure|failed", 900)
ok = "install complete" in out
print(f"\n[onie-test] installer result: {'OK' if ok else 'FAILED'}"); sys.stdout.flush()
if not ok:
    q.terminate(); sys.exit(1)
# ONIE reboots after the installer returns
c.expect(r"login:", 600)
print("\n[onie-test] EdgeNOS booted from the ONIE-installed disk"); sys.stdout.flush()
c.send("root"); c.expect("Password:", 20); c.send("edgenos"); c.expect(r"# ", 30)
c.send("export PS1='E# ' TERM=dumb"); c.expect("E# ", 10)
c.send("stty cols 200; echo __ST__; edgenos version | head -2; lsblk -o NAME,LABEL,SIZE | cat; grep -c ONIE /boot/grub/grub.cfg; echo __END__")
out = c.expect(r"(?<!echo )__END__\r?\n", 60)
body = out.rsplit("__ST__\r\n", 1)[-1] if "__ST__\r\n" in out else out.rsplit("__ST__\n", 1)[-1]
print("\n[onie-test] verification output:\n" + body[-600:])
ok = "x86_64-kvm_x86_64-r0" in body and "EDGENOS-BOOT" in body and "EDGENOS-DATA" in body
print("\n[onie-test] " + ("ALL OK" if ok else "CHECK FAILED") + " (firmware=%s)" % ("uefi" if uefi else "bios"))
c.send("poweroff"); time.sleep(3); q.terminate()
