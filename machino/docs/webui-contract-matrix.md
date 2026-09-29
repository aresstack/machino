# WebUI contract matrix (AP4)

> **ÜBERHOLT (AP32, 2026-09-23).** Diese Matrix stammt aus AP4 und wird als
> Herleitung aufgehoben. Der aktuelle Stand steht in **`dropin-matrix.md`** —
> 58 Endpunkte, gegen den laufenden Build gemessen und einzeln klassifiziert.
>
> Mindestens eine Zeile hier ist inzwischen falsch: `/stream.mjpeg` wird nicht
> mehr bedient, sondern antwortet seit AP24 mit **501**, weil es vorher 200
> meldete und nie ein Bild liefern konnte. Zwei Matrizen, die beide „jeder
> Endpunkt" behaupten, sind genau die Quelle, aus der solche Widersprüche
> kommen.

Every HTTP endpoint, WebSocket and config contract the **unmodified**
OpenIPC/majestic-webui actually calls, and what Machino does about it.
Derived from the upstream clone (executed code, not docs), cross-checked
against `src/app/http/http_server.cpp`, `src/app/compat/majestic_webui.cpp`
and `src/app/api/api_service.cpp`.

Status values:

| Status | Meaning |
|---|---|
| `NATIVE` | Machino answers it itself |
| `RELAY` | forwarded verbatim to the internal busybox httpd (127.0.0.1:85) which serves the stock page/CGI |
| `STUB` | answered, but with a placeholder rather than real data |
| `MISSING` | the UI calls it and nothing answers meaningfully |
| `BLOCKED_HARDWARE` | implementable only once a hardware blocker is cleared |
| `NOT_APPLICABLE` | this camera/build has no such feature |

Everything not listed as `NATIVE` falls through the front door to the relay;
for a path busybox has no handler for, the UI sees busybox's 404.

## Session / auth

| UI feature | Endpoint | Method | Contract | Machino | Where | Tests | Next action |
|---|---|---|---|---|---|---|---|
| Login page | `/login.html` | GET | stock page, public | `RELAY` (public) | `http_server.cpp` gate | `test_session` | — |
| Sign in | `/login` | POST form | 200+`Set-Cookie: session` / 403; root only | `NATIVE` | `session.cpp` | `test_session` | — |
| Sign out | `/logout` | POST | 200 + clear cookie | `NATIVE` | `session.cpp` | `test_session` | — |
| Gate | every other path | — | 401 **without** `WWW-Authenticate` (no browser popup); 302 to `/login.html` for HTML navigations; localhost trusted | `NATIVE` | `http_server.cpp` | `test_session` | — |

## Config / schema (this is what makes the Settings page dynamic)

| UI feature | Endpoint | Method | Contract | Machino | Where | Tests | Next action |
|---|---|---|---|---|---|---|---|
| Read config | `/api/v1/config.json` | GET | majestic-shaped JSON, strings coerced | `NATIVE` | `majestic_config` | `test_compat` | — |
| Settings form | `/api/v1/config.schema.json` | GET | JSON-Schema the UI renders controls from (type, enum, min/max, default, `x-reload`, groups) | `NATIVE` | `majestic_schema` | `test_compat` | — |
| Save | `/api/v1/config` | POST | majestic body -> native patch; per-field apply result | `NATIVE` | `majestic_post_to_native` + `patch_config` | `test_api`, `test_compat` | — |
| Single value | `/api/v1/get?key=` | GET | plain text value of a dotted key; absent = miss | `NATIVE` | `majestic_get` | `test_compat` | — |
| Per-row reset | `/api/v1/reset?key=` | POST | schema default -> set it; **no** default -> remove the key (unset) with immediate runtime effect; 404 if unknown | `NATIVE` | `majestic_reset` / `unset_config` | `test_api` (unset, custom-profile trap, 500 on apply failure) | — |
| Native config | `/api/v1/config` | GET/PATCH/PUT | Machino's own document + `If-Match` revision | `NATIVE` | `api_service.cpp` | `test_api` | — |

