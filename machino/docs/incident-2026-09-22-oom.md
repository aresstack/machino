# Incident: OOM after a clean pipeline stop — 2026-09-22, build `e611220`

Acceptance run 1 was stopped here. Nothing was restarted, reconfigured or
power-cycled; the state was captured first. All volatile evidence is on the
host in `/c/tmp/machino-forensics/` — **`/var/log` is tmpfs on this camera**, so
a power-cycle would have destroyed the log.

## What actually happened

Reconstructed from `machino.log`, `dmesg` and the OOM killer's own output.

```
05:49:38  boot - the operator cold power-cycle after the deploy (see the
          clock-step note below; there was only ever this one boot)
05:49:47  machino e611220 starts, pid 990
05:50-05:52  Live testing: WebRTC MAIN carries 38 MB of RTP, send_err=0,
             MSE and two windows all fine
05:52:50  last media consumer leaves -> demand -1, all=0 -> GRACE_IDLE
05:52:50  /ws/logs subscribes -> logread child forked (pid 1650)
05:52:55  GRACE_IDLE -> STOPPING -> "unit main stopped (3616 frames)"
          -> "ING_PLAT down" -> COLD_IDLE          [teardown reports success]
05:53:10  second /ws/logs subscriber
05:54:41  last /ws/logs subscriber leaves
05:54:42  a new HTTP consumer arrives -> COLD_IDLE -> STARTING
05:54:42  IMP_System_Init failed (-1)  x5   -> bring-up failed -> FAILED
          ... four such cycles, 05:54:42 - 05:55:03
05:56:43  kernel OOM killer fires, sacrifices the logread child
05:57:25  kernel OOM killer kills machino:
          total-vm 97 148 kB, anon-rss 27 208 kB
```

`streamerctl status` now reports `running: none`. The daemon did not come back.

## The part that matters

**The teardown reported success and the next bring-up could not initialise.**
The lifecycle did everything right on the way down — `demand -1 … all=0`,
`unit main stopped`, `ING_PLAT down`, `COLD_IDLE`. Ninety seconds later the
next consumer could not start the platform.

**The kernel driver disagrees with our error message.** On every failed
attempt `dmesg` shows a *complete, successful* sensor bring-up:

```
probe ok ------->imx307
imx307 chip found @ 0x1a (i2c1)
Create framechan0 OK!  Create framechan1 OK!  Create framechan2 OK!
[ tx_isp_vic_start:218 ] sensor type is SONY_MIPI!
imx307 stream on
imx307 stream off          (~0.8 s later)
```

Four full probe/stream cycles, one per retry round. There is also one
`IRQ Error, cpu: 0 Cause:0x08800000` right after a `stream off`.

**Classification: `INIT_FAILED_AFTER_DRIVER_BRINGUP`, cause unknown.**

An earlier version of this note read the `-1` as "already initialised". That
was over-reading the evidence and is withdrawn. All the driver messages prove
is that initialisation gets *far* — sensor probed, framechans created, sensor
streamed. It can still fail afterwards on a memory allocation, on a different
IMP subsystem, or on a half-released resource object. `-1` is the vendor's
generic failure and carries no such meaning on its own.

What *is* established: the teardown reported success, and the next bring-up
fails somewhere after the driver has come up.

**Memory was not released on stop, and each failed retry added more.** By the
time the OOM killer ran, machino held 27 MB anon-RSS of a 42 MB machine — with
*no* pipeline running.

## This is not the session-leak hypothesis

The obvious suspicion was that closing a viewer or switching transport leaves a
session behind. The log does not support it: demand went to `all=0`, the
pipeline stopped cleanly, and the WebRTC session reported `send_err=0` to the
end. The failure is in **re-initialisation after a clean stop**, not in
counting consumers down.

It is also **not** the known warm-install freeze. That one needs a warm install
plus a connection burst; this happened 90 seconds after an idle teardown on a
cold-booted camera.

## Prime suspect, stated as a hypothesis

