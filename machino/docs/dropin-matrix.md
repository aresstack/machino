# Drop-in-Matrix — Machino als Majestic-Ersatz

Stand 2026-09-23. Erzeugt mit `tools/dropin-matrix.ps1`, das jeden Endpunkt
anspricht, den die **ausgeführte** Stock-JS anfasst (`grep` über
`/c/tmp/majestic-webui/www/a/*.js`), dazu jede Route, die Machino selbst führt.

## Lesehinweis: gemessen wurde gegen `c1edd92`

Der Daemon auf der Kamera ist **`c1edd92`**; auf der Platte liegt `341a8d4`,
und HEAD ist neuer als beides. Ein Warmstart ist der dokumentierte
Hardlock-Auslöser, deshalb wurde nicht abgelöst. Zwei Zeilen unten sind davon
betroffen und ausdrücklich so markiert.

Klassen: `PASS` · `SUPPORTED_DIFFERENTLY` · `INTENTIONALLY_UNSUPPORTED` ·
`PENDING_PHYSICAL` · `FAIL`

---

## 1. Machino-eigene API

| Endpunkt | Antwort | Klasse |
|---|---|---|
| `/api/v1/config` | 200, 1213 B | PASS |
| `/api/v1/config.json` | 200, 680 B (majestic-förmig) | PASS |
| `/api/v1/config.schema.json` | 200, 3792 B — `video0 sensor image latency performance ai` | PASS |
| `/api/v1/capabilities` | 200, 3809 B | PASS |
| `/api/v1/state` | 200, 1519 B | PASS |
| `/api/v1/telemetry` | 200, 1802 B | PASS |
| `/api/v1/sources` | 200, 340 B | PASS |
| `/api/v1/reset` | 404 `{"ok":false,...}` ohne `?key=` — vertragsgemäß | PASS |
| `/api/v1/get` | 404 „no such key" ohne Schlüssel | PASS |
| `/api/v1/events` | SSE, nicht im Sweep (hält offen) | PASS (M6 abgenommen) |
| `/api/v1/image` | **404 (Relay)** — die Route existiert erst ab AP10, der laufende Build hat sie nicht | PENDING_PHYSICAL |
| `/api/v1/osd`, `/api/v1/osd/image` | 404 mit Machinos JSON | INTENTIONALLY_UNSUPPORTED (AP13: kein zeichnendes Backend; 404 ist laut Port-Vertrag gültig) |
| `/snapshot`, `/snapshot.jpg`, `/api/v1/snapshot` | 501 `unavailable` | INTENTIONALLY_UNSUPPORTED (JPEG-Wedge) |
| `/stream.mjpeg`, `/api/v1/stream.mjpeg`, `/stream` | **war 200 und lieferte nie ein Bild** → jetzt 501, siehe unten | FAIL → behoben, PENDING_PHYSICAL |
| `/setup`, `/setup.html` | 404 — die Kamera ist claimed | PASS (AP10-Vertrag) |

## 2. Streams

| Endpunkt | Antwort | Klasse |
|---|---|---|
| `WS /ws/video?stream=0` | offen, `{"type":"init","codecString":"avc1.640033","width":1920,...}` | PASS |
| `WS /ws/video?stream=1` | offen, `avc1.640033`, 640×360 | PASS |
| `WS /ws/webrtc?stream=0` | offen (signalisiert erst nach dem Offer) | PASS |
| `WS /ws/logs` | offen, liefert sofort echte syslog-Zeilen | PASS |
| RTSP MAIN / SUB | nicht im HTTP-Sweep; hardwareabgenommen (M2/M8), Auth-Default seit AP7 **an** | PENDING_PHYSICAL (AP7-Umstellung) |
| `WS /ws/upgrade` | **404 im laufenden Build**; ab AP21 angenommen und mit `ERROR: cannot start sysupgrade` beantwortet | PENDING_PHYSICAL |
| `WS /ws/analytics` | 404 | INTENTIONALLY_UNSUPPORTED (AP19) |
| `WS /ws/pins` | 404 | INTENTIONALLY_UNSUPPORTED (AP18) |

## 3. Von der Stock-WebUI erwartete Endpunkte, die Machino nicht führt

Alle fallen durch die Front-Door an busybox und kommen als HTML-404 zurück.
Das ist **nicht** still: jeder dieser Aufrufer hat einen dokumentierten
Rückfall, der in AP18/AP19 nachgelesen und zitiert ist.

