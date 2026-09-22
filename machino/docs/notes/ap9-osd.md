# AP9 — OSD backend and API contract

## What the contract actually is

Derived from the upstream oracle clone (`/c/tmp/majestic-webui`), from the
**executed JavaScript**, not from documentation. Two things had to be corrected
before any code was written:

1. **`/api/v1/osd` is not the text endpoint.** It is a **geometry report**. The
   settings page reads it for the real overlay rectangles (it counts regions and
   bytes from them, `refreshOsdRects` in `mj-settings.js`), and the preview
   overlays (`preview-roi.js`, `preview-peer.js`) read `group` and `streams` to
   map coordinates between the sensor frame and whichever stream is on screen.
2. **The OSD *settings* are plain config**, in the schema section `osd`, plus a
   per-channel `video<N>.osd` boolean. There is no OSD write endpoint at all.

The endpoint the assignment called `/osd/image` is really
**`/api/v1/osd/image?overlay=N`**.

## Supported OSD fields

Exactly the section the unmodified page renders — no field invented, none
dropped except where noted:

| Config key | Type | Default | Notes |
|---|---|---|---|
| `osd.enabled` | bool | `false` | |
| `osd.template` | string | `%d.%m.%Y %H:%M:%S` | strftime, plus `%f` = milliseconds |
| `osd.font` | string | `…/UbuntuMono-Regular.ttf` | |
| `osd.size` | string | `1.0` | scale factor; a string upstream because it is a free value |
| `osd.weight` | `normal`\|`thin` | `normal` | |
| `osd.outline` | bool | `true` | dark halo around each glyph |
| `osd.anchor` | enum, 10 values | `proportional` | `proportional`, `top-left`, `top`, `top-right`, `left`, `center`, `right`, `bottom-left`, `bottom`, `bottom-right` |
| `osd.offset_x` / `osd.offset_y` | string | `0` | `12` = px, `2%` = share of the frame, `1.5em` = multiple of the text size |
| `osd.pos_x` / `osd.pos_y` | int −16..16 | `16` | the legacy proportional grid; 16 = left/top, −16 = right/bottom |
| `osd.bg_alpha` | int 0..100 | `25` | opacity of the plate behind the text |
| `osd.image_dir` | string | `/etc/machino/osd` | where logo overlays are stored |
| `video.0.osd` / `video.1.osd` | bool | `true` | *where* the overlay is drawn |

**Deliberately not implemented:**

- `osd.privacyMasks` — it is in the same upstream section, but a privacy mask is
  a burnt-in rectangle, not an overlay, and it belongs with the region editor.
- **Per-stream text.** The content is global upstream and only the `video<N>.osd`
  booleans are per-stream. Inventing per-stream text would be a field the stock
  page cannot set.
- `%@` (lens zoom magnification) expands to **nothing**. This camera has no
  motorised lens to ask, and a made-up magnification is worse than none.

## Architecture

```
WebUI → /api/v1/osd[/image] → OsdService → IOsdBackend
                                             ├─ SoftOsdBackend   (shipped, cannot draw)
                                             └─ Ingenic backend  (NOT YET WRITTEN)
```

The split follows what is genuinely platform-specific:

- **Placement and text are not.** Resolving `2%` against a frame width, mapping
  the −16..16 grid onto pixels and expanding a strftime template is the same
  arithmetic everywhere, so it lives in `OsdService` and is host-tested.
- **Measuring and drawing are.** Only the backend knows how wide a string comes
  out in its font, which is why `measure()` is its own call rather than the
  service guessing glyph widths: a guessed rectangle in the geometry report
  becomes a printed region/byte count in the stock UI that is quietly wrong.

No IMP type appears in `ports/iosd.hpp`, and no IMP call is reachable from HTTP
or config code.

### Threading

Every `OsdService` method takes its own mutex **and nothing else** — never a
pipeline or StreamHub lock. The backend is *mutated* only from `pump()`, which
releases the mutex first. `report()` additionally calls the pure const
`measure()`. Consequence: a slow or wedged backend delays the next overlay
update and nothing else. It can never stall the media path.

## `/api/v1/osd`

```json
{ "group":   [1920, 1080],
  "streams": [ {"stream":0, "frame":[1920,1080], "view":[0,0,1920,1080]},
               {"stream":1, "frame":[640,360],   "view":[0,0,1920,1080]} ],
  "overlays":[ {"overlay":0, "frame":[1920,1080], "rect":[x,y,w,h]} ],
  "budget":  {"overlays": 8} }
```

- `view` is the whole group for every stream: Machino never crops a stream out
  of the sensor frame, both outputs show the same scene at different sizes.
