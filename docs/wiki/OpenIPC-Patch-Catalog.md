# OpenIPC Patch & Upstream Catalog

The Cam Tool contains executable patches discovered while making the tested OpenIPC
camera usable. They are valuable for Machino, but they are **not all Machino features**
and they do not all belong permanently in the same layer.

This page classifies the current built-ins by purpose, maturity and likely long-term
owner.

Executable source of truth:
[`Miguel0888/ipcam-lan-discovery/tool/src/core/known_patches.cpp`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/tool/src/core/known_patches.cpp)

## Why document patches here?

A patch can prove three different things:

1. an **OpenIPC/platform defect** that should eventually be fixed upstream;
2. a **compatibility gap** needed while Machino replaces Majestic;
3. a deliberately small **integration hook** that makes Machino feel native without
   deeply modifying OpenIPC or its WebUI.

Keeping those categories separate prevents a useful workaround from silently becoming
permanent architecture.

## Current catalog

| Patch | Status | Purpose | Preferred long-term owner |
| --- | --- | --- | --- |
| `t40nn-yt8512-rmii-refclk` v5 | hardware-verified | Restore T40-family + YT8512B Ethernet state lost/missed by OpenIPC; handle warm-reboot autoneg and late DHCP | OpenIPC board/kernel/PHY setup; runtime workaround until then |
| `t40nn-ddrphy-idle-wakeup` v3 | **provisional** | Test T40NN DDR-PHY idle/wakeup hypothesis while avoiding the dangerous board-dependent bit | OpenIPC T40NN boot/platform init if hypothesis is confirmed |
| `openipc-network-cgi-pipe` | upstream backport/compatibility | Backport the argv-based network save path for old WebUI versions so shell metacharacters in Wi-Fi passphrases are preserved | OpenIPC WebUI; remove locally once minimum supported OpenIPC includes upstream fix |
| `openipc-video-check-off` | compatibility workaround | Disable a browser-side false-positive camera-health banner on flaky links when exposure telemetry is unavailable | Prefer capability-aware OpenIPC/Machino health semantics; patch remains narrow fallback |
| `machino-footer-brand` | optional integration polish | Add a second Machino credit line without removing OpenIPC attribution | Small optional WebUI integration hook |
| generated `machino-<platform>` patch | deployment integration | Install/update/uninstall the Machino media stack and optional payloads as a bounded component | Cam Tool deployment adapter + Machino manager |

## 1. T40NN / YT8512B Ethernet

See [T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md) for the full curated
finding.

The important architectural point is that the patch does not merely "force 100 Mbit".
It reconstructs a hardware state that U-Boot establishes and the tested OpenIPC path
does not preserve correctly, verifies readback, handles asynchronous PHY reset, and
only then lets the normal OpenIPC network service own address configuration.

Verified combination on the tested board:

- `MACCDR 0x10000054`: set bit 29;
- `MACPHY 0x100000e8`: `bits 2:0 = 4`;
- YT8512B extended `0x4000 = 0x12` **absolute**;
- YT8512B extended `0x50 = 0x67`;
- one BMCR `0x1200` autoneg restart only when the warm-reboot state requires it.

This is strong upstream material because the evidence includes the working bootloader
state, the broken Linux state, exact register deltas, ordering and measured network
validation.

## 2. T40NN DDR-PHY idle/wakeup hardlock — provisional

This is intentionally documented more cautiously than the Ethernet fix.

### Symptom

The camera can sporadically hard-freeze: no ping/SSH, followed by watchdog reset.
Forensic reset-PC analysis placed CPU0 in `ingenic_wait_irqoff` / MIPS WAIT while CPU1
was still executing userspace, which supports an idle-wakeup failure rather than a
normal clean reboot.

### Stock/OpenIPC differences under investigation

The stock firmware performs DDR-PHY userspace initialisation that OpenIPC does not.
Observed differences include:

| Register | Stock/reference observation | OpenIPC observation | Treatment |
| --- | --- | --- | --- |
| `0x1301206c` | bit 0 cleared | bit 0 set | provisional patch clears bit 0 with RMW |
| `0x13012020` | low nibble `0xf` | low nibble `0x0` | provisional patch sets low nibble with RMW |
| `0x13012030` | **board dependent** | may differ | **never changed by current patch** |
| `0x13012038` | same in observed comparison | same | untouched |

Two T40NN boards showed the same deltas for the first two fields, while
`0x13012030 bit 0` differed by board/DDR setup. That is why the current patch only
performs read-modify-write on the common fields.

### Critical runtime hazard

`0x13012030` must **not** be treated as a runtime tuning bit.

