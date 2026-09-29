# Getting Started

Machino now contains an **active C++ runtime** under `machino/`. The original
timps-derived C implementation remains in the repository as the M1 hardware-validation
baseline and reference for known-good Ingenic IMP sequences; it is no longer the
runtime being evolved as Machino's production architecture.

The repository therefore has three useful layers:

1. **Machino C++ runtime (`machino/`)** — active implementation: ports & adapters,
   demand-driven lifecycle, capability-backed APIs and OpenIPC/Majestic compatibility.
2. **timps-derived C runtime (top-level legacy/prototype tree)** — working validation
   reference that proved the media hardware path and preserves detailed IMP knowledge.
3. **platform/integration research** — OpenIPC/T40NN enablement, USB/modem payloads,
   recovery evidence and optional system extensions.

Do not mechanically rename inherited identifiers such as `timpsd`, `timps.conf` or
`hub.c`: where they appear in the prototype-reference pages they still name real code.
They simply no longer define Machino's target architecture.

## Clone Machino

Use the Machino repository, not the original timps upstream:

```sh
git clone --recurse-submodules https://github.com/aresstack/machino.git
cd machino
```

The timps upstream remains the project's MIT-licensed ancestry/reference baseline.
Development happens in `aresstack/machino`.

## Build the active C++ runtime

The active runtime lives below `machino/`:

```sh
make -C machino test
```

The current runtime README documents the concrete architecture and build/test entry
points:

[`machino/README.md`](https://github.com/aresstack/machino/blob/main/machino/README.md)

The C++ runtime already has a real implementation of the architecture described in
this wiki. Among the established pieces are:

- platform-neutral core and explicit ports;
- Ingenic adapter isolated below `src/adapters/ingenic/`;
- board/sensor descriptors rather than board constants in the core;
- RAII ownership of IMP resources;
- a demand-driven `PipelineManager` (`no consumer, no pipeline`);
- bounded per-consumer queues;
- power/performance service and telemetry;
- `/api/v1` control/capability API;
- ISP image-control abstraction;
- low-latency stream controls;
- a growing Majestic/OpenIPC compatibility layer.

The exact implemented feature set continues to move quickly. Treat code/tests and the
runtime's own `machino/docs/` as executable/current detail; this wiki describes the
stable system architecture and curated hardware knowledge.

See [Current C++ Runtime](Current-Runtime.md).

## The timps-derived prototype is still useful

The top-level `Makefile` and inherited wiki pages document the C validation runtime.
That implementation remains valuable when answering questions such as:

- Which Ingenic IMP call sequence is known to work?
- How did the first RTSP/fMP4 pipeline behave?
- Which SDK generations differ?
- What did a working encoder/FrameSource path look like before the C++ abstraction?

Typical host simulation build of that reference implementation:

```sh
make sim
./timpsd-sim -c timps.conf
```

Typical cross-build shape:

```sh
make PLATFORM=T40 CROSS_COMPILE=mipsel-linux-
```

See [Prototype Runtime Architecture](Architecture.md),
[Building the current prototype](Building.md) and
[Platform & SDK Support](Platform-SDK-Support.md).

Those pages are **reference documentation**, not the architectural entry point for new
Machino code.

## T40NN + OpenIPC

For the first target, separate the media runtime from the platform below it.

### Media runtime

Machino exists because the tested T40NN could not use the normal Majestic/OpenIPC media
path successfully. The C++ runtime is the replacement media service and owns the ISP /
media chain while active.

A second media daemon must not initialise the same ISP in parallel.

### Platform enablement

Some T40NN issues are below Machino's media core: Ethernet/PHY state, boot-time hardware
initialisation and potentially DDR idle/wakeup behaviour.

See:

- [T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md)
- [OpenIPC Patch & Upstream Catalog](OpenIPC-Patch-Catalog.md)
- [T40NN Research & Recovery](T40NN-Research-Recovery.md)

A runtime workaround may make the board usable today, but verified root causes should
move upstream when they belong in OpenIPC, Linux, U-Boot or the board description.

## Installing through the Cam Tool

The Cam Tool repository contains a patch/deployment registry for the tested camera:

[github.com/Miguel0888/ipcam-lan-discovery](https://github.com/Miguel0888/ipcam-lan-discovery)

Machino release payloads are treated differently from tiny built-in shell patches: a
bundle is shipped as an asset, verified against its manifest/SHA-256, transferred to
the camera and installed through `machino-manager`.

The manager records ownership and whether Majestic was active so uninstall can restore
the previous service state. Optional payloads are selected explicitly rather than
forcing every Machino installation to carry every extension.

That bounded install/uninstall model is part of the low-intrusion OpenIPC strategy:
Machino should be removable without requiring a private fork of the whole firmware or
WebUI.

## OpenIPC WebUI integration

The current C++ tree already contains explicit compatibility code for the stock OpenIPC
WebUI. The architectural rule remains that UI compatibility lives at the boundary:
Machino should expose the paths/config/capabilities the UI needs rather than moving
platform knowledge into JavaScript.

Recent compatibility work demonstrates both sides of that rule:

- real Machino capabilities are translated into Majestic-shaped configuration where
  the stock WebUI expects them;
- unsupported capabilities are omitted or return an explicit unsupported result rather
  than publishing fake values;
- small compatibility hooks may adapt an existing OpenIPC page, but broad WebUI forks
  are avoided.

See [OpenIPC Integration](OpenIPC-Integration.md).

## Optional extensions

Machino's system integration is broader than the media process itself. Optional
capabilities may include:

- USB host support;
- LTE/cellular connectivity;
- Wi-Fi payloads;
- NNA/AI payloads;
- VPN-related components;
- board/platform compatibility patches.

These may be separate services or payloads. They do not belong in the media core merely
because they are distributed through the Machino ecosystem.

See [USB & Modem Connectivity](USB-Modem-Connectivity.md).

## Before register or flash work

Do not treat low-level platform work like a normal runtime configuration change.

For the T40NN research path, preserve at least:

- UART/U-Boot access;
- a verified full-flash backup;
- a known recovery procedure;
- exact before/after register state;
- cold-boot and warm-reboot observations;
- bounded retries/timeouts;
- hardware/PHY/SoC detection gates;
- a clear maturity label (verified/provisional/hypothesis).

The Cam Tool research contains a measured case where changing a DDR-PHY bit at runtime
caused a CPU hardlock followed by watchdog reset. See
[OpenIPC Patch & Upstream Catalog](OpenIPC-Patch-Catalog.md).

## Where new work belongs

Choose ownership from the responsibility, not from the repository where the symptom
was discovered:

| Change | Preferred location |
| --- | --- |
| platform-neutral stream/media behaviour | Machino C++ core |
| Ingenic SDK calls / IMP / NNA details | Ingenic adapter |
| board-specific runtime workaround | explicit platform-enablement module, with evidence |
| correct device tree / PHY / U-Boot initialisation | upstream OpenIPC/Linux/U-Boot where possible |
| modem/USB service | optional connectivity module/service |
| WebUI control | small capability-driven OpenIPC UI hook |
| broad camera research / recovery evidence | Cam Tool repository, curated into the wiki when stable |

## Documentation workflow

`docs/wiki/` is the canonical GitHub Wiki source. Edit it through normal commits/PRs;
the sync workflow mirrors it to the separate GitHub Wiki repository.

The sync is intentionally deterministic. Human-reviewed structure and prose stay in
Git; CI validates navigation and consistency rather than inventing documentation during
a build.

See [Documentation Sources](Documentation-Sources.md).