**Dynamic-settings reality check.** The Settings page builds controls purely
from the schema, so any field Machino publishes appears without a UI change.
Machino already uses: type (bool/int/enum/string), `enum` values, min/max
ranges, defaults, `x-reload` apply semantics, and section grouping
(`video0`, `sensor`, `latency`, `image`, `performance`, `lifecycle`, `rtsp`,
`ai`). Deliberately **not** published: fields whose native class is
`daemon_restart`/`boot_only` (the stock UI could not truthfully apply them) -
`rtsp.max_clients`, `lifecycle.idle_grace_ms`, the latency socket/stall knobs.
Since AP2 `rtsp.enabled`/`rtsp.port` are genuinely live and *could* be
published; held back until the rendered section is seen on hardware.

What is **not** reachable this way: anything with a bespoke frontend -
ROI/region drawing, PTZ pads, the OSD editor, pin mux, the live transport
switch. Those need the specific endpoints below, not schema fields.

## Live video

| UI feature | Endpoint | Method | Contract | Machino | Where | Tests | Next action |
|---|---|---|---|---|---|---|---|
| Live (MSE) | `/ws/video?stream=N` | WS | text `{"type":"init",...}` + fMP4 init, then moof+mdat per frame; client `{"request":"idr"}`; `&audio=opus,mp4a.40.2` adds track 2 (first buildable codec, `audioCodec`/`mime` in the init JSON) | `NATIVE` main+sub (+audio with `CODECS=`) | `http_server.cpp`, `fmp4.cpp` | `test_fmp4`, `test_sps`, hardware-accepted | — |
| Live (WebRTC) | `/ws/webrtc?stream=N` | WS | `{req:offer/candidate}` -> `{reply:answer/candidate/stats/served/busy/error/closed}` | `NATIVE` main+sub | `webrtc/*`, `http_server.cpp` | `test_webrtc`, `test_dtls`, hardware-accepted | camera `stats` replies not sent yet (UI tolerates absence) |
| MJPEG fallback | `/mjpeg` | GET | multipart JPEG | `MISSING` **and** `BLOCKED_HARDWARE` | Machino serves `/stream.mjpeg`, not `/mjpeg` | — | JPEG path wedges the T40NN (`jpeg.enabled=false`); URL alias is pointless until that is solved |
| Snapshot | `/image.jpg` | GET | one JPEG | `MISSING` **and** `BLOCKED_HARDWARE` | Machino serves `/snapshot.jpg` | — | same blocker; consider `SetbufshareChn` instead of a second encoder |
| Raw frames | `/image.yuv420?crop=XxYxWxH` | GET | the Live page's detail still (preview-still.js): NV12 of the main channel, `X-Frame-Width/-Height`, `X-Pixel-Format`, `X-Stride-Luma/-Chroma` | `NATIVE` (`IMP_FrameSource_SnapFrame` on the running main channel, cropped on the 2x2 grid; captured on its own thread, the request parked - the poll loop never waits for a frame) | `PipelineManager::snap_nv12`, `http/stills` | `test_audio` (crop), `test_lifecycle` (snap) | `/image.dng` stays absent: no ISP raw tap |
| MP4 / HLS | `/video.mp4`, `/hls`, `/hls/index.m3u8`, `/video.m3u8` | GET | progressive fMP4 (H.264 + AAC when the microphone is on; `?audio=opus`, `?audio=none`, `?stream=1`); HLS v7 with fMP4 segments cut at key frames, a 4-segment / 6 MB window in RAM, stopped 30 s after the last player request; `/hls` is a viewer page (native HLS, else hls.js from the CDN) | `NATIVE` | `http_server` (shared with /ws/video), `http/hls` | `test_audio` (segmenter) | — |

## Streams / sources

| UI feature | Endpoint | Method | Contract | Machino | Where | Tests | Next action |
|---|---|---|---|---|---|---|---|
| Stream list | `/api/v1/sources` | GET | `sources[].streams[]` with id/subtype/codec/fps/size/flowing/rtsp | `NATIVE` | `majestic_sources` | `test_compat` (fixture shape) | advertises sub only when configured |
| RTSP URLs | (page text) | — | `rtsp://host[:port]/stream=N` | `NATIVE` | `rtsp_server.cpp` (`/ch0`,`/ch1` + `/stream=N` aliases) | `test_multistream` | — |
| Dashboard metrics | `/metrics` | GET | Prometheus text incl. the fields the dashboard reads | `NATIVE` | `majestic_metrics` | `test_compat` | — |
| Records panel | `/metrics/records` | GET | recording counters | `MISSING` | — | — | needs SD recording (not implemented) |
| Night panel | `/metrics/night?value=` | GET | IR-cut/light state | `STUB` | `majestic_config` reports `irCut:off` | — | needs IR-cut pin facts |

