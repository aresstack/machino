# Machino M2 — C++ foundation + minimal Ingenic adapter

This directory is the Machino runtime. It is **not** a rename of the C
prototype in `../src` (timps); that prototype stays as the reference that
proved the hardware path in M1 and is not developed further.

## Scope (M2)

One process, ports & adapters, exactly this chain and nothing else:

```
Config -> Platform (sensor/ISP) -> FrameSource -> H.264 encoder -> StreamHub -> RTSP/RTP
```

Acceptance: real H.264 → RTSP → VLC/ffmpeg on the OpenIPC T40NN/IMX307 camera,
≥ 60 s stable, no watchdog reboot, no Majestic, plus `stop_pipeline()` /
`start_pipeline()` in the same process.

Out of scope: audio, JPEG, substream, OSD, recording, WebRTC, SRT, AI, ONVIF,
Majestic compatibility, WebUI, power profiles, ISP/exposure tuning, low-latency
tuning.

## Layout

```
src/core/               platform-neutral: log, result, frame (+AuPool), config, stream_hub, pipeline
src/ports/              iplatform (sensor+ISP+system), iframesource, iencoder, stream_server
src/adapters/ingenic/   imp_sessions (RAII over IMP), ingenic_platform/framesource/encoder, board_wiring
src/app/                main (epoll/signalfd event loop), rtsp/ (RTSP/RTP H.264 server = IStreamServer)
```

Rules: C++17, `-fno-exceptions -fno-rtti -Werror`, no third-party frameworks,
RAII for every vendor resource, deterministic reverse-order teardown, bounded
per-consumer queues, no per-frame heap allocation once the frame pool is warm.
Vendor headers are included only under `src/adapters/`; the core contains no
`IMP_*` call.

## RAII sessions (adapters/ingenic/imp_sessions.hpp)

```
IspSession          IMP_ISP_Open / Close
SensorSession       AddSensor + EnableSensor / DisableSensor + DelSensor
SystemSession       IMP_System_Init (retry) / Exit
TuningSession       EnableTuning / DisableTuning (non-fatal)
FrameSourceChannel  CreateChn + SetChnAttr / DisableChn + DestroyChn
EncoderGroup        CreateGroup / DestroyGroup
EncoderChannel      CreateChn + RegisterChn / UnRegisterChn + DestroyChn
Binding             IMP_System_Bind / UnBind
StreamReceiver      StartRecvPic / StopRecvPic  (+ GetStream/ReleaseStream cycle)
```

Bring-up constructs them into locals in order; any failure returns early and
the locals roll back in reverse. No `goto fail_x` chains.

## Lifecycle

`Pipeline` is the single owner of the media resources.

- consumers (RTSP sessions) call `acquire()` / `release()`; the first acquire
  brings everything up, the last release arms a grace timer after which the
  pipeline is torn down — "no consumer, no pipeline" (basis for M4).
- `start_pipeline()` / `stop_pipeline()` are the explicit operator controls
  (`pipeline.always_on = 1`, `SIGUSR2` / `SIGUSR1`). Both work repeatedly in
  one process without restart.

## Board wiring (fail-closed)

```
platform          = ingenic-t40nn
sensor.model      = imx307
sensor.i2c_bus    = 1
sensor.i2c_addr   = 0x1a
sensor.mclk       = 1
sensor.reset_gpio = 91
sensor.pwdn_gpio  = 0
```

Defaults for the GPIOs are `-1` ("no such pin"): an unknown board never
toggles a GPIO by accident. The T40 tx-isp driver pulses `reset_gpio` before
the chip-id probe; without it the IMX307 answers `ret = -5` ("not an imx307").

## Build / CI

`.github/workflows/build-machino-t40.yml` fetches the thingino musl toolchain
and the vendor IMP libraries of exactly SDK 1.3.1 (`../build.sh deps T40
--libc-musl`), builds with `-Werror`, and publishes the hardware test bundle

```
machino-m2-t40/
  machino            (static vendor libs, static libstdc++, non-PIE)
  machino.conf       (T40NN/IMX307 example)
  required-libs/     NEEDED.txt (runtime shared libs: musl libc only)
  SHA256SUMS
  BUILDINFO          commit, SDK version, toolchain, libc, target, compiler flags
```

Local build: see the Makefile header.

## Run

```
machino -c machino.conf [-v]        # rtsp://<camera>:554/ch0
kill -USR1 <pid>                    # stop_pipeline
kill -USR2 <pid>                    # start_pipeline
```
