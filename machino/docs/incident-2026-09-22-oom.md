# Incident: OOM after a clean pipeline stop — 2026-09-22, build `e611220`

Acceptance run 1 was stopped here. Nothing was restarted, reconfigured or
power-cycled; the state was captured first. All volatile evidence is on the
host in `/c/tmp/machino-forensics/` — **`/var/log` is tmpfs on this camera**, so
a power-cycle would have destroyed the log.

## What actually happened

Reconstructed from `machino.log`, `dmesg` and the OOM killer's own output.

```
05:49:35  boot (see "an earlier reboot" below)
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

Four full probe/stream cycles, one per retry round. So `IMP_System_Init`
returning `-1` is **not** the driver refusing — the most likely reading is
"already initialised", i.e. the previous `IMP_System_Exit` did not fully
release even though our teardown path completed. There is also one
`IRQ Error, cpu: 0 Cause:0x08800000` right after a `stream off`.

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

A `fork()` duplicates the address space and every open descriptor. Our
descriptors are `CLOEXEC`, so they close when the child `exec`s `logread` — but
between `fork` and `exec` the child holds a reference to the IMP device nodes.
If the vendor driver counts those references, the parent's `IMP_System_Exit`
five seconds later would release nothing, and the next `IMP_System_Init` would
return "already initialised".

**This is a hypothesis, not a finding.** It fits the timing and it fits M4's
result (twelve cold cycles in one process, proven before `/ws/logs` existed),
but nothing here proves it.

## An earlier reboot, unexplained

`/var/log` is tmpfs. The log contains exactly one daemon start (05:49:47),
while the deploy power-cycle was at 05:43:57 — so **the camera restarted at
about 05:49:35** and that boot's log is gone. Nobody power-cycled it then. The
UI was reported as "hanging sporadically" around that time, so this may have
been a first, unrecorded occurrence of the same fault. It cannot be
reconstructed.

## Discriminating test (do not run yet)

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