## Night / IR-cut

| UI feature | Endpoint | Method | Contract | Machino | Where | Tests | Next action |
|---|---|---|---|---|---|---|---|
| Toggle | `/night/toggle` | POST | switch day/night | `MISSING` | — | — | blocked on IR-cut GPIO identification |
| IR-cut | `/night/ircut` | POST | drive the filter | `MISSING` | — | — | same |
| IR light | `/night/light` | POST | drive the illuminator | `MISSING` | — | — | same |

## OSD

| UI feature | Endpoint | Method | Contract | Machino | Where | Tests | Next action |
|---|---|---|---|---|---|---|---|
| Overlay config | `/api/v1/osd` | GET/POST | overlay definitions | `MISSING` | — | — | AP8 (backend + schema, adapter mocked) |
| Overlay image | `/api/v1/osd/image?overlay=` | GET/POST | bitmap upload/fetch | `MISSING` | — | — | AP8 |

## System / setup / firmware

| UI feature | Endpoint | Method | Contract | Machino | Where | Tests | Next action |
|---|---|---|---|---|---|---|---|
| Claim flow | `/setup`, `/setup.html` | GET/POST | first-password flow for an UNCLAIMED camera (empty root hash) | `MISSING` (page relays, backend absent) | `shadow_check` already refuses an empty hash | — | AP9; also flips `system.unsafe` |
| Auth kill switch | `system.unsafe` (config key) | — | when true, **every** endpoint is unauthenticated (stock UI says so on the endpoints page) | `MISSING` | — | — | AP9; also the switch that turns RTSP auth on by default (see AP3) |
| Firmware upload | `/upload` | POST raw body | streamed upload with progress | `MISSING` | — | — | relay cannot carry it well; needs a native streaming sink |
| Upgrade progress | `/ws/upgrade` | WS | progress stream during sysupgrade | `MISSING` | — | — | WS cannot pass the relay |
| Logs page | `/ws/logs` | WS | one `logread -f` stream (klogd folded in) | `MISSING` | — | — | **AP5** |
| Terminal | `/ws/terminal` | WS | interactive shell | `MISSING` | — | — | out of scope for a media runtime |
| Dashboard/system pages | `/cgi-bin/*.cgi`, `/cgi-bin/j/*.cgi` (dashboard, network, time, save, run, files, download, sdcard, dmesg, recordings, fw-latest, logmeta, pulse, ptz, live) | GET/POST | stock CGI | `RELAY` | `relay_upstream` | exercised on hardware (AP2 slow-CGI test) | non-blocking since `8fc14d5`, ≤3 concurrent since `3c511bb` |

## Analytics / AI / peripherals

