# T40NN Enhanced Stack — plan (AP8)

Goal: design a portable "Enhanced T40 Stack" that keeps OpenIPC as the system
substrate (boot, network, SSH, WebUI, overlay, updates) while giving T40NN a
consistent, newer media / crypto / NNA substrate underneath Machino.

**Planning only.** Nothing in this document was built, swapped or deployed. No
`.ko` was compiled, no firmware replaced, no DTS touched, no clock changed, no
reboot, no hardware test. Camera facts were read over SSH read-only.

Builds on [AP6](t40nn-stock-vs-openipc-media-stack.md). AP7 (the 1.2 vs 1.3.1
API diff) had not been executed when this was written, so the SDK-version facts
below were gathered here directly from the build system and the running binary
and are marked as such; AP7 can deepen them but does not change them.

---

## 0. The single most important finding: half the Enhanced Stack is already live

The premise "OpenIPC runs 1.2.0, an Enhanced Stack would move us to 1.3.1" is
**already false for userspace**.

| Fact | Evidence |
|---|---|
| Machino links the vendor IMP libraries **statically**, from SDK **1.3.1** | `build.sh: T40) echo "ingenic-lib/T40/lib/1.3.1/uclibc/7.2.0"`; CI pins `SDK_VERSION: "1.3.1"` and *fails the build* unless `3rdparty/install/.soc` records `lib/1.3.1/` |
| It compiles against 1.3.1 headers | `IMP_INC = include/T40/1.3.1/en` |
| The deployed binary says so | `/usr/bin/machino` (2569980 B) banner: `OpenIPC 4.4.94 tx-isp, libimp 1.3.1` |
| It does **not** use the system library | no `libimp.so` dependency; `/usr/lib/libimp.so` (1.2.0) is what *majestic* used |
| The driver underneath is the **old** one | `tx-isp-t40.ko` tag `H20220606a` (AP6) |

So the camera has been running **libimp 1.3.1 userspace on top of an OpenIPC
H20220606a tx-isp driver and the stock-identical IQ tuning file** since
2026-09-18 — and that combination is hardware-accepted, including the WebRTC
~100 ms result.

Two consequences for this plan:

1. The question "may libimp 1.3.1 be mixed with an older tx-isp?" is **not
   hypothetical and not open**. It is the production configuration and it
   works. Phase 2 is therefore far less urgent than it looked.
2. The real ABI risk in this project is **header/library skew, not
   library/driver skew**. The Makefile already records a case that bit:
   the 1.2.0 header lacks the trailing `isVI` member of `IMPEncoderStream`
   that 1.3.1 added, so building 1.3.1 libs against 1.2.0 headers makes
   `IMP_Encoder_GetStream` write one word past the caller's struct **on every
   frame**. `video_thread()` carries a compile-time tripwire against it.

## 1. Evidence base (all newly measured for AP8)

| Fact | Value | Why it matters |
|---|---|---|
| Kernel | 4.4.94, `buildroot-gcc-13.3.0`, built 2026-09-17 | any in-tree module must be built from this exact tree |
| vermagic (all modules) | `4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT` | **no `modversions` token** |
| `grep -c __crc_ /proc/kallsyms` | **0** | `CONFIG_MODVERSIONS` is **off**: module ABI is guarded by the vermagic *string alone*, with no per-symbol CRCs |
| Vendor libs | `ingenic-lib/T40/lib/1.3.1/uclibc/7.2.0` | uClibc/GCC-7.2.0 blobs, bridged into the musl toolchain by `libmuslshim.a` |
| Overlay free space | **6.0 MB** of 8.7 MB (`/dev/mtdblock4`) | the binding constraint on any dual-stack / rollback design |
| `/tmp` | 20.9 MB tmpfs | usable for staging, lost on reboot |
| Hardware crypto | **none exposed**: no `/dev/crypto`, no `hw_random`, no AES/HASH/DTRNG window in `/proc/iomem`, `/proc/crypto` is software-only (`drbg_*`, `ecb-cipher_null`) | the crypto phase starts from zero |
| `dtrng_dev.ko` | present at `/lib/modules/4.4.94/ingenic/dtrng_dev.ko`, **not loaded**, `depends=`, "Ingenic DTRNG hw support", author Elvis Wang | the only vendor crypto driver already shipped |
| ISP "firmware" | there is **no** separate `request_firmware` blob and **no** `libt40-firmware.a` anywhere in this project; `tx-isp-t40.ko` loads `/etc/sensor/%s-t40.bin` | what the assignment calls ISP firmware is, on T40, the IQ tuning file — and AP6 proved it is byte-identical to stock |
| NNA models | **no** `.nna` / `.aip` / model file found in the stock dump | the model format is unknown; stock cannot serve as a model reference |

