# Der person-Detector (AP-NNA3): Architektur und Spec-Abgleich

Stand 2026-09-26. Ergänzt nna.md/nna-evidence.md um die AP3-Sicht: was der
GPT-Plan verlangte, was davon wie umgesetzt ist, und wo die Architektur
BEWUSST abweicht.

## Die eine große Abweichung — und warum sie die Spec selbst erzwingt

AP3 war um `dlopen(libvenus.so)` herum formuliert: In-Prozess-Runtime,
FrameView-Pumpe im DetectionService, latest_frame_slot. Die Spec sagt aber
auch: *"Welche konkreten Symbole geladen werden, richtet sich ausschließlich
nach AP1. Keine Fantasie-API erfinden."* Und AP1 hat gemessen: **es gibt für
einen musl-Prozess nichts zu dlopen-en.** Jede Venus-Bibliothek (Stock wie
öffentlich) ist uClibc — `NEEDED libc.so.0`, `ld-uClibc-mipsn8.so.0`. Ein
dlopen über die libc-Grenze ist keine Fleißfrage, sondern Loader-Realität.

Die umgesetzte Form ist deshalb der **Helferprozess** `machino-nna`
(uClibc, statisch gegen die öffentliche libvenus.a, Ingenics eigenes
Makefile_t40-Rezept), gesteuert über ein Zeilenprotokoll. Sie erfüllt jedes
Schutzziel der Spec härter als die dlopen-Form:

| Spec-Ziel | dlopen-Form | Helfer-Form (umgesetzt) |
|---|---|---|
| fehlende Runtime ≠ fatal | dlopen-Fehler abfangen | Binary fehlt → Backoff, machinod läuft unberührt weiter |
| Symbol fehlt ≠ Absturz | dlsym-Prüfliste | AUSGESCHLOSSEN: kein Venus-Symbol existiert in machinod (nachprüfbar am Binary) |
| inference fails ≠ Video-Ende | try/catch-artig | Helfer stirbt → SIGCHLD, Respawn nach Backoff; der Encoder-Prozess kann es nicht mitreißen |
| ein Binary, Runtime optional | ein Binary + .so | ein machinod + optionales Payload-Binary — dieselbe Eigenschaft |

## Spec-Abgleich Punkt für Punkt

- **§1 Architektur respektiert:** DetectionService, IDetector, Motion —
  unverändert; person ist der zweite Backend-Zweig in
  `IngenicPlatform::create_detector` (motion → ivs, person → NnaDetector).
- **§2/3 FrameView-Pumpe + latest_frame_slot:** NICHT gebaut, bewusst. Es
  gibt keinen In-Prozess-Producer-Thread, der einen Slot füllen könnte —
  der Analyse-FrameSource (IMP, Tiefe 1) hält hardwareseitig genau ein
  Bild, und der Detector holt gepacet das jeweils vorliegende. "Newest
  wins, kein wachsender Lag" gilt: Rückstand wird als übersprungene
  Perioden GEZÄHLT (`skipped`, jetzt echt befüllt), nie im Burst
  nachgeholt; Staleness ist durch die Periodendauer beschränkt.
  latest_frame_slot.hpp bleibt (getestet) für einen künftigen
  In-Prozess-FrameView-Backend liegen.
- **§4 eigene Analyse-Quelle:** eigener herunterskalierter FrameSource-
  Kanal (UNIT_AI, NV12, ~640 breit, Kadenz = inference_fps), nie der
  H.264-Strom. Die Modell-Eingangsgröße liest der HELFER aus dem Modell
  (`get_input(0)->shape()`) und letterboxt selbst — machinod muss sie
  nicht kennen.
- **§5 Frame-Lifetime:** bounded copy, dokumentiert im Code: der
  IMP-Frame wird nur für die Dauer eines fwrite in die tmpfs-Datei
  gehalten und VOR dem Warten auf den Helfer freigegeben (ein 2-Buffer-
  Kanal darf nicht über eine Inferenz hinweg verhungern). Keine rohen
  Pointer verlassen den Adapter.
- **§6/7 Runtime-Kapselung / optional:** Der Prozess IST die Kapsel.
  machinod kennt zwei Ports (INnaProcess, IAnalysisSource) und ein
  Zeilenprotokoll — kein `venus_*` im Core, keine ELF-Abhängigkeit.
