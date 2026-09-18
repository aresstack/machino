# Board profiles, sensor descriptors and capabilities

Machino must not be tied to the one camera it was first proven on. This
document describes how hardware is modelled so that the core stays generic
and every concrete value lives in data.

## Platform vs board vs sensor

```
Machino core
    |
    +-- PlatformDescriptor   vendor / family / model        e.g. ingenic / t40 / t40nn
    +-- SensorDescriptor     model, interface, native size, verified modes
    +-- BoardProfile         platform + sensor + SensorWiring + default mode   (ONE board)
    +-- CapabilitySet        what the running adapter can do (supported / unsupported / unknown)
    +-- adapters/ingenic     the only code that knows IMP_*
```

* **Platform** — which SoC an adapter drives. Selected by `PlatformDescriptor::vendor`.
* **Sensor** — what the imager is, independent of any board. Lists only
  *verified* modes (see below).
* **Board** — how *this* board wires the sensor: I²C bus and address, clock
  index, reset/power-down pins. These are properties of a PCB, not of a SoC or
  a sensor. A profile never claims to cover every camera with the same SoC and
  sensor. In particular: **`t40nn-imx307-board-a` says reset_gpio=91 for that
  board only** — another T40NN/IMX307 camera may be wired differently.

Types: `src/core/hw/descriptors.hpp`. Registry: `src/core/hw/registry.hpp`.
Data: `src/profiles/builtin_profiles.cpp` (outside the core) or a profile file.

The core contains no `t40nn`, `imx307`, GPIO number or `IMP_*` call. The
adapter is constructed from the resolved description and performs the
translation (`src/adapters/ingenic/sensor_params.cpp` → `IMPSensorInfo`).

## Precedence rules

Resolution (`src/core/hw/resolve.cpp`) is strict and logged:

```
explicit user config   (machino.conf: platform, sensor.*)
        ↓
board profile          (board = <id>  or  board_profile_file = <path>)
        ↓
safe platform defaults (declared by the adapter; contains NO GPIO entries)
        ↓
unsupported / fail closed
```

Every effective value carries its source. At start:

```
Platform: ingenic t40nn (t40)
Board: t40nn-imx307-board-a [hardware verified]
Sensor: imx307 (mipi-csi, native 1920x1080)
Mode: 1920x1080@20
I2C: bus=1 addr=0x1a
MCLK: 1
Reset GPIO: 91
PWDN GPIO: 0
```

and with `-v` (debug) the provenance:

```
i2c_bus=1 [board-profile] ... reset_gpio=91 [board-profile] ... mode=1920x1080@20 [board-profile]
```

Conflicts are warnings, the user value wins:

```
conflict: reset_gpio=92 [user-config] overrides 91 [board-profile]
```

## Safe defaults (fail closed)

* GPIO numbers are **never guessed**. Neither user nor board set → effective
  `none` (-1) → the adapter passes -1 and the vendor driver never requests or
  toggles a pin. `none` is a valid, permanent state.
* I²C bus / address and the sensor model have **no** default. Missing →
  resolution fails with a clear message and machino refuses to start. It does
  not scan buses, try addresses, toggle pins or initialise several sensors.
* `mclk` has no platform default on Ingenic T40 (the clock index is board
  wiring). An adapter may declare one for platforms where it is fixed.
* A requested mode must be one of the sensor's *verified* modes; anything
  else is rejected. No datasheet-derived modes are entered without a hardware
  run.
* Exactly one deterministically selected profile → exactly one media init.

## Capability model

`src/core/capabilities.hpp` — tri-state on purpose:

```
supported | unsupported | unknown        (unknown != unsupported)
```

`CapabilitySet` groups: `video` (h264, h265, max_streams), `sensor`
(configurable_fps), `isp` (available), `encoder` (hardware), `power`
(isp/encoder clock control, cpu frequency control), `ai` (available).
Adapters report only what they have established; `IngenicPlatform` marks
H.264, ISP and hardware encoding as supported (proven M1–M3) and everything
not yet exercised as unknown. No HTTP API yet — that is M6.

## Adding another board profile

Preferred: a profile file, no rebuild.

```
# /etc/machino/my-board.conf
board_id   = vendor-model-rev
platform   = ingenic-t40nn         # must be a registered platform id
sensor     = imx307                # must be a registered sensor model
i2c_bus    = 0
i2c_addr   = 0x1a
mclk       = 0
reset_gpio = none                  # or a pin number; omit = none
pwdn_gpio  = none
mode       = 1920x1080@20          # must be a verified mode of the sensor
hardware_verified = no
notes      = where the wiring came from
```

and in `machino.conf`: `board_profile_file = /etc/machino/my-board.conf`.

Built-in: add an entry in `src/profiles/builtin_profiles.cpp`, mark
`hardware_verified = true` only after a real bring-up, and add a line to the
table below. New sensors get a `SensorDescriptor` with verified modes only.

## Verified boards

| board_id | platform | sensor | wiring (bus/addr/mclk/rst/pwdn) | mode | status |
|---|---|---|---|---|---|
| `t40nn-imx307-board-a` | ingenic-t40nn | imx307 | 1 / 0x1a / 1 / 91 / 0 | 1920x1080@20 (default), @15, @10 | **hardware verified** (2026-09-18, OpenIPC 4.4.94 tx-isp, libimp 1.3.1, H.264/RTSP ≥60 s per point, ffmpeg-decoded, sensor fps read back from the ISP); presets balanced=15 fps, battery=10 fps @ 2000 kbps |

This entry describes one physical board. It does not mean all T40NN cameras
or all IMX307 modules are supported.