`CONFIG_MODVERSIONS=n` is the pivotal build fact. It makes out-of-tree modules
**easier** to produce (no CRC matching) and **more dangerous** to get wrong
(nothing catches a silent struct-layout change at load time — it loads and then
corrupts memory).

---

## 2. Component matrix

Fields per the assignment. "OpenIPC unchanged" means the component needs no
work for the Enhanced Stack.

### 2.1 `tx-isp`

| | |
|---|---|
| current OpenIPC component | `tx-isp-t40.ko`, 988760 B, tag `H20220606a` |
| desired source/version | `H20221114a`-class (stock vintage) or the thingino T40 equivalent |
| source repository/path | no public source; **prebuilt blob only** (stock dump, or thingino's ingenic-lib/kernel-module drops) |
| build dependency | none (binary) — cannot be rebuilt, only substituted |
| kernel ABI dependency | vermagic string must match `4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT`; the stock blob **already carries exactly this string** (AP6) |
| userspace ABI dependency | pairs with `libimp`; currently *mismatched by ~1 year and working* |
| firmware dependency | `/etc/sensor/<sensor>-t40.bin` |
| device-tree dependency | none observed; configured via insmod parameters |
| required config options | none (module is prebuilt) |
| runtime device nodes | `/dev/tx-isp*` created by the module |
| replacement strategy | drop-in file swap + `loadko` order change; **not worth doing on its own** |
| rollback strategy | keep the current `.ko`, revert file, power-cycle |
| hardware test required | **yes, supervised** — this is the single highest-risk swap |

**Verdict: do not touch.** The insmod parameters are already character-identical
to stock (AP6) and the IQ file is byte-identical. There is no identified defect
this swap would fix. Risk is high, expected benefit is unquantified.

### 2.2 IMX307 sensor driver

| | |
|---|---|
| current | `sensor_imx307_t40.ko`, 19576 B, `H20220531a` |
| desired | `H20221205a` (stock) — or unchanged |
| source | prebuilt blob only |
| build dep | none |
| kernel ABI | vermagic match; stock blob matches |
| userspace ABI | none directly; talks to tx-isp |
| firmware dep | none (register tables are compiled in) |
| DT dep | none; GPIO wiring via insmod params (`rst/pwdn_gpio=91/0`, see M1) |
| config | none |
| device nodes | none of its own |
| replacement | file swap; **must be swapped together with tx-isp** (they are a vendor drop pair) |
| rollback | keep old `.ko` |
| hardware test | yes, supervised |

**Verdict: do not touch alone.** Both sides already contain the identical three
register sets including 60 fps (AP6), so a swap buys nothing currently known.

### 2.3 ISP firmware / `libt40-firmware.a`

| | |
|---|---|
| current | `/etc/sensor/imx307-t40.bin`, md5 `7295ef76…` |
| desired | unchanged — it is **already byte-identical to stock** (AP6) |
| source | stock dump (for the missing variants) |
| everything else | n/a |

**Verdict: nothing to do.** The assignment's `libt40-firmware.a` does not exist
on this platform. The only real gap is the *missing variants*
`imx307-wdr-t40.bin` and `imx307-cust-t40.bin`, which are only useful once
`IMP_ISP_WDR_OPEN` is reachable (see 2.6).

### 2.4 AVPU / VPU

| | |
|---|---|
| current | `avpu.ko` 30688 B, `H20220825a`, loaded with `clk_name=mpll avpu_clk=550000000` |
| desired | unchanged |
| kernel ABI | vermagic match |
| userspace ABI | driven entirely through `libimp` |
| replacement | file swap, pairs with tx-isp |
| hardware test | yes, supervised |

**Verdict: do not touch.** Parameters are already identical to stock, and the
encoder is not the current bottleneck.

### 2.5 `libimp`

| | |
|---|---|
| current | **1.3.1**, statically linked into `/usr/bin/machino` (system `/usr/lib/libimp.so` is 1.2.0 and unused by Machino) |
| desired | 1.3.1 — **already achieved** |
| source | `gtxaspec/ingenic-lib`, `T40/lib/1.3.1/uclibc/7.2.0` |
| build dep | thingino musl toolchain + `libmuslshim.a`; headers **must** be `include/T40/1.3.1/en` |
| kernel ABI | indirect, via tx-isp ioctls — empirically tolerant across the ~1 year gap |
| userspace ABI | **header version must equal library version** (the `isVI`/`IMPEncoderStream` trap) |
| firmware dep | none |
| DT dep | none |
| config | CI gate: `.soc` must record `lib/1.3.1/` |
| device nodes | opens the tx-isp/avpu nodes |
| replacement | rebuild + redeploy one binary — **fully atomic, no system files touched** |
| rollback | keep the previous binary; this is the cheapest rollback in the whole stack |
| hardware test | normal deploy acceptance |

**Verdict: done.** This is why Machino can use the encoder APIs at all.

### 2.6 What is actually missing from the picture

AP6 found 15 symbols the OpenIPC `libimp.so` 1.2.0 lacks. **Machino does not use
that library**, so the relevant question is what *1.3.1* lacks. ROI
(`IMP_Encoder_SetChnRoiAttr`) and `IMP_ISP_WDR_OPEN` were found missing in the
**OpenIPC 1.2.0 .so**; whether they exist in the statically linked 1.3.1
archive was **not** verified and is an explicit AP7 item, not a conclusion here.

### 2.7 `libsysutils`

| | |
|---|---|
| current | 1.3.1 static in the binary; system `.so` is 1.2.0 (31460 B), unused by Machino |
| desired | unchanged |
| replacement | travels with `libimp` in the same static link |

**Verdict: done, no separate work.**

### 2.8 `libaudioProcess`

| | |
|---|---|
| current | **absent** on OpenIPC |
| desired | stock `libaudioProcess.so`, 734080 B |
| source | stock appfs dump |
| build dep | none (blob) |
| kernel ABI | none |
| userspace ABI | uClibc blob; would need the same musl shim treatment, dynamically loaded next to a statically linked binary — **untested combination** |
| firmware/DT | none |
| device nodes | none |
| replacement | add file to `/usr/lib`; costs 734 KB of the 6.0 MB overlay budget |
| rollback | delete file |
| hardware test | yes |

**Verdict: out of scope until audio is on the roadmap.** Also note
`IMP_AI_Enable/DisableHs` (howling suppression) is missing from the OpenIPC
1.2.0 `.so`; its presence in 1.3.1 is unverified.

### 2.9 IVS

| | |
|---|---|
| current | `IMP_IVS_*` via the statically linked 1.3.1 libimp; Machino's M9 motion backend already targets it |
| desired | unchanged |
| everything else | no module, no firmware, no DT |
| hardware test | **outstanding** — M9 has never had hardware acceptance |

**Verdict: no stack work needed; it needs a hardware test, not a new substrate.**

### 2.10 NNA / `soc-nna`

| | |
|---|---|
| current | `soc-nna.ko` present (`H20230310`, **newer** than stock's `version=20190724a`), **not loaded** |
| desired | loaded and usable |
| source | already on the device |
| build dep | none |
| kernel ABI | vermagic match (verified: identical string) |
| userspace ABI | needs the Venus/JZDL/AIP runtime — **not present** |
| firmware dep | a model file — **format unknown, no model found in the stock dump** |
| **DT / boot dependency** | **`nmem=8M@0x7800000` in bootargs. Absent on OpenIPC. Without it the NNA has no memory and cannot work regardless of everything else.** |
| config | bootarg change ⇒ touches the boot path |
| device nodes | created by `soc-nna` once loaded |
| replacement | n/a — this is an *addition*, not a replacement |
| rollback | do not load the module; revert bootargs |
| hardware test | yes, supervised, with UART attached |

**Verdict: blocked at the first step.** The bootarg is a boot-path change, which
is a standing No-Go. Also recall the known libimp↔NN ABI mismatch on T40NN.

### 2.11 Venus / JZDL / AIP runtime

| | |
|---|---|
| current | absent |
| desired | vendor runtime matching `soc-nna` `H20230310` |
| source | **unknown** — stock ships `libvenus.so` (2775076 B) but that is the *stock* pairing for `soc-nna 20190724a`, not for the newer OpenIPC module |
| overlay cost | 2.7 MB of a 6.0 MB budget — nearly half |
| hardware test | yes |

**Verdict: NEEDS_MORE_RESEARCH.** Pairing an old vendor runtime with a newer
kernel module is exactly the mismatch class this plan exists to avoid, and no
model file exists to test with.

### 2.12 AES @ `0x13430000`

| | |
|---|---|
| current | **nothing**: no driver, no `/proc/iomem` window, no `/proc/crypto` entry |
| desired | a kernel driver exposing AES-CM through AF_ALG, or a userspace ioctl bridge |
| source | no vendor AES driver found in the stock dump or on the device |
| build dep | would require writing a driver against the OpenIPC 4.4.94 tree |
| kernel ABI | in-tree build against the exact kernel |
| userspace ABI | AF_ALG availability **unverified** (no `/proc/config.gz` on the device) |
| DMA/alignment | unknown — undocumented register block |
| hardware test | yes, supervised |

**Verdict: NEEDS_MORE_RESEARCH, and probably not worth it.** See §5.

### 2.13 HASH @ `0x13480000`

Same as AES in every field: no driver, no window, no reference implementation
found. Would be needed for HMAC-SHA1 offload.

### 2.14 DTRNG @ `0x10072000`

| | |
|---|---|
| current | `dtrng_dev.ko` **ships but is not loaded**; no `hw_random` node |
| desired | loaded, feeding `/dev/hwrng` or the kernel entropy pool |
| source | already on the device |
| build dep | none |
| kernel ABI | vermagic verified identical |
| userspace ABI | none — it is an entropy source |
| DT dep | unknown; the module has `depends=` (nothing) and no parameters |
| device nodes | expected `/dev/hwrng` |
| replacement | `insmod` only |
| rollback | `rmmod` / do not load |
| hardware test | yes, but **low risk**: an entropy driver is not in the media path |

**Verdict: this is the one crypto item that is cheap and plausible.** It also
addresses a real, already-observed problem: the camera hangs on shutdown at
`Seeding 2048 bits` (entropy starvation), and WebRTC DTLS reads `/dev/urandom`
for every handshake. Still requires a supervised test — `insmod` of an
unexercised vendor module can wedge the box.

---

## 3. Dependency matrix — what may and may not be mixed

| Combination | Verdict | Basis |
|---|---|---|
| libimp **1.3.1** + tx-isp `H20220606a` + stock-identical IQ bin | **PROVEN COMPATIBLE** | this is production since 2026-09-18, hardware-accepted incl. WebRTC |
| libimp **1.3.1** headers + libimp **1.2.0** library (or vice versa) | **FORBIDDEN** | `IMPEncoderStream.isVI`: `IMP_Encoder_GetStream` writes past the caller's struct every frame. Compile-time tripwire exists — keep it |
| Any vendor `.ko` + this kernel | **loads if the vermagic string matches** | `CONFIG_MODVERSIONS=n`, zero `__crc_` symbols |
| …but that means | **no safety net** | a silent struct-layout change between vendor drops will load cleanly and then corrupt memory |
| tx-isp swapped **without** its sensor driver | **DO NOT** | vendor ships them as a dated pair (`H20221114a` / `H20221205a`) |
| `soc-nna` `H20230310` + stock `libvenus` (for `20190724a`) | **DO NOT** | ~3.5 year runtime/module gap, no test model |
| NNA anything **without** `nmem=` | **IMPOSSIBLE** | no reserved memory |
| WDR IQ bin **without** `IMP_ISP_WDR_OPEN` | **POINTLESS** | no way to open WDR |
| `libaudioProcess` (uClibc `.so`) alongside a statically linked musl binary | **UNKNOWN** | never attempted here |

The useful generalisation is **not** "everything must be 1.3.1". It is:

> **Userspace may run ahead of the drivers** (proven, by a year). **Headers must
> never run ahead of or behind their own library** (proven harmful). **Driver
> blobs must move as dated sets.**

## 4. Build plan

```text
OpenIPC kernel tree @ 4.4.94, buildroot-gcc-13.3.0
      ↓  (only needed for NEW modules — vendor .ko are blobs, unbuildable)
out-of-tree module build, KDIR = that exact tree
      ↓
.ko with vermagic "4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT"
      ↓
staged package under /tmp (20.9 MB tmpfs), never written to overlay first
      ↓
supervised insmod with UART attached
```

Checks, with what is already known:

| Check | Status |
|---|---|
| Kernel symbol/version deps | `CONFIG_MODVERSIONS=n`, no `__crc_` symbols — **no CRC matching required, and no protection either** |
| vermagic | must be the exact string above; every existing module (OpenIPC *and* stock) already carries it |
| Toolchain/GCC | kernel is GCC 13.3.0; vendor blobs are GCC 7.2.0/uClibc. They coexist today only because there are no CRCs. A **newly built** module must use a toolchain producing this vermagic |
| MIPS ABI/libc | o32, MIPS32r2, 32-bit; Machino userspace is musl + `libmuslshim.a` for the uClibc vendor blobs |
| Module load order | current: `gpio → audio → sensor → tx-isp → avpu → sinfo`; NNA would insert `soc-nna` and needs `nmem` reserved at boot |

For the userspace half, **no new build infrastructure is needed at all** — the
existing CI already produces a 1.3.1-linked binary and gates the library
version.

## 5. Crypto hardware — sub-plan

Target shape:

```text
DTLS/SRTP
  ├─ AES-CM     → hardware AES  @ 0x13430000
  └─ HMAC-SHA1  → hardware HASH @ 0x13480000
```

Findings:

- **Nothing exists to build on.** No driver, no iomem window, no `/proc/crypto`
  entry, no vendor reference in the stock dump. Both blocks would need a driver
  written from scratch against undocumented registers.
- **AF_ALG availability is unverified** (`/proc/config.gz` is absent). Even if
  present, AF_ALG adds a syscall and a copy per operation.
- **DMA/alignment requirements are unknown.**

**Is it worth it?** Almost certainly not, for the current load. SRTP here
protects roughly one 1080p H.264 stream — on the order of a few hundred KB/s.
AES-CM and HMAC-SHA1 at that rate are a small fraction of one core, and the
existing software path already delivers the accepted ~100 ms end-to-end. A
per-packet AF_ALG round trip could plausibly be *slower* than the software
implementation at this packet size.

**Recommendation:** drop hardware AES and HASH from the roadmap unless a
measurement first shows SRTP as a real cost. Keep **DTRNG only**, and justify it
by entropy (the `Seeding 2048 bits` shutdown hang), not by throughput.

## 6. NNA — sub-plan

```text
Analysis tap → Venus/JZDL/AIP runtime → soc-nna → person/vehicle detection
```

| Requirement | Status |
|---|---|
| Kernel module | `soc-nna.ko` `H20230310` present, unloaded, vermagic OK |
| Reserved memory | **`nmem=8M@0x7800000` missing** — hard blocker, boot-path change |
| Userspace runtime | absent; stock `libvenus.so` pairs with a 2019 module, not this one |
| Model format | **unknown**; no model file exists in the stock dump |
| Known issue | libimp↔NN ABI mismatch on T40NN (prior finding) |
| Stock as reference | **only partially usable** — the stock module is much older than the one OpenIPC ships |

**Verdict: the weakest-supported item in this plan.** Four independent unknowns,
one of which is a boot-path change. Machino's existing IMP_IVS motion backend
(M9) delivers detection today without any of this and merely needs a hardware
test.

## 7. Phased migration

| Phase | Content | Expected benefit | Risk | Hardware test | Rollback |
|---|---|---|---|---|---|
| **0** | current compatibility stack | baseline: WebRTC ~100 ms, accepted | — | — | — |
| **1** | userspace-only: encoder buffer/pool sizing, `GetFd`/`PollingStream`, RC/GOP tuning, SEI | lower CPU and jitter; better fit to the 48 MB budget | **low** — one binary, atomic swap | normal deploy acceptance | redeploy previous binary |
| **1b** | load `dtrng_dev`, wire entropy | fixes the `Seeding 2048 bits` shutdown hang; faster DTLS handshakes | **low-medium** — an unexercised vendor module can wedge the box | supervised, UART attached | `rmmod`, do not load at boot |
| **2** | ISP/sensor stack (tx-isp + sensor as a dated pair) | **none identified** — params and IQ already match stock | **high** | supervised, UART | restore both `.ko`, power-cycle |
| **3** | AVPU/media | none identified | high | supervised | restore `.ko` |
| **4** | NNA | person/vehicle detection | **very high**, boot-path change | supervised, UART, recovery plan | revert bootargs |
| **5** | crypto hardware | marginal at this bitrate | high effort, undocumented registers | supervised | do not load |

Phases 2 and 3 are deliberately **demoted**: AP8 found no defect they would fix.
They should stay parked until a measurement identifies one.

## 8. Rollback and recovery design

Constraint that shapes everything: **6.0 MB free on the overlay.** A full
duplicate stack does not fit comfortably (`tx-isp` alone is ~1 MB, `libvenus`
2.7 MB).

Design:

1. **The compatibility stack stays the default.** Enhanced components are
   *additions* under a separate path, never overwrites.
2. **Selection at init, not at build.** `/etc/init.d/machino` picks the stack
   from a single config value, so switching back is editing one line — no
   reflash, no file restore.
3. **Never delete the outgoing component.** Rename in place
   (`tx-isp-t40.ko.compat`) and keep it until the new one has survived a
   supervised test *and* a power-cycle. Check free space before staging.
4. **Stage in `/tmp` first.** 20.9 MB tmpfs, and it evaporates on reboot — a
   failed stage cannot persist into the next boot.
5. **Healthcheck after start.** Machino already has the pieces: the runtime
   metrics from AP5, `machino-manager status`, and the lifecycle `Stats`. The
   check is "pipeline reaches a running generation and produces frames within
   N seconds".
6. **Automatic fallback where technically possible.** For userspace (phase 1)
   this is real: the init script can fall back to the previous binary on a
   failed healthcheck. For kernel modules it is **not** — a module that wedges
   the box takes the healthcheck with it. Be honest about that: phases 2-5 have
   *manual* recovery only.
7. **Recovery path.** UART console is the primary route (it survives a broken
   userspace); SSH is the convenience route. Given the known
   `t40nn-reboot-haengt-im-shutdown` behaviour, **a power-cycle must be assumed
   as the reset mechanism**, and given `t40nn-freeze-nach-install`, a
   power-cycle is mandatory after every install before any load is applied.

---

## 9. Classification

### SAFE_TO_IMPLEMENT_NEXT

- Encoder buffer/pool sizing (`SetPool`, `SetStreamBufSize`, `SetMaxStreamCnt`)
  and `SetbufshareChn` for main/sub — userspace only, atomic rollback.
- Event-driven stream fetch via `IMP_Encoder_GetFd` / `PollingStream`.
- Rate control work (`SetChnAttrRcMode`, `SetChnQpBounds`, `SetChnGopAttr`) —
  the open rate-adaptation item from the WebRTC slice.
- Stack-selection + healthcheck + fallback plumbing in the init script
  (pure infrastructure, changes no media behaviour).
- Keeping the header/library version tripwire and the CI `.soc` gate.

### NEEDS_SUPERVISED_HARDWARE_TEST

- Loading `dtrng_dev.ko` (phase 1b).
- Everything in phases 2-5 without exception.
- M9 IVS motion (already code-complete, never tested on hardware).
- Any `libaudioProcess` experiment.

### NEEDS_KERNEL_BUILD

- Hardware AES and HASH drivers (would have to be written from scratch).
- Anything requiring `CONFIG_*` verification, because `/proc/config.gz` is
  absent — the running configuration cannot currently be read back.

### NEEDS_MORE_RESEARCH

- Whether 1.3.1 exports `IMP_Encoder_SetChnRoiAttr` and `IMP_ISP_WDR_OPEN`
  (AP7 — AP6 only proved they are missing from the OpenIPC **1.2.0 .so**, which
  Machino does not use).
- AF_ALG availability in this kernel.
- The Venus/JZDL/AIP runtime matching `soc-nna H20230310`, and the NNA model
  format — no artifact of either exists in the stock dump.
- Whether the `nmem`/`mem` bootargs can be changed with a safe recovery path.
- Whether SRTP is measurably expensive at this bitrate at all (this decides
  whether phase 5 should exist).

## 10. Honest limits

- No component was built, staged or loaded. Every compatibility statement is
  either **measured on the running system**, **read out of the build system**,
  or explicitly marked unverified.
- The proven-compatible mixed stack (1.3.1 userspace + 2022-06 driver) is proven
  for *the paths Machino exercises* — H.264 main stream, RTSP, WebRTC. It is not
  proven for WDR, ROI, NNA or audio.
- `CONFIG_MODVERSIONS=n` was inferred from the absence of the `modversions`
  token in vermagic plus zero `__crc_` symbols in `/proc/kallsyms`. That
  inference is solid, but it was not confirmed against a kernel config file,
  because the device does not expose one.
- Overlay free space (6.0 MB) was measured at one point in time and will shrink
  as logs and config accumulate.
