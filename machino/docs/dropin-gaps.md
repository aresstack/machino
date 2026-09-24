
## isp_* Gauges: machino hat Nullen veroeffentlicht, die niemand gemessen hat

Aufgefallen ueber eine falsche Black-Frame-Warnung der unveraenderten
OpenIPC-WebUI auf einem Android-Telefon, waehrend das Livebild sichtbar lief
und sich bewegte.

### Was die WebUI tut, und warum sie NICHT der Fehler ist

`/var/www/a/video-check.js` hat fuer "die Kamera sieht nichts" zwei
Beweisquellen. Die ISP-Quelle:

    ispBlind(v): braucht isp_avelum UND isp_exposureismax.
                 Fehlt eines, ist die Antwort null -- "weiss nicht".

Und eine bildbasierte aus einem Luma-Histogramm, das im Browser per
`drawImage(video)` + `getImageData` gewonnen wird. Gefeuert hat allein die
zweite; der Meldungstext sagt das sogar selbst ("This camera does not report
its own exposure").

### Der Punkt, der die Sache entscheidet

**Majestic liefert `isp_exposureismax` auf Ingenic ebenfalls nicht.** Nachgesehen
im Fixture des Upstream-Klons, `tests/fixtures/metrics-ingenic.txt`: die Datei
enthaelt isp_exptime, isp_again, isp_dgain, isp_bgain, isp_rgain, isp_avelum,
isp_afmetrics, isp_tgain -- und kein isp_exposureismax. Nur die HiSilicon- und
SigmaStar-Fixtures haben es. Der Kommentar in video-check.js nennt den Ingenic
T40 sogar ausdruecklich als Kamera, die diese Frage nicht beantworten kann.

Daraus folgt zweierlei, und das zweite ist unbequem:

  * `isp_exposureismax` in machino zu implementieren wuerde vom Majestic-
    Verhalten ABWEICHEN, nicht zu ihm hin. Es bleibt draussen.
  * Diese Fehlwarnung ist KEINE Drop-in-Luecke. Mit dem originalen Majestic auf
    dieser Kamera erschiene sie genauso. Sie ist ein Fall der Browser-
    Heuristik auf einem Android-Geraet, dessen Compositor den dekodierten
    Frame nicht in ein Canvas zurueckgibt.

### Was dabei aber WIRKLICH gefunden wurde

Mit laufender Pipeline auf der Kamera gemessen (RTSP-Zug, 301 Frames):

    exposure.available  true
    luma 53   target 50   stable true          <- IMP_ISP_Tuning_GetAeScenceAttr
    integration_time 0   analog_gain 0
    digital_gain 0   total_gain_db 0           <- IMP_ISP_Tuning_GetAeExprInfo
    /metrics: isp_avelum 52  isp_again 0  isp_dgain 0  isp_exptime 0

Der zweite IMP-Aufruf liefert auf dem T40NN nichts. `ExposureReadback` hatte
aber nur EIN `available`-Flag fuer beide Aufrufe, also wurden die
uninitialisierten Nullen als Messwerte veroeffentlicht.

`isp_exptime 0` ist eine Aussage ueber den Sensor. Majestic sagt dort 78964.
Eine Null, die niemand gemessen hat, ist schlimmer als ein fehlender Wert:
dem Leser ist nicht anzusehen, dass sie fehlt. Der Compat-Layer hatte das
sogar schon richtig vorgesehen -- "an absent value must not print as 0" steht
woertlich ueber `tel_num` -- nur kam die Null bereits aus der Serialisierung.

Behoben: `have_scene` und `have_expr` getrennt. Was nicht gelesen wurde, ist
jetzt `null` in der Telemetrie und fehlt in `/metrics`, genau wie bei Majestic.

### Weiterhin offen

Majestic liefert auf Ingenic vier Gauges, die machino gar nicht kennt:

    isp_bgain   isp_rgain   isp_tgain   isp_afmetrics

Und warum `GetAeExprInfo` auf dem T40NN leer zurueckkommt, ist nicht geklaert.
Beides sind echte Drop-in-Luecken, beide beruehren den ISP-Lesepfad und keine
davon hat mit der Black-Frame-Warnung zu tun.
ature |
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
