# Hardware acceptance — HEAD of 2026-09-22

Fixed test plan for the first deploy since the WebRTC slice. Fill `Observed`
and the verdict **while testing**, not afterwards.

The camera currently runs `1b7225b` (WebRTC slice). This deploy carries AP1–AP11
plus 14 review fixes — roughly twenty commits — so the point of a written plan
is that nothing gets tested twice and nothing gets forgotten once.

## Verdicts

| Verdict | Means |
|---|---|
| **PASS** | did what `Expected` says |
| **FAIL** | did not — a regression or a defect |
| **ABSENT-AS-EXPECTED** | the feature is not implemented and the page/client degraded without an error |
| **ABSENT-BUT-NOISY** | not implemented **and** the UI showed an error, a spinner that never ends, or a broken layout |

`ABSENT-BUT-NOISY` is the verdict this run exists for. `docs/dropin-gaps.md`
claims several pages treat a missing answer as "this camera cannot say" — that
was **read from their code, never observed**. Any `ABSENT-BUT-NOISY` reclassifies
that gap from "decide whether it is in scope" to "must be handled".

## Before starting

1. Deploy HEAD, then a **cold power-cycle** — not a reboot.
   `t40nn-freeze-nach-install`: a warm install wedges the whole box on the first
   parallel connection burst; a freshly booted one survives the same load.
2. Do not open a browser before the power-cycle.
3. Recovery if anything wedges: restore `/c/tmp/machino-rollback/
   machino-preDeploy-1b7225b.tar.gz` over `/`, `sync`, cold power-cycle.

---

## 1 Bring-up

| # | Feature | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 1.1 | Boot / daemon starts | after the power-cycle, `ps w \| grep machino` | one process, argv0 `{majestic}` | | | |
| 1.2 | Listening sockets | `netstat -ltnp` | 80 + 554 by machino, 127.0.0.1:85 busybox | | | baseline has exactly this |
| 1.3 | Startup log | `head -40 /var/log/machino.log` | non-empty, claim state line, `onvif: off` | | | 0 bytes = AP5 regression |
| 1.4 | No crash loop | `uptime`, PID stable after 5 min | same PID | | | |

## 2 Session

| # | Feature | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 2.1 | Login | WebUI login, correct password | session cookie, Live page loads | | | |
| 2.2 | Wrong password | deliberately wrong | rejected, no session | | | |
| 2.3 | Logout | `#nav-logout` | back to login page | | | |
| 2.4 | Session after browser close | close browser, reopen | still signed in within 12 h | | | |
| 2.5 | Dashboard | open it | tiles populate, no "not responding" banner | | | `/metrics` |

## 3 Video

| # | Feature | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 3.1 | MAIN WebRTC | Live page, default transport | picture, ~100 ms | | | the accepted baseline |
| 3.2 | MAIN MSE fallback | switch transport to MSE | picture | | | ~276 ms is the known figure |
| 3.3 | SUB WebRTC | stream selector → sub | picture | | | **first hardware run of AP1** |
| 3.4 | SUB MSE | same, MSE | picture | | | |
| 3.5 | Transport switch | WebRTC ↔ MSE repeatedly | no stall, no black frame | | | |
| 3.6 | Two viewers | two tabs on MAIN | both live | | | |

## 4 RTSP

**Read this before filling in 4.3.** `rtsp.auth` defaults to **`false`** and is
settable only in `machino.conf` — it is not in the WebUI schema. With
`system.unsafe=false` and the camera claimed, `RtspAuth::required()` is
therefore **false**: a bare RTSP client gets a stream with **no credentials**.
That is *not* Majestic's behaviour (upstream: "these endpoints authenticate as
user root with the same password you use for this WebUI"), and it is a
drop-in deviation that `docs/dropin-gaps.md` does not list. Record it here as
found, not as expected-to-be-fixed.

