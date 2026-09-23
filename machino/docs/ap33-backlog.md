# AP33 — Dead Code / Backlog / Release-Blocker

2026-09-23, letzter Sweep. Alles Offene aus AP0–AP32 in **genau vier Gruppen**.

## Der Sweep selbst

| Geprüft | Befund |
|---|---|
| TODO / FIXME / HACK / XXX | **keine** im ausgelieferten Quelltext (`src/`, `tests/`, `openipc/`, `tools/`) |
| Deaktivierte Tests | **keine** — die Treffer auf „disabled"/„skip" sind Testnamen und Zusicherungen *über* übersprungene Watchdog-Fütterungen |
| Feature-Flags | **keine** außer `#ifdef SO_REUSEPORT`, einer Portabilitätsklammer |
| Diagnose-/Gerüstcode | **keiner** |
| Temporäre Telemetrie | **keine** — alle fünf `ws_video_*`-Zähler werden gesetzt **und** in `/api/v1/telemetry` gemeldet |
| Alte Retry-/Fork-/Restart-Pfade | **keine** (AP27) |
| Ungenutzte Funktionen | **eine gefunden und entfernt**: `ws::close_frame()` war deklariert, definiert und nirgends aufgerufen |
| Tote Compatibility-Shims | keine; die Compat-Schicht wird vollständig von `majestic_config`/`majestic_schema`/`majestic_post_to_native` benutzt |
| Unerreichbare Fehlerpfade | keine gefunden; die Unwind-Pfade in `stop_unit_locked` sind seit AP2.6 an erworbene Zustände gebunden statt blind |

Zu `close_frame()`: der Server beendet einen WebSocket durch Schließen der
TCP-Verbindung, und jeder Konsument im Vertrag behandelt das über `onclose`.
Ein RFC-6455-Close-Handshake wäre drei Zeilen — aber eine
**Verhaltensänderung**, keine Aufräumarbeit, und steht deshalb unten unter
POST_RELEASE statt als ungenutzter Code im Baum.

---

# RELEASE_BLOCKER

**Keiner.**

Zwei Dinge sind nahe dran und sind es bei genauem Hinsehen nicht:

* **Die beiden AP30-Sicherheitsfixes sind nicht auf der Kamera.** Header-
  Smuggling und der unbegrenzte RTSP-Puffer sind in HEAD behoben, aber der
  laufende Prozess ist `c1edd92`. Das blockiert nicht den *Release* — das
  Artefakt enthält die Fixes — sondern verlangt, dass die Kamera abgelöst
  wird. Steht als S1–S3 in `pending-physical.md`.
* **Der steigende COLD_IDLE-Boden** (~77 kB/Zyklus, `ap25-soak.md`). Kein
  Blocker, weil `MemAvailable` über den ganzen Lauf bei ~20,8 MB blieb und
  Allokator-Retention genauso zu den Daten passt wie ein Leck. Aber die
  wichtigste offene Sache nach dem Hardlock, und mit einem konkreten nächsten
  Schritt versehen.

---

# POST_RELEASE

| # | Sache | Woher |
|---|---|---|
| P1 | **Den COLD_IDLE-Anstieg entscheiden.** 15–20 Zyklen fahren; flacht die Kurve ab, ist es Retention, bleibt sie gerade, ist es ein Leck. Dann MSE/RTSP/WebRTC einzeln, um den Pfad einzugrenzen. Erst danach im Code suchen — die statische Paarungsprüfung hat nichts gefunden. | AP25 |
| P2 | **Hardlock-Ursache.** Auslöser gepinnt, kein Reproducer. Instrumentierung liegt bereit (Konsole spricht, begrenzter PCAP-Ring). | AP17 |
| P3 | **`rtsp.*` ins Schema.** `enabled`, `port`, `max_clients` sind live setzbar, aber die WebUI erreicht sie nicht — Upstream bietet sie an. Neue Funktion mit eigener Neustart-Semantik. | AP28 |
| P4 | **`lifecycle.idle_grace_ms` und `power.*` ins Schema.** Gleiche Klasse wie P3, geringerer Nutzen. | AP28 |
| P5 | **OSD-Backend.** Blockiert an einer Architekturentscheidung: TTF-Rasterer ja/nein, und die Bindetopologie des funktionierenden Videopfads. | AP13 |
| P6 | **Audio-Capture**, falls E2 (Mikrofon vorhanden?) positiv ausfällt. Braucht einen Opus-Encoder — zweite Fremdabhängigkeit, Architekturentscheidung. | AP20 |
| P7 | **RFC-6455-Close-Handshake**, falls gewünscht. Drei Zeilen, Verhaltensänderung. | AP33 |
| P8 | **Wer setzt `kernel.printk` beim Booten auf 0?** Die Binärsuche ist auf dem PC am Rootfs-Image zehn Sekunden Arbeit, auf der Kamera minutenlange Last. | AP17 |
| P9 | **RTSP-Auth-Widerspruch klären.** `c1edd92` verlangt Auth, obwohl sein Default `false` ist und keine Config sie setzt. Nach der Ablösung auf HEAD ist es ohnehin erklärt (`enabled = true`) — die Frage ist, ob am alten Stand etwas übersehen wurde. | AP24 |

