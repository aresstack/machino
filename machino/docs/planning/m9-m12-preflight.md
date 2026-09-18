# M9-M12 implementation preflight

Baseline reviewed: `main` at `e99a67e745ea340113ef1cda4d270e960bab8c9f`.
M8 is intentionally treated as **in flight**. These notes avoid modifying M8
code and identify the seams M9-M12 should use after M8 lands.

The goal of this document is to reduce re-reading and, more importantly, avoid
building M9-M12 against assumptions that M8 is about to invalidate.

## Cross-milestone conflict rule

M8 changes the media ownership model from the current single H.264 chain to
shared sensor/ISP plus per-consumer resources (main/sub/JPEG). Therefore:

- do **not** implement M9 against today's single-chain `PipelineManager`;
- M9 must bind to the resource/demand model that M8 actually lands;
- M10 consumes public services/API from M8/M9 and must not reach into media
  internals;
- M11 owns OpenIPC packaging/service selection, not media behavior;
- M12 changes the Ingenic backend/build target, not the core contracts.

Files likely to be hot in M8 and therefore deliberately left untouched by this
preflight: `pipeline_manager.*`, `iplatform.hpp`, `config.*`,
`api_service.*`, `http_server.*`, RTSP, telemetry, tests and Makefile.

---

## M9 - detection / AI

### What already exists

- `ConsumerType::Ai` already exists in `core/lifecycle/demand.hpp`.
- `CapabilitySet::Ai` exists but currently carries only
  `available = Unknown`.
- The current `StreamHub` carries encoded H.264 access units, not raw frames.
- `IFrameSource` currently exposes lifecycle/channel only; it has no raw-frame
  acquisition contract.
- The old C reference implementation already contains a useful Ingenic motion
  backend in `src/hal/imp_motion.c`: IMP_IVS group/channel creation,
  FrameSource -> IVS binding, polling/result handling, bounded grid state,
  cleanup and stall recovery.
- `src/motion_caps.h` derives availability and the ROI limit from the exact
  SDK header instead of hardcoding it. This is a good pattern to preserve.

### Important architectural decision

Do not route AI through the encoded H.264 `StreamHub`, and do not force every
Ingenic detector through a CPU-visible raw-frame copy.

There are two legitimate hardware input modes:

1. **frame-input detector** - consumes a platform-neutral `FrameView`; useful
   for model runtimes that need a YUV/RGB tensor;
2. **bound-source detector** - the vendor backend binds directly to a logical
   FrameSource/ISP output (IMP_IVS is the concrete example).

The core should model a detector, scheduling, events and demand. The Ingenic
adapter decides whether that detector is fed by a frame view or by a direct
hardware binding.

Suggested split:

```text
core/detection/
    types.hpp              Detection, box, class, confidence, timestamp
    detection_service.*    enable/disable, demand, state, events, telemetry
    latest_frame_slot.*    bounded single-slot input for frame-driven backends

ports/
    idetector.hpp          vendor-neutral detector lifecycle/result contract

adapters/ingenic/detection/
    ivs_motion.*           IMP_IVS MoveInterface backend
    nna_*                  optional object/person backend only when proven
```

The port can expose an input-mode capability rather than pretending every
backend implements exactly `infer(FrameView)`.

### Scheduling

For frame-driven AI use one in-flight inference and one replaceable latest
candidate. No FIFO backlog:

```text
new frame -> due?
              no -> skip
              yes
               |
        detector idle? ---- no ---> replace latest candidate / count skip
               |
              yes
               v
             infer
```

Use the frame timestamp/monotonic clock to decide when the next inference is
due. `video_fps` must never be used as the inference loop cadence.

For a directly-bound IVS backend, the same external semantics still apply:
`ai.inference_fps`/sampling becomes the backend's skip/sampling parameter
where possible, and result delivery remains bounded.

### Demand after M8

M9 should request the **minimum upstream resource**, not "the video pipeline".
Expected AI-only state:

```text
sensor/ISP or selected raw FrameSource : ON
H.264 main encoder                    : OFF
H.264 sub encoder                     : OFF
JPEG                                  : OFF
detector                              : ON
```

This is the main reason M9 must wait for M8's resource graph.

### Motion is not the same capability as Edge AI

