# Machino as a drop-in for Majestic — what is still missing

> **VERALTET (AP28, 2026-09-23).** Dieses Dokument beschreibt den Stand vor
> AP10–AP24 und wird als Analyse aufgehoben, nicht als Lagebericht.
>
> Was sich geändert hat:
>
> * **`/api/v1/image` ist implementiert** (AP10). Unten steht noch, es sei „the
>   only gap that today's Machino configuration actively walks into" — das gilt
>   nicht mehr. Auf der Kamera ist es noch nicht aktiv, weil der laufende
>   Prozess älter ist; siehe `pending-physical.md`.
> * `video1` wird seit AP9 im Schema geführt (`x-reload: "pipeline"`).
> * `nightMode`, `audio` und `jpeg` werden inzwischen ausdrücklich gemeldet
>   und beim Schreiben mit **403 und Begründung** abgelehnt statt verschwiegen.
>
> **Der aktuelle Stand steht in `dropin-matrix.md`** — 58 Endpunkte, jeder
> klassifiziert.

Final verification, no implementation. The question is narrow: with the
**unmodified** OpenIPC/majestic-webui installed, what does it ask for that
Machino does not answer?

## Method

Three sources, no recollection:

1. Every endpoint the WebUI fetches, extracted from the upstream oracle clone
   (`/c/tmp/majestic-webui`, `www/**`): `apiFetch(`/`fetch(` call sites plus the
   `new WebSocket(...)` paths.
2. Every route Machino serves natively, extracted from `http_server.cpp`.
3. **Which pages are actually installed on this camera** (`/var/www`, read over
   SSH). This matters more than it looks: a gap nobody's page can reach is not
   a gap, and the camera turns out to carry the *full* enhanced WebUI —
   `cameras.html`, `recordings.cgi`, `sdcard.cgi`, `mj-pins.js`,
   `analytics-overlay.js` and the rest are all present.

Anything Machino does not serve natively falls through to the front-door relay
and reaches busybox httpd. The CGIs (`/cgi-bin/**`) live there and are answered.
**`/api/v1/**` is majestic's own surface** — the CGIs do not implement it, so a
relayed `/api/v1/...` is a 404.

## What already works

`/api/v1/config`, `config.json`, `config.schema.json`, `reset`, `get`,
`sources`, `osd`, `osd/image`, `/metrics`, `/login`, `/logout`, `/setup`,
`/setup.html`, `/ws/video`, `/ws/webrtc`, `/ws/logs`, plus the native
`/snapshot` and the ONVIF surface. The Live page, the session login, the
settings save path and the unclaimed/first-run flow are covered.

---

## Gaps

Severity is about what an operator sees, not about how much code is missing.

### A. Reachable today, and visible

| Endpoint | Page that calls it | What happens now |
|---|---|---|
| `POST /api/v1/image?…` | Image / "Live adjustments" (`mj-settings.js`) | **No live preview.** The page pushes slider values while you drag so the picture follows; Machino 404s, the push resolves to `false`, nothing is shown. The values still apply on **Save** through `/api/v1/config`. |

This is the only gap that today's Machino configuration actively walks into,
because it is the one place where Machino advertises a section (`image`,
`x-reload: live`) whose page then reaches for an endpoint that is not there.
It degrades quietly — no error, no retry storm — but the feature is gone.

### B. Reachable, on pages that are installed but whose feature Machino has no backend for

| Endpoint(s) | Page | Feature |
|---|---|---|
| `/api/v1/records/resume`, `/api/v1/records/standdown`, `/metrics/records` | `sdcard.cgi`, `recordings.cgi` (`sdcard.js`, `recordings.js`) | SD recording control and counters |
| `/api/v1/analytics/day`, `/ws/analytics` | `recordings.cgi`, `analytics-overlay.js` | recording timeline and the live detection overlay |
| `/api/v1/peers` | `cameras.html` (`cameras-switch.js`) | the multi-camera roster |
| `/api/v1/calibration/coverage`, `/calibration/peer`, `/calibration/pair` | Live page peer overlay (`preview-peer.js`) | cross-camera calibration |
| `/api/v1/gpio`, `/api/v1/pinmux`, `/ws/pins` | Pins UI (`mj-pins.js`) | GPIO map and live pin state |
| `/night/toggle`, `/night/ircut`, `/night/light`, `/metrics/night` | Night mode (`mj-settings.js`) | IR-cut and illuminator control |

These are whole features, not endpoint stubs. Machino has no recording, no
analytics store, no peer roster, no calibration, no GPIO service and no IR-cut
driver. `preview-peer.js` and `ircut-check.js` are written to treat an absent
answer as "this camera cannot say" rather than as a fault, so the pages should
degrade rather than break — **but that is read from their code, not observed on
this camera.**

### C. Known and deliberate

