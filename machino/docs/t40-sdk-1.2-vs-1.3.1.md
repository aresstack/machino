# T40 SDK 1.2.0 vs 1.3.1 — API and symbol diff (AP7)

Executed after AP8, which is why AP8 carries open questions this answers. Two
of AP8's conclusions are corrected below.

## Method and sources

| Artifact | Where from |
|---|---|
| libimp **1.3.1** (`libimp.so`, 1215092 B) | `gtxaspec/ingenic-lib`, `T40/lib/1.3.1/uclibc/7.2.0` — the exact path `build.sh` pins, so this is the library Machino links |
| libimp **1.2.0**, stock (1041032 B) | vendor firmware dump, `appfs/lib/` |
| libimp **1.2.0**, OpenIPC (1030372 B) | read off the running camera, `/usr/lib/` |
| Headers **1.3.1** | `include/T40/1.3.1/en` (the set Machino compiles against) |
| Headers **1.2.0** | `include/T40/1.2.0/zh` — Chinese only; comments differ, declarations do not |

Symbols: `readelf -W --dyn-syms`, defined `FUNC` entries only. **`-W` is
mandatory** — without it readelf truncates names and invents differences.
Structs: all 196 typedef'd structs in the 1.3.1 headers, parsed and compared
member by member.

Note on the version string: the 1.3.1 `libimp.so` reports `1.3.0` internally.
The *path* is what identifies the drop, and it is what CI gates on.

## Counts

| | exported `IMP_*` functions |
|---|---|
| 1.2.0 OpenIPC (`/usr/lib/libimp.so`, unused by Machino) | 357 |
| 1.2.0 stock | 371 |
| **1.3.1 (what Machino links)** | **384** |

Declared in headers: 1.2.0 = 359, 1.3.1 = 386.

---

## The distinction the assignment asked for

**"Present in 1.3.1" is not "added in 1.3.1".** Most of what looks new is
older than it appears, in two different ways:

1. **Exported by 1.2.0 but never declared.** ROI, WDR, the YUV still-encode
   path, the Vbm helpers, `IMP_OSD_SetGroupCallback` and the audio
   howling-suppression calls are all **in the stock 1.2.0 library** and were
   simply **not in the 1.2.0 headers**. 1.3.1 declares them. They are
   `DECLARED_IN_1_3`, not `NEW_IN_1_3`.
2. **Missing from the OpenIPC build specifically.** AP6 found 15 symbols absent
   from the OpenIPC 1.2.0 `.so`. That is a property of *that build*, not of the
   SDK generation — stock 1.2.0 has 14 of them and 1.3.1 has all 15.

### `NEW_IN_1_3` — genuinely absent from *both* 1.2.0 libraries (13)

```
IMP_Encoder_SetChnMapRoi
IMP_FrameSource_QueueBuffer            IMP_FrameSource_DequeueBuffer
IMP_FrameSource_ExternInject_CreateChn    …_DestroyChn
IMP_FrameSource_ExternInject_EnableChn    …_DisableChn
IMP_ISP_Tuning_Awb_SetRgbCoefft        IMP_ISP_Tuning_Awb_GetRgbCoefft
IMP_ISP_Tuning_SetAeExpList            IMP_ISP_Tuning_GetAeExpList
IMP_ISP_Tuning_SetAeSpeed              IMP_ISP_Tuning_GetAeSpeed
```

Two themes: **frame injection** (feed your own buffers into the pipeline) and
**finer AE/AWB control**. Fifteen structs come with them: `IMPEncoderMapRoiAttr`,
`IMPEncoderRoiAttr/RoiRect/RoiWin`, `IMPEncoderQPMode`, `IMPEncoderYuvIn/YuvOut`,
`IMPISPAeExpListAttr`, `IMPISPAeSpeedAttr`, `IMPISPBypass/BypassAttr`,
`IMPISPCoefftWb`, `IMPISPWdrInitMode/WdrOpenAttr/WdrRunMode`.

### `DECLARED_IN_1_3` — in the stock 1.2.0 library, newly declared (16)