| # | Feature | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 4.1 | RTSP MAIN | `ffplay rtsp://cam:554/ch0` | picture | | | |
| 4.2 | RTSP SUB | `…/ch1` | picture | | | needs `video.1.enabled` |
| 4.3 | RTSP without credentials, defaults | `ffplay` with no user | **stream plays** (see note) | | | a 401 here would be a *change*, record it |
| 4.4 | RTSP with `rtsp.auth=true` | set in `machino.conf`, restart daemon | 401 without credentials | | | needs a restart, not a PATCH |
| 4.5 | RTSP with credentials | `rtsp://root:<pw>@cam/ch0` | picture | | | only meaningful with 4.4 |
| 4.6 | `system.unsafe=true` | set, restart, retry 4.4 | auth off again | | | `unsafe` outranks everything |
| 4.7 | Revert | put `machino.conf` back, restart | as 4.1 | | | |

## 5 RTSP lifecycle (AP2)

| # | Feature | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 5.1 | `rtsp.enabled=false` | via API/config, apply | port 554 gone from `netstat` | | | |
| 5.2 | back to `true` | | 554 listening again, stream works | | | |
| 5.3 | port 554 → 8554 | | 8554 listening, 554 gone, stream on 8554 | | | |
| 5.4 | 8554 → 554 | | back as 5.2 | | | |
| 5.5 | Rebind under load | while a client is streaming | old session ends cleanly, no crash | | | |

## 6 Logs (AP5)

| # | Feature | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 6.1 | `/ws/logs`, one viewer | open Logs page | lines arrive live | | | |
| 6.2 | two viewers | second tab | both get lines | | | one shared `logread` |
| 6.3 | close one | | the other keeps running | | | |
| 6.4 | close both | `ps \| grep logread` | child gone, **no zombie** | | | review finding 13 |
| 6.5 | Log source | does the daemon's own output appear? | yes, under the `majestic` filter | | | syslog ident |
| 6.6 | Rotation | `ls -la /var/log/machino.log*` | ≤ 256 KiB + one `.1` | | | may need a long run |

## 7 Settings

| # | Feature | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 7.1 | Settings load | open the page | sections render, no console errors | | | |
| 7.2 | Save | change bitrate, save | applied, survives reload | | | |
| 7.3 | Per-row reset | reset one field | back to default | | | `/api/v1/reset` |
| 7.4 | Image controls persist | change brightness, save, reload | value held | | | |
| 7.5 | Image live preview | drag a slider **without** saving | **nothing happens live** | | | **the one known visible gap** — `/api/v1/image` 404s. Values still apply on save (7.4). Expect `ABSENT-AS-EXPECTED`; `ABSENT-BUT-NOISY` if an error is shown |
| 7.6 | Config survives restart | restart daemon | settings held | | | |

## 8 Pages with no backend — *the real purpose of this run*

For each: does the page degrade quietly, or does it shout?

| # | Page | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 8.1 | OSD | settings → OSD | section **not offered** | | | deliberately not in the schema |
| 8.2 | OSD geometry | `curl -s -o /dev/null -w '%{http_code}' …/api/v1/osd` | `404` | | | 404 is the contract, not a fault |
| 8.3 | Recording / SD card | `sdcard.cgi` | degrades, no error banner | | | `/api/v1/records/*`, `/metrics/records` |
| 8.4 | Recordings / timeline | `recordings.cgi` | degrades | | | `/api/v1/analytics/day` |
| 8.5 | Analytics overlay | Live page | no overlay, no error | | | `/ws/analytics` |
| 8.6 | GPIO / Pins | the Pins UI | degrades | | | `/api/v1/gpio`, `/pinmux`, `/ws/pins` |
| 8.7 | Cameras / Peers | `cameras.html` | degrades | | | `/api/v1/peers` |
| 8.8 | Night / IR-cut | night mode section | degrades; upstream treats `irCut:"off"` as a decision, not a defect | | | `/night/*`, `/metrics/night` |
| 8.9 | Peer overlay | Live → peer | degrades | | | `/api/v1/calibration/*` |
| 8.10 | `image.jpg` | `curl -o /dev/null -w '%{http_code}' …/image.jpg` | 404 | | | JPEG path is off by mandate |
| 8.11 | Snapshot | `…/snapshot` | note what it does | | | JPEG wedge — **stop if the daemon hangs** |

## 9 ONVIF (AP11)

