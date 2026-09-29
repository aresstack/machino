# Machino Wiki

**Machino** is a lightweight media runtime and extension layer for embedded IP cameras.
It is designed to fit naturally into **OpenIPC** while keeping platform-specific work,
optional services and WebUI integration loosely coupled.

Machino started from a practical gap: the existing OpenIPC media stack around
**Majestic did not provide a usable path for the Ingenic T40NN hardware being tested**.
The project therefore began by validating the camera directly, using the
MIT-licensed [Lu-Fi/timps](https://github.com/Lu-Fi/timps) codebase as a known-good
Ingenic IMP reference. That ancestry is still visible in parts of the repository and
in several detailed runtime pages in this wiki, but it does **not** define the target
architecture.

The target is broader:

> **Provide the media/runtime capabilities an OpenIPC camera needs, add capabilities
> OpenIPC/Majestic do not provide, and do so without requiring invasive changes to
> OpenIPC itself.**

Machino is not a firmware distribution. It is intended to run *on top of* OpenIPC and
to feel like a native part of the system.

## Why Machino exists

The first concrete target is **Ingenic T40/T40NN**, with T40NN currently requiring
additional engineering work around the platform, PHY/network setup and vendor SDK.
That work uncovered information that is useful beyond Machino itself: register values,
boot-time state, PHY setup, warm-reboot behaviour and other hardware facts can form the
basis for future upstream OpenIPC support.

Machino therefore has two related roles:

1. **Media runtime** — sensor/ISP ownership, hardware encoding, RTSP/HTTP/WebRTC,
   snapshots, audio, AI/detection, recording and control.
2. **Extension platform** — optional camera capabilities that do not naturally belong
   in a traditional media streamer, such as USB host support, LTE modems, additional
   networking/VPN components and hardware-specific enablement.

## Core design decisions

These are not implementation details; they are architectural constraints.

### 1. Extend OpenIPC, do not fork it into a different product

Machino should integrate through stable boundaries: services, files, APIs, capability
metadata and small WebUI hooks. A feature should not require broad edits throughout
OpenIPC or a private long-lived fork of the WebUI.

The desired result is **native-feeling integration with loose coupling underneath**.
To an operator, a Machino capability should simply look like another OpenIPC feature.
To a developer, it should remain possible to identify, update or remove the Machino
part without untangling it from unrelated OpenIPC code.

See [Design Principles](Design-Principles.md) and
[OpenIPC Integration](OpenIPC-Integration.md).

### 2. Extensibility is a first-class feature

Hardware support and optional services are adapters/modules, not reasons to put
vendor-specific conditionals everywhere. New SoCs, PHY quirks, AI accelerators,
USB devices or network services should enter through explicit capability boundaries.

### 3. Compatibility belongs at the edge

Machino aims to become a practical replacement for Majestic where useful, but it does
not reproduce Majestic internals. Compatibility with OpenIPC expectations is provided
at the boundary while the internal architecture remains independent.

### 4. Hardware knowledge should be reusable upstream

Runtime workarounds are valuable for getting hardware usable today, but a verified
root cause should also be documented in a form that can support a clean upstream fix.
The T40NN/YT8512B Ethernet investigation is the model: preserve the evidence, exact
register state, failure mode and validation result instead of keeping only a shell
script that happens to work.

See [T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md).

### 5. Do not make the WebUI own the architecture

The WebUI should discover capabilities and call stable APIs. It should not contain
camera-model-specific business logic or become the place where platform support is
implemented. New controls should be small additions that appear only when the runtime
reports the corresponding capability.

## Architecture direction

```text
                         OpenIPC

      existing services / WebUI / system integration
                         |
              small, stable integration points
                         |
+-----------------------------------------------------------+
|                        Machino                            |
|                                                           |
| Application / compatibility                               |
| RTSP | HTTP | WebRTC | Control API | AI | Recording      |
|                                                           |
| Platform-neutral media & capability core                  |
| Streams | Frames | Sensors | Events | Power | Telemetry   |
|                                                           |
| Ports                                                     |
| ISP | Encoder | Audio | AI | USB | Network | Platform     |
|                                                           |
| Adapters / optional capabilities                          |
| Ingenic | future SoCs | T40NN enablement | LTE | VPN ...  |
+-----------------------------------------------------------+
                         |
               Linux + vendor hardware
```

The production direction is a lightweight C++ runtime with ports-and-adapters
boundaries, bounded resources and hardware acceleration. The repository still
contains a substantial timps-derived C implementation because it is valuable as a
working hardware-validation baseline and as documentation of known-good Ingenic IMP
call sequences.

For the current runtime internals, see [Prototype Runtime Architecture](Architecture.md).
For the target integration model, see [OpenIPC Integration](OpenIPC-Integration.md).

## Main documentation areas

### Runtime and media

- [Prototype Runtime Architecture](Architecture.md) — current timps-derived process,
  HAL, hub and media pipeline internals.
- [Streaming Protocols](Streaming-Protocols.md) — RTSP, HTTP/fMP4, MJPEG and related
  protocol behaviour.
- [HTTP /control API](HTTP-Control-API.md) — runtime control and capability API.
- [Audio](Audio.md) — capture and speaker/backchannel paths.
- [AI & Person Detection](AI-Person-Detection.md) — T40 NNA/JZDL work and models.
- [Motion Detection](Motion-Detection.md), [Day/Night](Day-Night.md),
  [Recording & Timelapse](Recording-Timelapse.md).

### OpenIPC and hardware enablement

- [OpenIPC Integration](OpenIPC-Integration.md) — how Machino should extend OpenIPC
  without deep core or WebUI modifications.
- [T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md) — verified hardware findings,
  Ethernet/PHY state and upstream candidates.
- [Platform & SDK Support](Platform-SDK-Support.md) — vendor SDK capability matrix.

### Extensions beyond a traditional media streamer

- [USB & Modem Connectivity](USB-Modem-Connectivity.md) — USB host direction,
  Quectel EC200A-EU, PPP/USB networking and how such services fit beside the media
  runtime rather than inside its core.

### Engineering and provenance

- [Building](Building.md) and [Testing / QA](Testing-QA.md).
- [Documentation Sources](Documentation-Sources.md) — which repository owns which
  evidence, how raw dumps/PDFs relate to curated wiki pages, and what should be
  upstreamed rather than duplicated.

## Project lineage and license

Machino's repository history includes work derived from
[Lu-Fi/timps](https://github.com/Lu-Fi/timps), which is MIT-licensed. Machino retains
that permissive lineage for project code; third-party SDKs, binary libraries and vendor
documents retain their own terms.

The timps-derived implementation remains important evidence and a validation baseline,
but **Machino is the project; timps is its technical ancestry**.

## Documentation policy

`docs/wiki/` is the canonical source for this GitHub Wiki. The Wiki sync action mirrors
these files to GitHub; it should remain deterministic and should **not generate prose
with an LLM during CI**.

Curated knowledge belongs here. Raw boot logs, dumps, vendor PDFs, experiments and
one-off research remain in their source repositories and are linked from the relevant
wiki page. This keeps the wiki readable while preserving the evidence behind it.
