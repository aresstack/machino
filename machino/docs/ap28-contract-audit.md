# AP28 — Drop-in / API / Schema Contract Audit

2026-09-23, 30-Minuten-Prüfung. Keine neue Funktion begonnen.

Methodik: Das Schema wird **erzeugt**, nicht gepflegt — `majestic_schema()`
baut es aus den Capabilities. Also wurde es aus den **echten** Capabilities
dieser Kamera (`GET /api/v1/capabilities`, 3660 B) mit **HEADs** Code erzeugt
und gegen den PATCH-Dispatch und die Config-Schlüssel gestellt.

Wichtig für die Bewertung: die Stock-Seite rendert ausschließlich, was das
Schema hergibt (`mj-settings.js` → `treeOf().groups()`). **Eine Bedienung, hinter
der nichts steht, kann strukturell nicht entstehen** — die Gefahr liegt
ausschließlich in der Gegenrichtung.

---

## HEADs Schema für diese Kamera: 7 Sektionen, 33 Felder

```
video0:  fps[live] bitrate_kbps[live] gop[live]
video1:  fps[pipeline] bitrate_kbps[pipeline] gop[pipeline]
sensor:  fps[live]
image:   brightness contrast saturation sharpness hue hflip vflip
         anti_flicker ae_compensation highlight_depress backlight_comp
         white_balance_mode running_mode temporal_nr spatial_nr dpc defog
         — alle [live] und [x-live]
latency: profile gop framesource_buffers encoder_buffers consumer_queue_depth
performance: profile
ai:      enabled detector inference_fps
```

---

## MATCH

Jedes der 33 Schemafelder hat einen PATCH-Weg, und jeder dieser Wege endet in
einem Setter, der wirkt:

| Sektion | Weg |
|---|---|
| `video0` / `video1` | `majestic_post_to_native` → `video.0` / `video.1` → nativer `video`-Dispatch. Der `video.1`-Pfad war der AP6-Defekt (validiert, dann still verworfen) und ist behoben. |
| `sensor`, `image`, `latency`, `performance`, `ai` | direkt im nativen Dispatch |

`image` zusätzlich mit `x-live` → `POST /api/v1/image` (AP10), also Vorschau
beim Ziehen und nicht erst beim Speichern.

## MISSING_SCHEMA

Runtime kann es **live**, die WebUI erreicht es nicht. Nur über die API oder
durch Editieren von `machino.conf`.

| Schlüssel | Runtime | Warum nicht im Schema |
|---|---|---|
| `rtsp.enabled`, `rtsp.port`, `rtsp.max_clients` | live, mit atomarem Rebind (AP3) | `majestic_schema()` baut überhaupt keine `rtsp`-Sektion. **Der stärkste Fall:** die Stock-Seite bietet RTSP-Einstellungen an, Machino nicht. |
| `lifecycle.idle_grace_ms` | live | keine Sektion gebaut |
| `power.isp_performance`, `power.encoder_performance`, `power.cpu_performance` | live | keine Sektion gebaut; `performance.profile` deckt den gängigen Fall ab |

Das ist **kein** Fehlverhalten, sondern eine Lücke: nichts lügt, nichts ist
kaputt, aber ein Besitzer braucht SSH oder die API für etwas, das Upstream auf
der Seite anbietet. Eine `rtsp`-Sektion nachzurüsten ist eine neue Funktion mit
eigener Neustart-Semantik — AP28 verbietet das ausdrücklich, also steht es hier
und nicht im Code.

## MISSING_RUNTIME

**Keine.** Es gibt kein Schemafeld ohne Wirkung. Strukturell abgesichert:
`xreload_for()` weigert sich, Felder der Klassen `daemon_restart` und
`boot_only` überhaupt zu exponieren — „their POST only persists, and the stock
Apply (`killall -HUP majestic`) cannot restart the daemon to deliver them".

## INTENTIONALLY_UNSUPPORTED

Config-Sektionen ohne PATCH-Weg **und** ohne Schemaeintrag, jede mit Beleg:

| Sektion | Grund |
|---|---|
| `osd` (11 Schlüssel) | kein zeichnendes Backend (AP13); `/api/v1/osd` → 404, laut Port-Vertrag gültig |
| `jpeg`, `snapshot` | JPEG-Wedge; gemeldet, Schreibversuch → **403** mit Begründung (AP14) |
| `onvif` (3) | implementiert, aber **boot_only** — Default aus, weil nie gegen einen echten ONVIF-Client gelaufen (AP11) |
| `watchdog` (2) | boot_only (AP4) |
| `system.unsafe` | boot_only; hebt jede Auth auf, gehört nicht hinter einen Webschalter |
| `log`, `api`, `platform`, `board`, `board_profile_file`, `pipeline`, `telemetry` | Start-/Plattformparameter |
| `nightMode`, `audio` | gemeldet, Schreibversuch → 403 mit Grund (AP18/AP20) |
| `records`, `analytics`, `peers` | nie gemeldet, Schreibversuch → 400 `unknown_field` (AP19) |

`onvif` und `watchdog` sind der Grenzfall: beide **funktionieren**, sind aber
nur per Config und Neustart erreichbar. Sie stehen hier statt unter
MISSING_SCHEMA, weil sie boot_only sind — die vorhandene Regel exponiert genau
solche Felder bewusst nicht, und der Stock-Apply könnte sie nicht ausliefern.

## STALE_DOC — behoben

`docs/dropin-gaps.md` führte `/api/v1/image` noch als „the only gap that
today's Machino configuration actively walks into". AP10 hat den Endpunkt
implementiert. Das Dokument trägt jetzt oben einen Hinweis, dass es eine
Analyse von damals ist und der aktuelle Stand in `dropin-matrix.md` steht; die
Analyse selbst bleibt als Herleitung erhalten.

Ebenfalls geprüft und **nicht** veraltet: `ap6-ap8.md` (beschreibt `video1`
korrekt als `x-reload: "pipeline"`), `dropin-matrix.md` (kennzeichnet die zwei
Zeilen, die gegen den *laufenden* statt den ausgelieferten Build gemessen sind).

---

## 404/501 — nur wo gewollt

Aus dem AP24-Sweep, hier nur die Klassifizierung: **kein einziges stilles 404**
bei einer Funktion, die die Stock-WebUI wirklich braucht. Jeder nicht bediente
Endpunkt hat einen im Upstream-Quelltext nachgelesenen Rückfall, und die vier
Fälle, wo das nicht reichte — `/ws/upgrade`, `nightMode`, `audio`, `jpeg` —
antworten inzwischen mit einer Begründung statt mit Schweigen.

Ein FAIL dieser Klasse wurde in AP24 gefunden und behoben: `/stream.mjpeg`
antwortete 200 und konnte nie ein Bild liefern.