- **404 is a legitimate answer** and the one this build gives, because
  `SoftOsdBackend::available()` is false. The stock page treats 404 as a
  property of the build, stops polling and prints item counts instead of region
  counts. An empty `200` would instead make it repaint forever — so "cannot
  say" is never faked.
- `budget` is omitted rather than guessed when the backend reports 0.

## `/api/v1/osd/image?overlay=N`

| | |
|---|---|
| `GET` | raw **BGRA** body plus `X-Osd-Width`, `X-Osd-Height`, `X-Osd-Ref`. 404 when there is no picture |
| `POST` **with** a body | upload; `w`, `h`, `ref` in the query; `Content-Type: application/octet-stream`; body must be exactly `w*h*4` bytes |
| `POST` **without** a body | **delete** — this is how the page's staged logo removal lands on save (`flushLogoBin`) |

Validation is fail-closed and nothing is written when a request is refused:

- overlay index `0..3`, dimensions `1..512`, `w*h*4` computed in 64-bit,
- body length must match the declared size **exactly** (short *and* long are refused),
- a negative `ref` is refused; `ref = 0` means "drawn pixel for pixel" and is
  stored as the picture's own width,
- **512×512 BGRA = 1 MiB** is the hard cap. Justified, not arbitrary: the store
  lives on an overlay filesystem with 6.0 MB free (measured in AP8) and the
  hardware keeps two buffers per region.

Storage is one file per overlay under the single configured directory. The
caller supplies only an **index**, never a path, so no request can steer where
a file lands. Writes go to a temporary and are renamed, so a half-written
upload can never be the file the camera loads on its next start. A corrupted
file on disk is re-validated on read and refused rather than turned into an
allocation.

One integration detail worth recording: the HTTP parser caps request bodies at
8 KiB and the per-client input buffer at 16 KiB, so a 1 MiB upload would have
been rejected *before* reaching any of this. The allowance is granted from the
**request line** (`POST /api/v1/osd/image`) so exactly one route gets it, and
the service still validates the body against the declared `w*h*4` afterwards —
a buffer bound, not a trust grant.

## Two defects the tests found

Both were in code already written and believed correct:

1. **A trailing lone `%` in a template vanished.** It was passed through to
   `strftime`, which is entitled to swallow it. Now escaped to `%%` first.
2. **Turning OSD off left the old text burnt into the video.** `set_config()`
   reset the "we have drawn something" flag, so the next `pump()` saw nothing to
   clear. It now invalidates only the dedupe key and keeps the clear state.
   Fixing it also exposed that `clear()` was being called with the *loop index*
   instead of the stream unit, which would have cleared the wrong output on a
   build where only the sub stream carried the overlay.

## Tests — host, 1498/0 (was 1360)

`tests/test_osd.cpp`: length specs (px/%/em, and six refusals); all ten anchors
plus the proportional grid, clamping, and the guarantee that a rectangle never
leaves the frame; template expansion (`%f` clamped to three digits, `%@`,
`%%`, trailing `%`); the report shape including per-stream gating and the
disabled case; 404 both for "cannot draw" and "no geometry yet"; fail-closed on
an unparseable size or offset, and recovery afterwards; redraw-only-on-change
and the clear-on-switch-off transition; the full image store matrix (valid,
bad index, bad dimensions, over-cap, short body, long body, null body, negative
ref, corrupt file, delete, delete twice); config-file parsing including
out-of-range and unknown values being ignored without losing the rest of the
file; and assertions that neither `report()` nor the image store ever calls
`draw()`/`clear()`.

## Open for T40NN hardware acceptance — `NEEDS_HARDWARE_ACCEPTANCE`

1. **There is no drawing backend.** `SoftOsdBackend` knows the placement
   arithmetic but returns `Unsupported` from `draw()`. The Ingenic
   implementation (IMP_OSD region create/attach/update) is the next piece of
   work and is the whole of the hardware risk.
2. **`pump()` is not on a timer yet.** With no backend that can draw, a tick
   would do nothing every second. The timer belongs with the Ingenic backend.
3. **The `osd` section is deliberately NOT advertised in the majestic schema.**
   Offering controls that persist but draw nothing would be a UI lie. The
   capability gate goes in together with the backend. The config keys work
   today, and both HTTP routes are live and contract-conformant.
4. **`measure()` is a declared model, not a font.** `SoftOsdBackend` uses
   advance 0.6 em, line box 1.25 em, em = 32 px at scale 1.0 against a 1920 px
   reference. This is why `available()` is false: none of those rectangles may
   be published as facts about a camera.
5. Untested on hardware, by design for this package: no IMP overlay call was
   made on the camera, nothing was deployed, no reboot.
