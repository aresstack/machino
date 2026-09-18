# Power / performance control for continuous streaming (M5)

M4 handles the idle case ("no consumer, no pipeline"). M5 handles the other
case: consumers are permanently present and the camera streams continuously.
Then the pipeline cannot sleep, so Machino makes the **lowest useful active
operating point** controllable.

```
media configuration + profile
            |
            v
     PerformanceService      policy: validate, classify, requested vs effective
            |
            v
     PipelineManager         single owner: live setters, controlled restart
            |
            v
  IPlatform / IEncoder / IPowerControl   (adapter: adapters/ingenic)
```

The core knows no register, no clock address and no vendor call.

> `unsupported` is a valid capability result.
>
> Machino must never use undocumented raw clock/register writes merely to
> expose a power-control checkbox.

## Requested vs effective

Every control keeps the requested and the effective value apart:

| value | source |
|---|---|
| requested sensor fps | configuration / profile |
| effective sensor fps | read back from the ISP (`IMP_ISP_Tuning_GetSensorFPS`) — marked `(hw)` in telemetry |
| requested stream fps | FrameSource output rate |
| measured encoded fps | counted from encoder frames over a 1 s window |
| requested bitrate | configuration / profile |
| measured bitrate | bytes of encoded frames over a 1 s window |

If the sensor cannot be lowered, the service reports that: lowering only the
stream/encoder rate is **not** sensor power saving and is never presented as
such. Sensor fps and stream fps are separate controls.

## Apply modes

| control | mode on Ingenic T40 (SDK 1.3.1) | mechanism |
|---|---|---|
| `video.bitrate` | **LIVE** | `IMP_Encoder_Get/SetChnAttrRcMode` (target bit rate of the active RC mode), effective value read back |
| `sensor.fps` | **LIVE** | `IMP_ISP_Tuning_SetSensorFPS` + `GetSensorFPS` read-back; re-applied after every pipeline start |
| `video.fps` (stream) | **PIPELINE_RESTART** | FrameSource output rate is a channel attribute set before enable (no live API in 1.3.1); controlled restart with demand preserved |
| encoder fps only | LIVE (`IMP_Encoder_SetChnFrmRate`) | available on the port; not used by the service alone because it would only drop frames |
| `power.isp_performance` | **UNSUPPORTED** | ISP clock readable (`/proc/jz/clock/clocks: div_isp`), no safe runtime write interface |
| `power.encoder_performance` | **UNSUPPORTED** | encoder/EL150 clock readable (`div_el150`), no safe runtime write interface |
| `power.cpu_performance` | UNSUPPORTED on the OpenIPC T40 kernel (no cpufreq sysfs); if a kernel exposes `/sys/devices/system/cpu/cpu0/cpufreq`, only kernel-offered governors are accepted and the effective governor is read back |

`auto` for any power level means *adapter/kernel default* and changes
nothing. Invalid values are rejected, never clamped (a control with a known
range rejects values outside it; an unverified fps operating point is rejected
unless `sensor.allow_unverified_mode = 1` is set explicitly to qualify it).

Nothing is silently ignored: every setter returns an `ApplyResult` with
`ok`, `mode`, `requested`, `effective`, `deferred` (stored while cold) and a
message; the startup/SIGHUP log prints one line per setting.

## Profiles

`performance`, `balanced`, `battery`, `custom` are presets over the same
parameters — no hidden path.

* `performance`: the board/sensor default operating point.
* `balanced`: the board preset `preset.balanced_fps` or else the next lower
  *verified* operating point of the sensor.
* `battery`: `preset.battery_fps` (and `preset.battery_bitrate`) or else the
  lowest verified operating point.
* `custom`: individual `sensor.fps` / `video.fps` / `video.bitrate`.

If the sensor has only one verified operating point, `balanced`/`battery`
resolve to it and say so. No universal 10/15/20 values are hard-coded.

A profile is applied **as a whole or not at all**. Every component (fps against
the verified operating points, bitrate against the encoder range) is validated
before anything is touched; if one of them cannot be applied, none of them is
and the reply says so:

```json
{ "status": "rejected", "requested": 999999,
  "message": "battery not applied (nothing changed): bitrate 999999: bitrate outside known range" }
```