```
IMP_Encoder_SetChnRoiAttr   IMP_Encoder_GetChnRoiAttr
IMP_Encoder_YuvInit         IMP_Encoder_YuvEncode      IMP_Encoder_YuvExit
IMP_Encoder_VbmAlloc        IMP_Encoder_VbmFree
IMP_Encoder_VbmP2V          IMP_Encoder_VbmV2P
IMP_ISP_WDR_OPEN            IMP_ISP_Bypass_Bind
IMP_OSD_SetGroupCallback
IMP_AI_EnableHs             IMP_AI_DisableHs
IMP_AI_Set_WebrtcProfileIni_Path
IMP_DMIC_DisableAecRefFrame
```

### `REMOVED` (1)

```
IMP_OSD_GetRegionLuma
```

Gone from the 1.3.1 library **and** the 1.3.1 headers. It existed only in the
**OpenIPC** 1.2.0 build — stock 1.2.0 never had it. Since Machino links 1.3.1,
**Machino cannot use it**, which matters for any future OSD or night-mode work
that wants a region's luma.

Also removed at header level: `IMP_FrameSource_SetChnFPS`. It was declared in
the 1.2.0 headers and **exported by no library at all** — a phantom API. 1.3.1
drops the declaration. Machino never called it.

### `CHANGED_IN_1_3` — 4 real changes in 196 structs

| Struct | Change | Consequence |
|---|---|---|
| **`IMPEncoderStream`** | **+ `bool isVI` at the end** | The known ABI trap. Building 1.3.1 libs against 1.2.0 headers makes `IMP_Encoder_GetStream` write **one word past the caller's struct on every frame**. Machino has a compile-time tripwire in `video_thread()`; keep it. Independently re-verified here from the headers. |
| `IMPFSChnType` | `+ FS_INJ_CHANNEL` appended | Safe: appended, existing values keep their numbers. Goes with the new injection APIs. |
| `IMPISPDualSensorSplitJoint` | typo `IMPISP_MIAN_ON_THE_ABOVE` → `IMPISP_MAIN_...`, plus new explicit `0x10`..`0x4000` values | **Source-breaking** for anyone who spelled the typo. The first five values keep their numbers, so ABI is unchanged. Machino does not use dual-sensor. |
| `IMP_IVS_BaseMoveOutput` | **`int64_t timeStamp` removed** | The struct **shrinks**. Code compiled against 1.2.0 headers reading that field against a 1.3.1 library reads past the object. Machino touches neither the struct nor the field — verified. |

`IMPFrameInfo` differs only as `void *pool` vs `void* pool` — whitespace, not a
change. `IMP_IVS_MoveParam` and `IMP_IVS_MoveOutput`, which Machino's M9 motion
backend does use, are **byte-identical** between the versions.

### `HEADER_ONLY` in 1.3.1 — declared, exported by nothing

```
IMP_Decoder_*            (8: CreateChn, DestroyChn, GetFrame, PollingFrame,
                          ReleaseFrame, SendStreamTimeout, Start/StopRecvPic)
IMP_EmuFrameSource_*     (5)
IMP_Encoder_PollingModuleStream
IMP_ISP_Tuning_Get/SetISPHLDCAttr
IMP_Log_Get_Option / IMP_Log_Set_Option   (these live in libalog, not libimp)
```

Calling any of these is a **link error**, not a runtime surprise. Worth knowing
before designing around a decoder path on this SoC: there is no decoder
implementation in `libimp` 1.3.1.

### `SYMBOL_ONLY` in 1.3.1 — exported, not declared (36)

Allocator and pool internals (`IMP_Alloc*`, `IMP_Pool*`, `IMP_Virt_to_Phys`,
`IMP_FlushCache`, `IMP_MemPool_*`), ISP algorithm hooks
(`IMP_ISP_Set{Ae,Awb}AlgoFunc_internal/_close`), `IMP_OSD_UpdateRgnAttrData_ISP`,
`IMP_ISPOSD_Alloc`, `IMP_FrameSource_{Set,Get}Pool`,
`IMP_FrameSource_{Enable,Disable}ChnUndistort`, `IMP_Encoder_GetStream_Impl`,
and two `*_IMPDBG_Init`.