The one thing in this build that is new *at exactly that point in the
sequence* is the `/ws/logs` child: it was `fork()`ed at **05:52:50**, five
seconds **before** the teardown, while IMP was still initialised and holding
its device mappings.

The mechanism is **not** descriptor lifetime. The child `exec`s `logread`
immediately; `CLOEXEC` closes our descriptors at that point and the inherited
mappings are replaced by the new image, all of it long before the teardown five
seconds later. That reading was wrong and is withdrawn.

What is worth suspecting is `fork()` **itself**: it duplicates the VMAs of a
process that currently holds live Ingenic MMAP/DMA regions. On a vendor driver
of this vintage, duplicating those VMAs can disturb refcounts or VMA state even
though the child execs a moment later — and the damage would only become
visible at the *next* teardown/re-init, which is exactly where it showed up.

**This is a hypothesis, not a finding.** It fits the timing and it fits M4's
result (twelve cold cycles in one process, proven before `/ws/logs` existed),
but nothing here proves it.

## Withdrawn: the "second reboot" was a clock step

An earlier version of this note claimed the camera had restarted at ~05:49:35
for no reason. **It had not.** Reviewer correction, and the evidence settles it:

- `logread` begins at `Sep 21 05:49:38` with kernel timestamp `[0.656788]` and
  contains **exactly one** boot.
- The daemon start I had read earlier as `05:43:57` and the one in the current
  log at `05:49:47` differ by **5 min 50 s** — and `05:43:57 + 5:50 = 05:49:47`
  exactly. One event, two clock readings.
- `ntpd` is running and the camera's date is a day off from the host's, so the
  clock demonstrably moves.

I had compared `uptime` against wall-clock timestamps **across a clock step**,
which makes the arithmetic meaningless. There was one boot: the operator's
cold power-cycle after the deploy. No phantom to chase.

## If the fork is confirmed, the fix is probably small

Start the `logread -f` helper **once at daemon start, before IMP is ever
initialised**, and keep that one child for the process lifetime. Then nothing
is ever forked while Ingenic devices and their mappings are live. That is
architecturally better than any driver workaround - but it waits until the A/B
test says the fork is actually the trigger.

## Diagnostic restart in the broken kernel state — done

Before power-cycling, one controlled daemon start was run **in the same kernel
state the OOM left behind**, with a watchdog set to kill the daemon on the
first `IMP_System_Init failed` so no retry round could run. It answers a
question a power-cycle would have destroyed.

| | |
|---|---|
| Daemon starts | **yes**, pid 3938 |
| `IMP_System_Init` | **succeeds** |
| Pipeline comes up | **yes** — 1 900 325 bytes of RTP in 4 s (≈3.8 Mbit/s, the configured rate) |
| Teardown | clean: `demand all=0`, `unit main stopped (212 frames)`, `ING_PLAT down`, `COLD_IDLE` |

`VmRSS` across the cycle:

```
no daemon                       MemAvailable 28 248 kB
after start, COLD_IDLE          VmRSS  1 412 kB
ACTIVE, streaming               VmRSS  3 780 kB
after teardown, COLD_IDLE       VmRSS  2 656 kB
for comparison, at the OOM      anon-rss 27 208 kB
```

**Two things follow.**

*The broken state does not outlive the process.* A fresh process initialised
IMP without trouble on a kernel that had just OOM-killed its predecessor. So
there is no global, reboot-only stuck state — which is genuinely good news, and
it is information a power-cycle would have thrown away.

*This does not exonerate the fork hypothesis.* What the fork is suspected of
damaging — per-process device references and VMAs — is exactly what the kernel
reclaims when the process dies. "Does not survive process death" and
"process-bound" are the same statement here. The test rules out a kernel-wide
stuck state; it does not rule out the fork.

*Normal is 3.8 MB.* A streaming pipeline costs about 2.4 MB over idle. The
failure state was **27 MB — roughly seven times a working stream** with no
pipeline running at all. Whatever happened, it is not ordinary consumption.

One cycle also left ~1.2 MB behind (1 412 → 2 656 kB idle-to-idle). On its own
that is unremarkable — musl does not return every arena to the OS — but it is
the number to watch across repeated cycles in test A.

