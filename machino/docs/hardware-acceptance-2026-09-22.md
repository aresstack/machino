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