IMP_IVS move detection is a strong first real detector backend, but it must not
be reported as NNA/person-AI support. Keep capability vocabulary explicit, e.g.

```json
"ai": {
  "status": "unknown",
  "detectors": {
    "motion": { "status": "supported", "backend": "imp_ivs_move" },
    "person": { "status": "unknown" }
  }
}
```

Exact JSON can follow the final M8 API conventions.

The T40 stock image/research suggests NNA/Venus/JZDL-style components exist,
but M9 must not claim a person detector until the exact runtime/model path is
proved. Missing optional vendor libraries must degrade the AI capability, never
daemon startup or RTSP.

### Configuration/API

Canonical settings:

```text
ai.enabled
ai.detector
ai.inference_fps
ai.model_path          # only when a backend needs an external model
```

State should distinguish configured/enabled from active/error. Telemetry should
carry requested/completed/skipped/failed inference counts, effective FPS,
average/max inference duration, detections and last inference/detection time.

Detection SSE events should be emitted on detections/state transitions, not one
"nothing detected" event per analyzed frame.

### Tests that matter most

Host fake detector tests should prove:

- disabled -> no AI demand;
- AI-only starts only the minimum upstream resource;
- enable/disable releases demand and returns to cold idle;
- one in-flight inference, bounded latest-frame replacement;
- inference cadence independent of video FPS;
- detector failure leaves RTSP/main/sub alive;
- missing backend/model leaves video usable;
- normalized detection result/event format;
- API validation and telemetry counters.

For T40 hardware, first probe the available detector path separately. If a
hardware person detector is not proved, ship the architecture + motion backend
and keep person/NNA status honest rather than adding CPU YOLO as a milestone
shortcut.

---

## M10 - Majestic compatibility / OpenIPC drop-in surface

### Existing seed already on main

Commit `e99a67e745ea340113ef1cda4d270e960bab8c9f` already establishes a
clean compatibility boundary under `app/compat/majestic_webui.*` for:

- `GET /api/v1/config.schema.json`
- `GET /api/v1/config.json`
- `POST /api/v1/config`

It deliberately translates into the native Machino API instead of making the
Machino core Majestic-shaped. Preserve that direction after M8/M9 extend the
native config model.

M8 is likely to touch API/config tests and stream shape, so after M8 lands,
**adapt this compatibility layer to the final M8 schema** instead of adding
special cases elsewhere.

### Current OpenIPC WebUI dependencies worth inventorying first

The current WebUI also contains real dependencies beyond those first three:

- shell-side `GET /api/v1/get?key=<dotted>`;
- live-adjustment `POST /api/v1/image?... ` when schema fields carry
  `x-live`;
- `GET /api/v1/sources` for available video sources;
- snapshot/MJPEG/live-preview paths;
- CGI apply path that currently sends SIGHUP to the `majestic` process;
- additional feature-specific APIs (OSD, pinmux, recording, analytics, etc.).

M10 should create a compatibility matrix and implement only entries required by
the Machino feature set at M9. Unsupported WebUI pages/features should fail
cleanly or remain hidden through the schema rather than returning plausible
fake data.

### Suggested structure

```text
app/compat/majestic/
    webui_schema.*
    config_view.*
    config_post.*
    config_get.*
    endpoints.*
    migration.*
    coverage.*

tools/
    machino-import-majestic   # if keeping YAML parsing out of machinod is cleaner
```

A table-driven endpoint adapter is preferable to growing Majestic-specific
branches throughout `http_server.cpp`.

### majestic.yaml migration

Treat `majestic.yaml` as one-way compatibility input:

```text
majestic.yaml
    -> importer
    -> validated Machino config patch/model
    -> machino.conf (canonical)
```

Emit a structured report with each input key classified as
`mapped | ignored-with-warning | unsupported | invalid`.

Do not add a general YAML dependency to the media core. Decide between a
strict compatibility parser and a small standalone importer only after
inspecting the real OpenIPC YAML structures that need mapping.

M8/M9 mappings likely include video0/video1, FPS/bitrate/GOP, JPEG/snapshot,
image controls, RTSP and supported detection settings. Do not map settings
whose units/semantics are not equivalent.

### Boundary with M11

