# Beweislage — was wovon gestützt ist

2026-09-23 (AP32). Eine Seite, die für jede tragende Aussage des Projekts sagt,
**wie gut sie belegt ist**. Sie existiert, weil in diesem Projekt mehrfach eine
Hypothese für kurze Zeit wie ein Befund aussah und einmal eine Zahl
veröffentlicht wurde, die eine spätere Messung widerlegt hat.

| Stufe | Bedeutung |
|---|---|
| **BELEGT** | direkt gemessen, wiederholbar, mit Gegenprobe |
| **STARK GESTÜTZT** | mehrere übereinstimmende Messungen, aber der Mechanismus ist nicht isoliert |
| **HYPOTHESE** | plausibel, nicht gemessen |
| **WIDERLEGT** | wurde behauptet und ist durch eine spätere Messung gefallen |
| **NICHT GETESTET** | steht offen, niemand hat es versucht |
| **PENDING_PHYSICAL** | braucht Hardware, die gerade nicht verfügbar ist (fast immer: einen Kaltstart) |

---

## Medienpfad

| Aussage | Stufe | Beleg |
|---|---|---|
| RTSP MAIN und SUB streamen H.264 von der T40NN | BELEGT | M2/M8, ffmpeg-dekodiert; `OPTIONS` 200 und `DESCRIBE` 401 heute erneut gemessen |
| WebRTC MAIN bei ~100 ms | BELEGT | Hardwareabnahme `1b7225b` |
| MSE bei ~276 ms nach SPS-VUI-Fix und Link-Fix | BELEGT | `machino-latenz-befund`; die Einzelbeiträge sind dort mit ihren Messungen genannt |
| **Die Kamera verursacht kein stetiges MSE-Puffern** | BELEGT | 3 Clients × 10 min, 648 MB, Drift −27…−12 ppm, 0 Frameverlust (`ap15-mse-latency.md`) |
| Die „1 s" MSE-Latenz ist das Toleranzband des Upstream-Players | BELEGT | `LIVE_EDGE = 1.0` im Quelltext plus die Driftmessung, die die Serverseite ausschließt |
| Der Muxer erzeugt Bytes, die ein echter Decoder annimmt | BELEGT | `tools/mse-replay`: echte Frames, Chrome 153, eine Buffered Range, 1920×1080, auch über einen 2-s-Aussetzer |
| Chrome/Edge 153 handeln H.264 aus | BELEGT | echter Offer aufgenommen, als Fixture im Repo, pt 102 |
| Der neue Build spielt auf der Kamera weiterhin WebRTC | **PENDING_PHYSICAL** | pt wechselt 41 → 102; nicht auf Hardware nachgewiesen |

## Der Hardlock

| Aussage | Stufe |
|---|---|
| Auslöser ist der erste authentifizierte `GET /cgi-bin/live.cgi` | **STARK GESTÜTZT** — im Mitschnitt gepinnt, aber nur einmal beobachtet |
| Es ist ein Hänger, keine Panik | BELEGT — `kernel.panic = 20`, die Box startet nie neu |
| Warm-Install ist Voraussetzung | **WIDERLEGT** — trat nach einem echten Kaltstart auf |
| Relay-Keep-Alive behebt ihn | **WIDERLEGT als Fix** — es behebt einen echten TCP-Churn-Defekt, mehr nicht |
| „90 % weniger TIME_WAIT" | **WIDERLEGT** — ungleiche Messfenster (9 min/3 Browser gegen 2 min/frischer Boot) |
| Der Watchdog hätte ihn abfangen müssen | **WIDERLEGT** — es lief gar keiner: nichts hält `/dev/watchdog`, und der laufende Build ist älter als AP4 |
| Die UART-Stille war ein ruhiger Kernel | **WIDERLEGT** — `console_loglevel` stand auf 0 |
| Ursache | **NICHT GETESTET / offen** — kein Reproducer in ~12 synthetischen Versuchen |

## Ressourcen

