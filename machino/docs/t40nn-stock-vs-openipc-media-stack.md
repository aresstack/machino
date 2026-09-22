# T40NN: stock firmware vs the OpenIPC/Machino media stack (AP6)

Purpose: put the vendor firmware next to what the camera runs today and record
**what is actually different**, so later work argues from evidence instead of
from the assumption that "OpenIPC probably drives the hardware badly".

Nothing here was ported, deployed or changed on the camera. Every camera-side
fact was read over SSH read-only.

## Sources

| Side | Source | How |
|---|---|---|
| Stock | `stock-16M-20260914-223453.bin` (full 16 MB dump) | squashfs `rootfs` @ 0x2B0000 and `appfs` @ 0x417000, both xz, unpacked with `unsquashfs` |
| OpenIPC/Machino | the running camera at 192.168.1.10 | `cat`/`ls`/`lsmod`/`readelf`, no writes |
| Symbols | `readelf -W --dyn-syms` on both `libimp.so` | `-W` is required: without it readelf truncates names and invents differences |

## Headline result

**The media substrate is very nearly the same on both sides.** Same kernel
version, same ISP driver family started with *identical* parameters, a
**byte-identical** IMX307 IQ tuning file, and the same `libimp` **1.2.0** with
357 of 371 functions in common. The hypothesis that OpenIPC drives this sensor
through a fundamentally different or degraded path is **not supported**.

The real differences are narrow and concrete: a smaller userspace memory
budget, no reserved NNA memory, an ISP/sensor driver pair about five to six
months older, and fifteen missing `libimp` entry points (ROI encoding, WDR
open, the YUV still-encode path, audio howling suppression).

## Comparison

| Component | Stock | OpenIPC / Machino | Evidence | Likely impact | Confidence | Next action |
|---|---|---|---|---|---|---|
| Kernel | 4.4.94, `vermagic=4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT` | same vermagic | `strings` on `tx-isp-t40.ko` both sides | none | high | none |
| `mem=` (userspace) | `57856k` (alt. env line: `81920k`) | `48M`, measured `MemTotal 42816 kB` | stock U-Boot env in the dump; `/proc/cmdline` | ~8.5 MB less RAM than stock; matches the memory pressure seen under parallel CGI load | medium (two env lines, cannot prove which was active) | treat as a candidate, but changing bootargs is a kernel/boot change - out of scope |
| `rmem=` (media) | `65024k@0x3880000` (alt. `40960k@0x5000000`) | `64M@0x3000000` | same | effectively equal | high | none |
| `nmem=` (NNA) | `8M@0x7800000` | **absent** | same | the NNA has no reserved memory, so it cannot work regardless of the module | high | prerequisite for any NNA work; bootarg change, out of scope here |
| ISP driver | `tx-isp-t40.ko` 996888 B, tag **H20221114a** | 988760 B, tag **H20220606a** | `strings ... H2[0-9]+` | OpenIPC runs an ISP driver ~5 months older; unknown fixes missing | high (tags), low (impact) | only meaningful together with a matching libimp; see AP7 |
| ISP insmod params | `clk_name=mpll isp_clk=367000000 isp_memopt=2`, `avpu_clk=550000000` | **identical** (boot log) | stock `ko/loadko.sh` vs OpenIPC boot log | none - this rules out a clocking/memopt difference | high | closed |
| Sensor driver | `sensor_imx307_t40.ko` 18836 B, **H20221205a** | 19576 B, **H20220531a** | `strings` | ~6 months older | high (tags), low (impact) | none now |
| Sensor register sets | `..._30fps_mipi`, `..._30fps_mipi_2dol_lcg`, `..._60fps_mipi` | **the same three** | `strings ... imx307_init_regs_*` | 60 fps and 2-DOL WDR exist on both sides | high | 60 fps is an explicit No-Go for unattended testing |
| IQ tuning (IMX307) | `imx307-t40.bin`, md5 `7295ef76…` | `imx307-t40.bin`, md5 **identical** | `md5sum` both sides | image tuning is *not* a difference | high | closed |
| IQ variants | additionally `imx307-wdr-t40.bin`, `imx307-cust-t40.bin` | neither | `ls /etc/sensor` | no WDR tuning available on OpenIPC | high | needed if WDR is ever wanted |
| `libimp` | 1.2.0, 1041032 B, 371 defined `IMP_*` | **1.2.0**, 1030372 B, 357 defined | `readelf -W --dyn-syms` | same generation of the SDK | high | see AP7 |
| Encoder tuning APIs | `SetPool`, `SetStreamBufSize`, `SetMaxStreamCnt`, `SetbufshareChn`, `SetChnGopAttr`, `SetChnAttrRcMode`, `SetChnQpBounds`, `SetChnSeiAttr`, `SetFrameRelease`, `PollingStream`, `GetFd` | **all present too** | symbol diff | these are usable **today**, no SDK bump needed | high | AP7 picks the 3-5 worth using |
| ROI encoding | `IMP_Encoder_Set/GetChnRoiAttr` | **missing** | symbol diff | no region-of-interest quality steering | high | needs a different libimp; not available today |
| WDR entry point | `IMP_ISP_WDR_OPEN`, `IMP_ISP_Bypass_Bind` | **missing** | symbol diff | WDR cannot be opened through the OpenIPC lib | high | pairs with the missing WDR IQ bin |
| YUV still encode | `IMP_Encoder_YuvInit/YuvEncode/YuvExit`, `VbmAlloc/Free/P2V/V2P` | **missing** | symbol diff | stock had a separate still/YUV encode path; Machino does not and must not need one (no JPEG path by mandate) | high | no action - out of scope by mandate |
| OSD | `IMP_OSD_SetGroupCallback` | missing; has `IMP_OSD_GetRegionLuma` instead | symbol diff | the only symbol OpenIPC has and stock does not | high | note for AP8 (OSD) |
| Audio driver | `audio.ko` 90628 B, **H20210524a** | 90556 B, **H20210524a** | `strings` | same driver vintage | high | closed |
| Audio processing | `libaudioProcess.so` (734080 B); `IMP_AI_Enable/DisableHs` (howling suppression) | no `libaudioProcess.so`; `Hs` missing | `ls`, symbol diff | no vendor AEC/NS/howling stack | high | only relevant once audio is on the roadmap |
| NNA | `soc-nna.ko` `version=20190724a`, loaded | `soc-nna.ko` **H20230310** present but **not loaded**, and no `nmem` | `lsmod`, `find` | NNA is newer on OpenIPC but inert; consistent with the known libimp/NN ABI mismatch | high | blocked on `nmem` bootarg; explicit No-Go |
| Userland stack | full vendor app stack: `libvenus.so`, `libants_ivs.so`, `libonvif.so`, `libhikvision.so`, `libants_28181_sdk.so`, … | `libimp`, `libsysutils`, `libalog` only | `ls lib/` both sides | stock shipped IVS/ONVIF/GB28181 as vendor blobs; Machino implements its own | high | informational |

