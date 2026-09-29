# OpenIPC Integration

Machino is intended to run as a natural part of an OpenIPC camera while remaining
architecturally independent from OpenIPC's WebUI and from Majestic internals.

The key idea is simple:

> **OpenIPC should need to know that a capability exists, not how Machino implements it.**

## System boundary

```text
+--------------------------------------------------------------+
| OpenIPC                                                      |
|                                                              |
|  WebUI     init/service lifecycle     config/system services |
|    |                 |                         |              |
+----|-----------------|-------------------------|--------------+
     | small hooks     | stable process/service | existing OS
     v                 v                         v
+--------------------------------------------------------------+
| Machino                                                      |
|                                                              |
| Compatibility / Control API / Capability API                 |
| Media runtime              Optional extension services        |
| Platform adapters          Hardware enablement                |
+--------------------------------------------------------------+
```

Machino should not require the OpenIPC UI to understand Ingenic IMP calls, PHY
registers, modem AT commands or AI accelerator details.

## What should be integrated into OpenIPC

OpenIPC-facing integration can include:

- service installation and lifecycle;
- configuration file ownership/translation;
- stable HTTP/control endpoints;
- capability and telemetry discovery;
- small WebUI sections or controls;
- log/status exposure;
- package/build integration;
- board/kernel fixes whose responsibility belongs upstream.

## What should remain inside Machino

- media lifecycle and stream ownership;
- vendor SDK interaction;
- protocol implementations owned by Machino;
- hardware capability abstraction;
- optional AI/media modules;
- optional USB/modem service coordination when packaged with Machino;
- compatibility translation that exists only to satisfy external expectations.

## WebUI strategy

The OpenIPC WebUI should be extended by the smallest useful surface.

A preferred flow is:

```text
GET /api/capabilities
        |
        v
WebUI discovers supported feature
        |
        +-- supported -> render existing or small optional control
        +-- absent    -> do not render it
```

The UI should avoid hard-coded checks such as:

```text
if camera == "T40NN board X" then show modem page
```

Instead, platform and feature modules report capabilities. This allows the same UI
integration to work when a second board or SoC later gains the same capability.

## Majestic replacement without Majestic coupling

Machino exists partly because the Majestic-based path did not make the tested T40NN
camera usable. It therefore needs practical compatibility with OpenIPC expectations,
but compatibility is an adapter at the edge.

```text
OpenIPC expectation
       |
       v
compatibility adapter
       |
       v
Machino domain/core
       |
       v
platform adapter
```

Examples of boundary compatibility can include configuration mapping, service names,
API semantics or stream URLs. None of those require the internal media core to adopt
Majestic's implementation model.

## Platform enablement versus runtime compensation

T40NN engineering exposed an important distinction.

Some problems can be compensated at runtime so the camera is usable immediately. But
if the verified cause is a device-tree value, bootloader setup or Linux PHY behaviour,
the long-term correction may belong in OpenIPC or the kernel rather than Machino.

Machino documentation therefore records both:

- **operational workaround** — what makes current hardware work;
- **upstream candidate** — where a clean permanent fix likely belongs and which
  measurements support it.

See [T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md).

## Extensions beyond Majestic

Machino's scope is intentionally extensible. A traditional media daemon does not need
to own every extra capability, but the camera still benefits from a coherent extension
model.

Examples include:

- USB host enablement;
- LTE/4G modem support;
- PPP or USB Ethernet modes;
- VPN components;
- hardware AI/NNA helpers;
- platform-specific recovery/diagnostics.

These should be separate capability modules/services where appropriate, exposed to
OpenIPC through the same integration philosophy rather than being forced into the
media core.

See [USB & Modem Connectivity](USB-Modem-Connectivity.md).

## Integration rules

When adding a feature, ask in this order:

1. Can it use an existing OpenIPC interface unchanged?
2. Can Machino expose it through an existing control/capability API?
3. Can the WebUI add one isolated component or section rather than changing shared
   behaviour?
4. Is the required platform change actually an upstream OpenIPC/kernel responsibility?
5. Does the feature need to be in the media process at all, or should it be an optional
   companion service?

A feature that requires edits across unrelated OpenIPC layers should trigger an
architecture review before implementation.

## Desired end state

For an operator:

> Install/boot OpenIPC, use the normal WebUI, and see the capabilities that this camera
> actually has.

For a developer:

> OpenIPC remains recognisable OpenIPC. Machino is a bounded component with explicit
> APIs and adapters, not a web of private patches spread across the firmware.