| # | Feature | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 9.1 | Default off | `curl -o /dev/null -w '%{http_code}' …/onvif/device_service` | `404` | | | `onvif.enabled=false` |
| 9.2 | After enabling | set `onvif.enabled=true`, restart | `GetSystemDateAndTime` answers unauthenticated | | | |
| 9.3 | Auth required | `GetDeviceInformation` with no credential | `401` + `ter:NotAuthorized` | | | |
| 9.4 | WSSE PasswordText | with root + WebUI password | 200 | | | falls back to `/etc/shadow` |
| 9.5 | HTTP Digest | with `onvif.password` set | 200 after the challenge | | | needs the cleartext |
| 9.6 | Discovery | ODM / `wsdd` probe | camera appears | | | WS-Discovery, UDP 3702 |
| 9.7 | Stream URI | `GetStreamUri` | a `rtsp://<reachable-ip>:554/ch0` that plays | | | address must be reachable **from the client** |
| 9.8 | Turn off again | revert, restart | as 9.1 | | | leave it off |

## 10 Lifecycle and resources

| # | Feature | Test | Expected | Observed | Verdict | Notes |
|---|---|---|---|---|---|---|
| 10.1 | Shutdown after last consumer | close every viewer, wait past the grace | pipeline stops, sensor idle | | | `/api/v1/state` |
| 10.2 | Restart on demand | reopen Live | picture within the usual join time | | | |
| 10.3 | Memory when idle | `free` | compare with the baseline | | | baseline: `MemFree 11264 kB` |
| 10.4 | Memory under MAIN+SUB+WebRTC | `free` during | no steady decline over 10 min | | | 48 MB userspace total |
| 10.5 | Socket/process state | `netstat`, `ps` | no accumulating CLOSE_WAIT, no zombies | | | |
| 10.6 | 30 min soak | leave one viewer running | no stall, no growth | | | |

---

## Recording the result

- Every row gets a verdict. An untested row stays empty and is reported as
  untested — not as a pass.
- Any `FAIL` or `ABSENT-BUT-NOISY`: capture the HTTP status, the console error
  and the matching lines from `/var/log/machino.log` **before** moving on.
- If the camera wedges: stop, do not reboot from the shell (shutdown hangs at
  `Seeding 2048 bits`), cold power-cycle, and write down what the last action
  was.

---

# Run 1 — 2026-09-22, HEAD `e611220`, after a cold power-cycle

Deployed, verified by hash against the bundle, cold power-cycled. No wedge, no
crash, one `machino` process throughout.

## Filled in

| # | Verdict | Observed |
|---|---|---|
| 1.1 | **PASS** | one process, `{majestic} /usr/bin/machino`, uptime 0 min after the cycle |
| 1.2 | **PASS** | `:80` + `:554` machino, `127.0.0.1:85` busybox — matches the baseline |
| 1.3 | **PASS** | log populated. `machino e611220 starting`, `onvif: off`, `claim state: claimed`, `lifecycle=COLD_IDLE`. **This is AP5's hardware acceptance** — the symptom it fixed was a 0-byte log |
| 1.4 | **PASS** | PID stable, no crash loop |
| 2.1 | **PASS** | `POST /login` 200 + session cookie. Also exercises the review's CSPRNG change on real hardware |
| 2.2 | **PASS** | wrong password 403; unauthenticated `/api/v1/state` 401; log shows `login ok` / `login REJECTED` with **no credential in it** |
| 4.1 | **PASS** | OPTIONS/DESCRIBE/SETUP/PLAY over TCP-interleaved; **194 135 bytes of RTP in 3 s**; first frame 96 ms after PLAY (on-demand IDR) |
| 4.3 | **PASS (known deviation)** | DESCRIBE **without credentials → 200 + SDP**. Exactly as recorded: `rtsp.auth` defaults false, so Machino does not match Majestic here |
| 5.1 | **PASS** | `rtsp.enabled=false` → `:554` gone, live, no restart |
| 5.2 | **PASS** | back to `true` → `:554` listening again |
| 5.3 | **PASS** | port → 8554: rebinds, and answers OPTIONS on the new port |
| 5.4 | **PASS** | back to 554 cleanly. **AP2 hardware-accepted** |
| 7.2 | **PASS** | bitrate 3000→2500 stored and read back |
| 7.3 | **PASS** | reset of `video0.bitrate_kbps` → 200. *(First attempt used `video0.bitrate` and 404'd — my key was wrong, not the camera's: the schema advertises `bitrate_kbps`, which is what the page sends.)* |
| 7.5 | **ABSENT-AS-EXPECTED** | `/api/v1/image` → 404 (protocol level). Visual confirmation still owed |
| 8.1/8.2 | **PASS** | `/api/v1/osd` → 404, the contract's "this build cannot say" |
| 8.3–8.9 | *protocol level only* | all 404: `/api/v1/gpio`, `/pinmux`, `/peers`, `/records/resume`, `/analytics/day`, `/metrics/records`, `/metrics/night`, `/night/toggle`. **Visual degradation not yet checked** |
| 8.10 | **PASS** | `/image.jpg` → 404 |
| 8.11 | **PASS** | `/snapshot` → **501**, and the daemon survived. The JPEG wedge is not reached at all — the encoder reports unsupported instead of being created |
| 9.1 | **PASS** | `/onvif/device_service` → 404 with `onvif.enabled=false` |
| 10.1 | **PASS** | ACTIVE → GRACE_IDLE (5 s) → STOPPING → COLD_IDLE, 166 frames, `pool exhausted 0` |
| 10.3 | **PASS** | `MemFree 20 692 kB` idle (baseline before deploy: 11 264 kB) |
| 10.5 | **PASS** | one machino process, **no zombies**, no CLOSE_WAIT. *(An earlier count of "2 zombies" and a stray `logread` were artefacts of my own command line matching itself.)* |