Reachable only by writing your own declaration. That means **no header defines
the argument types**, so a wrong guess is a silent ABI mismatch with no
diagnostic. Treat as unavailable unless a specific need justifies the risk.

### `PRESENT_1_2_AND_1_3`

Everything else: **356 of the 357** functions the OpenIPC 1.2.0 build exports
are also in 1.3.1 (the exception being `IMP_OSD_GetRegionLuma`). The two
generations are overwhelmingly the same API.

### `UNKNOWN`

- `IMPVersion` was the one struct the parser could not read; not compared.
- Behavioural changes behind an unchanged signature cannot be seen this way at
  all. Everything above is about shape, not semantics.

---

## Corrections to AP8

AP8 was written before this diff and put two items under *"Requires newer
driver firmware or an SDK/library change"*. Both are wrong:

1. **ROI encoding is available today.** `IMP_Encoder_Set/GetChnRoiAttr` is
   exported by the 1.3.1 library Machino links and declared in the headers it
   compiles against. AP6 found it missing from the OpenIPC `.so` — which
   Machino does not use. It moves to *available now*, and 1.3.1 additionally
   offers `IMP_Encoder_SetChnMapRoi`.
2. **WDR's entry point is available today.** `IMP_ISP_WDR_OPEN` and
   `IMPISPWdrOpenAttr` are both present. The remaining blocker for WDR is only
   the **missing `imx307-wdr-t40.bin` IQ file** (AP6), not the API.

Unchanged from AP8: the encoder-tuning set is present in **both** generations,
so none of it ever needed an SDK bump.

## Machino's own usage, checked against 1.3.1

Every `IMP_*` function the adapter layer calls is exported **and** declared by
1.3.1 — no call depends on a symbol-only or header-only entry point. (The
leftovers in the raw scan are enum constants and type names, not calls.)

## Recommendation — the APIs worth adopting next

Ordered by benefit against known, measured gaps. All five are in the library
Machino already links, so none needs a driver or firmware change.

| # | API | Why this one |
|---|---|---|
| 1 | **`IMP_Encoder_SetChnRoiAttr`** | Quality-per-bit at a fixed bitrate. The WebRTC path already adapts bitrate downward; ROI is what keeps the part of the frame anyone cares about readable when it does. Newly proven available — this is the single biggest item this diff unlocked. |
| 2 | **`IMP_Encoder_GetFd` + `IMP_Encoder_PollingStream`** | Event-driven stream fetch instead of a polled wait. Directly targets CPU and jitter, and fits the existing single-poll-loop architecture rather than fighting it. |
| 3 | **`IMP_Encoder_SetChnQpBounds` + `IMP_Encoder_SetChnAttrRcMode`** | The open rate-adaptation item from the WebRTC slice. QP bounds are what stop a bitrate drop from turning into a smear. |
| 4 | **`IMP_Encoder_SetbufshareChn`** | Substream memory (AP1) against a 48 MB userspace budget that AP6 measured as 8.5 MB smaller than stock. |
| 5 | **`IMP_Encoder_SetChnSeiAttr`** | In-band SEI timing metadata — the cheapest way to measure end-to-end latency for real instead of by stopwatch. |

Deliberately **not** recommended: the frame-injection APIs (no use case here),
the AE/AWB additions (the IQ file is byte-identical to stock, so the tuning is
not the problem), and anything `SYMBOL_ONLY`.

## Honest limits

- Symbol presence proves an entry point exists, not that it works on T40NN.
  Every recommendation above still needs hardware verification.
- The struct sweep compares **declared shape**, not compiler layout: padding
  and alignment were not computed, only members. For the four changes found
  that is sufficient, because each is an added, removed or renamed member.
- Semantics behind unchanged signatures are invisible to this method.
- The 1.2.0 headers available here are the Chinese set. Declarations are
  language-independent, but if a declaration existed only in an English 1.2.0
  set that was never published, this diff would call it new.
