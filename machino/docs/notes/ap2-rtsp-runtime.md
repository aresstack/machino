# AP2 — RTSP `enabled` / port rebind / runtime lifecycle

## Before

- `RtspConfig` had no `enabled` field: the listener was always bound at start.
- `rtsp.port` existed in the config file but was read **once** at startup; the
  API did not accept it at all.
- `start()`/`stop()` were one-shot (`stop()` was effectively terminal for the
  process) and unsynchronised against the API thread.
- `rtsp.*` keys the API did accept (`send_buffer_bytes`, `send_stall_ms`,
  `max_clients`) were all `DaemonRestart`.

## Now

- **`rtsp.enabled`** (new config key, default `true`, majestic-compatible):
  `false` at startup = no listener at all. At runtime `false` closes the
  listener *and* ends running sessions - each session's `DemandHandle`
  releases as its thread unwinds, so the pipeline winds down through the
  normal grace path instead of being left hot. `true` re-binds with no process
  restart.
- **`rtsp.port` is live**: `set_port()` closes the old listener and binds the
  new one. If the new port cannot be bound the **previous port is restored**
  and the call is rejected - never a half-dead RTSP. If even the rollback
  fails (a third party grabbed the old port in between) the server marks
  itself disabled and says so loudly rather than pretending success.
- Both go through the new `IRtspControl` port, so the API layer does not
  depend on the socket implementation and the host tests drive a fake.
- `start`/`stop`/`set_enabled`/`set_port` serialise on `lifecycle_m_`; an API
  thread can no longer race the accept-loop teardown.
- Config API/schema now carry `rtsp.enabled` and `rtsp.port` with their real
  runtime class (`live`), validated before they reach the listener
  (`port` 1..65535, `enabled` must be a boolean).
- `main.cpp` constructs the RTSP server before the API service and hands it
  over, so the wiring exists at runtime.

Demand semantics are unchanged: a bound listener is still not demand, media
starts at PLAY. Mainstream/substream (AP1) are unaffected - enable/disable/
rebind act on the listener, not on the per-unit hubs.

## Tests (host, 1188/0)

`test_ap2_rtsp_runtime` covers: schema exposes both fields with live values;
disable reaches the listener and persists; re-enable without restart;
port change reaches the listener and persists; port change back; out-of-range
port (0, 70000) and a non-boolean `enabled` rejected 422 *before* touching the
listener; a refused bind is rejected and leaves both the effective and the
persisted port on the old value; with no control wired the value is still
accepted and persisted (deferred) rather than silently dropped.

The socket layer itself (`rtsp_server.cpp`) is Linux-only and gated by the
MIPS cross-build in CI.

## Deliberately not in AP2

Auth, audio/backchannel, ONVIF, TLS, HLS, new codecs.

The majestic compat schema still does **not** advertise the rtsp section.
`enabled`/`port` are now truthfully live so they *could* be exposed to the
stock UI; left out until the rendered section can be checked on hardware.

## Open (NEEDS_HUMAN)

Hardware acceptance: deploy, power-cycle, then exercise
`enabled=false/true` and a `554 -> 8554 -> 554` rebind over `/api/v1/config`
while a client streams. Not run tonight - a warm install followed by traffic
is exactly the pattern that wedged this camera before.
