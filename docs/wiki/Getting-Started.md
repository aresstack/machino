# Getting Started

Machino is currently in a **hardware de-risk / architecture transition** phase. The
repository therefore contains two things at once:

1. a working, timps-derived C runtime used to validate Ingenic hardware and media paths;
2. the target Machino architecture: a platform-neutral C++ runtime with ports and
   adapters, capability-driven OpenIPC integration and optional extension modules.

Do not confuse the inherited executable names (`timpsd`, `timps.conf`) with the project
identity. They describe the current validation runtime, not the final Machino
architecture.

## Clone Machino

Use the Machino repository, not the original timps upstream:

```sh
git clone --recurse-submodules https://github.com/aresstack/machino.git
cd machino
```

The timps upstream remains the project's MIT-licensed ancestry/reference baseline and
is linked from [Home](Home.md), but development happens in `aresstack/machino`.

## Understand the current state before building

The repository README is the current project-level architecture statement. In
particular:

- T40/T40NN is the first hardware target;
- other SoCs should enter through adapters rather than core conditionals;
- compatibility with Majestic/OpenIPC belongs at the boundary;
- inactive media pipelines should not consume hardware unnecessarily;
- the current C implementation is retained for known-good Ingenic IMP call sequences
  while the production architecture moves toward C++.

See [Design Principles](Design-Principles.md) and
[OpenIPC Integration](OpenIPC-Integration.md) before adding a new platform-specific
feature.

## Build the current validation runtime

The existing top-level `Makefile` still builds the timps-derived validation runtime.
That is intentional: those binaries are useful for exercising the current media stack
and proving vendor SDK behaviour.

Typical host simulation build:

```sh
make sim
./timpsd-sim -c timps.conf
```

Typical cross-build shape:

```sh
make PLATFORM=T40 CROSS_COMPILE=mipsel-linux-
```

Exact SDK/header/library requirements remain documented in
[Building the current prototype](Building.md) and
[Platform & SDK Support](Platform-SDK-Support.md).

When those inherited pages use names such as `timpsd`, `timps.conf`, `hub.c` or
`hal_ingenic.c`, they are describing real current prototype code. They should not be
mechanically renamed in documentation before the code itself changes.

## T40NN + OpenIPC

For the first target, there are two different layers of work:

### Media runtime

Machino replaces the non-working Majestic path on the tested T40NN and owns the media
pipeline while active.

A second media daemon must not initialise the same ISP in parallel.

### Platform enablement

Some T40NN issues are below the media runtime: Ethernet/PHY state, boot-time hardware
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

Machino release payloads are treated differently from tiny built-in shell patches:
the bundle is shipped as an asset, verified against its manifest/SHA-256, transferred
to the camera and installed through `machino-manager`.

The manager keeps ownership state and remembers whether Majestic was active so an
uninstall can restore the previous service state. Optional payloads are selected
explicitly rather than forcing every installation to include every extension.

This deployment route is useful because it keeps Machino **bounded and removable**
instead of scattering unrelated files and WebUI changes across OpenIPC.

## Optional extensions

Machino's system integration is broader than the media daemon itself. Optional
capabilities may include:

- USB host support;
- LTE/cellular connectivity;
- Wi-Fi payloads;
- NNA/AI payloads;
- VPN-related components;
- board/platform compatibility patches.

These capabilities may be separate services or payloads. They are not required to run
inside the media process merely because they are distributed as part of the Machino
ecosystem.

See [USB & Modem Connectivity](USB-Modem-Connectivity.md).

## Before doing register or flash work

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

The Cam Tool research contains a measured example where changing a DDR-PHY bit at
runtime caused a CPU hardlock followed by watchdog reset. See
[OpenIPC Patch & Upstream Catalog](OpenIPC-Patch-Catalog.md).

## Where to put new work

Use the responsibility, not the place where the bug was discovered, to choose the
home of a change:

| Change | Preferred location |
| --- | --- |
| platform-neutral stream/media behaviour | Machino core |
| Ingenic SDK calls / IMP / NNA details | Ingenic adapter |
| board-specific runtime workaround | explicit platform-enablement module, with evidence |
| correct device tree / PHY / U-Boot initialisation | upstream OpenIPC/Linux/U-Boot where possible |
| modem/USB service | optional connectivity module/service |
| WebUI control | small capability-driven OpenIPC UI hook |
| broad camera research / recovery evidence | Cam Tool repository, curated into the wiki when stable |

## Documentation workflow

`docs/wiki/` in this repository is the canonical GitHub Wiki source. Edit it through
normal commits/PRs; the sync workflow mirrors it to the separate GitHub Wiki repo.

The sync is intentionally deterministic. The documentation structure is curated by
humans and reviewable in Git; CI should validate it, not invent prose during a build.

See [Documentation Sources](Documentation-Sources.md).