## Two findings from this run

**The substream cannot be enabled through the API.** `PATCH {"video":{"1":…}}`
answers `unknown_field: video.1`. AP1's substream is therefore **config-file
only**, and since the schema does not advertise a `video1` section the stock
WebUI cannot enable it either. Rows 3.3, 3.4 and 4.2 need a `machino.conf`
edit plus a daemon restart.

**The schema advertises six sections** — `video0`, `sensor`, `image`,
`latency`, `performance`, `ai`. Not `rtsp` and not `lifecycle`, so the RTSP
enable/port controls that 5.1–5.4 just proved work are reachable only through
the native API, never from the stock settings page. Neither is a regression;
both belong in `dropin-gaps.md`.

## Run 1 stopped here

| # | Verdict | Observed |
|---|---|---|
| 3.1 | **PASS** | MAIN WebRTC ~57 ms, 0 % loss, EXCELLENT |
| 3.2 | **PASS** | MAIN MSE ~247-285 ms |
| 3.5 | **PASS** | WebRTC <-> MSE switching |
| 3.6 | **PASS** | two live windows, WebRTC ~54 ms and MSE >=285 ms in parallel |
| 6.1 | **PASS** | /ws/logs, one viewer |
| 6.2 | **PASS** | /ws/logs, two viewers |
| 10.4 | **FAIL** | a further live client stays on "connecting", memory climbs to ~95 %, WebUI becomes unreachable. Kernel OOM-killed machino (anon-rss 27 208 kB). See `docs/incident-2026-09-22-oom.md` |

The overlay sitting at ~32 % is **not** implicated: AP8 measured 6.0 MB free of
8.7 MB before the deploy, which is the same occupancy.

## Still open

- **Browser rows**, which need a person at the UI: 2.3–2.5, 3.1–3.6 (Live
  picture and transport switching), 6.1–6.5 (`/ws/logs`), 7.1, and the *visual*
  half of 8.3–8.9 — the `ABSENT-AS-EXPECTED` vs `ABSENT-BUT-NOISY` call, which
  is the whole point of this run.
- **Rows needing a daemon restart**, best done as one batch to keep the number
  of warm restarts down: 3.3/3.4/4.2 (substream), 4.4–4.6 (`rtsp.auth=true`),
  9.2–9.8 (ONVIF).
- 6.6 (log rotation) needs a long run; 10.4/10.6 need sustained load.

## Round 4 — Fix B (`065da41`), cold power-cycle, 2026-09-22

Deployed after the CI/MIPS build, installed cold, power-cycled by the user,
nothing tested warm. Running binary `machino 065da41`, uptime 0 min at the
start of the run.

