# T40NN Research & Recovery

Machino's first hardware target was not developed in isolation from the camera below
it. Bringing the tested T40NN board from stock firmware to a usable OpenIPC system
required bootloader analysis, full-flash backup, recovery planning, firmware analysis
and hardware experiments.

The detailed procedures and executable tooling live in
[`Miguel0888/ipcam-lan-discovery`](https://github.com/Miguel0888/ipcam-lan-discovery).
This page explains how that evidence relates to Machino and which sources matter for
future platform work.

## Why this belongs in the Machino documentation

A media runtime cannot compensate for every broken platform assumption. Before a
T40NN-specific runtime finding can be trusted, it is often necessary to answer:

- Which SoC/board is actually present?
- Which bootloader and device tree established the current hardware state?
- What did the stock firmware initialise that OpenIPC does not?
- Can the camera be recovered if a register or flash experiment removes networking?
- Is the failure in Machino, OpenIPC userspace, Linux, U-Boot or board-specific init?

The research repository provides the forensic layer for those questions.

## Evidence chain

```text
Stock camera
    |
    +-- identify SoC / board / sensor / flash
    +-- obtain full flash backup
    +-- analyse boot + firmware behaviour
    +-- establish UART/U-Boot recovery route
    |
    v
Controlled OpenIPC migration
    |
    +-- generator-derived write plan
    +-- fail-closed validation
    +-- readback / CRC / bootloader detection
    |
    v
OpenIPC hardware comparison
    |
    +-- stock vs OpenIPC register/boot state
    +-- bounded runtime experiments
    +-- cold vs warm boot evidence
    |
    v
Curated Machino/OpenIPC findings
    |
    +-- runtime workaround where necessary now
    `-- upstream platform fix where responsibility belongs
```

## Full-flash backup is the prerequisite, not an afterthought

The Cam Tool documents a complete **16 MiB SPI-NOR** backup of the investigated stock
camera. The backup flows are intentionally read-only: no `saveenv`, no `sf write`.
The fast path boots the existing stock kernel with `init=/bin/sh`, mounts the minimum
runtime filesystems, reads each MTD partition and transfers it over the network while
verifying partition size and device-side hashes.

The resulting image includes the stock bootloader, factory data, configuration,
kernel, rootfs and application filesystem. The stock kernel region also has a verified
uImage data CRC.

Source:
[`docs/flash-backup.md`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/docs/flash-backup.md)

For Machino development this means: a risky platform experiment should begin from a
known recoverable state, especially when it touches DDR, PHY, clocks, boot state or
flash layout.

## OpenIPC installation is generator-driven

The Cam Tool does not treat one camera's hard-coded flash offsets as universal truth.
Its OpenIPC flow takes the generated OpenIPC installation commands as input, parses the
actual operations and blocks unknown or contradictory plans rather than guessing.

Important design properties include:

- the first persistent write is locked behind an explicit safety gate;
- a complete stock backup is required before replacing the boot area;
- erase/write ranges and filenames come from the OpenIPC generator;
- the tool re-detects the actual bootloader after reboot;
- unknown commands are blocking, not silently reinterpreted;
- readback/verification is part of the write path.

Source:
[`docs/openipc-flashen.md`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/docs/openipc-flashen.md)

That fail-closed philosophy should also guide Machino's platform-enablement code.

## Destructive-path audit

The Stage-A audit is especially useful as a model for future automatic platform
changes. It reviewed the complete path from user confirmation to `reset` because an
incorrect full-image write could remove the only boot path.

The audit verifies, among other things:

- exact U-Boot prompt/signature handling;
- that command echo cannot spoof bootloader detection;
- exact byte count and offset in `sf erase/write/read` confirmation;
- a clean verification buffer before readback;
- CRC of the RAM source before the first erase;
- watchdog state immediately before persistent writes;
- full-flash readback CRC before reset.

Source:
[`docs/stufe-a-audit.md`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/docs/stufe-a-audit.md)

The lesson for Machino is broader than flashing: **automation around hardware should
prove preconditions and postconditions instead of assuming that a command returning
`OK` means the intended state exists.**

## Hardware preflight and fail-closed planning

The Advanced hardware audit compares the planner against the **actual measured U-Boot
environment**. It found concrete cases where generated expectations did not match the
running loader, including missing macro names and overlapping partition ranges. The
planner correctly rejected those plans rather than synthesising a plausible command.

Source:
[`docs/advanced-hardware-audit.md`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/docs/advanced-hardware-audit.md)

This same principle applies to OpenIPC integration:

> Unknown board state is not a reason to choose a default register value.

## Firmware and stock-behaviour analysis

The research repository also keeps the lower-level evidence used to understand what
the original firmware did before OpenIPC was installed:

- [`docs/firmware-analyse.md`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/docs/firmware-analyse.md)
  — stock firmware structure and extracted behaviour;
- [`docs/kamera-verhalten.md`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/docs/kamera-verhalten.md)
  — observed camera behaviour used for comparison;
- [`docs/nna-boot-environment.md`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/docs/nna-boot-environment.md)
  — NNA-related boot/environment evidence;
- [`docs/root-zugang.md`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/docs/root-zugang.md)
  and `telnet-analyse.md` — access/recovery research for the stock system;
- `docs/camera/` and `docs/screenshots/` — board/camera-specific visual and raw
  evidence.

These documents should be linked as evidence, not copied wholesale into the Machino
Wiki. Findings become Machino pages when they affect the supported runtime,
architecture or an upstream OpenIPC fix.

## Register experiments: recovery changes the engineering threshold

The T40NN work contains both successful hardware fixes and evidence of how quickly a
register experiment can remove a working system.

Two examples show why recovery matters:

### Ethernet/PHY

Incorrect PHY state can leave the link apparently negotiated while carrying no frames.
A network-only recovery path is therefore insufficient for Ethernet experiments.

See [T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md).

### DDR-PHY

A measured runtime change of the board-dependent `0x13012030` DDR-PHY bit caused CPU0
to hardlock in the idle path and the watchdog to reset the camera. This is not a normal
runtime-control failure mode.

See [OpenIPC Patch & Upstream Catalog](OpenIPC-Patch-Catalog.md).

For such work, UART/U-Boot access and a verified flash backup are part of the test
setup, not optional emergency tools.

## What should be upstreamed?

A useful decision rule is:

| Finding | Likely permanent owner |
| --- | --- |
| board/device-tree register address or pin/clock declaration | OpenIPC board support / device tree |
| PHY reset/fixup required for a known PHY | Linux/OpenIPC PHY/platform init |
| U-Boot-established SoC/DDR state missing before Linux | U-Boot / board init / OpenIPC platform support |
| media SDK ABI handling | Machino platform adapter |
| stream/control/protocol behaviour | Machino media/runtime core |
| generic OpenIPC WebUI bug | OpenIPC WebUI upstream |
| temporary compatibility shim for an older image | bounded Machino/Cam Tool integration patch, then retire |

The repository that discovered a problem does not automatically own the permanent
fix.

## Research status labels

Machino documentation should distinguish at least:

- **verified** — reproduced and validated on the relevant hardware;
- **verified combination** — the complete configuration works, but individual
  contributing bits were not isolated;
- **provisional** — evidence is strong enough for a scoped experiment/workaround but
  a decisive validation step is still open;
- **hypothesis** — explanatory model, not yet suitable for automatic rollout.

This matters especially for register-level work. A detailed comment is not equivalent
to hardware validation.