A measured runtime transition of its bit 0 (`1 -> 0`) on 2026-09-29 caused CPU0 to hang
in `ingenic_wait_irqoff`, followed by a watchdog reset — the same class of signature
being investigated.

Therefore:

- do not copy an absolute DDR-PHY register set from one board to another;
- do not expose this register as a WebUI/runtime control;
- do not flip `0x13012030` after boot;
- keep recovery/UART available for experiments;
- upstream only after the cold-boot hypothesis is properly confirmed.

The Cam Tool currently labels this patch **PROVISIONAL** and records that the decisive
cold-boot counter-test is still outstanding. The wiki must preserve that uncertainty.

### Likely architectural destination

If confirmed, this is not primarily a Machino media-runtime concern. The durable fix
most likely belongs in the T40NN-specific OpenIPC/U-Boot/device-tree/platform-init
path so the hardware reaches Linux in a correct, board-aware state.

Machino benefits from the fix but should not become the permanent owner of DDR
initialisation.

## 3. Old OpenIPC `network.cgi` Wi-Fi passphrase bug

Older OpenIPC WebUI code built a shell command string and executed it through `eval`.
A Wi-Fi passphrase containing shell metacharacters such as `|`, `;` or spaces could
therefore be parsed/truncated instead of being passed as one argument.

The Cam Tool patch backports OpenIPC's argv-based fix, keeps a backup and only applies
to the recognised old implementation shape.

This is a **backport**, not a new Machino feature. Once the supported OpenIPC baseline
contains the upstream correction, the local patch should disappear.

## 4. Video health false-positive compatibility patch

The current OpenIPC WebUI can display a persistent "camera is not seeing anything"
finding based partly on sampled black frames. On a flaky uplink such as LTE, a stalled
browser player can cause black samples even though the camera and stream are healthy.
The issue is more visible when the media daemon does not provide the exposure signal
expected by the current diagnostic.

The existing compatibility patch disables only the exported diagnosis result while
leaving the rest of the JavaScript module intact.

This demonstrates the current **minimal-intrusion** policy, but it is not the ideal
final interface. A better long-term integration is capability-aware health reporting:
OpenIPC should know which telemetry Machino provides and only run diagnostics whose
inputs actually exist.

## 5. Machino footer integration

The optional footer patch adds `Machino by AresStack` beneath the existing OpenIPC
credit in the central footer include. It deliberately **does not remove or replace**
OpenIPC attribution.

This is an example of the desired WebUI integration style:

- one bounded insertion point;
- additive rather than replacing a page;
- gated on the known upstream structure;
- idempotent;
- backed up and uninstallable;
- no Machino-specific fork of the complete WebUI.

Branding itself is optional; the architectural value is the integration pattern.

## 6. Machino deployment patch

Machino itself is exposed through the Cam Tool patch registry because on the tested
T40NN camera it repairs the larger functional gap: OpenIPC's Majestic path does not
provide a working video path with the tested NN `libimp` ABI.

Unlike the small built-ins, the Machino payload is not compiled into the Cam Tool. A
release bundle and manifest are shipped as assets, verified by SHA-256 before the patch
is offered, transferred through TFTP, and installed through `machino-manager`.

The manager records ownership and whether Majestic was active before installation so
uninstall can restore the previous service state.

Optional payloads (for example Wi-Fi, cellular, NNA or VPN-related components) are
selected explicitly rather than making every Machino installation carry every
extension.

This is an important precedent for the extension architecture: **the Cam Tool can
install a bounded capability package without turning OpenIPC itself into a private
fork.**

## Patch lifecycle

Every built-in should move through a lifecycle rather than live forever by accident:

```text
Observation
   -> scoped experiment
   -> executable gated patch
   -> hardware validation
   -> curated documentation
   -> choose permanent owner
        |-- Machino capability
        |-- OpenIPC/WebUI
        |-- kernel/driver
        |-- U-Boot/device tree
        `-- retire after upstream release
```

## Safety rules for platform patches

Register-level patches should normally provide:

- exact hardware/SoC/PHY detection where possible;
- bounded waits and retries;
- readback or observable post-verification;
- a documented cold-boot and warm-reboot result;
- non-persistent experimentation before automatic rollout;
- explicit warning where a register/value is board dependent;
- recovery access for changes that can remove network or bootability;
- a statement of whether the finding is verified, provisional or merely a hypothesis.

A patch that can reboot or hardlock the camera should never be presented as an ordinary
runtime setting.

## Relationship to the OpenIPC integration strategy

Machino's goal is not to make these patches invisible in the engineering sense. Their
ownership and evidence should be explicit.

The goal is that the **operator experience remains coherent**: OpenIPC plus Machino
behaves like one camera system while the implementation stays modular enough that a
proper upstream fix can replace a workaround without rewriting the UI or media core.