| Aussage | Stufe |
|---|---|
| Kein Leck in FDs, Threads, Sockets | BELEGT — 25 FDs / 12 Threads über Stichproben konstant, 0 CLOSE_WAIT, 0 Zombies, alle Paare statisch geprüft (`ap29`) |
| Kein monotones Heap-Leck | **WIDERLEGT als Aussage — der Befund ist NICHT ENTSCHIEDEN.** Über Zyklusmittel steigen die COLD_IDLE-Böden geradlinig: 4794 → 4906 → 4953 → 5063 → 5103 kB, ~77 kB je Zyklus über 5 Zyklen. Allokator-Retention und echtes Leck sind beide mit den Daten vereinbar; fünf Zyklen trennen ein Plateau nicht von einer Geraden (`ap25-soak.md`) |
| Der OOM vom 2026-09-22 kam vom `fork()` bei lebendem IMP | **STARK GESTÜTZT** — A/B belegt (ACTIVE → nächster Init scheitert, COLD_IDLE → nicht), aber der Fork selbst ist nicht isoliert, und *warum* bleibt unerklärt. Der Bericht sagt das selbst. |
| Die Fünffach-Retry-Schleife hat den OOM verstärkt | BELEGT — entfernt, `init_retries` muss 0 bleiben und tut es |

## Plattform

| Aussage | Stufe |
|---|---|
| Kein IR-Cut, keine belastbare Pinquelle für t40 | BELEGT — DT, `/sys`, Stock-Config und die wiki-generierte Tabelle, alle vier leer |
| Kein ADC vorhanden | **WIDERLEGT** — es gibt einen SAR-ADC (`sadc@10070000`, `/dev/ingenic_adc_aux_*`); er fehlt nur in IIO/hwmon. An der Pin-Schlussfolgerung ändert das nichts |
| Kein Recording-Ziel | BELEGT — kein Blockgerät außer SPI-NOR, SD-Host ohne Karte, 4,6 MB frei |
| Kein Talkback | BELEGT — `spk_gpio = -1`, kein externer Codec |
| Audio-Capture ist unmöglich | **HYPOTHESE, und zwar eine schwache** — der Pfad existiert vermutlich (`aic_enable=1`, innerer Codec hochgefahren). Nicht gebaut, weil unbekannt ist, ob ein Mikrofon dranhängt |
| NNA nutzbar | **WIDERLEGT für diesen Stand** — Treiber da, aber kein `nmem=`, keine SDK-API, keine Bibliothek, kein Modell |
| Der MIPS-Build ist reproduzierbar | BELEGT — derselbe Commit zweimal, bitidentisch |

## Sicherheit

| Aussage | Stufe |
|---|---|
| Header-Smuggling über das Relay war möglich | BELEGT — gegen den echten Parser vorgeführt, dann behoben |
| Der RTSP-Lesepuffer war unbegrenzt | BELEGT — im Code gezeigt, dann begrenzt |
| Path Traversal war nicht ausnutzbar | BELEGT — 400 gemessen, aber **von busybox**; seit AP30 auch von uns |
| Der laufende Daemon hat die Fixes **nicht** | **BELEGT (2026-09-23, Audit)** — gemessen, nicht angenommen: bare-LF-Header und `%2e%2e`-Traversal ergeben beide **401**, nicht 400. Der Parser nimmt die Requests an und reicht sie an die Auth weiter. Laufender Prozess ist `c1edd92`, die exe ist `(deleted)` — die Platte trägt schon `341a8d4` |
| Die Fixes wirken im neuen Build auf der Kamera | **PENDING_PHYSICAL** — verlangt die Ablösung; der Gegentest (400 statt 401) ist derselbe und läuft fern, ohne Hardware am Platz |

---

## Überholte Dokumente

Aufgehoben als Herleitung, oben mit Hinweis versehen, **nicht** als Lagebericht
zu lesen:

* `dropin-gaps.md` — nannte `/api/v1/image` „the only gap"; seit AP10 implementiert
* `webui-contract-matrix.md` (AP4) — `/stream.mjpeg` antwortet seit AP24 mit 501
* Gedächtnisnotiz `machino-dropin-luecken` — dieselbe überholte Kernaussage
* Gedächtnisnotiz `machino-stock-audit` — „kein NNA" ist präzisiert: Treiber da, Rest fehlt

Der aktuelle Stand steht in **`dropin-matrix.md`**, die offenen Hardwarepunkte
in **`pending-physical.md`**.