| Endpoint | Why |
|---|---|
| `/image.jpg`, `/image.dng` | The JPEG path wedges the whole daemon on T40NN and is default-off (`machino-t40nn-jpeg-wedge`). `/snapshot` exists natively; `/image.jpg` is not aliased to it. |
| `/audio.pcm`, `/play_audio` | No audio path. `libaudioProcess` is absent from OpenIPC (AP6) and audio was never in scope. |
| `/upload`, `/ws/upgrade` | Firmware upload, excluded from AP10 by the assignment. |
| `/api/v1/live` | OSD placement dragging. Falls back to a legacy query form on 404 **and** the `osd` section is not advertised, so the drag UI never mounts. Becomes relevant the moment OSD is switched on. |

### D. Advertised-but-inert

Not a missing endpoint — a missing backend behind a working surface:

- **OSD**: config keys, `/api/v1/osd` and `/api/v1/osd/image` are implemented,
  but `SoftOsdBackend` cannot draw, so `/api/v1/osd` answers the 404 that means
  "this build cannot say" and the `osd` section is deliberately kept out of the
  schema. Nothing is broken; nothing is drawn either.
- **ONVIF**: complete enough for a client to find the camera and pull a stream,
  but `onvif.enabled` defaults to `false` and it has never met a real client.

---

## Suggested order of work

Ordered by operator-visible value per unit of risk. No implementation is
proposed here, only sequence.

**1. Decide `/api/v1/image` — cheapest visible win.**
The image controls already exist natively (`TuningService`, `IImageControl`).
The endpoint is a query-string form of a PATCH that already works. This is the
one gap where the backend is present and only the wire form is missing. It is
also the only gap a user of today's build will actually notice.

**2. Alias `/image.jpg` to the existing snapshot path — or decide not to.**
`/snapshot` works; `/image.jpg` is what the stock pages ask for. The blocker is
not the route but the JPEG wedge, so this is a *decision about the wedge*, not
about HTTP. Worth resolving explicitly rather than leaving implicit.

**3. Take the OSD backend to hardware.**
Everything above it is built and tested. It is the largest single piece of
already-paid-for work that delivers nothing until the Ingenic backend exists.

**4. Everything in group B: decide per feature whether it is in scope at all.**
Recording, analytics, peers, calibration and GPIO are each a subsystem, not a
gap. My recommendation is to declare them out of scope for the drop-in and say
so in the README, rather than leave them looking like an unfinished list.
Night mode is the exception worth reconsidering — it is a small, self-contained
feature that users expect from a camera.

**5. Verify group B's graceful degradation on the real camera.**
Open every installed page against Machino and note which ones show an error
rather than an absence. This is an afternoon with a browser and it converts a
list of *expected* behaviour into *observed* behaviour. It should come before
any of 1–4, because it may reclassify several of them.

---

## Honest limits of this verification

- **Nothing here was observed on the running camera.** It is a static
  comparison of what the WebUI's JavaScript calls against what Machino's router
  answers. The severity column says what the code implies, not what a browser
  did.
- The camera is still running `1b7225b` from the WebRTC slice. None of the AP9–
  AP11 work is deployed, so even the endpoints listed as working are working
  *in the build*, not on that camera right now.
- The oracle clone is the enhanced majestic-webui. Where a page is gated by the
  config schema rather than by its own presence, I have said so; where I was
  not certain whether a page renders at all under Machino's schema (night mode
  in particular), I have marked it rather than guessed.
- Endpoints reached by string concatenation that my extraction missed would not
  appear here. The two extraction passes (literal `fetch`/`apiFetch` arguments
  and `/api/v1/...` literals anywhere in the JS) agree with each other, which is
  weak evidence that the list is complete, not proof.

## NEU 2026-09-22: Machino laesst majestics Watchdog fallen

**Schwere: hoch.** Das ist die Antwort auf die Frage, warum die Kamera sich aus
einem Hardlock nie selbst befreit.

Stock-majestic auf dieser Kamera:

```yaml
# /etc/majestic.yaml
watchdog:
  enabled: true
  timeout: 15
```

Machino:

```cpp
// src/app/compat/majestic_migrate.cpp:282
if (sec_l == "watchdog") return ignored(key, val, "handled by the OpenIPC init + streamerctl");
```

**Diese Begruendung ist auf dieser Kamera nachweislich falsch.** Gemessen im
laufenden Betrieb:

* kein Prozess haelt `/dev/watchdog` offen (alle `/proc/*/fd` durchsucht)
* kein Skript unter `/etc/init.d/` oder `/etc/` fasst den Watchdog an - der
  einzige Treffer fuer "watchdog" unterhalb `/etc` ist `majestic.yaml` selbst
* `/sys/class/watchdog/watchdog0/` existiert, aber der Timer laeuft nicht

Auf diesem SoC startet der Watchdog-Timer erst, wenn `/dev/watchdog` geoeffnet
wird. Wird er nie geoeffnet, ist er nie scharf.

### Konsequenz

Jeder der drei beobachteten Hardlocks (2026-09-22, Runden 4 und 6, dazu der
Freeze vom 2026-09-21) endete damit, dass ein Mensch den Stecker ziehen musste.
Mit majestics Verhalten haette sich die Kamera nach 15 Sekunden selbst
zurueckgesetzt. Fuer eine Ueberwachungskamera an einer schwer zugaenglichen
Stelle ist das der Unterschied zwischen einer Stoerung und einem Totalausfall.

