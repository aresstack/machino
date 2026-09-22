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

**The failure comes late in the bring-up.** On every failed attempt `dmesg`
shows the driver getting all the way through a sensor bring-up:

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
IMP without trouble on a kernel that had just OOM-killed its predecessor.

> **WITHDRAWN.** Later testing contradicts this: after Test B broke the camera,
> neither a graceful restart nor `SIGKILL` + start produced a process that
> could initialise IMP. See "The damage survives process replacement" below.
> Why *this* particular restart succeeded is still unexplained. The reading was
> premature — one successful restart is not a property.

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

## Fix A — hardware accepted

Deployed `107f05f`, cold power-cycle, then the acceptance run with no browser
at any point: RTSP and scripted WebSocket only.

| check | result |
|---|---|
| `logread` at daemon start, **zero** log clients | **1** (pid 993, ppid 990) - forked before IMP could exist |
| `logread` with 1 client, 2 clients, back to 0 | **1**, unchanged throughout |
| `/ws/logs` subscriptions over the run | **10** |
| forks of `logread` over the run | **1** (`started once at boot`) |
| pipeline generations over the run | **8** |
| **`IMP_System_Init` failures** | **0** |

The decisive one: a media start **after** a `/ws/logs` cycle taken while the
pipeline was ACTIVE. Before Fix A that answered `503 Service Unavailable` every
time. Now:

```
after 1 log client   RTSP/1.0 200 OK   2 050 049 B
after 2 log clients  RTSP/1.0 200 OK   1 966 536 B
```

`VmRSS` 3 880 kB and `MemAvailable` 26 240 kB at COLD_IDLE after the whole run -
the same healthy figures as a fresh boot, with no accumulation.

What this does *not* prove is the mechanism. The fix removes the dangerous
operation rather than explaining the Ingenic behaviour behind it, and the
`fork()` itself was never isolated. But the reproducible failure is gone, and
the three invariants that were broken - one logread ever, no fork from a
request, no orphaned child - all hold.

## C1 — run, and it settles the question (and subsumes C3)

Cold boot, scripted session, no browser. `/ws/logs` opened and closed
**entirely while COLD_IDLE**, so IMP had never been initialised when the child
was forked. Then media.

```
05:58:19  /ws/logs: streaming logread (pid 1036)      <- fork, IMP never up
05:58:26  /ws/logs: last subscriber left
06:00:03  COLD_IDLE -> STARTING   ING_PLAT up          -> 200 OK, 2 098 618 B
06:00:15  ING_PLAT down                                   [child still alive]
06:00:25  COLD_IDLE -> STARTING   ING_PLAT up          -> 200 OK, 2 047 694 B
```

**C1 PASSES.** Both starts worked. `VmRSS` finished at 2 948 kB, the same
healthy pattern as Test A.

### This also answers C3, without needing to run it

C3 was meant to test whether the logread child *overlapping the IMP teardown*
is what breaks things. In C1 that is exactly what happened — the child was
still alive at `ING_PLAT down` and right through the following restart — and
**nothing broke**.

So the child's lifetime overlapping the teardown is **not sufficient**.

### What is left is the fork's timing

| | fork happened while | next re-init |
|---|---|---|
| Test B | IMP **live** (ACTIVE) | **FAIL** |
| C1 | IMP **down** (COLD_IDLE) | **PASS** |

Same camera, same build, same `/ws/logs` code path, same lingering child. The
one variable is whether Ingenic/IMP state existed at the moment of `fork()`.

Stated precisely, because the distinction matters:

- **Proven:** opening `/ws/logs` while IMP is ACTIVE makes the *next* IMP init
  fail after the following teardown.
- **Proven:** a `logread` child started in COLD_IDLE may then overlap an IMP
  start, a teardown and a re-init without triggering anything.
- **Therefore:** "`fork()` while IMP is live" is the leading mechanism and is
  strongly supported — but **the fork itself has not been isolated**. Doing so
  needs a build that forks something harmless at the same point, which has been
  deferred because the architectural fix avoids the dangerous operation either
  way.

*Why* it damages anything, in which layer, and why `IMP_System_Init` fails only
after a full driver bring-up all remain unexplained.

It also means the proposed fix is aimed at the right thing: start the `logread`
helper **once at daemon start, before the first IMP init**, and keep it. C2 as
designed cannot be run anyway — see the next finding.