## A/B test — run, and conclusive

Cold boot, no browser at any point, RTSP and the API only, memory sampled once
a second on the camera, aborting on the first `IMP_System_Init failed`.

### Test A — three start/stop cycles, no `/ws/logs`: **PASS**

| | RTP delivered | `VmRSS` ACTIVE | `VmRSS` back at COLD_IDLE |
|---|---|---|---|
| fresh boot | — | — | 1 500 kB |
| cycle 1 | 2 654 732 B | 4 168 kB | 2 696 kB |
| cycle 2 | 2 348 267 B | 4 384 kB | 2 908 kB |
| cycle 3 | 2 338 712 B | 4 440 kB | — |

`IMP_System_Init` succeeded every time. The idle-to-idle residue was +1 196 kB
after the first cycle and **+212 kB** after the second — decelerating, which
reads as allocator arenas settling rather than a per-cycle leak.

**The plain teardown/re-init path is not the defect.**

### Test B — one cycle with `/ws/logs` opened while the pipeline was ACTIVE: **FAIL**

```
RTSP PLAY                          -> ACTIVE, 1 619 024 B of RTP
/ws/logs opened while ACTIVE       -> HTTP/1.1 101 Switching Protocols
both closed, grace elapsed         -> ING_PLAT down, COLD_IDLE, VmRSS 3 344 kB
RTSP PLAY again                    -> RTSP/1.0 503 Service Unavailable, 0 bytes
                                      IMP_System_Init failed (-1) x5
```

Same camera, same boot, the only difference being one `/ws/logs` subscription
inside the cycle.

**`/ws/logs` is the trigger.** Test A repeated the identical sequence three
times without it and passed every time.

### And the failed retries are what eats the memory

`VmRSS` across the failing attempt: **3 352 → 4 588 kB during it, 9 428 kB
after the five retries** — roughly **1.2 MB per failed attempt**, none of it
returned. That is the mechanism behind the original OOM: four rounds of five
retries, unattended, on a 42 MB machine.

The daemon is not spinning now — the RTSP client went away, demand is 0, and it
is sitting in `FAILED` at 9 428 kB. It only runs away while something keeps
asking for media.

### What this does and does not establish

Established: a `/ws/logs` subscription taken while IMP is initialised leaves
the process unable to re-initialise IMP after the next teardown, and every
failed retry costs about 1.2 MB.

Not established: *why*. The fork remains the suspect — `/ws/logs` is the only
thing in the daemon that calls `fork()` — but nothing here distinguishes "the
fork damaged something" from "the child's lifetime overlapped the teardown" or
from a defect in the `/ws/logs` teardown path itself that has nothing to do
with forking. A test that forks something harmless at the same point, with no
`logread` involved, would separate those.

## Discriminating test (superseded by the A/B result above)

One power-cycle, then two sequences, in this order:

1. Live → stop → wait past the grace → Live again.
   *M4 proved this works. If it fails now, `/ws/logs` is innocent and the
   regression is elsewhere in the teardown path.*
2. Live → open `/ws/logs` → close everything → wait → Live again.
   *If only this one fails, the fork is implicated.*

Watch `VmRSS` in `/proc/<pid>/status` before and after each stop — that
separates "does not restart" from "does not release".

## Evidence on the host

```
/c/tmp/machino-forensics/
  01-state.txt          free / meminfo / ps after the kill
  02-dmesg-log.txt      OOM lines + log tail
  03-firstfail.txt      the first failure with 25 lines of context
  04-reboot-question.txt  daemon starts, mounts, streamerctl status
  05-dmesg-full.txt     full ring buffer (231 lines)
  06-machino.log        the whole daemon log (224 lines)
  07-syslog.txt         logread output (552 lines)
```

## Status

- Camera is **up and reachable over SSH**; machino is **not running**.
- Memory is healthy again (26 MB free) precisely because the daemon is dead.
- No fix attempted, no configuration changed, no restart, no power-cycle.
- Rollback to the pre-deploy build remains available and verified:
  `/c/tmp/machino-rollback/`.