| Row | Result | Evidence |
|---|---|---|
| 1.1 | **PASS** | one process, argv0 `{majestic} /usr/bin/machino`; busybox httpd separate on :85 |
| 1.2 | **PASS** | `:80` + `:554` owned by machino (991), `127.0.0.1:85` by httpd (980) — matches the baseline |
| 1.3 | **PASS** | 2 228 bytes, `machino 065da41 starting`, board/sensor/GPIO lines. Fix A's line is there verbatim: `logread -f started once at boot (pid 994); /ws/logs will never fork again` |
| 1.4 | **PASS** | PID 991 stable, no crash loop |
| 10.1 | **PASS** | two full cycles: `COLD_IDLE -> STARTING -> ACTIVE -> GRACE_IDLE -> STOPPING -> COLD_IDLE`, `pipeline_generation` 2 |
| 10.3 | **PASS** | idle `VmRSS 2 996 kB`, `MemFree 21 856 kB` — no accumulation across the run |
| 10.5 | **PASS** | zero zombies (checked via `State:` in `/proc`, not a `ps | grep`), zero CLOSE_WAIT, machino has exactly one child: `logread`, state `S` |

### The regression that mattered

The sequence that returned **503** before Fix A — RTSP to PLAY, `/ws/logs`
opened *while the pipeline is ACTIVE*, both closed, grace expired, RTSP
started again — was re-run against Fix B:

| | |
|---|---|
| RTSP-1 (with `/ws/logs` open during ACTIVE) | 3 396 901 bytes |
| `/ws/logs` | `HTTP/1.1 101 Switching Protocols`, lines delivered |
| RTSP-2, the start that used to fail | **1 333 796 bytes** |
| `503` in the log | **0** |
| `IMP_System_Init failed` / `bring-up failed` / `refusing bring-up` | **none** |
| `logread -f started` | exactly 1, for the whole run |
| `/ws/logs subscribed` | 1 |
| Telemetry | `init_failures 0`, `init_retries 0`, `last_init_rc 0`, `last_init_stage ""` |

All counts were read from the camera's own log and `/api/v1/telemetry`, not
from the test driver: two earlier PowerShell runs died silently, and in this
one the driver swallowed its own RTSP status lines into a return value, so the
byte counts and the log are what carry the result.

`init_retries 0` is the one that matters for Fix B: the counter exists so a
returning retry loop would be visible, and it stayed at zero through a full
start/stop/start cycle.

**Caveat, unchanged:** no bring-up actually failed in this run, so the new
stage telemetry (`last_init_stage`) and the sticky-FAILED path were NOT
exercised on hardware — only on the host, where they are pinned by tests. This
run shows the fix does no harm and the Fix A result holds; it cannot show the
fix working, because reproducing a failed init would mean provoking the very
condition we removed the trigger for.

### Still owed

- **10.4** — the row that failed in round 2 with the OOM. Needs a browser and
  several parallel live clients; that is the real test of Fix A + Fix B
  together and it is the user's to drive.
- **Rows needing a daemon restart** (3.3/3.4/4.2 substream, 4.4–4.6
  `rtsp.auth`, 9.2–9.8 ONVIF) are still batched and still deliberately not
  attempted: a warm restart is the documented lockup hazard, so that batch
  wants its own power-cycle window rather than being slipped into this run.

### 10.4 — BLOCKED / FAIL-PRECONDITION (nicht ausgeführt)

```
10.4 BLOCKED/FAIL-PRECONDITION:
first authenticated browser page load hard-locks device;
multi-client portion not reached.
```

Die Zeile darf **nicht** als durchgeführter Mehrclient-Test gelesen werden. Der
Ablauf endete an der Voraussetzung:

| Schritt | | |
|---|---|---|
| Cold Boot | ok | uptime 0 min, VmRSS 1516 kB, COLD_IDLE |
| Login-Seite | ok | |
| Login | ok | |
| erster Post-Login-Seitenaufbau | **Hard-Lockup** | weiße Seite, lange Ladephase, dann netztot |
| 2./3./4. Client, `/ws/logs`, 10-min-Soak | **nie erreicht** | |

