# Machino — C++ runtime (M2 foundation, M3 board/sensor abstraction)

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

Out of scope: audio, JPEG, substream, OSD, recording, WebRTC, SRT, AI, ONVIF,
Majestic compatibility, WebUI/HTTP API, power profiles, ISP/exposure tuning,
low-latency tuning.

## Layout

```
src/core/               platform-neutral: log, result, frame (+AuPool), config, stream_hub, pipeline,
                        capabilities, hw/ (descriptors, registry, board-profile parser, resolver)
src/ports/              iplatform (sensor+ISP+system+capabilities), iframesource, iencoder, stream_server
src/profiles/           DATA: built-in platforms, sensors, board profiles (outside the core)
src/adapters/ingenic/   imp_sessions (RAII over IMP), sensor_params (abstract -> IMP, host-testable),
                        ingenic_platform/framesource/encoder
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