---

# PENDING_PHYSICAL

Vollständig in `pending-physical.md`, hier nur die Gruppen und ihre Zahl:

| Gruppe | Zahl | Kern |
|---|---|---|
| **A** — beim Kaltstart, risikolos | 9 | printk, Watchdog scharf, `init_retries` 0, Baseline, `ws_video_*`, Driftmessung gegen den neuen Build, `/ws/upgrade`-Ablehnung, Snapshot-Kachel, Audio-Panel |
| **S** — Sicherheit, erst nach Ablösung | 3 | Header-Smuggling → 400, RTSP-Puffergrenze greift, Traversal-400 von Machino |
| **B** — mit Browser | 6 | **WebRTC spielt weiter (pt 102 statt 41)**, Latenzgate, `prft` im Stats-Panel, MSE-Band, RTSP-401 für Altclients, AP6–AP10-Laufzeitwege |
| **C** — gezielte Fehlerfälle | 5 | Rebind-Rollback, Watchdog feuert, Shutdown-Reaping, echter Install, Rollback-Pfad |
| **D** — `NEEDS_HUMAN` | 4 | Factory-Reset, `nmem=` in U-Boot, Modultausch, echter `sysupgrade` |
| **E** — Fragen an die Platine | 4 | IR-Cut vorhanden? Mikrofon? Lautsprecher? SD-Slot verdrahtet? |

**B1 ist der Punkt mit dem größten Rückrollwert** — er verschiebt die
ausgehandelte WebRTC-Nutzlast auf einem hardwareabgenommenen Pfad. Spielt
WebRTC nach dem Kaltstart nicht mehr, ist Commit `37d1619` das Erste, was
zurückgedreht wird; er ist isoliert.

---

# INTENTIONALLY_UNSUPPORTED

Jedes mit Beleg, keines aus Bequemlichkeit.

| Funktion | Grund | Doku |
|---|---|---|
| JPEG / Snapshot / MJPEG | Encoder-Anlage hängt den Daemon auf; `/snapshot` und `/stream.mjpeg` antworten 501 | `ap13-ap14.md` |
| OSD-Zeichnen | kein Schriftrasterer auf der Kamera und keiner im Binary; `/api/v1/osd` 404 ist laut Vertrag gültig | `ap13-ap14.md` |
| Night / IR-Cut / GPIO-Pins | keine Hardwarebelege und keine belastbare Pinquelle für t40 — es werden keine Pins erfunden | `ap18-night-gpio.md` |
| Recording | keine Karte im vorhandenen SD-Host; 4,6 MB Flash als einziges Ziel | `ap19-...md` |
| Analytics-Tagesindex | Index über Aufnahmen, die es nicht gibt | `ap19-...md` |
| Peers | Flotten-Umschalter; Upstream behandelt 404 selbst als „gibt es hier nicht" | `ap19-...md` |
| Audio / Talkback | `spk_gpio = -1`, kein externer Codec; Capture ungebaut, weil unbekannt ist, ob ein Mikrofon dranhängt | `ap20-...md` |
| NNA / Personenerkennung | Treiber da, aber kein `nmem=`, keine SDK-API, keine Bibliothek, kein Modell | `ap23-...md` |
| Firmware-Flashen | `sysupgrade` gehört das; `/ws/upgrade` lehnt mit Begründung ab | `ap21-update.md` |
| Kalibrierung | gehört im Upstream zu Kennzeichen/Peer-Homographie, nicht zur Kamera | `ap18-night-gpio.md` |
| ONVIF standardmäßig an | Code fertig, aber nie gegen einen echten ONVIF-Client gelaufen; eine halb antwortende Kamera ist schlechter als eine stille | `machino-onvif` |

---

## Regressionsstand

```
C++ Hosttests               2582 bestanden, 0 fehlgeschlagen
Installer-Shelltests          99 lokal / 103 CI, 0 fehlgeschlagen
streamerctl-Tests             29 bestanden, 0 fehlgeschlagen
check-linux-only              ok
sauberer Klon                 baut und besteht alles
MIPS-Build                    reproduzierbar, bitidentisch
```