Es liegen daher **keine** Aussagen vor zu: mehreren parallelen Live-Clients,
Speicherverlauf unter Last, `logread`-Stabilität unter Last oder dem Re-Init
nach dem Teardown. Die vier Kriterien, an denen 10.4 zu messen wäre, sind
sämtlich **unbeantwortet**, nicht etwa negativ beantwortet.

**UART war ebenfalls tot.** Das ist ein eigener Befund: ein harter SoC-/Kernel-
Lockup, nicht ein gestorbener Machino-Prozess und nicht ein ausgefallener
Netzwerkdienst. Ein Userspace-Prozess, der stirbt, nimmt die serielle Konsole
nicht mit.

Beweislage: keine. `/tmp/soak.csv` (Sekundenkurve in den Lockup hinein) und
`/var/log/machino.log` liegen auf tmpfs und sind mit dem Power-Cycle verloren.
Für den nächsten Lauf läuft daher ab Sekunde 0 ein **lokaler Paketmitschnitt**
auf `Ethernet 5` — der überlebt jeden Kamerawedge und zeigt, welcher Request
zuletzt beantwortet wurde und welcher nur noch SYN sah.

## Runde 5 — 10.4 durchgeführt und **PASS**, 2026-09-22

Gleicher Cold Boot, gleiches Binary `065da41`. Diesmal **ohne** den
Sekunden-Sampler auf der Kamera (siehe "Störgröße" unten); gemessen wurde
passiv über einen lokalen Paketmitschnitt plus drei kurze SSH-Abfragen.

### Die vier Kriterien

| Kriterium | Ergebnis |
|---|---|
| mehrere parallele Live-Clients | **3 gleichzeitig** (1× WebRTC, 2× MSE), ~15 min, 302 MB via WebRTC ausgeliefert |
| ein persistenter `logread` | **durchgehend genau 1**, State `S`, einziges Kind von machino |
| Speicher fällt zurück | COLD_IDLE **4948 kB**, nach einem weiteren Zyklus **4952 kB** |
| **der nächste Start gelingt** | `/ws/video` nach dem Teardown: **2 105 342 Bytes in 6 s**, Generation 3 |

Dazu: `init_failures 0`, `init_retries 0`, `dtls_failures 0`, `srtp_failures 0`,
0 Zombies, 0 CLOSE_WAIT, **0** Zeilen mit `send stalled` / `refusing bring-up` /
`bring-up failed`. Zustandsmaschine sauber
`COLD_IDLE -> STARTING -> ACTIVE -> GRACE_IDLE -> STOPPING -> COLD_IDLE`, zweimal.

### Kein Hinweis auf ein lineares Lifecycle-Leak

```text
Boot:          1536 kB
COLD_IDLE #1:  4948 kB
COLD_IDLE #2:  4952 kB   (+4 kB)
```

Das belegt **bounded warm retention über die beobachteten Zyklen**. Ein lineares
Zyklusleck hätte sich im zweiten Durchlauf wiederholt; +4 kB tut das nicht.

**Die Ursache der retained ~3,4 MB ist damit nicht experimentell isoliert.**
Naheliegend sind die Puffer-Pools, die ihre Kapazität absichtlich behalten
(`keeps capacity: no per-frame allocation once warm`), und musl-Heap, der
freigegeben aber nicht ans System zurückgegeben wird — beides ist eine
**Vermutung**, kein Messergebnis. Zwei Zyklen zeigen die Schranke, nicht den
Mechanismus.

### `send_err=3 (errno 11)` — vernachlässigbar, aber korrekt eingeordnet

`EAGAIN`: der UDP-Sendepuffer war dreimal kurz voll. Über den **gesamten**
13-Minuten-Mitschnitt blieb der Zähler bei **3**, bei 256 395 RTP-Paketen und
302 MB — 0,001 %. `pli` ging von 0 auf 1.

Ausdrücklich **nicht** behauptet: dass der Browser den Verlust "nicht bemerkt"
hat. `pli=0` beweist nur, dass **kein** Keyframe-Request kam, nicht dass nichts
aufgefallen wäre. Belastbar ist: der Zähler steigt nicht weiter und das Bild
blieb stabil.

