# USB & Modem Connectivity

USB host and cellular connectivity are examples of capabilities that go beyond the
traditional scope of a camera media streamer. They are nevertheless important for a
self-contained OpenIPC camera and are therefore part of Machino's **extension model**.

The architectural rule is:

> **Connectivity may be packaged and coordinated by Machino without being coupled to
> the media pipeline.**

A camera that has no USB host controller or modem should not carry modem-specific
runtime complexity through the core.

## Why this belongs in the Machino ecosystem

Majestic's responsibility is primarily media. It does not provide a general model for
turning a camera's USB host port into a modular connectivity platform.

Machino already needs a capability/control boundary for media, AI and platform
features. The same boundary can expose optional connectivity in a coherent way:

```text
OpenIPC WebUI / system
        |
        | capability + state + commands
        v
Machino integration layer
        |
        +-- media runtime
        +-- USB host capability
        +-- modem service
        +-- Wi-Fi adapter service
        +-- VPN service
        +-- future USB/network modules
```

These branches do not need to execute in one process. "Machino extension" describes
ownership/integration, not a requirement to turn every utility into the media daemon.

## Current EC200A-EU reference hardware

The current modem reference is the **Quectel EC200A-EU** (`VID 2C7C`, `PID 6005`).
The separate reference repository is:

[github.com/Miguel0888/quectel-ec200a-eu](https://github.com/Miguel0888/quectel-ec200a-eu)

That repository owns vendor documents, driver research and host-side experiments.
Machino should consume the verified conclusions relevant to the embedded camera rather
than duplicating the vendor archive.

Known USB functions for the tested module include serial AT/diagnostic/modem functions
plus a USB-network function whose mode can be configured by AT command.

## Linux USB networking modes

The EC200A family uses usbnet-style networking rather than a QMI-only design. The
reference research documents these modes:

| Mode | `AT+QCFG="usbnet",x` | Typical Linux driver |
| --- | ---: | --- |
| ECM | `1` | `cdc_ether` |
| RNDIS | `3` | `rndis_host` |
| NCM | `5` | `cdc_ncm` |

For Linux/OpenIPC, ECM or NCM is generally the cleaner target when the kernel image has
the required modules. PPP remains a useful fallback and compatibility path.

Reference:
[`doc/linux.md`](https://github.com/Miguel0888/quectel-ec200a-eu/blob/master/doc/linux.md)

## OpenIPC T40 kernel gap

The tested OpenIPC T40 image was not built for carrying a USB LTE modem and therefore
lacked several in-tree Linux modules required for the path.

Machino has a dedicated build workflow for the missing T40 modules. The current work
covers the in-tree Linux 4.4.94 drivers around:

- `usbserial`;
- `usb_wwan`;
- `option`;
- `usbnet`;
- `cdc_ether`.

The workflow validates architecture, vermagic and required kernel symbols against the
target camera instead of assuming that any T40-built `.ko` will load.

See:
[`.github/workflows/build-modem-modules-t40.yml`](https://github.com/aresstack/machino/blob/main/.github/workflows/build-modem-modules-t40.yml)

This is a good example of **non-invasive OpenIPC extension**: reuse the existing kernel
ABI and add loadable modules rather than replacing the camera kernel solely to obtain a
modem driver.

## Device binding must be evidence-driven

The kernel's older `option` driver does not necessarily contain `2c7c:6005` in its
static table. During bring-up, Linux supports dynamic binding through the driver's
`new_id` interface.

A permanent static device-table entry should only be added after the EC200A's actual
USB interface layout is known, because the interface reservation mask determines which
functions must *not* be claimed as serial ports.

This is the same design rule used for T40NN platform work: do not replace an unknown
with a guessed constant merely because the guess boots once.

## PPP fallback

Machino also has a T40 cross-build for `pppd` and `chat`:

[`.github/workflows/build-ppp-t40.yml`](https://github.com/aresstack/machino/blob/main/.github/workflows/build-ppp-t40.yml)

PPP is intentionally an optional payload, not an OpenIPC patch. It exists because
OpenIPC images vary and a UI option must not silently depend on a binary that happens
to exist on one image but not another.

The build is intentionally small: modem-relevant `pppd` + `chat`, without unrelated
plugins and diagnostic utilities.

## Capability model

A future stable API should report layers separately rather than one ambiguous
`modem=true` flag. For example:

```json
{
  "usb": {
    "host": true
  },
  "connectivity": {
    "cellular": {
      "present": true,
      "vendor": "Quectel",
      "model": "EC200A-EU",
      "at": true,
      "ecm": true,
      "ppp": true
    }
  }
}
```

Runtime state can then report enumeration, SIM/network registration, active data path,
IP address and signal telemetry independently.

The WebUI does not need EC200A-specific implementation logic. It needs a generic
cellular-connectivity component backed by capabilities.

## Service ownership

Recommended separation:

```text
Machino media process
    owns: ISP / encoder / streams / media APIs

Modem connectivity service
    owns: USB modem discovery / AT / link establishment

OpenIPC network layer
    owns: routes / DNS / firewall / system network policy

WebUI
    owns: presentation and user commands through stable APIs
```

The exact process split can evolve, but responsibility should remain explicit.

## USB as an extension bus

The modem is not the only reason to treat USB as a first-class capability. The same
architecture can support optional Wi-Fi adapters or future peripherals without adding
per-device logic to the media core.

A useful model is:

```text
USB host
  |
  +-- device discovery
  +-- driver/module availability
  +-- capability adapter
       |
       +-- cellular modem
       +-- Wi-Fi
       +-- future peripheral
```

## Power and reliability

Cellular modems can have substantially different power/current behaviour from the
camera SoC. USB enumeration success does not prove that the supply remains stable under
RF transmit load.

Hardware validation should distinguish:

- USB host/VBUS enablement;
- enumeration;
- driver binding;
- AT communication;
- SIM registration;
- data-session establishment;
- sustained traffic under realistic power load.

A powered hub may be useful during bring-up, but a final camera integration needs an
explicit power design rather than treating the hub as part of the software solution.

## Documentation ownership

- Machino Wiki: architecture, supported flow, OpenIPC integration and validated camera
  behaviour.
- `quectel-ec200a-eu`: Quectel-specific research, vendor documentation and desktop/Linux
  reference material.
- Machino CI/workflows: reproducible target-camera payload builds and ABI gates.

See [Documentation Sources](Documentation-Sources.md).
