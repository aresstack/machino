# AP1 — Substream (`stream=1`) end to end

## What was already there (M8 core + earlier wiring)

- `PipelineManager` units: per-stream framesource+encoder+capture with own
  demand counters and grace timers (`acquire_unit`, `configure_sub`,
  `request_idr(unit)`, `stream_unit`, `unit_configured`).
- `main.cpp`: resolves `video.1.*` via `effective_sub_stream` (no invented
  geometry - enabling requires explicit width/height), creates the sub
  `StreamHub`, calls `configure_sub`.
- RTSP: second mount point (`rtsp.sub_path`, default `/ch1`, plus the
  majestic-style `/stream=1` alias) already served from the sub hub with
  `acquire_unit(UNIT_SUB, Rtsp)`.
- Compat: `/api/v1/config` exposes `video1`, `/api/v1/sources` advertises the
  sub stream when configured.

## What AP1 added

The HTTP side had no path to the sub hub - `stream=1` was fail-closed 404 on
both browser transports. Now:

- `HttpServer` takes the sub hub (`main.cpp` passes it when the substream is
  configured).
- `unit_for_stream()`: `""`/`"0"` -> main, `"1"` -> sub **only when configured
  and fed**, everything else 404 (never a silent main feed - unchanged
  fail-closed rule).
- `/ws/video?stream=1` (MSE): sub sink + `acquire_unit(UNIT_SUB, HttpStream)`,
  init JSON/fMP4 geometry from `stream_unit(unit)` (this also removed the
  main-only api-state lookup), per-unit on-demand IDR (client `{"request":"idr"}`
  included).
- `/ws/webrtc?stream=1`: the stock signalling URL already carries the stream
  id; the negotiated session now streams the chosen unit with its own demand,
  PLI maps to `request_idr(unit)`.
- Per-client hub bookkeeping so disconnect/stop unsubscribe from the hub the
  sink actually came from.

`stream=0` behaviour is untouched (same acquire path via
`acquire_unit(UNIT_MAIN, ...)` == the previous `acquire(...)`).

## Supported paths after AP1

| Transport | Main | Sub |
|---|---|---|
| RTSP | `rtsp://cam/ch0` (+`/stream=0` alias) | `rtsp://cam/ch1` (+`/stream=1` alias) |
| MSE | `/ws/video` or `?stream=0` | `/ws/video?stream=1` |
| WebRTC | `/ws/webrtc` or `?stream=0` | `/ws/webrtc?stream=1` |

## Tests

- Core mechanics (separate hubs, per-unit demand/grace, both units at once,
  sub config/resolve refusals) were already covered by the multistream suite;
  suite stays green at 1167/0.
- The new routing layer is Linux-only (`http_server.cpp`) and gated by the
  MIPS cross-build in CI.

## Open (NEEDS_HUMAN)

- Hardware acceptance: enable `video.1.*` in the camera config (explicit
  geometry, e.g. 640x360@15), power-cycle, then verify RTSP `/ch1`,
  `/ws/video?stream=1` and the webui's SUB button live. Not run tonight
  (config change + deploy fall under the supervised rules).
