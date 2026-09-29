# T40NN OpenIPC Enablement

This page collects verified platform knowledge discovered while bringing an Ingenic
**T40NN** camera up on OpenIPC. It is deliberately separate from Machino's generic
media architecture: these are board/SoC/PHY enablement findings that may ultimately
belong upstream in OpenIPC, U-Boot or Linux.

The detailed experiment history remains in
[`Miguel0888/ipcam-lan-discovery`](https://github.com/Miguel0888/ipcam-lan-discovery).
This page records the stable findings and their architectural implications.

## Why this work matters to Machino

The first Machino hardware work started because the normal Majestic/OpenIPC media path
did not provide a usable solution for the tested T40NN camera. Validating the media
runtime therefore also required validating the platform below it.

That produced reusable information about:

- correct SoC identity;
- OpenIPC boot and device-tree state;
- Ethernet MAC/PHY setup;
- values established by U-Boot but lost/reset by Linux;
- cold-boot versus warm-reboot behaviour;
- recovery-safe runtime workarounds;
- likely upstream fixes.

This is exactly the kind of knowledge Machino should preserve even when the permanent
fix eventually moves out of Machino.

## Hardware scope

The investigated camera was sold as T40N but later identified from the Ingenic CPM
chip registers as **T40NN**.

The Ethernet issue is not inherently an "NN" feature. The workaround is gated on the
external **Motorcomm YT8512B** PHY (`phy_id = 0x00000128`) and may apply to another
T40-family board using the same wiring/PHY setup.

Do not apply register values to an unrelated board merely because it has a T40-family
SoC.

## Verified Ethernet failure

Under the tested OpenIPC T40 image the interface could repeatedly negotiate poorly or
fail to carry traffic. The working U-Boot state and the Linux state exposed multiple
missing/different settings.

The verified v5 workaround in the Cam Tool currently establishes this combination:

| Component | Address/register | Required state in tested board |
| --- | --- | --- |
| Ingenic CPM | `MACCDR` `0x10000054` | bit 29 set (`0x20000000`) — MAC/PHY clock enable |
| Ingenic CPM | `MACPHY` `0x100000e8` | bits `2:0 = 4` — RMII mode |
| YT8512B extended PHY | `0x4000` | **absolute** value `0x12` |
| YT8512B extended PHY | `0x50` | `0x67` — includes PLL/refclk-select bit `0x40` |

The combination was validated at **100/Full**, with bidirectional ping and a measured
291 MB transfer at about 93 Mbit/s. The individual contribution of every bit was not
isolated, so the evidence supports the combination rather than claiming that every
setting is independently necessary.

Source investigation:
[`docs/t40n-ethernet-analysis.md`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/docs/t40n-ethernet-analysis.md)

Current built-in patch implementation:
[`tool/src/core/known_patches.cpp`](https://github.com/Miguel0888/ipcam-lan-discovery/blob/main/tool/src/core/known_patches.cpp)

## Important: `EXT 0x4000` must not be treated as a simple OR-mask

After the kernel PHY reset, the observed register state can make a naïve
read-modify-write incorrect. During investigation, OR'ing the desired bit into the
existing value produced `0x1010`: link could appear up while frames did not pass.

The tested workaround writes `0x12` **absolutely** for the scoped YT8512B hardware.

This is a good example of why the wiki records before/after state instead of reducing a
hardware fix to "set bit X".

## Device-tree discrepancy

The investigation found that the T40 kernel/device-tree path used a mode register at
`0xb00000e4`, while the T40 CPM definitions identify `MACPHY` at offset `0xe8`.
U-Boot's network initialisation writes the RMII mode to `0xb00000e8`.

Changing only the MACPHY RMII bits was **not sufficient** to restore the link, but the
discrepancy remains a concrete upstream candidate.

A clean OpenIPC-side fix should therefore be evaluated as a combination of:

- correct `ingenic,mode-reg` / MACPHY setup;
- correct MAC/PHY clock enable;
- YT8512B-specific PHY initialisation/fixup for the board;
- correct reset/autoneg ordering.

## Linux PHY reset and asynchronous state loss

A further failure mode appeared after the initial workaround: Linux can reset the PHY
asynchronously around interface bring-up and overwrite a value that a startup script
has already set. `EXT 0x50` was observed reading back as `0x27` after having been
written as `0x67`.

The current Cam Tool patch therefore uses a **bounded write/readback loop**, not an
unbounded boot-time busy wait. It retries only a limited number of times and fails
explicitly when the expected state does not stick.

This behaviour matters for any eventual upstream implementation: ordering is part of
the bug, not just the final register values.

## Warm reboot is different from cold boot

A cold boot and a warm reboot do not necessarily reach the patch with the same Ethernet
negotiation state.

On a warm reboot, the link was observed already negotiated at **10/Half** while the
reference-clock setup was still wrong. Correcting the registers afterwards did not
retroactively reopen completed autonegotiation.

The current v5 workaround therefore checks the resulting speed and, when needed,
performs **one** autonegotiation restart using BMCR `0x1200`. In the measured case this
moved the link immediately from 10/Half to 100/Full.

This should be treated as a boot-order/state-machine issue, not as a rule that every
10-Mbit link is defective.

## Network service ordering

The repaired Ethernet link can become usable only after OpenIPC's initial network
startup has already given up on DHCP. The workaround therefore also checks whether
`eth0` acquired IPv4 and may restart the existing OpenIPC network service once.

The network service remains responsible for the actual address policy; the hardware
patch does not invent a second network configuration mechanism.

That is consistent with Machino's integration rule: **repair the missing hardware
state, then hand control back to the system that owns networking.**

## Recovery and reboot safety

Register-level platform work must assume that a wrong value can make the camera lose
network connectivity or become unstable.

Before experimenting with new values:

- keep UART access available;
- preserve a known-good flash backup/recovery route;
- make experimental writes non-persistent first;
- use hardware/PHY detection gates;
- verify readback;
- bound polling/retry loops;
- distinguish a recoverable network outage from a boot/hardlock condition;
- record cold-boot and warm-reboot results separately.

The related recovery and flashing evidence is maintained in the Cam Tool repository;
see [Documentation Sources](Documentation-Sources.md).

## Runtime workaround versus upstream fix

The current init-script patch is valuable because it makes the hardware usable without
waiting for a firmware/kernel release. It should not automatically become the final
architecture.

Current direction:

```text
verified hardware evidence
          |
          +--> bounded OpenIPC runtime workaround (usable now)
          |
          +--> document exact cause / ordering
          |
          +--> propose clean board/kernel/PHY fix upstream
```

Once OpenIPC correctly establishes the required state itself, Machino should detect
that the workaround is unnecessary rather than fighting the upstream implementation.

## Other Cam Tool patches

`ipcam-lan-discovery` also contains OpenIPC compatibility/repair patches that are not
all T40NN-specific, for example a backport for older `network.cgi` versions that mishandle
Wi-Fi passwords containing shell metacharacters. These are useful evidence for the
same modular patch model, but they should not be conflated with Machino media-core
features.

The long-term documentation rule is:

- hardware/media knowledge relevant to Machino/OpenIPC integration gets a curated wiki
  page here;
- the executable patch and detailed experiment history remain in the repository that
  owns them;
- fixes that properly belong upstream should be tracked as upstream candidates rather
  than permanently duplicated.
