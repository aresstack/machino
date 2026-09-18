# Machino — C++ runtime (M2–M7)

This directory is the Machino runtime. It is **not** a rename of the C
prototype in `../src` (timps); that prototype stays as the reference that
proved the hardware path in M1 and is not developed further.

## Scope so far

M2: one process, ports & adapters, exactly this chain:

```
Config -> Platform (sensor/ISP) -> FrameSource -> H.264 encoder -> StreamHub -> RTSP/RTP
```

M3: the core no longer knows any board. Hardware is described by
`PlatformDescriptor` / `BoardProfile` / `SensorDescriptor` (+ modes) /
`SensorWiring`, resolved with a strict precedence and reported through a
tri-state `CapabilitySet`. See `docs/architecture/board-profiles.md`.

M4 adds demand-driven lifecycle, M5 performance/power controls, M6 the native
HTTP API, and M7 capability-backed ISP image controls plus a measured,
bounded low-latency stream path.

Out of scope through M7: audio, JPEG, substream, OSD, recording, WebRTC, SRT,
AI, ONVIF, Majestic compatibility and a full WebUI.

## Layout

```
src/core/               platform-neutral: log, result, frame (+AuPool), config, stream_hub, lifecycle,
                        media/tuning_service, power, capabilities, hw/
src/ports/              iplatform, iframesource, iencoder, iimage_control, stream_server
src/profiles/           DATA: built-in platforms, sensors, board profiles (outside the core)
src/adapters/ingenic/   imp_sessions (RAII over IMP), sensor_params (abstract -> IMP, host-testable),
                        ingenic_platform/framesource/encoder/image_control
src/app/                main (event loop, signals), rtsp/ (RTSP/RTP H.264 server = IStreamServer)
tests/                  host unit tests (make test)
docs/architecture/      board-profiles.md
```

Rules: C++17, `-fno-exceptions -fno-rtti -Werror`, no third-party frameworks,
RAII for every vendor resource, deterministic reverse-order teardown, bounded
per-consumer queues, no per-frame heap allocation once the frame pool is warm.
Vendor headers are included only under `src/adapters/`; the core contains no
`IMP_*` call and no board constant.

## Hardware description

```
board = t40nn-imx307-board-a          # ONE board profile (hardware verified)
#board_profile_file = /etc/machino/my-board.conf
#sensor.reset_gpio = none             # explicit user values override the profile
```

Precedence: user config > board profile > safe platform defaults > fail
closed. GPIOs are never guessed (`none` is the safe state); missing I²C
bus/address refuses to start. Exactly one profile → one media init, no
probing. Startup logs the resolved platform/board/sensor/mode/wiring and, with
`-v`, each value's provenance (`reset_gpio=91 [board-profile]`).

## RAII sessions (adapters/ingenic/imp_sessions.hpp)

```
IspSession · SensorSession · SystemSession · TuningSession · FrameSourceChannel
EncoderGroup · EncoderChannel · Binding · StreamReceiver
```

Bring-up constructs them into locals in order; any failure returns early and
the locals roll back in reverse. No `goto fail_x` chains.

## Lifecycle — "no consumer, no pipeline" (M4)

> An open Machino daemon is not equivalent to an active camera pipeline.
> Media hardware is activated by demand, not by process lifetime.

`lifecycle::PipelineManager` is the single owner of the media chain and runs
the state machine `COLD_IDLE → STARTING → ACTIVE → GRACE_IDLE → STOPPING →
COLD_IDLE` (+ `FAILED`). Consumers hold RAII `DemandHandle`s (`PLAY` takes
one, TEARDOWN/disconnect/dead socket releases it; a bare TCP connection is
no demand). The last release arms a one-shot `timerfd` (`lifecycle.idle_grace_ms`,
default 5 s); returning demand cancels it, expiry tears the chain down
completely while the process and the RTSP listener stay up. `SIGUSR2` /
`SIGUSR1` take / drop a manual demand (`pipeline.always_on` holds one).
Details: `docs/architecture/lifecycle.md`.

## Power / performance for continuous streaming (M5)

When consumers are permanently present the pipeline cannot sleep, so the
lowest useful active operating point is controllable through
`power::PerformanceService` (the layer the M6 API will expose):
profiles `performance | balanced | battery | custom` as presets over
`sensor.fps`, `video.fps`, `video.bitrate`; every setter is classified
(`live`, `pipeline-restart`, `unsupported`) and returns the *effective*
value. On Ingenic T40: bitrate and sensor fps are live (read back from the
hardware), stream fps is a controlled pipeline restart with demand
preserved, ISP/encoder clocks are read-only (`unsupported` — no raw register
writes), cpufreq is probed and unsupported on the OpenIPC kernel. Telemetry
(state, requested vs effective fps, measured fps/bitrate, drops, CPU, RSS,
threads, known clocks) is available internally and optionally logged every
`telemetry.log_interval_s`. `SIGHUP` re-applies the configuration. Details:
`docs/architecture/power.md`.

## HTTP control & telemetry API (M6)

`/api/v1` (default `0.0.0.0:8080`, `api.*` in the conf): `capabilities`,
`state`, `config` (GET/PATCH), `telemetry`, `events` (SSE). Platform-neutral
schema: a UI shows/hides controls from `capabilities.controls[*].status`
(`supported | unsupported | unknown`) and knows how a change lands from
`apply` (`live | pipeline_restart | …`). PATCH is partial, validated as a
whole before anything is applied, goes exclusively through the
PerformanceService/PipelineManager, returns requested vs effective per change,
persists atomically (temp → fsync → rename) with a `revision` (optimistic
`If-Match`). Unknown telemetry values are `null`. Reading the API or holding
an SSE connection is never media demand. Small poll()-based server, one
thread, bounded buffers/clients. Docs: `docs/api/v1.md`; test client:
`tools/webui_sim.py`.

## Image quality and low latency (M7)

`media::TuningService` is the platform-neutral policy layer;
`IImageControl` is the port and the Ingenic adapter alone calls
`IMP_ISP_Tuning_*`. Image settings are optional: omission preserves the ISP
tuning-bin defaults. The API exposes requested/effective values and live AE
read-back (luma, target, stability, integration time and gains). Anti-flicker
is an explicit `off|50hz|60hz` deployment choice; it is not globally forced.

`latency.profile=low` is not a hidden encoder mode. It resolves to visible
individual settings: one-second GOP, one FrameSource buffer, one encoder
stream buffer and a one-AU consumer queue. Explicit `latency.*` values win
over the preset. Every queue is bounded; a slow consumer drops stale frames,
discards the now-undecodable P-frame tail, requests/resumes at a fresh IDR,
and cannot block other consumers. RTSP uses TCP_NODELAY, a bounded socket
send buffer and a bounded send-stall timeout, with no packet-pacing sleeps.
Camera capture→encoder-output and encoder-output→socket latency are reported
separately from client buffering. Details and the hardware matrix are in
`docs/architecture/image-latency.md`.

## Build / test / CI

```
make -C machino test           # host unit tests
```

`.github/workflows/build-machino-t40.yml`: host tests, then thingino musl
toolchain + vendor IMP libraries of exactly SDK 1.3.1, `-Werror` build, and
the hardware test bundle `machino-m2-t40/` (`machino`, `machino.conf`,
`required-libs/`, `SHA256SUMS`, `BUILDINFO`).

## Run

```
machino -c machino.conf [-v]        # rtsp://<camera>:554/ch0
kill -USR1 <pid>                    # stop_pipeline
kill -USR2 <pid>                    # start_pipeline
```