- **§8 keine Stock-Blobs:** Helfer + Modell kommen vollständig aus dem
  öffentlichen Toolkit (CI build-nna-t40, beide Artefakte grün).
- **§9 Lifecycle:** start() spawnt und wartet NICHT auf den Modell-Load
  (poll() treibt Loading mit Deadline); stop() idempotent (quit + TERM);
  Teilinitialisierung rollt in den Down-Zustand zurück.
- **§10 Preprocessing NNA-freundlich:** Letterbox/NV12→RGBA macht
  Ingenics `common_resize` (AIP-Pfad der Runtime) im Helfer, nicht die
  MIPS-CPU in machinod.
- **§11 Decoder getrennt:** im Helfer (`generate_box` + Klassen-NMS aus
  Ingenics Sample — exakt das Layout, das AP1 für die nna1-YOLO-Köpfe
  belegt); machinod parst nur das neutrale det-Zeilenformat. Ein anderes
  Modell-Layout ist damit ein Helfer-Thema, nie ein machinod-Umbau.
- **§12 Boxen:** Helfer normalisiert auf [0,1] des Originalframes und
  clampt; machinod validiert NOCHMAL (parse_det_line verwirft
  Out-of-Range/deformierte Boxen — getestet).
- **§13 Scope person:** COCO-Klasse 0 → "person"; wenige weitere Namen
  gemappt, alles andere ehrlich "class<N>".
- **§14 Fabrik:** umgesetzt, mit konkretem Grund je fehlender
  Voraussetzung (Device, Helfer, Modell).
- **§15 Capability lügt nicht:** ai.person bleibt Unknown; die
  detectors-Liste bietet person erst bei Supported an (= nach AP-NNA6).
  Die "konkreten Gründe" wohnen dort, wo sie hingehören: KI-Seite der
  Kamera und NNA-Dialog des Tools zeigen jede Stufe einzeln.
- **§16 Config:** ai.enabled/detector/inference_fps/model_path — keine
  zweite Konfigdatei; model_path wird real verwendet (Fabrik prüft,
  Helfer lädt).
- **§17 Telemetrie:** skipped jetzt echt (bewusst ausgelassene Perioden),
  last/avg_infer_duration_ms NUR wenn gemessen (NNA-Helfer-Umlauf; IVS
  wartet auf Treiberergebnisse und bleibt ehrlich null) — beides in
  /api/v1/telemetry.
- **§18 Events:** das detection-Ereignis trägt jetzt die Box (x/y/w/h,
  nur wenn echt; Motion-Ganzbild bleibt boxlos → kompatibel).
- **§19 UI:** Dropdown speist sich aus der detectors-Liste (dynamisch);
  mehr UI absichtlich nicht.
- **§20 Fehlerisolation:** hosttests: Helfer fehlt/stirbt/schweigt/
  antwortet Müll → Timeouts/Fehler + Backoff-Respawn, Video nie beteiligt.
- **§21/22 Tests:** Fake-Prozess + Fake-Quelle; Pacing (5 fps),
  Rückstand→skipped ohne Burst, tote/fehlende Helfer, Malformed-Zeilen,
  Box-Validierung, Detector-Wechsel motion→person→motion auf
  Service-Ebene (Basis blinzelt nie), Event-Box, Dauer-Telemetrie.
  Deterministische Fake-Uhr, kein sleep-Roulette im NNA-Pfad.
- **§23 Build-Varianten:** genau EIN machinod-Binary, unverändert ohne
  jede Venus-Abhängigkeit; der Helfer ist ein eigenes Artefakt.

## Status

SOFTWARE_IMPLEMENTED: alles Obige; CI grün für machinod-Crossbuild,
Helfer-Build und Modellkonvertierung.

PENDING_PHYSICAL (AP-NNA6): nmem setzen (Tool-Dialog), soc-nna.ko laden
(Treiberversions-Gate der öffentlichen Runtime ist Messpunkt Nr. 1),
/dev/soc-nna, erste echte Inferenz, Venus-Heap in mem=48M, CPU/Latenz
unter WebRTC-Last. Erst danach wird ai.person Supported, und erst dann
erscheint person im Dropdown. **Niemand behauptet, die NNA liefe schon.**
