# initrd/ — the pieces that were only ever on one disk

These three files built and ran every EdgeNOS image on this switch and, until
2026-08-20, existed **nowhere but `/home/smiley/edgenos-work/lls2/` on one
machine**. A power outage destroyed the machine running this project once
before and took five days of unpushed work with it; that is the reason these are
here.

| file | what it is |
|---|---|
| `init` | the initrd's `/init` — watchdog, management NIC, thermal loop, LED blanking |
| `ospfup` | one command from a cold chip to two routed ports with dual-stack OSPF |
| `pack.sh` | packs a tree into an initrd **and verifies every file in it** |

## Three things `init` does that are not obvious

**It sets the management MAC before bringing the link up.** The tg3 on this
board has no NVRAM MAC, so the driver invents `00:10:18:00:00:00` on every cold
boot. The upstream gateway holds an ARP entry for the board's real address, so
the box answers nobody and looks dead while sitting at a live prompt. Order
matters: down, set, up — the kernel refuses the change on a live interface, and
setting it after the link is up means the wrong address has already been
announced.

**It no longer reboots the box every 50 minutes.** That used to be an
unconditional `( sleep 3000; reboot -f ) &`. It was a reasonable backstop while
bringing a chip up and wrong for anything meant to stay running — a routing
adjacency, a link under test, or a thermal loop, which cannot do its job if the
machine restarts on a timer. It also made the box look flaky for reasons that
had nothing to do with the work. The watchdog already covers that case and
covers it better, because it fires when something is wrong rather than when a
clock runs out. `EDGENOS_AUTOREBOOT=<seconds>` brings the timer back.

**It blanks the port LEDs.** Those registers survive the kexec, so at that point
they still show the vendor OS's last picture of *its* link state. The bridge
lights whatever is genuinely up once it starts; until then a dark panel is
honest and a stale one is not.

## `pack.sh` verifies, and that has mattered

It checks the entry count and `cmp`s every regular file. A pack once silently
truncated when the staging filesystem filled — 31 MB instead of 62 — and the
check in use at the time compared a single file that happened to sit before the
cut, so it passed. Do not weaken it.

⚠ Its output path is used **after** `cd "$W"`, so a relative path lands *inside*
the tree being packed and the archive swallows itself. Pass an absolute path.