## New defect found while setting C1 up: the logread child never dies

The log says `/ws/logs: last subscriber left`, and `logs_stop()` sends
`SIGTERM` — but the child is still there:

```
05:58:26  last subscriber left
06:01:xx  1036  991  S  logread -f        (still running, >2 minutes later)
          State: S (sleeping)   SigIgn: 0000...0000
```

`SigIgn` is zero, so it is not ignoring the signal. Either the `kill` never
reached it or busybox's handler does not exit. Two consequences:

1. **A child forked from the daemon outlives its purpose indefinitely.** In the
   original incident this is why the child was still alive across the teardown —
   not because of timing, but because it never leaves.
2. `logs_start()` returns early only while `logs_fd_ >= 0`. After a stop that
   is `-1`, so **the next subscriber forks another child while the old one
   lives** — repeated visits to the Logs page accumulate `logread` processes.

This is independent of the OOM and wants its own fix.

## Planned next: C1/C2/C3, no code change

The `/bin/true` diagnostic build is deferred. Three tests can separate the
candidates without touching code, each from a cold boot, no browser, scripted
session:

- **C1** - `/ws/logs` opened and closed entirely while COLD_IDLE, child reaped,
  *then* the first media start. Fails ⇒ the `/ws/logs`/logread lifecycle alone
  is enough and "fork during live IMP" is largely cleared.
- **C2** (if C1 passes) - `/ws/logs` opened and closed while ACTIVE, child
  fully reaped, streaming continued afterwards, *then* teardown and restart.
  Fails ⇒ forking/cleanup during live IMP is enough, independent of the
  teardown.
- **C3** (if C2 passes) - `/ws/logs` held open *across* the teardown and closed
  only after COLD_IDLE. Fails alone ⇒ the overlap of the logread child with
  `IMP_System_Exit` is the interaction, which is exactly what the original
  incident looked like.

Stop at the first FAIL, capture, cold power-cycle before the next branch.

## Test C — could not be run as designed, and what was found instead

The test asked for was `fork()` + `exec("/bin/true")` from machino while the
pipeline is ACTIVE, to separate "forking is the problem" from "`/ws/logs`'s own
lifecycle is the problem". **It cannot be run without changing code.** The fork
has to happen *inside* the machino process to mean anything, and machino forks
in exactly two places: `logread` (the suspect) and `chpasswd` in the setup path,
which this claimed camera refuses with 403 before it ever forks. A fork from a
shell proves nothing about machino's IMP state.

The substitute — open `/ws/logs` while COLD_IDLE, so no IMP state exists at
fork time — **also could not run**: the session cookie was invalidated by the
daemon restart and the upgrade answered `401`, so no child was forked. But the
attempt produced a more important result anyway.

### The damage survives process replacement

After Test B had broken it:

| | result |
|---|---|
| graceful `restart`, fresh process, `/ws/logs` never touched | `IMP_System_Init failed (-1)`, **503** |
| `SIGKILL` + start, fresh process | `IMP_System_Init failed (-1)`, **503** |

**This contradicts an earlier conclusion in this note and withdraws it.** After
the *original* OOM, a restarted process initialised IMP without trouble, and I
wrote that the broken state "does not outlive the process". It does. Two
different terminations, both followed by a process that cannot initialise.

Why the earlier restart worked is unexplained. The two cases differ in when
`/ws/logs` was opened (GRACE_IDLE then, ACTIVE now) and in how much else the
OOM killer tore down, but nothing here settles it.

### How far the bring-up gets before it fails

On every failed attempt, including after the restarts:

```
probe ok ------->imx307 · chip found @ 0x1a · Create framechan0/1/2 OK
tx_isp_vic_start · imx307 stream on · imx307 stream off   (~0.8 s later)
IRQ Error, cpu: 1 Cause:0x08800000
```

`lsmod` shows `tx_isp_t40` and `avpu` loaded with their usual users.

**What this establishes:** `IMP_System_Init()` reports failure only *after* an
extensive and apparently successful driver bring-up — probe, chip detection,
three frame channels, VIC start, sensor streaming on and off again.

**What it does not establish:** which layer is at fault. An earlier version of
this note concluded "the driver is fine, the fault is in libimp". That does not
follow and is withdrawn — a driver, ioctl or underlying-resource problem can
produce exactly this picture, with the vendor library only noticing at the end.
The `IRQ Error` after `stream off` is a hint that something below is unhappy,
not proof either way.