| Endpunkt | Wozu | Rückfall der Seite | Klasse |
|---|---|---|---|
| `/api/v1/gpio` | Pin-Editor | „No pad list, no map … unhide them" → Zahlenfelder | INTENTIONALLY_UNSUPPORTED |
| `/api/v1/pinmux` | Pad-Tabelle des Chips | dito | INTENTIONALLY_UNSUPPORTED |
| `/api/v1/calibration/{coverage,pair,peer}` | Kennzeichen/Peer-Homographie | Feature bleibt aus | INTENTIONALLY_UNSUPPORTED |
| `/api/v1/analytics/day` | Bewegungsspur über Aufnahmen | „this camera is too old to keep an index" | INTENTIONALLY_UNSUPPORTED |
| `/api/v1/peers` | Flotten-Umschalter | Upstream: 404 ⇒ Schalter bleibt verborgen | INTENTIONALLY_UNSUPPORTED |
| `/api/v1/records/{resume,standdown}` | Rekorder-Steuerung | `records.enabled` ist nicht wahr ⇒ nie aufgerufen | INTENTIONALLY_UNSUPPORTED |
| `/api/v1/live` | Live-Hilfsdaten | — | INTENTIONALLY_UNSUPPORTED |
| `/api/v1/outgoing.json` | Ausgehende Ziele | — | INTENTIONALLY_UNSUPPORTED |
| `/metrics/records`, `/metrics/night` | Rekorder-/Nacht-Telemetrie | 404 ⇒ `{absent:true}`, ein definierter Zustand | INTENTIONALLY_UNSUPPORTED |
| `/image.jpg`, `/mjpeg` | JPEG-Kanal | `jpeg.enabled !== true` ⇒ nie gepollt (AP14) | INTENTIONALLY_UNSUPPORTED |
| `/upload` | Firmware-Upload | AP21: Firmware ist Sache von `sysupgrade` | INTENTIONALLY_UNSUPPORTED |

## 4. Relayed busybox-CGIs (die Stock-WebUI selbst)

| Endpunkt | Antwort | Klasse |
|---|---|---|
| `/cgi-bin/dashboard.cgi` | 200, 18 877 B | PASS |
| `/login.html` | 200, 11 589 B | PASS |
| `/metrics` | 200, node-exporter-Format | PASS |
| `/cgi-bin/j/files.cgi` | 200, Verzeichnisliste | PASS |
| `/cgi-bin/j/sdcard.cgi` | 200 `{"present":false,"health":"absent"}` | PASS (ehrlich) |
| `/cgi-bin/j/recordings.cgi` | 200 `{"error":"no recording path is configured"}` | PASS (ehrlich) |
| `/cgi-bin/j/pulse.cgi`, `logmeta.cgi`, `network.cgi`, `fw-latest.cgi`, `save.cgi`, `run.cgi` | 200 | PASS |
| `/cgi-bin/j/ptz.cgi` | 405 „Use POST to move the camera." | PASS (busybox' eigene Antwort) |
| `/cgi-bin/j/download.cgi` | 404 „not a file" ohne Parameter | PASS |
| `/cgi-bin/j/time.cgi` (Knopf **Synchronize now**) | **504 nach 6,2 s** | SUPPORTED_DIFFERENTLY, siehe unten |
| `/cgi-bin/j/time.cgi?set=…` (Knopf **Set from browser**) | 200 in 38 ms, „Camera clock set from browser." | PASS |

---

## Die zwei Befunde dieses Durchgangs

### FAIL → behoben: `/stream.mjpeg` meldete Erfolg und lieferte nie ein Bild

Die Route akzeptierte **bedingungslos**: 200, Multipart-Kopf, und dann ein
Stream, der nie ein Frame tragen kann, weil die JPEG-Einheit gar nicht
konfiguriert ist. Schlimmer: MJPEG-Clients sind vom Idle-Timeout ausgenommen
(`!c.sse && !c.mjpeg && …`), die Verbindung blieb also **unbegrenzt** offen und
belegte einen von 16 Client-Plätzen.

`/snapshot` hat für dieselbe Lage immer 501 geantwortet. Jetzt tut es
`/stream.mjpeg` auch — geprüft über `unit_configured(UNIT_JPEG)`, dieselbe
Flagge, die `snapshot()` `unsupported` zurückgeben lässt.

