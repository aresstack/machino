# AP5 — logging and runtime diagnostics

## The problem this closes

During the WebRTC debugging the daemon was effectively undiagnosable on a
normal boot: `/var/log/machino.log` was **0 bytes**. The cause was not the
logger but the launcher - busybox `start-stop-daemon -b` reopens the daemon's
stdio on `/dev/null`, so the init script's `>>"$LOGFILE"` never reached the
process. Every diagnostic that night needed a manual relaunch, and a manual
relaunch is exactly the warm-restart state that wedges this camera.

## Final log destination and start path

| | |
|---|---|
| Boot path | `S95streamer` -> `streamerctl` -> `/etc/init.d/machino start` -> `start-stop-daemon -b` |
| Why the redirect failed | `-b` reopens stdio on `/dev/null`; a shell redirect on the launcher cannot survive it |
| Fix | the daemon opens its **own** fd, path from `MACHINO_LOGFILE`, which the init script exports |
| File | `/var/log/machino.log` (+ one rotated generation `.1`) |
| Rotation | 256 KiB cap, one previous generation kept, so the worst case is bounded at ~512 KiB on the overlay |
| System log | in drop-in mode the daemon also logs to syslog with ident **`majestic`** |

The syslog ident matters: the stock log viewer filters its "majestic" source
on exactly that token (`/\bmajestic[[:]/` in `logs.js`), and the process
already renames itself to `majestic` for `pidof`/`killall`. Outside drop-in
mode the ident stays `machino`, which is the honest name there.

The sink lives in its own translation unit (`core/log_file.cpp`) with no
syslog dependency, so the **host tests exercise the shipped code** rather than
a stub - the stub now delegates to the same sink.

## `/ws/logs`

Implemented to the contract the unmodified viewer expects (read out of
`logs.js`, not guessed):

- WebSocket at `/ws/logs`, session-gated like everything else.
- **Binary** frames (the client sets `binaryType = 'arraybuffer'` and decodes
  UTF-8 itself).
- Payload is raw syslog lines; the viewer splits on `\n` and keeps a partial
  tail, so the server only ever forwards **whole lines**.
- Source is the system log, exactly as upstream describes it ("a single
  `logread -f` over which klogd has already merged kernel + application
  logs") - which is why Machino now logs to syslog in drop-in mode, so its own
  lines appear under the viewer's "majestic" filter.

Implementation: **one** `logread -f` child for the whole server, started with
the first subscriber and reaped with the last, `fork`+`exec` (no shell), read
end non-blocking and polled in the same loop as every socket. A viewer that
cannot keep up is dropped rather than buffered without bound. If the box has
no `logread`, the upgrade is accepted and the socket closes immediately
instead of pretending to stream.

## New runtime metrics

Added to `/api/v1/telemetry` under `sessions` - deliberately **only** what no
other field already reports, so two numbers can never disagree:

```
rtsp_sessions          webrtc_dtls_failures     webrtc_send_errors
webrtc_sessions        webrtc_srtp_failures     webrtc_pli
ws_video_clients       webrtc_rtp_packets
ws_logs_clients        webrtc_rtp_bytes
```

Already reported elsewhere and therefore **not** duplicated:
`pipeline_start_count` / `pipeline_stop_count` / restarts (lifecycle `Stats`
has `start_count`, `stop_count`, `restart_count`, `generation`) and per-unit
dropped frames (the measurement block). `streamhub_dropped_aus` is likewise
already visible per unit as `dropped_frames`; a second aggregate would only
be another number to keep in sync.

The counters replace the ad-hoc WebRTC debug counters: `peer.cpp` still keeps
its per-session totals for the 2 s `media:` line (the fault-localisation view),
but the process-wide facts now live in `RuntimeStats` and are served over the
API. Relaxed atomics - a diagnostic must never add ordering to the media path.

## Secrets

Nothing logged anywhere carries a password, an RTSP credential, a session
token or a key: the RTSP auth failure path logs peer/method/unit only, the
session gate logs `ok`/`REJECTED`, and DTLS logs error codes.

## Tests (host, 1358/0)

`test_logging.cpp` drives the shipped sink: an unconfigured sink writes
nothing and creates no file; lines land in order and the byte count tracks;
with a small cap the live file **and** the kept generation both stay bounded
(200 x 65 bytes that would otherwise be ~13 KB); the newest line is in the
live file, so a tailing reader sees current events; reopening keeps what is on
disk. Plus the counter semantics (gauges up and down, counters monotonic,
single instance).

`/ws/logs` itself is Linux-only (`fork`/`pipe`/`poll`) and gated by the MIPS
cross-build.

## Remaining diagnostic gaps

- `/ws/logs` has no hardware acceptance yet (**NEEDS_HUMAN**): needs a deploy,
  a power-cycle and the Logs page opened, ideally with two viewers at once.
- There is no rate limit on the log stream itself; a pathological log storm is
  bounded per client (slow viewers are dropped) but not globally.
- `/cgi-bin/j/dmesg.cgi` continues to serve kernel logs through the relay;
  only the live viewer is native.
