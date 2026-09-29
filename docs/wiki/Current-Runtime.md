# Current C++ Runtime

Machino's active implementation lives under [`machino/`](https://github.com/aresstack/machino/tree/main/machino).
It is a real C++ runtime, not a future rewrite plan and not a rename of the inherited
C prototype.

The timps-derived top-level runtime proved the first Ingenic media path. The C++ tree
then took over as the architecture that is actively extended.

## Architectural boundary

The core rule is visible in the directory layout:

```text
machino/
  src/core/               platform-neutral policy and media lifecycle
  src/ports/              hardware/service contracts
  src/profiles/           board/sensor/platform data
  src/adapters/ingenic/   IMP/JZDL/vendor-specific implementation
  src/app/                protocols, APIs, compatibility and Linux integration
  tests/                  host tests
  docs/                   implementation/work-package evidence
  openipc/                OpenIPC packaging/integration assets
```

Vendor headers and `IMP_*` calls stay below the Ingenic adapter. Board identity is data
(`PlatformDescriptor`, `BoardProfile`, `SensorDescriptor`, wiring), not a chain of
model-name conditionals through the core.

## Resource ownership

Ingenic resources are wrapped with RAII-style session objects. Bring-up constructs
sensor/ISP/system/FrameSource/encoder/binding resources in order and failed startup
unwinds in reverse order.

That design is important on a camera daemon: partial hardware initialisation should not
leave the next restart inheriting unknown SDK state.

## Demand-driven media lifecycle

`PipelineManager` is the single owner of the media chain and implements the project's
central power rule:

> **No consumer, no pipeline.**

A running daemon does not imply that the sensor/ISP/encoder must remain active.
Consumers hold demand handles; after the final demand disappears, a bounded grace
period can tear down the hardware pipeline while listeners/control services remain
alive.

This is both a power feature and an ownership feature: one component is responsible for
starting and stopping the chain.

## Capabilities instead of camera-name guesses

Machino exposes hardware/runtime capability through explicit data rather than requiring
the WebUI to know which exact board is connected.

The `/api/v1` layer reports controls and their status, including whether an operation
is supported and how it applies (`live`, pipeline restart, unsupported, etc.). Unknown
telemetry is represented as unknown/null instead of being fabricated as a numeric
zero.

This contract is the basis for low-intrusion OpenIPC WebUI integration:

```text
hardware adapter
      |
      v
capability/runtime API
      |
      +--> native Machino clients
      |
      `--> Majestic/OpenIPC compatibility adapter
                         |
                         v
                    stock WebUI
```

See [OpenIPC Integration](OpenIPC-Integration.md).

## Majestic/OpenIPC compatibility is an application adapter

The current C++ tree contains a dedicated `src/app/compat/` layer. That placement is a
major design decision: compatibility with Majestic-shaped configuration, paths and
OpenIPC WebUI behaviour does **not** contaminate the media core.

This lets Machino progressively support what the installed OpenIPC WebUI expects while
keeping three outcomes distinct:

- **real support** — Machino has a backend and can expose the capability;
- **unsupported/unknown** — the interface reports absence honestly and the UI should
  degrade gracefully;
- **small compatibility adaptation** — an isolated boundary hook maps existing OpenIPC
  behaviour to Machino without requiring a full WebUI fork.

The implementation notes under `machino/docs/dropin-gaps.md` are the detailed working
inventory. They should remain implementation evidence; this wiki records the stable
integration principle.

## Do not publish values that were never measured

Current compatibility work exposed an important API rule: a numeric zero is not a safe
replacement for an unavailable hardware reading.

Where an Ingenic call cannot provide a metric, Machino should publish `null`/absence and
let the compatibility layer omit the corresponding metric if that matches Majestic's
behaviour. This prevents the UI or monitoring code from treating an invented zero as a
real sensor state.

That rule generalises beyond ISP telemetry: **unknown is a first-class state**.

## Bounded failure and honest feature exposure

Machino's current engineering docs show the intended pattern for risky features:

- expose a capability only when the backend actually works;
- preserve an explicit unsupported response when hardware validation is missing;
- keep queues, clients and waits bounded;
- distinguish a safe fallback from a silently broken implementation;
- require physical validation before claiming that a hardware path works.

For example, an unavailable OSD backend should not advertise OSD merely because the API
surface exists. A risky JPEG path should not be silently exercised just to make a stock
WebUI tile look populated.

## Watchdog and process reliability

The current application tree includes Linux watchdog integration. This matters because
the T40NN work demonstrated that a complete camera hardlock can otherwise require a
physical power cycle.

A watchdog is not a root-cause fix: it limits outage duration. Platform hardlocks still
need forensic evidence and a proper fix, such as the DDR-PHY investigation documented
in [OpenIPC Patch & Upstream Catalog](OpenIPC-Patch-Catalog.md).

## Current implementation versus stable wiki contract

The C++ implementation evolves faster than this curated wiki. Use the layers as
follows:

- **this wiki** — stable architecture, integration contracts, verified hardware
  findings and cross-repository navigation;
- **`machino/README.md`** — active runtime overview and build entry points;
- **`machino/docs/`** — detailed work-package decisions, experiments and current gaps;
- **tests/code** — executable truth for the exact current build;
- **Cam Tool research** — board/firmware recovery and platform evidence.

This avoids turning the public wiki into a chronological engineering notebook while
still making every important conclusion discoverable.

## Build gate

The active runtime has a dedicated T40 CI path plus host tests. The basic host test
entry point is:

```sh
make -C machino test
```

Target builds should continue to pin the actual OpenIPC/Thingino-compatible toolchain,
vendor IMP ABI and required libraries rather than assuming that any MIPS build is
binary-compatible with the camera.

See [Getting Started](Getting-Started.md) for the repository-level entry point.