M10 should define compatibility behavior; M11 should wire OpenIPC's shell/CGI
environment to the selected daemon. In particular, do not make Machino pretend
its process name is `majestic` merely so an existing `killall majestic`
happens to work.

---

## M11 - OpenIPC packaging, service lifecycle and rollback

### Concrete OpenIPC assumptions already found

Current OpenIPC firmware installs Majestic through:

```text
general/package/majestic/
    Config.in
    majestic.mk
    files/S95majestic
```

The package installs `/usr/bin/majestic`, `/etc/majestic.yaml` and
`/etc/init.d/S95majestic`.

There are also non-package assumptions that M11 must address:

- `/etc/profile` implements `streamer() { /etc/init.d/S95majestic "$1"; }`;
- `S99bootok` currently judges streamer health using executable/pid state of
  Majestic;
- `sysupgrade` contains Majestic-specific stop/restart/ownership handling;
- USB dual-role code can signal/stop Majestic around media ownership;
- wifibroadcast and other scripts send Majestic reload signals;
- `extutils` contains Majestic config/reload helpers.

This means "add package/machino" alone is insufficient for safe runtime
selection/rollback.

### Recommended integration seam

Introduce one OpenIPC-level **streamer selector/service helper** and move the
few firmware callers to that abstraction, e.g. conceptually:

```text
openipc-streamer current
openipc-streamer start|stop|restart|reload|healthy
```

The exact filename should follow OpenIPC conventions discovered during M11,
but the principle is important: do not scatter
`if machino ... else majestic ...` through every firmware script.

A persistent selector chooses `majestic` or `machino`. The helper must
guarantee:

1. stop current owner;
2. wait until it has actually exited/released media;
3. only then start the other owner;
4. rollback if the new owner fails health/readiness checks.

Never allow both daemons to race on ISP/FrameSource.

### Health semantics

Machino in `COLD_IDLE` is healthy. A health check must therefore not require
an active RTSP encoder/stream. Prefer native API discovery/state + stable
process presence.

This matters especially for `S99bootok`: if Majestic remains installed for
rollback, the current `[ -x /usr/bin/majestic ] || pidof majestic` logic
would misclassify a valid Machino-selected boot. M11 must make boot health
selection-aware.

### Config and upgrade behavior

- Preserve `/etc/majestic.yaml` untouched for rollback.
- Use M10 once to seed/migrate Machino config; thereafter `machino.conf` is
  canonical.
- Do not overwrite user config during package upgrades. Prefer installing a
  factory/default copy separately and creating the writable config only when
  absent.
- Keep runtime/vendor model assets separate and license-attributed.
- Test overlay/reboot/sysupgrade behavior, not just Buildroot target install.

### Packaging layout

A likely OpenIPC package is:

```text
general/package/machino/
    Config.in
    machino.mk
    files/
        S95machino (or selector-integrated equivalent)
        machino.conf.default
```

but the init-file naming should follow the final mutual-exclusion design, not
blindly create a second always-started S95 service.

### Regression sequence

Automate at least:

```text
boot -> Machino cold healthy
Machino -> stream -> stop
10 service restarts
5 reboots
Machino -> Majestic -> working stream
Majestic -> Machino -> working stream
upgrade simulation with existing machino.conf
```

The rollback test is a media-ownership test, not merely "both commands exit 0".

---

## M12 - T31 as the first architecture proof

### Preflight facts from the existing tree/build

Current Machino still has several T40-centric build/runtime assumptions:

- `builtin_profiles.cpp` registers T40NN/T40N only;
- `main.cpp::make_platform()` dispatches only on vendor == `ingenic` and
  always instantiates the same `IngenicPlatform`;
- `Makefile` defaults to `PLATFORM=T40` and SDK headers `1.3.1`;
- the CI workflow is T40/xburst2/SDK-1.3.1 specific.

The existing top-level `build.sh` already knows the important toolchain/library
split:

```text
T31 -> xburst1
       libimp 1.1.6 for the normal kernel path
       libimp 1.1.5.2 with --kernel-4

T40 -> xburst2
       libimp 1.3.1
```

So M12 should reuse that source-of-truth rather than inventing a second mapping.

