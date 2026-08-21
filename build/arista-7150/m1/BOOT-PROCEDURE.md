# Boot procedure: EOS <-> EdgeNOS M1 on the 7150 (hard-won, 2026-07-28)

The 7150 dual-boots **EOS** (flash default) or an **EdgeNOS M1** SWI. Switching direction is done at the
**Aboot** prompt over the **serial console** (gateway `smiley@<console-host>`, `/dev/ttyUSB1 @ 9600 8N1`,
passwordless sudo). Network SSH: `<switch>` — **EOS = legacy crypto (ssh-rsa/dss), M1 = modern (ed25519)**;
the offered host-key type tells you which OS is running.

## GOLDEN RULES (these bit us repeatedly)
1. **`reload` over network SSH is UNRELIABLE** after the box has been power-cycled hard several times (the EOS
   management plane gets flaky and silently ignores `reload now`). **Reload via the SERIAL CONSOLE instead**
   (log in admin/arista, `enable`, `reload now`). That always takes.
2. **NEVER run `pkill -f "cat /dev/ttyUSB1"` from your gateway SSH command** — the pattern matches YOUR OWN
   command line and kills your session (exit 255, silent). Use short `timeout N ... cat`/`dd` readers that
   self-terminate, and let the catcher script do its own reader cleanup internally.
3. **Only ONE reader on /dev/ttyUSB1 at a time.** Two `cat`s split the byte stream -> the Aboot-catcher's log
   is garbled and it reports `aboot-caught=0 after 400 tries`. Make sure your serial-reload reader has ENDED
   (timeout) before you start the catcher.
4. **`copy ... flash:` then immediate `reload` can lose the file** (page-cache write not flushed; Aboot says
   "not found"). Always `bash sudo sync; sync` after the copy and verify the on-disk md5 before reloading.
5. M1 has **no block driver** -> it cannot see/write the flash. All flash writes (new SWIs) must be done from
   **EOS** (`copy http://...`). See TODO-flash-block-driver.md to fix this.

## EOS -> EdgeNOS M1 (boot an M1 SWI)
Prereq: the M1 SWI is on flash (`ls /mnt/flash/*.swi`); if not, from EOS:
`enable` -> `copy http://<host:8000>/edgenos-m1-X.swi flash:edgenos-m1-X.swi` -> `bash sudo sh -c "sync;sync;
md5sum /mnt/flash/edgenos-m1-X.swi"` (verify). Host HTTP server: `python3 -m http.server 8000` in the SWI dir;
**port 8000 is firewall-open to the box, 8080/8099 are NOT** (and the local host has no passwordless sudo to
open ports).

Then (all on the gateway, as sudo):
```
# 1. serial reload (reliable). Use a SELF-TERMINATING reader so it doesn't clash with the catcher.
stty -F /dev/ttyUSB1 9600 raw -echo
timeout 22 dd if=/dev/ttyUSB1 of=/tmp/login.log bs=1 2>/dev/null &   # reader ends at 22s, before Aboot
printf '\r\n';  sleep1.5;  printf 'admin\r\n';  sleep2.5;  printf 'arista\r\n'; sleep3
printf 'enable\r\n'; sleep1.5;  printf 'reload now\r\n'; sleep3; printf '\r\n'      # each -> /dev/ttyUSB1
# confirm "The system is going down for reboot NOW! / Restarting system" appears in /tmp/login.log
# 2. WAIT for the timeout-22 reader to end (~sleep 4), THEN start the catcher as the SOLE reader:
setsid bash -c '/tmp/aboot-catch3.sh "boot flash:/edgenos-m1-X.swi" /tmp/aboot.log > /tmp/catch.out 2>&1 &'
# 3. ~60s later: catch.out shows "aboot-caught=1 after N tries"; aboot.log shows the i2c-bringup + ENUMERATED.
```
`aboot-catch3.sh` spams Ctrl-C until `Aboot#`, then types the boot command. M1 comes up on <switch>
(root/arista, modern crypto, `UserKnownHostsFile=/dev/null` to skip the changed-host-key error).

## EdgeNOS M1 -> EOS (recover to the default)
Easiest: from an M1 root shell, **SCD power-cycle**: `scdreg 0x7000 0xdead` -> board power-cycles -> Aboot ->
(no catcher running) -> **default boots EOS** (`boot-config` SWI=flash:/EOS-4.16.8M.swi). ~4 min to EOS login.
(Do NOT arm a catcher if you want EOS — Aboot autoboots the default when uncaught.)

Alternative (from Aboot prompt, e.g. after a failed M1 boot): the catcher leaves you at `Aboot#`; drive it over
serial: `boot flash:/EOS-4.16.8M.swi`. Aboot is a busybox shell (`ls -la /mnt/flash`, `rm`, `df` work; `dir`
does not).

## Recovery / gotchas
- Box wedged (FM6000 off-bus, CSR reads 0xffffffff) but x86 alive: keep the SCD watchdog re-armed with a LOCAL
  pet loop on M1 (`while :; do scdreg 0x0120 0xC0000BB8; sleep 3; done`) so a hard hang auto-power-cycles to
  EOS in ~30s. External held-open-SSH petting does NOT land reliably.
- If the catcher reports `aboot-caught=0` but the box DID reboot: a second reader was on the port (rule 3), or
  the reload didn't take (rule 1) — check /tmp/login.log for "Restarting system".
- Flash is ~1.5G, ~550M free; delete obsolete SWIs from Aboot/EOS if a copy fails for space.