### Practical severity

On this build, **one visit to the Logs page while the camera is streaming
appears to stop it streaming until the box is rebooted.** Restarting the daemon
does not recover it. That has not been confirmed against an actual
power-cycle — the camera is in the broken state now and a power-cycle is the
obvious next thing to try — but every process-level recovery tried has failed.

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

---

## Nachtrag 2026-09-22, Runde 4: Hard-Lockup nach COLD BOOT

**Die bisherige Erklärung "Warm-Start + Browser-Burst" ist in dieser Form
widerlegt.** Derselbe Hard-Lockup trat nach einem echten Cold Power-Cycle beim
**ersten Post-Login-Seitenaufbau** im Browser auf. Warmstart kann ihn
begünstigen, ist aber **nicht notwendige Voraussetzung**.

### Ablauf

```
Cold Power-Cycle (Fix B, 065da41)
  uptime 0 min, VmRSS 1516 kB, COLD_IDLE, 1 logread, alle Zähler 0
CLI-Regression (RTSP + /ws/logs während ACTIVE, 2 Generationen)  -> sauber
Baseline 09:05, Sampler ab 09:06                                 -> flach
Browser: Login-Seite  ok
Browser: Login        ok
Browser: weiße Seite, lange Ladephase
Browser: "Webseite nicht erreichbar"
Kamera: kein Ping, kein ARP, Ports 22/80/554 tot
```

Der lokale Adapter war nachweislich intakt (`192.168.1.222` gesetzt, Gateway
antwortet); am Adapter wurde nichts verändert. Der Ausfall liegt an der Kamera.

### Was das für 10.4 heißt

10.4 ist **FAIL**, und die Zeile ist **gar nicht bis zum Mehrclient-Test
gekommen**. Der Fehler trat bei Schritt 1 auf, bevor ein zweiter Live-Client
oder `/ws/logs` beteiligt war. In Runde 2 brauchte es den dritten/vierten
Client, und SSH überlebte; hier stirbt die gesamte Netzwerkschicht sofort.

Ob das derselbe Mechanismus ist, ist **offen**. Ein OOM-Kill mit lebendem SSH
und ein Totalausfall der Netzwerkschicht sind zwei verschiedene Bilder.

### Was Fix A und Fix B damit (nicht) leisten

Sie schließen diesen Lockup **nicht**. Was sie belegbar geschlossen haben, gilt
weiter und wurde in diesem Lauf 20 Minuten vorher noch einmal bestätigt: ein
einziger `logread`, keine Retry-Kaskade, `init_failures 0`, `init_retries 0`,
und die zuvor mit 503 scheiternde Sequenz liefert wieder Daten. Das war
offenbar nicht die Ursache dieses Symptoms.

### Warum die frühere Gegenprobe trog

Die Gegenprobe, die "frisch gebootet ist dieselbe Last harmlos" stützte, benutzte
**synthetische curl-Bursts**, keinen echten Browser nach dem Login. Der
Browser-Pfad enthält mehr als paralleles GET: Session-Cookie, die von busybox
auf `:85` durchgereichten Assets über die Front-Door, `/ws/video`, WebRTC.
Meine CLI-Regression deckte RTSP und `/ws/logs` ab — genau diesen Delta **nicht**.

### Beweislage

Unvollständig, und das ist eine Lehre für sich. `/tmp/soak.csv` (die
Sekundenkurve in den Lockup hinein) und `/var/log/machino.log` liegen auf
**tmpfs** und sterben mit dem Power-Cycle. Ohne UART ist von diesem Lauf nichts
zu retten.

### Nächster Test: den Browser-Pfad zerlegen, nicht wiederholen

```
1. nur /login.html
2. nur POST /login
3. danach GET /
4. danach Assets sequenziell
5. danach Assets parallel
6. erst danach MSE/WebRTC
```

Ziel ist die Frage, ob bereits der parallele Proxy-/Asset-Burst die Box umlegt,
**bevor Video überhaupt beteiligt ist**. Erst danach der Restart-Batch
(3.3/3.4, 4.2, 4.4-4.6, 9.2-9.8) als eigener Block mit eigenem Power-Cycle.
