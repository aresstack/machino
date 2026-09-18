# M7 image quality and low-latency architecture

M7 deliberately keeps two independent control planes:

```
ImageSettings   -> TuningService -> IImageControl -> Ingenic ISP tuning
LatencySettings -> TuningService -> PipelineManager / IEncoder / StreamHub / RTSP
```

The core contains no vendor type or call. All SDK 1.3.1 mappings live in
`adapters/ingenic/ingenic_image_control.*` and `ingenic_encoder.*`.

## Image controls

The T40 adapter maps public SDK functions for brightness, contrast,
saturation, sharpness, hue, H/V flip, anti-flicker, AE target compensation,
highlight depression, backlight compensation, AWB mode, day/night running
mode, temporal/spatial noise reduction, DPC and defog. WDR is not advertised
for the current linear IMX307 mode, and the SDK marks the DRC module interface
reserved; both are reported `unsupported`, not silently accepted.

All image keys are optional. An absent key means “leave the ISP/tuning-bin
value alone.” Automatic exposure and automatic white balance therefore stay
the initial policy. M7 does not hardcode a manual exposure or sensor gain.
`/api/v1/state` and `/telemetry` expose AE luma/target/stability, integration
time, analog/digital/ISP gains and total gain read from the ISP so an
overexposure diagnosis is based on evidence. Configured values are reapplied
after every cold wake or controlled pipeline restart without creating demand
while the pipeline is cold.

Anti-flicker accepts only `off`, `50hz`, or `60hz`. `50hz` is appropriate for
the German test deployment but remains an explicit region/board setting, not
a universal default.

## Latency inventory and policy

The bounded stages are:

| stage | normal | low preset | behavior under pressure |
|---|---:|---:|---|
| FrameSource `nrVBs` | configured (default 2) | 1 | pipeline restart |
| encoder stream buffers | SDK default | 1 | set before `CreateChn`; pipeline restart |
| Machino AU pool | 16, fixed | 16, fixed | frame dropped if every slot is retained |
| per-consumer AU queue | 4 | 1 | drop-oldest, then discard P tail until IDR |
| TCP/UDP socket send buffer | 64 KiB default | configurable | bounded kernel queue |
| stalled TCP client | 750 ms default | configurable | disconnect and release demand |

There is no unbounded media FIFO. A lost P-frame invalidates the remaining
GOP for that client, so those stale dependent frames are discarded and the
consumer resumes on a requested/new IDR. SPS/PPS remain signalled in SDP and
cached from the stream. TCP interleaving uses `TCP_NODELAY`; UDP is supported;
there are no userspace sleeps between RTP packets.

SDK 1.3.1 exposes live GOP length and an encoder stream-buffer count, but no
documented B-frame/reordering or generic “low latency” switch in this path.
Those capability slots are reported `unsupported`; Machino does not invent
them.

## GOP changes and the join path

`latency.gop` applies live (`IMP_Encoder_SetChnGopLength`). The encoder keeps
reporting the previous length until the next GOP boundary, so that pending
read-back is never treated as the effective value: a successful set is the
acceptance, `/state` reports the accepted GOP immediately and a later pipeline
restart recreates the channel with it.

Measured on T40NN/IMX307 with a warm pipeline (a second client joining while
another is streaming), 7 probes per setting:

| GOP | PLAY -> first decodable frame (median) | range |
|---:|---:|---|
| 40 | 94 ms | 76-120 ms |
| 20 | 93 ms | 40-95 ms |
| 10 | 94 ms | 42-117 ms |

Join latency does not scale with GOP because a new consumer triggers an
on-demand IDR instead of waiting for the next scheduled one. A shorter GOP
therefore buys error-recovery speed after packet loss, not join latency.
Joining a *cold* pipeline costs about 810 ms, dominated by sensor/ISP bring-up
(~700 ms), again independent of GOP.

## Profiles and precedence

`latency.profile` is `normal | low | custom`.

* `normal`: configured video GOP/FrameSource buffers, SDK encoder-buffer
  default, per-consumer depth 4.
* `low`: GOP equals current stream FPS (one-second scheduled IDR), FrameSource
  buffers 1, encoder buffers 1, queue depth 1.
* `custom`: the normal base plus explicit overrides.

Explicit `latency.gop`, `latency.framesource_buffers`,
`latency.encoder_buffers`, and `latency.queue_depth` always override the
selected preset. Performance profiles remain orthogonal: changing FPS causes
the relative low-profile GOP to be re-resolved, while an explicit GOP stays
unchanged.

## Measurements

Machino records two camera-side spans in separate clock-domain-safe places:

* capture/encoder PTS to `IMP_System_GetTimeStamp()` immediately after
  `GetStream`;
* encoder fetch (`CLOCK_MONOTONIC`) to completion of the socket send.

The API reports one-second-window average/max values and discontinuities.
These do not include decoder/display buffering. Client startup and display
latency must be measured separately with ffmpeg/ffplay low-buffer options or
a clock visible in the scene. No millisecond end-to-end claim is made without
such a setup.

## T40NN / IMX307 qualification

Host tests cover mapping, unsupported controls, GOP validation, profile
precedence, bounded overflow/resync, IDR requests, persistence and
requested/effective API values. Hardware qualification records baseline,
low preset and a tuned candidate for at least 60 seconds each, including FPS,
bitrate, GOP, drops, RSS/CPU, AE read-back, reconnect and cold-wake lifecycle.
Only results from that run are marked hardware verified; stock/Majestic is a
behavioral reference, never a source of blindly copied configuration keys.