Das macht den Watchdog **nicht** zu einer Loesung der Ursache - ein Geraet, das
sich alle paar Stunden selbst neu startet, ist immer noch kaputt. Aber es ist
eine Verhaltensabweichung gegenueber majestic, die Machino unbemerkt eingefuehrt
hat, und sie hat genau in der Situation zugeschlagen, fuer die der Watchdog da
ist.

### Vorschlag (nicht umgesetzt)

`/dev/watchdog` beim Start oeffnen, `timeout` aus der Config setzen (Default 15 s
wie majestic), aus dem bestehenden Lifecycle-Timer fuettern und bei einem
geordneten Shutdown mit dem Magic-Close-Zeichen `V` sauber entschaerfen.

Zwei Dinge sind dabei heikel und gehoeren vor die Umsetzung, nicht danach:

1. **Ein Fehler im Fuetter-Pfad startet die Kamera alle 15 s neu.** Der Feed
   muss aus einem Pfad kommen, der nachweislich laeuft, solange der Daemon
   gesund ist - nicht aus einem Thread, der bei Last verhungern kann.
2. **Ein Watchdog verdeckt Fehler.** Genau die Forensik, die uns heute gefehlt
   hat, waere nach einem automatischen Reset ebenfalls weg. Sinnvoll ist er
   deshalb erst zusammen mit einer Spur, die einen Reset ueberlebt.

## NEU 2026-09-22: Der Relay erzwingt `Connection: close` und treibt die Box an ihr Socket-Budget

**Schwere: hoch.** Erster harter Messwert fuer Ressourcenknappheit an der Stelle,
an der die Hardlocks auftreten.

### Der Mechanismus

`src/app/http/http_parse.cpp` setzt beim Weiterreichen an busybox hart:

```cpp
out += "Connection: close\r\n";
```

Gemessen am laufenden Geraet:

| Pfad | Request 1 | Request 2 auf demselben Socket |
|---|---|---|
| nativ (`/api/v1/state`) | 200, `Connection: keep-alive` | **200** - Reuse funktioniert |
| relayed (`/a/main.js`) | 200, `Connection: close` | **nichts**, Socket weg |
| relayed CGI | 200 | **nichts**, Socket weg |

Machinos eigener Code kann Keep-Alive und gewaehrt es. Der **Relay-Pfad schliesst
immer** - und zwar auf beiden Seiten: eine Verbindung Browser->Machino und eine
Verbindung Machino->busybox, beide landen danach in TIME_WAIT.

### Die Zahlen

Ein einziger Seitenaufbau der Live-Seite zieht **38 Assets**. Ueber den Relay
kostet das rund **76 TCP-Verbindungen** statt der ~6, die ein Browser mit
Keep-Alive brauchen wuerde. Jede haengt anschliessend 60 s in TIME_WAIT
(`tcp_fin_timeout = 60`).

Das Budget dieser Kamera:

```
tcp_max_tw_buckets = 512
tcp_max_orphans    = 512
somaxconn          = 128
tcp_mem            = 483 645 966 Seiten
MemTotal           = 42 816 kB
```

Gemessen nach **einem** synthetischen Seitenaufbau bei sechs laufenden
Medien-Clients:

```
netstat TIME_WAIT      = 213
sockstat tw            = 183
TW (kumuliert)         = 1744
TCPTimeWaitOverflow    = 2      <-- das Limit wurde tatsaechlich ueberschritten
MemFree                = 1436 kB
```

Im selben Burst bekam ein Asset (`/a/mj-luma.js`) **keine Antwort**, ohne dass
Machino eine Zeile geloggt haette - der Ausfall lag unterhalb der Anwendung.

### Was das bedeutet und was nicht

**Belegt:** Der Relay vervielfacht die Verbindungszahl pro Seitenaufbau um den
Faktor ~6 und treibt eine 42-MB-Kamera mit einem 512er TIME_WAIT-Budget
nachweislich ueber dieses Budget. Ein Request blieb unbeantwortet.

**Nicht belegt:** Dass dies die Hardlocks verursacht. `TCPTimeWaitOverflow` ist
normalerweise ein geordneter Vorgang - der Kernel verwirft dann einfach
TIME_WAIT-Eintraege. Es ist ein Beleg fuer Druck an der richtigen Stelle, keine
Ursachenkette.

### Vorschlag (nicht umgesetzt)

Keep-Alive auch auf dem Relay-Pfad halten: die Upstream-Verbindung zu busybox
wiederverwenden statt pro Request neu aufzubauen, und die Downstream-Verbindung
offen lassen, wenn der Client sie offen haben will. Das wuerde die
Verbindungszahl pro Seitenaufbau von ~76 auf ~12 druecken.

Der Kommentar im Code begruendet das verbatim-Weiterreichen der Header damit,
dass "haserl/CGI see the real request" - das bleibt richtig und ist von dieser
Aenderung nicht betroffen. Betroffen ist nur der Verbindungslebenszyklus.
