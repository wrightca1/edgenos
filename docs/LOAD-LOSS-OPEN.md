# Frame loss under load: RESOLVED — matches EOS exactly, and pacing was the confound

**Status: closed 2026-08-21.** Measured against Arista's own EOS 4.16.8M on the same
box, same cabling, same peer, same harness. The generators are exonerated, and the
"~2.7% pre-existing loss" that this document previously treated as an open defect
was an artifact of how the load was generated.

## The numbers

| generator | EOS 4.16.8M | EdgeNOS 0.3.0-alpha42 |
|---|---|---|
| paced, 2 ms gap | 1996 / 1994 / 1995 (0.25%) | 1998 / 1993 / 1996 (0.22%) |
| unpaced burst | 1174 / 1165 / 1174 (**41.5%**) | 1168 / 1155 / 1190 (**41.5%**) |

2000 ICMP frames, counted with tcpdump on the egress side (`tools/load-test.sh`).
EdgeNOS is marginally *better* than EOS at both pacings; the difference is noise.

**Conclusion: nothing in our bring-up causes loss.** Burst loss is what this path
does under vendor firmware too.

## Two measurement errors this file used to enshrine

1. **Pacing is the whole story, and it was never controlled.** Paced traffic is
   forwarded at ~99.8%. An unpaced 2000-packet blast from the peer's CPU loses
   ~42% — under EOS as well. These measure different things (forwarding vs buffer
   absorption) and must never be compared to each other. Every earlier claim in
   this project that compared a paced run to an unpaced one was comparing noise.

2. **Interface-counter deltas are not a measurement.** They are contaminated by
   background traffic and once produced *negative* loss (2007 forwarded of 2000).
   Count on the egress side with tcpdump, filtered to the flow under test.

## Where the loss is NOT

Punt counters (`/proc/net/dev` for et1/et2) do **not move at all** across a
2000-frame burst. The entire burst is handled in silicon and the CPU never sees a
frame of it, so this is not a punt-path or software-forwarding limit.

## Harness traps that produce a convincing 100%-loss reading

- The peer's `ping` is **busybox**: no `-f`, and `-A` needs replies (the test
  destination has no host, so there are none). A flood-ping harness silently sends
  nothing and reports 100% loss, which looks exactly like a dead dataplane.
  `tools/load-test.sh` generates load from a raw socket in python3 instead.
- A `tcpdump` backgrounded from a *separate* ssh invocation dies with that session
  and captures nothing. Capture, send and read back in one remote shell.
- ARP must be primed **from the switch** (`ping -I et1` / `-I et2`), not only from
  the peer, or the switch's neighbour entry goes FAILED, Linux backs off, and only
  a handful of frames are ever sent.
