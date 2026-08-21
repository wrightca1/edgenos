# 0.3.0-alpha43 — self-recovery hardening, and a diagnosable watchdog

No generator changes: provenance is alpha42's **119,347 of 124,889 = 95.6%**, both
ports clean-lock, paced loss ~0.37%. This release is about what happens when
things go wrong.

## Why: alpha42 rebooted itself, and nothing said why

`/mnt/flash/wdog.log` survives reboots and held the whole story:

    14:25:40 start bdf=0000:02:00.0 interval=10s grace=120s strikes=3 route_floor=2
    14:28:22 FIRING: PIN_STRAP=0xffffffff (3 strikes) routes=43 (0 strikes)

The FM6000 dropped off the PCI bus (`0xffffffff`; healthy is `0x208`) while the
control plane was fine at 43 routes, and `fm6000_wdog` rebooted the box unattended
— correct behaviour. Two firings in the log's entire history. EOS's own
`show reload cause` reports nothing, because a watchdog reset is not a reload.

It was **not reproducible**: 20 rounds of alternating paced and burst load plus 30
monotonic liveness samples, no reset.

## 1. A panic no longer hangs the box

**`/proc/sys/kernel/panic` was 0** — a kernel panic hung forever and needed
physical access. `fm6000_wdog` cannot cover that; it is userspace and dies with the
kernel. `init-m1` now sets, and alpha43 verifies:

    panic=10        reboot 10s after a panic (Aboot then self-reverts to EOS)
    panic_on_oops=1 an oops becomes a panic, so a half-dead kernel reboots too

⚠ **Still unrecoverable: a silent CPU lockup.** This kernel has no lockup
detectors at all — `softlockup_panic`, `hung_task_panic` and `nmi_watchdog` are
absent from `/proc/sys/kernel`. That needs a kernel config change, not a sysctl.

## 2. The watchdog now records WHY, not just THAT

`capture_pci()` dumps the ASIC's PCI config space into the log before rebooting.
Config space keeps answering after the device stops responding on MMIO, so it
separates the failure modes: vendor `0xffff` = gone from the bus, status bit 14 =
signalled system error, AER status = a latched PCIe error. Verified by forcing a
dry-run firing on a healthy device:

    FIRING: PIN_STRAP=0x00000208 (0 strikes) routes=44 (4 strikes) [dry-run, not rebooting]
      pci: vendor=0x8086 device=0x155b command=0x0406 status=0x0010
      cfg+00: 86 80 5b 15 06 04 10 00 00 00 00 02 00 00 00 00

## Three documentation defects corrected

- `FEATURE-COMPLETE-CHECKLIST.md` claimed the cmdline carries `nmi_watchdog=panic`
  and `reboot=p` so "the box already survives a CPU hard lockup". It carries
  neither. That claim appears to have come from `to-eos.sh`, which is the line
  that boots **EOS**, not us.
- The same file's **E0a** said `fm6000_wdog` "must be launched by hand". It is
  started at boot by `init-m1:245` and was running as pid 1609 — stale since at
  least alpha42.
- `m0/boot0` credited `nosmp + reboot=p,force` with fixing the reboot hang.

⚠ **And one error of my own, corrected in the same pass.** I first concluded from
`/proc/cmdline` that boot0's append was dead code. It is not: **dmesg's
"Command line:" shows the kernel does receive it** — `/proc/cmdline` on this box
reports Aboot's line instead. Check dmesg, never `/proc/cmdline`. The append's
`panic=10` nonetheless did not take effect (hence fix 1), which remains
unexplained and is deliberately not chased: setting the sysctl is verifiable
after the fact and independent of who wins the cmdline.

md5 `f4a1dc6bdf4e3ab3e55dd7fce0cfadfa`, 19,029,026 bytes, verified on the switch.