### Nebenbefund: MSE degradiert unter Parallellast, WebRTC nicht

Gemessen im Browser bei drei gleichzeitigen Consumern:

| | WebRTC | MSE |
|---|---|---|
| Delay | **73 ms** | **>= 904 ms** |
| Verlust | 0,0 % | 165 von 11 788 Frames verworfen |
| Re-Buffering | picture restarts 1 | **60x** |

Der bekannte MSE-Wert aus dem Latenz-Paket war ~276 ms. Die UI nennt den Grund
selbst: `fMP4 over WebSocket/TCP - no feedback channel`. WebRTC sieht den Verlust
über RTCP und passt sich an, MSE über TCP kann nur puffern. Erwartbares
Verhalten, aber es gehört als Messwert festgehalten und nicht stillschweigend
unter "10.4 PASS" verbucht.

### Störgröße: mein eigener Sampler

Im gescheiterten Lauf (Runde 4) lief ein Sekunden-Sampler **auf der Kamera**, der
pro Sekunde rund sechs Prozesse forkte (`date`, 3x `awk`, `pidof`, `wc`) und alle
fünf Sekunden zusätzlich zwei `wget` plus vier `sed`. In diesem Lauf lief er
nicht — und derselbe Browser, dieselbe Seite, dasselbe Binary kamen sauber durch.

Das ist ein **Kandidat, kein Beweis**: der OOM aus Runde 2 lief ohne Sampler und
ist damit nicht erklärt. Die belastbare Konsequenz ist methodisch: Messapparatur
auf dieser Box muss passiv sein. Der Paketmitschnitt kostet die Kamera nichts.
Das gesicherte WebUI-Log des Laufs enthält als einzige Nicht-Routinezeilen genau
drei `dropbear`-Logins - meine eigenen Abfragen.

### Was 10.4 ausdrücklich NICHT mit abgenommen hat

**Einen dauerhaft offenen `/ws/logs`-Client.** `ws_logs_clients` stand am Ende
auf **0**, obwohl die Logs-Seite offen sein sollte. Ob der Tab geschlossen war
oder die WebSocket-Verbindung abriss, ist ungeklärt. Die Log-WebSocket-Zeilen
**6.1-6.4 bleiben separat offen** und dürfen nicht aus 10.4 abgeleitet werden.

Der Nachweis "genau ein persistenter `logread`" gilt davon unberührt: der
Prozess existierte durchgehend genau einmal — das ist eine Aussage über Fix A,
nicht über einen abonnierten Client.

## Runde 8 — Relay-Keep-Alive (`c1edd92`) hardwareverifiziert

### Die direkt beobachtete Verhaltensaenderung

Zwei Requests ueber **einen** Socket, gemessen am Geraet:

| Pfad | vor dem Fix | nach dem Fix |
|---|---|---|
| nativ `/api/v1/state` | req2: 200 | req2: **200** |
| **relayed `/a/main.js`** | req2: **keine Antwort** | req2: **200, 15 155 B** |
| relayed CGI `live.cgi` | 200, 18 923 B (schliesst) | **200, 18 923 B** (schliesst weiterhin - kein `Content-Length`) |

Das ist der Nachweis fuer die Zustandsrueckstellung im Server, die sich als
Hosttest nicht schreiben laesst: `http_server.cpp` ist nicht im Host-Testbuild.

### Verbindungen pro Seitenaufbau (aus dem Paketmitschnitt)

Gleicher Reiz (echter Chrome-Seitenaufbau), gleiche Metrik, 20-s-Fenster:

| | vor dem Fix | nach dem Fix |
|---|---|---|
| SYNs zu :80 | 45 | **4** |
| HTTP-Requests | 57 | 46 |
| **Verbindungen pro Request** | **0,79** | **0,09** |

46 Requests ueber 4 TCP-Verbindungen. Faktor ~9.

**Vorbehalt:** der Vorher-Lauf war ein frischer Login, der Nachher-Lauf ein F5
mit warmem Cache - die Reize sind aehnlich, nicht identisch. Deshalb ist die auf
den Request normierte Zahl die belastbare, nicht die absolute.