### SUPPORTED_DIFFERENTLY: „Synchronize now" läuft in ein 504

Gemessen, nicht vermutet:

```
time.cgi direkt auf der Kamera     41 s, dann
                                   {"result":"danger","message":"Synchronization failed!"}
stderr                             ntpd: bad address '0.pool.ntp.org' (x4)
Machinos Relay                     relay_timeout_ms = 6000 (INAKTIVITÄT)
über die Front-Door                504 nach 6,2 s, reproduzierbar
```

Die Ursache ist **kein** Machino-Fehler: `ntpd -n -q -N` scheitert 41 Sekunden
lang an DNS, weil diese Kamera keine Route nach draußen hat
(`t40nn-uhr-ohne-rtc`). Ein CGI, das 41 s lang schweigt, ist von einem
hängenden nicht zu unterscheiden — genau dafür ist die Inaktivitätsgrenze da,
und sie global über 41 s zu heben hieße, jedem hängenden CGI eine Verbindung
für 41 s zu schenken. Auf einer Kamera **mit** Gateway antwortet `ntpd`
üblicherweise in ein bis drei Sekunden und der Fall tritt nicht ein.

Geändert wurde deshalb nur die Antwort: sie nennt jetzt Grenze **und** Pfad —
`OpenIPC WebUI backend sent nothing for 6000 ms (GET /cgi-bin/j/time.cgi)` —
statt eines nackten „timed out", das von einer hängenden Kamera nicht zu
unterscheiden war.

Der **funktionierende** Weg, die Uhr zu stellen, geht durch: „Set from browser"
in 38 ms, dazu ONVIF `SetSystemDateAndTime` aus AP12.

---

## Schema und Runtime zeigen dasselbe Bild

Stichprobe gegen die Regel „Schema und Runtime müssen dasselbe Capability-Bild
zeigen":

| | Schema (`config.schema.json`) | Runtime |
|---|---|---|
| `video0`, `sensor`, `image`, `latency`, `performance`, `ai` | vorhanden | PATCH akzeptiert sie |
| `video1` | **nicht** im Schema dieser Kamera | SUB existiert und streamt (`/ws/video?stream=1`) |
| `nightMode`, `audio` | nicht im Schema | gemeldet in `config.json`, Schreibversuch → 403 mit Grund |
| `records`, `analytics`, `peers` | nicht im Schema | nicht gemeldet, Schreibversuch → 400 `unknown_field` |

Die `video1`-Zeile ist der einzige Unterschied und **beabsichtigt**: AP9 hat
die Sektion mit `x-reload: "pipeline"` eingeführt; dass sie im Schema dieser
Kamera fehlt, liegt daran, dass der laufende Build älter als AP9 ist. Nach der
Ablösung ist das nachzuprüfen — es steht in `pending-physical.md`.

## Alte Workarounds, die nicht mehr gelten

* „Das Relay verdoppelt Header" — **nie passiert.** Der Verdacht kam aus einem
  Analysefehler meinerseits: der CGI-Kopf endet mit `\n\n`, und ein
  `IndexOf("\r\n\r\n")` lieferte −1. Die Rohbytes sind korrekt.
* „90 % weniger TIME_WAIT" — bleibt **zurückgezogen** (ungleiche Messfenster).
* „Warm-Install ist Voraussetzung für den Hardlock" — **widerlegt**.
* „`/api/v1/image` fehlt" (aus `machino-dropin-luecken`) — seit AP10 im Code,
  auf der Kamera noch nicht aktiv.

## Stand gegen „Fertig wenn"

> Eine einzige aktuelle Drop-in-Matrix beschreibt den tatsächlichen Stand
> vollständig.

Diese Datei ist sie. 58 Endpunkte, jeder klassifiziert, keiner mit einem
stillen 404 bei einer Funktion, die die Stock-WebUI wirklich braucht — jeder
nicht bediente Endpunkt hat einen im Upstream-Quelltext nachgelesenen
Rückfall, und wo das nicht reichte (`/ws/upgrade`, `nightMode`, `audio`,
`jpeg`) antwortet Machino inzwischen mit einer Begründung statt mit Schweigen.

**Ein FAIL gefunden und behoben** (`/stream.mjpeg`), **ein
SUPPORTED_DIFFERENTLY** mit belegter Fremdursache (`time.cgi`/NTP ohne
Gateway).
