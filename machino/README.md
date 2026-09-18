# Machino M2 — C++ foundation

This directory is the new C++ runtime. It is **not** a rename of the C
prototype in `../src` (timps); that prototype stays as the reference
implementation that proved the hardware path in M1.

## Scope (M2)

One process, ports & adapters, exactly this chain and nothing else:

```
Platform (sensor/ISP) -> FrameSource -> Encoder (H.264) -> StreamHub -> RTSP
```

Acceptance: real H.264 → RTSP → VLC on the same OpenIPC T40NN/IMX307 camera,
produced by `machinod`.

Out of scope for M2: audio, WebRTC, recording, OSD, AI, ONVIF, Majestic
compatibility, power policy (the lifecycle is already shaped for it, see below).

## Layout

```
src/core/        platform-neutral: log, result, frame, config, stream_hub, pipeline
src/ports/       IPlatform, IFrameSource, IEncoder
src/adapters/    ingenic/  IMP SDK 1.3.1 adapter + board wiring (the M1 quirk lives here)
src/app/         main (event loop, signals), rtsp/ (RTSP/RTP H.264 server)
```

Rules: C++17, `-fno-exceptions -fno-rtti`, no third-party frameworks, RAII for
every vendor resource, deterministic reverse-order teardown. Vendor headers
are included only under `src/adapters/`.

## Lifecycle ("no consumer, no pipeline")

`Pipeline::acquire()` / `release()` are called by consumers (RTSP sessions).
The first acquire brings sensor/ISP/encoder up; the last release arms a grace
timer after which everything is torn down. `pipeline.always_on = 1` holds one
permanent reference. Power policy in later milestones builds on this.

## Board wiring

`sensor.i2c_bus / i2c_addr / mclk / reset_gpio / pwdn_gpio` describe how the
sensor is wired on the board. The T40 tx-isp driver pulses `reset_gpio` before
the chip-id probe; `-1` skips the pulse and the sensor never answers
(`imx307_detect ... ret = -5`). T40NN + IMX307: `1 / 0x1a / 1 / 91 / 0`.

## Build

CI (`.github/workflows/build-machino-t40.yml`) fetches the thingino musl
toolchain and the vendor IMP libraries via `../build.sh deps T40 --libc-musl`
and then runs this Makefile with the same variables. Locally:

```
../build.sh deps T40 --libc-musl
make CROSS_COMPILE=../toolchain/xburst2-musl/mipsel-thingino-linux-musl_sdk-buildroot/bin/mipsel-linux- \
     PLATFORM=T40 IMP_INC=../include/T40/1.3.1/en IMP_LIB=../3rdparty/install/lib \
     IMPLIBS="-Wl,--start-group -l:libimp.a -l:libalog.a -l:libsysutils.a -Wl,--end-group -l:libmuslshim.a"
```

## Run

```
machinod -c machino.conf [-v]
```