## What this rules out

- **"The ISP is mis-clocked or mis-configured under OpenIPC."** The insmod
  parameters are character-for-character the same as stock, including
  `isp_memopt=2`. Neither side sets `isp_dual_buf`.
- **"The image tuning is worse."** The IQ file is the same bytes.
- **"The latency is a driver-parameter problem."** Nothing in the driver
  configuration differs. The latency work already found its causes elsewhere
  (SPS/VUI DPB hint, the 10 vs 100 Mbit link, then WebRTC as the transport).
- **"The encoder-tuning APIs need SDK 1.3.1."** They are already exported by
  the 1.2.0 library the camera runs today.

## What is genuinely missing or smaller

1. **Userspace RAM**: 48 MB vs stock 56.5 MB (`MemTotal 42816 kB` measured).
2. **`nmem=8M`**: absent, which makes the NNA unusable no matter what else is done.
3. **ROI encoding** (`IMP_Encoder_SetChnRoiAttr`): not in the OpenIPC library.
4. **WDR**: neither the `IMP_ISP_WDR_OPEN` entry point nor the WDR IQ bin.
5. **ISP/sensor driver vintage**: ~5-6 months older than stock.
6. **Vendor audio processing** (`libaudioProcess.so`, howling suppression).

## Prioritised follow-up

### A. Usable today on the compatibility stack (no driver, no kernel, no bootarg change)

| Prio | Item | Why it is available |
|---|---|---|
| 1 | Event-driven stream fetch via `IMP_Encoder_GetFd` / `PollingStream` | both exported by the running 1.2.0 lib; candidate for lower CPU and jitter |
| 2 | Encoder buffer sizing: `SetStreamBufSize`, `SetMaxStreamCnt`, `SetPool` | exported; directly relevant given the 8.5 MB smaller memory budget |
| 3 | `SetbufshareChn` for main/sub | exported; the substream work (AP1) is the natural place to use it |
| 4 | Rate control: `SetChnAttrRcMode`, `SetChnQpBounds`, `SetChnGopAttr` | exported; the open rate-adaptation item from the WebRTC slice |
| 5 | `SetChnSeiAttr` | exported; useful for timing/diagnostic metadata in-band |

AP7 turns this list into a concrete 3-5 API recommendation.

### B. Requires newer driver firmware or an SDK/library change

| Prio | Item | Blocker |
|---|---|---|
| 1 | ROI encoding | entry point absent from the OpenIPC `libimp`; needs a different library, which drags the ISP-driver ABI with it |
| 2 | WDR / 2-DOL | needs both `IMP_ISP_WDR_OPEN` and a WDR IQ bin |
| 3 | ISP driver H20221114a-class fixes | only meaningful as a matched driver+libimp pair |
| 4 | Vendor audio processing | separate blob, not present |

### C. Requires a boot/kernel change - deliberately not attempted

| Item | Change needed | Status |
|---|---|---|
| NNA usable at all | add `nmem=8M@…` to bootargs, load `soc-nna.ko` | **No-Go** for unattended work; also hits the known libimp/NN ABI mismatch |
| Recover ~8 MB userspace RAM | raise `mem=` toward stock's 57856k | bootarg change; needs a human and a recovery path |

### D. Requires hardware verification before it can be claimed

- 60 fps: the register set exists on both sides but has never been run here
  (explicit No-Go for unattended testing).
- Every item in list A: exported symbol is not the same as working behaviour on
  this board.

## Honest limits of this audit

- The stock firmware was **never booted**. This is a static comparison of
  images, symbols and parameters, not a behavioural one. No stock latency,
  bitrate or image-quality number was measured.
- The stock U-Boot environment contains **two** bootargs lines with different
  `mem`/`rmem` splits. Which one was active on this unit cannot be proven from
  the dump, so the memory row is medium confidence.
- Symbol presence proves an entry point exists, not that it is implemented
  behind it or that it works on T40NN.
- Module build tags (`H2022…`) are vendor date stamps read out of the binaries;
  they order the builds but say nothing about which fixes they contain.
