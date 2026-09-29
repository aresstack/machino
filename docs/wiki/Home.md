# Machino Wiki

**Machino** is a lightweight media runtime and extension layer for embedded IP cameras.
It is designed to fit naturally into **OpenIPC** while keeping platform-specific work,
optional services and WebUI integration loosely coupled.

Machino started from a practical gap: the normal OpenIPC media path around
**Majestic did not provide a usable video path for the Ingenic T40NN hardware being
tested**. The first hardware validation therefore used the MIT-licensed
[Lu-Fi/timps](https://github.com/Lu-Fi/timps) codebase as a known-good Ingenic IMP
reference.

That ancestry remains valuable, but it no longer defines the active implementation.
Machino now has its own C++ runtime under `machino/`, built around ports and adapters,
explicit capabilities and demand-driven media lifecycle management. The inherited C
runtime remains as an M1 validation/reference baseline.

The project goal is broader than replacing one streamer:

> **Provide the media/runtime capabilities an OpenIPC camera needs, add capabilities
> OpenIPC/Majestic do not provide, and do so without requiring invasive changes to
> OpenIPC itself.**

Machino is not a firmware distribution. It runs *on top of* OpenIPC and should feel
like a native part of the camera while remaining a bounded, identifiable component.

Start with [Getting Started](Getting-Started.md) and
[Current C++ Runtime](Current-Runtime.md).

## Why Machino exists

The first concrete target is **Ingenic T40/T40NN**. Work on the tested T40NN exposed
both media-runtime gaps and platform issues below the media layer: PHY/network setup,
boot-time hardware state, vendor SDK/ABI behaviour and potentially DDR idle/wakeup
initialisation.

Those findings are useful beyond Machino itself. Exact register state, failure modes,
warm-reboot behaviour and recovery evidence can support clean OpenIPC/Linux/U-Boot
fixes instead of living forever as private runtime scripts.

Machino therefore has two related roles:

1. **Media runtime** — own the sensor/ISP/media pipeline and provide streaming,
   snapshots, audio, control, detection and related camera services as they are
   implemented and validated.
2. **Extension platform** — integrate optional camera capabilities that do not belong
   in a traditional media streamer, such as USB host support, LTE modems, networking /
   VPN components, AI payloads and hardware-specific enablement.

The second role does **not** mean every extension runs inside the media process.
Separate services and payloads are preferred when that keeps responsibilities clearer.

## Core design decisions

These are architectural constraints, not presentation preferences.

### 1. Extend OpenIPC; do not turn it into a private Machino fork

Machino integrates through stable boundaries: services, files, APIs, capability
metadata and small WebUI hooks. A feature should not require broad edits throughout
OpenIPC or a long-lived private fork of the WebUI.

The desired result is **native-feeling integration with loose coupling underneath**.
To an operator, Machino capabilities should simply look like capabilities of the
OpenIPC camera. To a developer, Machino ownership must remain explicit enough that the
component can be updated, removed or replaced without untangling unrelated firmware.

See [Design Principles](Design-Principles.md) and
[OpenIPC Integration](OpenIPC-Integration.md).

### 2. Extensibility is first-class

Hardware support and optional services enter through adapters/modules and explicit
capabilities, not by spreading vendor/model conditionals throughout the core or WebUI.

New SoCs, PHY quirks, AI accelerators, USB devices and networking services should be
additions at defined boundaries.

### 3. Majestic/OpenIPC compatibility belongs at the edge

Machino aims to be a practical Majestic replacement where useful, but it does not copy
Majestic internals into the core. Majestic-shaped configuration, paths and stock-WebUI
expectations are handled by an application/compatibility adapter around the independent
runtime.

The current C++ tree already follows this separation through `src/app/compat/`.

### 4. Hardware knowledge should be reusable upstream

Runtime workarounds are valuable when they make unsupported hardware usable today, but
a verified root cause should also be recorded in a form that can support the correct
permanent owner.

The T40NN/YT8512B Ethernet investigation is the reference pattern: preserve the exact
working state, broken state, ordering, validation result and hardware scope rather than
keeping only a shell script that happens to work.

See [T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md) and
[OpenIPC Patch & Upstream Catalog](OpenIPC-Patch-Catalog.md).

### 5. Do not make the WebUI own the architecture

The OpenIPC WebUI should discover capabilities and call stable APIs. It should not
contain board-specific register logic or become the place where platform support is
implemented.

Prefer small, capability-driven additions and compatibility hooks. Broad page forks,
model-name decision trees and duplicated settings pages are a last resort, not the
default integration method.

### 6. Unknown is a real state

If hardware cannot provide a metric or capability, Machino reports it as unavailable /
unknown rather than inventing a zero or pretending support. This prevents compatibility
code and the WebUI from treating fabricated values as measured camera state.

## Architecture

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
| RTSP | HTTP | APIs | OpenIPC/Majestic adapters | ...      |
|                                                           |
| Platform-neutral media & capability core                  |
| Streams | Frames | Sensors | Events | Power | Telemetry   |
|                                                           |
| Ports                                                     |
| ISP | Encoder | Audio | AI | Platform | optional services |
|                                                           |
| Adapters / bounded extensions                             |
| Ingenic | future SoCs | T40NN enablement | LTE | VPN ...  |
+-----------------------------------------------------------+
                         |
               Linux + vendor hardware
```

The active implementation is the C++ runtime in `machino/`. It uses explicit ports,
Ingenic adapters, board/sensor descriptors, RAII resource ownership, bounded queues,
demand-driven pipeline lifecycle and a capability-backed control API.

The top-level timps-derived C implementation remains because it is useful evidence of
known-good Ingenic IMP behaviour. Its detailed wiki pages are grouped under
**Prototype Runtime Reference** rather than presented as Machino's current
architecture.

See [Current C++ Runtime](Current-Runtime.md).

## Documentation map

### Machino architecture and integration

- [Getting Started](Getting-Started.md) — repository layers, active runtime, prototype
  reference and deployment context.
- [Current C++ Runtime](Current-Runtime.md) — active implementation architecture.
- [Design Principles](Design-Principles.md) — non-negotiable extension and ownership
  rules.
- [OpenIPC Integration](OpenIPC-Integration.md) — native-feeling integration without
  deep OpenIPC/WebUI coupling.

### T40NN / OpenIPC enablement

- [T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md) — verified platform findings,
  especially Ethernet/PHY state and upstream candidates.
- [OpenIPC Patch & Upstream Catalog](OpenIPC-Patch-Catalog.md) — executable workarounds,
  maturity, risks and preferred long-term owner.
- [T40NN Research & Recovery](T40NN-Research-Recovery.md) — stock firmware, flash,
  recovery and fail-closed engineering evidence.
- [Platform & SDK Support](Platform-SDK-Support.md) — inherited SDK/platform reference.

### Extensions beyond a traditional media streamer

- [USB & Modem Connectivity](USB-Modem-Connectivity.md) — USB host direction,
  Quectel EC200A-EU, PPP/usbnet payloads and service boundaries.
- [AI & Person Detection](AI-Person-Detection.md) — NNA/JZDL and model-related work.

### Prototype runtime reference

The inherited pages preserve detailed knowledge from the C validation runtime:

- [Prototype Runtime Architecture](Architecture.md)
- [Configuration Reference](Configuration-Reference.md)
- [Streaming Protocols](Streaming-Protocols.md)
- [HTTP /control API](HTTP-Control-API.md)
- [Audio](Audio.md), [Motion Detection](Motion-Detection.md),
  [Day/Night](Day-Night.md), [Recording & Timelapse](Recording-Timelapse.md)
- rate-control, logging, build and test reference pages listed in the sidebar.

These pages describe real reference code but are not the architectural starting point
for new Machino development.

### Sources and provenance

- [Documentation Sources](Documentation-Sources.md) explains which repository owns raw
  evidence, vendor PDFs, executable patches and curated conclusions.

## Project lineage and license

Machino's repository history includes work derived from
[Lu-Fi/timps](https://github.com/Lu-Fi/timps), which declares MIT licensing. Machino
retains that permissive lineage for project code; third-party SDKs, binary libraries
and vendor documents retain their own terms.

The timps-derived implementation remains important evidence and a validation baseline,
but **Machino is the project; timps is its technical ancestry**.

## Documentation policy

`docs/wiki/` is the canonical source for this GitHub Wiki. The sync workflow mirrors
that directory to GitHub, including deletions.

The workflow is intentionally deterministic. CI validates that navigation is complete
and resolvable; it does **not** generate or rewrite prose with an LLM during a build.
New knowledge remains reviewable in normal Git commits and pull requests.

Raw boot logs, dumps, vendor PDFs, screenshots, experiments and chronological research
stay in their owning repositories and are linked from the relevant curated page. This
keeps the public wiki structured while preserving the evidence behind it.