Otherwise a refused profile would still move the camera - the fps of the new
profile with the bitrate of the old one - which is an operating point nobody
chose and no one verified. Past validation only the hardware can still refuse
(a pipeline restart that does not come back); the profile then stays `custom`,
because that is what the box is.

## M4 integration

* Setting anything while `COLD_IDLE` stores it; no hardware is started just
  to apply a setting. The next consumer starts directly with it.
* A `PIPELINE_RESTART` setting while `ACTIVE` performs `stop -> apply ->
  start` inside the `PipelineManager` with the demand preserved; the RTSP
  sessions keep their sinks and resume on the next key frame. `GRACE_IDLE`
  re-arms its timer after the restart. A failed restart rolls back, logs the
  exact stage, enters `FAILED` and is not retried automatically.
* `SIGHUP` re-reads `machino.conf` and applies only the changed values
  through the service (the same path the M6 API will use).

## Telemetry

`PerformanceService::telemetry()` (optionally logged every
`telemetry.log_interval_s` seconds via a timerfd — no polling thread):
lifecycle state and generation, profile, requested/effective sensor fps,
requested stream fps, measured encoded fps, requested/measured bitrate,
dropped frames, CPU %, RSS, threads, known ISP / encoder / CPU clocks.
Unavailable values are explicit (`n/a`), never 0.

## Known T40NN inventory (OpenIPC 4.4.94, read-only investigation)

| area | readable | writable safely | live |
|---|---|---|---|
| ISP clock (`div_isp`, 366.7 MHz, mux mpll) | yes (`/proc/jz/clock/clocks`) | **no** (no documented interface) | — |
| encoder clock (`div_el150`, 550 MHz) | yes | **no** | — |
| CPU frequency | `/proc/jz/clock/clocks` (`div_cpu`) read-only; no cpufreq sysfs | **no** | — |
| sensor fps | yes (`GetSensorFPS`) | yes (`SetSensorFPS`) | yes |
| encoder bitrate | yes | yes | yes |
| stream (FrameSource) fps | attr | yes | restart |

## Hardware-verified operating points (T40NN/IMX307 board A, 2026-09-18)

Each point: ≥ 60 s RTSP/H.264 decoded by ffmpeg, no freeze, no watchdog,
capture drops 0, same process. Sensor fps is the value read back from the
ISP, not assumed. Proxy values only — no power meter in the setup, so no
savings percentage is claimed.

| point | sensor fps (hw) | encoded fps | bitrate req / measured | CPU % (1 core) | RSS | ISP / encoder clock |
|---|---|---|---|---|---|---|
| 1920x1080 @ 20, 3000 kbps (`performance`) | 20 | 20.0 | 3000 / ~2600–3100 | 4.0–4.6 | 3.5 MB | 366 / 550 MHz (unchanged, read-only) |
| 1920x1080 @ 15, 3000 kbps (`balanced`) | 15 | 15.0 | 3000 / ~2600–3300 | 3.4–3.6 | 3.9 MB | 366 / 550 MHz |
| 1920x1080 @ 10, 2000 kbps (`battery`) | 10 | 10.0 | 2000 / ~1700–1760 | 2.0–2.2 | 4.0 MB | 366 / 550 MHz |

Live controls verified on hardware:

* bitrate 3000 → 1000 → 3000 during a running RTSP session: measured
  ~3100 → ~700–1700 → ~3000 kbps, decoder kept running, `restart_count` 0
  (`bitrate live: requested=1000 effective=1000`).
* stream fps 20 → 15 while `ACTIVE` (`PIPELINE_RESTART`): orderly stop/start
  inside the manager, demand preserved, the same RTSP session resumed at
  15.0 fps after the restart.

The ISP and encoder clocks did not change across operating points (they are
not controllable on this kernel); the measurable effect of a lower operating
point is the lower sensor/ISP frame rate, lower encoder load (CPU %) and
lower bitrate. Operating points are added to a sensor's verified list only
after such a run (`sensor.allow_unverified_mode = 1` is used to qualify a new
point; afterwards it is listed and the opt-in is no longer needed).

## Power measurement

No power meter is part of the test setup. **No percentage savings are
claimed.** Only technical proxy values are recorded per operating point:
effective sensor fps, encoded fps, bitrate, CPU load, RSS, known clocks.