| UI feature | Endpoint | Method | Contract | Machino | Where | Tests | Next action |
|---|---|---|---|---|---|---|---|
| Detection overlay | `/ws/analytics` | WS | event stream drawn over the video | `MISSING` | M9 detection exists natively | — | good candidate once M9 is hardware-accepted |
| Analytics history | `/api/v1/analytics/day` | GET | per-day aggregates | `MISSING` | — | — | needs storage |
| AI settings | (schema `ai` section) | — | dynamic | `NATIVE` | `majestic_schema` | `test_compat`, `test_detection` | — |
| GPIO view | `/api/v1/gpio` | GET | kernel pin view | `MISSING` | — | — | read-only; cheap later |
| Pin mux | `/api/v1/pinmux`, `/ws/pins` | GET / WS | live pin state | `MISSING` | — | — | low priority |
| PTZ | `/ptz`, `/cgi-bin/j/ptz.cgi` | POST | pan/tilt | `NOT_APPLICABLE` (relay for the CGI) | — | — | this camera has no PTZ |
| Autofocus | `/autofocus`, `/autofocus/status` | POST/GET | lens AF | `NOT_APPLICABLE` | — | — | fixed lens |
| Audio in | `/audio.pcm`, `/audio.alaw`, `/audio.g711a`, `/audio.ulaw` | GET | microphone streams | `NATIVE` (at most 2 s of the wire format buffered per client; a client that stops reading for 10 s is dropped) | `AudioService`, `http/audio_stream` | `test_audio` | hardware acceptance with `machino --audio-test` |
| Speaker | `/play_audio` | POST | raw s16le at audio.srate (or WAV) to the speaker | `NATIVE` | `audio::Speaker`, `http_server` | `test_audio` | — |
| Encoded audio | `/audio.opus`, `/audio.m4a` | GET | Ogg Opus (48 kHz clock) / fragmented-MP4 AAC-LC at audio.srate | `NATIVE` with `CODECS=` (pinned libopus 1.5.2 + libfaac via `tools/fetch-codecs.sh`); 501 with the reason in a build without | `audio::AudioEncoder`, `http/ogg`, `http/fmp4` | `test_audio` (Opus decode round trip, Ogg CRC, esds/dOps) | — |
| MJPEG | `/mjpeg`, `/mjpeg.html` | GET | multipart JPEG + viewer page | `NATIVE` (alias of `/stream.mjpeg`; 501 while `jpeg.enabled=false`) | `http_server` | — | — |
| Stills | `/image.heif` | GET | one IDR of the main stream as a HEIF image item (`avci`: H.264 in HEIF - the encoder makes H.264) | `NATIVE` (its own capture thread, like `/image.yuv420`) | `fmp4::heif_avc_still` | `test_audio` | decodes with libheif >= 1.18 (AVC); older readers only know HEVC HEIF |
| RTSP MJPEG | `rtsp://CAM/stream=2` | RTSP | MJPEG over RTP (RFC 2435, Q=255 in-band tables) from the JPEG unit at 5 fps | `NATIVE` while `jpeg.enabled`; 404 otherwise; 415 for frames over 2040 px | `rtsp/rtp_jpeg`, `RtspServer::send_jpeg` | `test_audio` (packetiser) | — |
| Multi-camera | `/api/v1/peers`, `/api/v1/calibration/*` | GET | camera roster / stereo calibration | `NOT_APPLICABLE` | — | — | single camera |
| Outgoing | `/api/v1/outgoing.json` | GET | RTMP/SRT push targets | `MISSING` | — | — | no outgoing publisher |
| Live beacon | `/api/v1/live` | POST beacon | viewer heartbeat; **404 is expected and remembered** by the UI | `MISSING` (404) | — | — | harmless: the UI explicitly tolerates an older daemon |
| Image tuning beacon | `/api/v1/image?...` | POST beacon | live image knobs | `MISSING` | native image tuning is in the schema instead | — | check whether the tuning page needs it |
| Records control | `/api/v1/records/resume|standdown` | POST | recording control | `MISSING` | — | — | needs SD recording |

## Priority list that falls out of this

1. **`/ws/logs`** - the only missing endpoint that makes the camera harder to
   support, and it is cheap (AP5).
2. **`/setup` + `system.unsafe`** - completes the drop-in for a fresh camera
   *and* is the switch AP3's RTSP auth is waiting on (AP9).
3. **OSD** - the most visible everyday feature still absent (AP8).
4. **`/ws/analytics`** - turns M9 detection into something the stock UI draws.
5. **`/upload` + `/ws/upgrade`** - firmware handling from the UI.
6. **Night/IR-cut** - blocked on pin facts, not on code.
7. **`/image.jpg` + `/mjpeg`** - blocked by the T40NN JPEG wedge; revisit via
   `IMP_Encoder_SetbufshareChn` rather than a second encoder.
8. Audio, recording, outgoing, GPIO/pinmux - real features, no user pull yet.

## Notes

- No WebUI file was changed and no feature was implemented in AP4; the two
  code touches are documentation only (this file and a comment).
- WebSocket endpoints can never be served by the relay (the upgrade does not
  survive it), so every missing `/ws/*` is necessarily native work.
