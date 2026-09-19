# Majestic compatibility

Machino is not a drop-in for Majestic's config file, and it does not try to be.
Its native contract is the platform-neutral `/api/v1` surface. Majestic
compatibility is provided at two clearly separated boundaries:

1. **A live WebUI bridge** (`src/app/compat/majestic_webui.*`) that presents the
   legacy majestic-webui schema/config documents and translates form POSTs back
   into native PATCHes. Validation and lifecycle semantics stay single-source in
   `ApiService::patch_config`.
2. **A one-way migration** (`src/app/compat/majestic_migrate.*`) that imports an
   existing `majestic.yaml` once, at install/import time, into a `machino.conf`.

This document is the compatibility matrix for the migration.

## Running the migration

```
machino --migrate-majestic /etc/majestic.yaml -o /etc/machino.conf
```

The full classification is printed to stderr (a line per source key); the
resulting `machino.conf` is written to `-o` (or stdout). The migration never
touches the running system and never guesses: anything it cannot carry over is
reported, not silently dropped.

Every source key is classified:

| Disposition | Meaning |
|-------------|---------|
| `mapped` | copied 1:1 to a machino key |
| `converted` | carried over, value transformed (units/scale) |
| `ignored` | machino handles this elsewhere; dropping it is safe |
| `unsupported` | a real Majestic feature machino does not implement |
| `invalid` | present but unusable (bad value, or missing data needed to convert) |

## Matrix

| majestic.yaml | machino | Disposition | Notes |
|---------------|---------|-------------|-------|
| `video0.fps` | `video.fps` | mapped | |
| `video0.bitrate` | `video.bitrate` | mapped | both kbit/s |
| `video0.size` | `video.width` + `video.height` | converted | `WxH` split |
| `video0.gop` | `latency.gop` | converted | Majestic GOP is **seconds**; converted to frames using `video0.fps`. Invalid if fps is missing. |
| `video0.rcMode` | `video.rc_mode` | mapped | `cbr`/`vbr`/`fixqp`; `avbr` → `vbr` (converted) |
| `video0.codec` | — | ignored (`h264`) / unsupported (`h265`) | machino builds H.264 only |
| `video0.enabled` | — | ignored (`true`) / unsupported (`false`) | the main stream is demand-driven and cannot be disabled |
| `video1.*` | `video.1.*` | as `video0` | substream |
| `image.contrast`/`hue`/`saturation`/`luminance`/`sharpness` | `image.*` | converted | rescaled 0..100 → 0..255 (`luminance` → `brightness`) |
| `image.mirror` / `image.flip` | `image.hflip` / `image.vflip` | converted | boolean → 0/1 |
| `image.rotate` | — | unsupported | no rotate |
| `rtsp.port` | `rtsp.port` | mapped | |
| `rtsp.enabled` | — | ignored | RTSP is always available |
| `jpeg.quality` | `jpeg.quality` | mapped / converted | clamped to machino's max (99) |
| `motionDetect.enabled` | `ai.enabled` (+ `ai.detector=motion`) | converted | mapped to machino detection (IMP_IVS motion) |
| `motionDetect.*` (roi/sensitivity) | — | unsupported | machino motion uses a fixed grid; per-region config is not migrated |
| `system.logLevel` | `log.level` | converted | `error`→0 `warn`→1 `info`→2 `debug`→3 |
| `system.webPort`/`httpPort` | — | ignored | the WebUI is served by the selector `httpd` on the fixed port 80 |
| `isp.*` | — | ignored | sensor/ISP wiring comes from the machino **board profile** |
| `watchdog.*` | — | ignored | handled by the OpenIPC init + `streamerctl` |
| `audio.*` | — | unsupported | no audio path |
| `records.*` | — | unsupported | no on-device recording |
| `mqtt.*` / `outgoing.*` / `onvif.*` | — | unsupported | not implemented |
| lists / flow values | — | unsupported | structured values are reported, never guessed |
| anything else | — | unsupported | unknown Majestic key |

## Notes and caveats

- **GOP units.** Majestic's `gop` is a duration in seconds; machino's
  `latency.gop` is a keyframe interval in frames. The migration multiplies by
  the stream fps. If a build of Majestic already stored `gop` in frames, review
  the converted value.
- **Image scale.** Majestic image controls are 0..100; machino's are 0..255.
  The migration rescales linearly. Fine-tune afterwards if a value matters.
- **One way only.** This is an import, not a sync. After migrating, edit
  `machino.conf` (or use the WebUI / `/api/v1`); do not expect changes to flow
  back to `majestic.yaml`.
- **Board profile wins.** Sensor identity and wiring are never taken from
  `majestic.yaml`; machino resolves them from its board profile so it can fail
  closed instead of guessing buses or pins.