### Zurueckgezogen: die "90 % weniger TIME_WAIT"-Zahl

Ich hatte `TW +515` (vorher) gegen `TW +51` (nachher) gestellt und daraus 90 %
gemacht. **Das war methodisch falsch und ist zurueckgezogen.** Das
Vorher-Fenster lief **neun Minuten** mit drei Browsern, mehreren Tabs und sechs
laufenden Medien-Consumern; das Nachher-Fenster zwei Minuten auf einem frisch
gebooteten System mit einem Consumer. Die +515 enthalten Hintergrundverkehr
vieler Tabs, nicht einen Seitenaufbau. Der Nutzer hat den Fehler gesehen, bevor
ich ihn gesehen habe.

Ebenfalls misslungen: der Peak-Sampler fuer gleichzeitig belegte TIME_WAIT-Slots
las das falsche Feld aus `/proc/net/sockstat` (`$6` ist das Wort `tw`, die Zahl
steht in `$7`) und meldete durchgehend 0. Der Peak ist damit **nicht gemessen**.

### Die Regression, die der Fix zuerst einbaute

Unmittelbar nach dem ersten Deploy: statische Assets korrekt mit Keep-Alive,
aber **jedes CGI 502** - die ganze WebUI waere tot gewesen. Ursache aus den
Bytes des Geraets: busybox schreibt fuer statische Dateien CRLF, reicht die
**bare-LF-Header eines CGI** aber unveraendert durch, und `find("\r\n\r\n")`
fand nie ein Ende. Behoben in `c1edd92`, 20 Zusicherungen mit den verbatim
abgegriffenen Bytes.

Gefunden wurde das, weil nach dem Install als erstes eine Zwei-Request-Probe
gegen einen statischen **und** einen CGI-Pfad lief - nicht ein Browser.

---

## AP0 — Baseline / Testzustand bereinigt, 2026-09-22

Vor dem Cold Power-Cycle erledigt:

| Punkt | Ergebnis |
|---|---|
| AP0.7 Diagnose-Reste | keine laufenden Testprozesse; `/tmp/soak*`, `/tmp/peak*` und die Install-Tarballs entfernt; `/overlay/flash-test-DELETE-ME.bin` geloescht; genau ein Kind (`logread`), 0 Zombies |
| AP0.1 Config | aus `machino.conf.prebatch` wiederhergestellt, **md5 identisch** (`a290cd9a…`); `video.1.*`, `rtsp.auth` und `onvif.enabled` sind wieder Default; `sync` |
| AP0.3 keine Warmlast | eingehalten - seit dem Restore kein Browser, kein RTSP, keine Last |

### Abweichung von AP0.2, bewusst und dokumentiert

Das Arbeitspaket nennt `c1edd92` als Baseline-Binary. Installiert ist
**`d094fbc`** (SHA256 `b838f9b9…`), weil AP12 danach fertig wurde. Das ist eine
**Obermenge**: alle sieben in AP0.2 gelisteten Fixes sind enthalten, dazu ONVIF
`SetSystemDateAndTime` und eine praezisere SDP-Ablehnungsmeldung.

Beide Ergaenzungen sind in der Baseline wirkungslos: `SetSystemDateAndTime` ist
nur bei aktivem ONVIF erreichbar, und AP0.1 hat ONVIF gerade abgeschaltet; die
SDP-Aenderung betrifft ausschliesslich den Meldungstext bei identischem
Annahme- und Ablehnungsverhalten. Ein Rueckbau auf `c1edd92` wuerde Arbeit
entfernen, nicht Risiko.

### Diagnose-Einstellung

`printk` steht wieder auf `0 0 0 0` - es hat den letzten Power-Cycle nicht
ueberlebt, wie vorgesehen. Soll es fuer die weitere Hardlock-Beobachtung aktiv
sein, muss es nach **jedem** Boot neu gesetzt werden; ob das zur Baseline
gehoert, ist offen.

### Offen bis zum Cold Power-Cycle

AP0.4 (Boot-Baseline), AP0.5 (ein einzelner MAIN-Zyklus), AP0.6
(Relay-Smoke via `tools/relay-gate.ps1`) und AP0.8 (Messwerte eintragen).
