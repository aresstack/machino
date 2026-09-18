# Media lifecycle — "no consumer, no pipeline"

> An open Machino daemon is not equivalent to an active camera pipeline.
>
> Media hardware is activated by demand, not by process lifetime.

The daemon, its RTSP listener and the control side stay up permanently. The
expensive media chain — sensor, ISP, FrameSource, encoder — exists only while
at least one real consumer needs it.

## State machine

```
COLD_IDLE ──first demand──> STARTING ──ok──> ACTIVE
                                │                │ last demand gone
                                │ failure        v
                                │ (rolled back)  GRACE_IDLE ──demand returns──> ACTIVE
                                v                │ grace timeout
                             FAILED ──new demand──> STARTING
                                                 v
                                              STOPPING ──> COLD_IDLE
```

Implemented in `src/core/lifecycle/pipeline_manager.*`. All transitions
happen in one place under one mutex that is held only during a transition;
the frame path (capture thread → `StreamHub` → sessions) never takes it.
There is exactly one media owner and exactly one start or stop at a time: a
consumer arriving during STOPPING simply waits for the mutex and then starts
the next generation — no overlap, no double start, no double stop.

## Demand

```cpp
auto demand = pipeline.acquire(ConsumerType::Rtsp);   // RAII token
...
// handle destroyed -> demand released
```

`DemandHandle` is the only way to express demand. There are no hand-managed
counters. A lost network client releases its demand when its session object
dies. Consumer types (`rtsp`, `snapshot`, `recording`, `ai`, `http`, `onvif`,
`manual`) are counted separately for observability but share **one** media
pipeline.

RTSP semantics:

| event | demand |
|---|---|
| TCP connect / OPTIONS / SETUP | none |
| DESCRIBE without cached SPS/PPS | scoped demand until the first key frame, then released; SPS/PPS cached |
| PLAY | +1 (held by the session) |
| TEARDOWN / disconnect / dead socket (send stalls > 5 s) | −1 |

`pipeline.always_on = 1` and `SIGUSR2` take a *manual* demand; `SIGUSR1`
releases it. With no other consumer the pipeline then follows the normal
grace → cold path.

Sessions are bounded and reclaimed while the daemon runs:

* `rtsp.max_clients` (default 4) caps the concurrent sessions. Connection
  number *n+1* is answered with `453 Not Enough Bandwidth` and closed — it is
  refused, not queued, so a client learns immediately instead of waiting on a
  stream that will not come.
* A client that leaves has its thread joined and its slot freed by the accept
  loop. Without that every connect/disconnect cycle would leave a finished
  thread behind and the list would only ever grow: a camera that reconnects all
  day would accumulate thread stacks until the next restart.

## Grace idle

When the last consumer leaves, the pipeline stays up for
`lifecycle.idle_grace_ms` (default 5000 ms) so reconnects, transport
switches, VLC reopening or a short network hiccup do not toggle the sensor.
Returning demand cancels the timer and the same pipeline generation
continues. The timer is a one-shot `timerfd` watched by the main `epoll`
loop (port `IGraceTimer`, Linux implementation `app/linux_grace_timer.*`);
there is no polling thread and no periodic tick.

## Cold idle

After the grace period the chain is torn down completely, in the proven
reverse order: `StopRecvPic` → `FrameSource_DisableChn` → `System_UnBind` →
encoder `UnRegisterChn/DestroyChn` + `DestroyGroup` → `FrameSource_DestroyChn`
→ `DisableTuning` → `System_Exit` → `DisableSensor/DelSensor` → `ISP_Close`.
The process keeps running:

```
PID exists · RTSP :554 listens · media pipeline inactive
```

The next consumer performs a full bring-up again — any number of times in the
same process (verified: 12 cold cycles on T40NN/IMX307 without process
restart, RSS/thread count flat).

## Failure

A failed start rolls back every resource created so far (RAII sessions in the
adapter, reverse order), logs the exact stage, and enters FAILED. There is no
automatic retry loop; the next consumer may attempt a controlled new start.

## Observability

```
lifecycle: COLD_IDLE -> STARTING (consumer=rtsp)
lifecycle: STARTING -> ACTIVE
lifecycle: demand +1 type=rtsp total=1
lifecycle: demand -1 type=rtsp total=0
lifecycle: ACTIVE -> GRACE_IDLE timeout=5000ms
lifecycle: GRACE_IDLE -> ACTIVE (new demand)
lifecycle: GRACE_IDLE -> STOPPING
lifecycle: STOPPING -> COLD_IDLE
```

`PipelineManager::stats()` exposes state, total and per-type demand, pipeline
generation / start / stop / failed counts and the last start error. No HTTP
API yet (M6). M5 (power) will build on the ACTIVE / GRACE_IDLE / COLD_IDLE
distinction.