A lexical check of the IMP calls used by today's T40 C++ adapter against the
vendored T31 1.1.6 headers found five current tuning calls absent on T31:

```text
IMP_ISP_Tuning_GetAeExprInfo
IMP_ISP_Tuning_GetAeScenceAttr
IMP_ISP_Tuning_GetAwbAttr
IMP_ISP_Tuning_SetAeScenceAttr
IMP_ISP_Tuning_SetAwbAttr
```

That is enough to reject the idea that T31 is merely "compile the exact T40
adapter with PLATFORM=T31".

### Build architecture

Do **not** try to link incompatible T31 and T40 libimp SDKs into one universal
binary.

Prefer same Machino core, separate target builds:

```text
machino-t40  -> xburst2 + T40 SDK 1.3.1 adapter backend
machino-t31  -> xburst1 + verified T31 SDK/backend
```

Refactor the Ingenic layer around family-specific compilation while sharing
only genuinely common semantics:

```text
adapters/ingenic/
    common/          # SDK-independent mapping/helpers only
    t40/             # T40-only attrs/tuning/API details
    t31/             # T31-only attrs/tuning/API details
```

An equivalent traits/backend split is fine. The important rule is that T31/T40
header/ABI differences stop at the adapter boundary.

Move SDK version to an explicit build variable/matrix input rather than the
current hardcoded `1.3.1` include default.

### Runtime factory/capabilities

The current vendor-only factory is too coarse for M12. The build-selected
Ingenic backend should reject a mismatched platform family/model rather than
silently instantiate a T40 implementation for T31.

Capabilities must be supplied by the selected family backend. Do not inherit
T40 "supported" values into T31 until compiled and hardware verified.

AI from M9 is naturally optional: a T31 target can report
unsupported/unknown without changing the M9 core.

### Hardware first

Before creating the T31 board profile, inventory the real unit read-only:

- exact SoC/subtype;
- sensor identity;
- I2C bus/address where safely known;
- MCLK/reset/pwdn wiring;
- kernel + tx-isp generation/modules;
- RAM;
- working stock/OpenIPC stream mode.

No GPIO guessing, sensor scans or copied T40 wiring.

Only after that add the T31 sensor/board profile and hardware-verified modes.

### CI

Refactor the current T40-only workflow into a build matrix after T31 backend
exists. Host core tests run once; family-specific compile jobs prove both SDKs.

Add at least one compile test/guard that catches accidental T40-only IMP calls
in the T31 source set.

The architecture proof is complete only after a real T31 can do the normal
Machino lifecycle/RTSP path and return to cold idle; "T31 cross-compiles" is
necessary but not sufficient.

---

## Suggested execution order after M8 lands

1. **Rebase/read M8 once** and record its final resource ownership API. Do not
   start M9 before this short reconciliation.
2. **M9 core + fake detector first**, then IMP_IVS motion, then optional NNA
   probe/person backend. This isolates scheduling/lifecycle from vendor risk.
3. **M10 inventory first**, then extend the already-existing WebUI compatibility
   adapter to M8/M9. Keep a coverage matrix and make unsupported UI surfaces
   explicit.
4. **M11 introduce the OpenIPC streamer abstraction before package activation**.
   Packaging without mutual-exclusion/boot-health changes is incomplete.
5. **M12 refactor build/backend family boundary before writing T31 media code**,
   then add the real board profile and hardware verification.

## Files most likely to be touched per milestone

```text
M9:
  core/detection/* (new)
  ports/idetector.hpp (new)
  adapters/ingenic/detection/* (new)
  config/API/events/telemetry
  M8 resource manager integration
  tests/test_detection.cpp (prefer new file)

M10:
  app/compat/majestic/* (mostly new / move current compat here)
  compatibility matrix docs
  migration tool/tests
  thin HTTP route registration

M11:
  packaging/openipc/* in this repo and/or a focused OpenIPC firmware patch
  no media-core changes expected

M12:
  adapters/ingenic/{common,t40,t31}/*
  platform factory
  build variables / CI matrix
  profiles for the verified T31 board
  family-specific compile tests
```

Prefer new test/source files over growing `test_api.cpp` and other M8-hot files
where possible. That makes each milestone easier to review/cherry-pick and
reduces conflict surface.
