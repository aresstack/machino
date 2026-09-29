# Design Principles

Machino is intentionally more than a collection of camera features. The project has a
set of architectural constraints that determine *how* features are added.

## Integration over invasion

Machino should extend OpenIPC without turning OpenIPC into a Machino-specific fork.
The preferred integration order is:

1. use an existing OpenIPC interface or convention;
2. add a small, well-defined integration point;
3. expose a capability through a stable Machino API;
4. only change OpenIPC core code when the responsibility genuinely belongs there.

A feature is not considered well integrated merely because it works. It should also be
possible to identify its ownership and update or remove it without touching unrelated
parts of the firmware.

## Native experience, loose coupling

The user should not need to understand which process implements a feature. A modem,
AI option, stream setting or platform fix may be provided by Machino, OpenIPC or
another component; the operator should see one coherent camera system.

That does **not** mean hiding ownership or creating opaque behaviour. It means keeping
the UX coherent while maintaining explicit technical boundaries underneath.

## Minimal WebUI intrusion

The OpenIPC WebUI is an integration surface, not Machino's internal architecture.
Machino should avoid:

- broad WebUI forks;
- duplicated pages that compete with existing OpenIPC configuration;
- camera-model-specific logic embedded in JavaScript;
- UI code that directly knows vendor register details;
- mandatory UI changes for headless/API use.

Prefer capability-driven UI additions. The runtime reports what is available; the UI
renders only the relevant controls.

```text
Machino capability API
          |
          +-- video.webrtc = true
          +-- ai.person_detection = true
          +-- connectivity.usb_modem = true
          +-- platform.t40nn_yt8512_workaround = true
          |
          v
OpenIPC WebUI renders small optional controls/sections
```

This keeps the WebUI generic and lets new hardware capabilities appear without adding
a model-name decision tree.

## Ports and adapters

Vendor SDKs and board quirks belong behind adapters. The platform-neutral core should
express intent such as:

- start a stream;
- request a key frame;
- query capabilities;
- change an ISP setting;
- run person detection;
- expose network state;
- activate an optional service.

It should not express "write this T40 register" or "call this Ingenic IMP function".
Those details belong in platform adapters or explicit hardware-enablement modules.

## Optional capabilities stay optional

USB host support, LTE modems, VPN services and AI models are useful additions, but they
must not become hard dependencies of the media path.

A build or camera without one of these capabilities should still have a clean runtime.
Modules should declare prerequisites and provide capability/state information rather
than leaking conditional logic across unrelated components.

## Compatibility at the boundary

Machino aims to fit into environments that expect Majestic-like behaviour. Compatibility
belongs at the system boundary: configuration translation, HTTP/API behaviour, service
lifecycle and paths where they are useful.

The core does not need to imitate Majestic's internal design.

## Evidence before workaround

Hardware fixes must preserve the chain from symptom to cause:

```text
symptom -> measurement -> difference -> experiment -> verified state -> workaround
                                                        |
                                                        +-> upstream candidate
```

A register script with no explanation is technical debt. A documented register script
with before/after state, hardware scope and recovery behaviour is reusable engineering
knowledge.

The T40NN + YT8512B investigation is the current reference example. See
[T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md).

## Runtime workarounds are not automatically permanent architecture

A Machino-side fix may be the fastest way to make unsupported hardware useful. Once a
root cause is sufficiently understood, decide where the permanent fix belongs:

- OpenIPC board definition / device tree;
- Linux/kernel driver;
- U-Boot;
- vendor/platform adapter;
- Machino runtime;
- WebUI integration.

The goal is not to collect every workaround inside Machino. The goal is to make the
whole camera stack work while keeping responsibilities clear.

## Resource-aware embedded design

Machino targets constrained devices. Architectural choices should favour:

- bounded queues and memory use;
- event-driven operation over busy polling;
- on-demand media pipelines;
- hardware acceleration where available;
- minimal copies on frame paths;
- explicit ownership of vendor resources;
- graceful degradation when an optional capability is unavailable.

## One owner for media hardware

Only one component should own ISP/media initialisation at a time. Running competing
media daemons against the same vendor SDK/hardware is not a compatibility mechanism.
Machino's OpenIPC integration should therefore make service ownership explicit.

## Documentation is part of the architecture

The wiki records stable knowledge and design intent. Experimental evidence stays close
to the experiment in the source repository and is linked from the curated page.

This separation is deliberate:

- **Wiki:** stable concepts, supported flows, verified findings, architecture.
- **Engineering notes:** experiments, logs, hypotheses, incident timelines.
- **Vendor documents:** external reference material with its original license/terms.
- **Code/tests:** executable truth.
