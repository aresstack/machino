# NNA-Beweiskette und Entscheidungsmatrix (AP-NNA1, Abschluss)

Ergänzt nna.md um die geforderte Evidenz. Alles hier ist am Objekt gemessen
(Stock-Dump `/c/tmp/ap6`, öffentliches magik-toolkit @ wispytrace-Mirror);
was nicht messbar war, steht als UNKNOWN.

## Entscheidungsmatrix

| Frage | Antwort | Beleg |
|---|---|---|
| Hardware accelerator | Ingenic NNA, Generation **nna1** | InferenceKit_CHANGELOG: "first release venus for t40n" unter nna1; nna2 listet A1/T41, kein T40 |
| Kernel driver | `soc-nna.ko` ("Ingenic soc nna driver") | Stock `ko/loadko.sh:68 insmod soc-nna.ko`; Modul im Stock-ko-Verzeichnis |
| Boot-Reservierung | `nmem=8M@0x7800000` | Stock-Bootargs; Fenster 120–128 MB liegt im heute UNGENUTZTEN Bereich (OpenIPC: mem=48M + rmem=64M@0x3000000 endet bei 112 MB) |
| Device node | `/dev/soc-nna` | Strings in Stock-libvenus: open + Fehlerpfad + Versionshandshake ("The soc-nna version is %08x") |
| Runtime-Generation | Venus (magik::venus), nna1 | Symbole `magik::venus::venus_get_version_info` u. a. in Stock-libvenus UND im öffentlichen venus.h |
| Runtime-ABI | Stock: **uClibc dynamisch** (NEEDED libc.so.0, ld-uClibc-mipsn8.so.0, gcc7-C++) | readelf -d, 2026-09-26 |
| Verhältnis zu machino | **ABI_MISMATCH zu musl-machinod → eigener uClibc-Helferprozess** (machino-nna, statisch) | Entscheidung + Begründung in nna.md; Ingenics eigenes Makefile_t40 baut genau solche Binaries |
| Runtime-Quelle | öffentlich: `InferenceKit/nna1/mips720-glibc229/lib/uclibc/libvenus.a` (14 MB, statisch) + venus.h | magik-toolkit, geklont und benutzt (tools/nna) |
| Modellformat | TransformKit-serialisiertes Magik-Modell (.bin) | run_t40.sh: magik-transform-tools --config cfg/magik_t40.cfg (SOC=T40, INPUT_SHAPE 1x3x640x640, quantisiert) |
| Modell-Konvertierung | ONNX/TF/TFLite → TransformKit (x86-Binary, im Toolkit enthalten) → T40 .bin | TransformKit_CHANGELOG ("Serialize ... to Txx and T40 platforms"); CI-Job build-nna-t40/model führt es aus |
| Machino-API | BoundSource-Detector `venus_nna` (poll), Helferprozess + IAnalysisSource | umgesetzt: core/detection/nna_detector.*, Hosttests |
| Shipping | siehe Lizenzabschnitt | |
| Offene Unbekannte | (a) Venus-Userland-Heap in mem=48M (Stock hatte 80M) (b) läuft `-static -muclibc` libvenus auf der echten NNA (c) lädt die ÖFFENTLICHE Runtime das Stock-`ivs_detect.bin`? | alles AP-NNA6 bzw. bewusst nicht gebraucht |

## Stock-AI-Kette (belegt vs. Inferenz)

**BEWIESEN** (readelf/strings/Dateisystem):
- `loadko.sh` lädt `soc-nna.ko`; Bootargs reservieren `nmem=8M@0x7800000`.
- `libvenus.so` (2,78 MB) öffnet `/dev/soc-nna`, macht einen Versions-Handshake
  mit dem Treiber und trägt `magik::venus`-Symbole inkl. NNA-Convolution-Pfaden
  (`Convolution/bit8_k123_nna2` u. ä.).
- Kette der DT_NEEDED: `libants_ivs.so` → `libants_ai_common.so` + `libvenus.so`;
  alles uClibc (libc.so.0, ld-uClibc-mipsn8.so.0).
- `libants_ai_common.so`: `generateBBox(... yolo_param ...)`, `magik::venus::nms`,
  `net_create`, `venus_init` — YOLO-ARTIGES Postprocessing.
- `libants_ivs.so`: `PersonDetectYuv420`, `detect_objects`, Verweis auf
  `ivs_detect.bin`.

**INFERENZ** (plausibel, nicht bewiesen):
- `ivs_detect.bin` ist ein Venus-serialisiertes Modell: sein Header ist ANDERS
  als der des öffentlichen Beispielmodells (beginnt `47 40 48 45 69 ...` statt
  `0f 00 01 00 ...` — möglicherweise Vendor-Wrapper/Obfuskation), aber beide
  tragen bei Offset ~0x33 dieselbe 4-Byte-Marke `90 57 d1 5f`. Welche konkrete
  YOLO-Architektur darin steckt: UNKNOWN, und für unseren Weg egal.

**UNKNOWN:**
- Ob die öffentliche libvenus das Stock-Modell lädt. Nicht weiter verfolgt:
  der Produktweg ist ein EIGENES TransformKit-Modell, das Stock-Modell bleibt
  lokale Referenz.

## Nachmessungen aus dem AP1-Review (2026-09-26 Nacht)

Fingerprints (SHA-256, Stock-Dump; Build-IDs tragen die ELFs keine):

```
72f95528…d44a94  libvenus.so           2 775 076 B
230aea68…f4d857  libants_ivs.so          258 128 B
9230d7b0…4fe2a2a libants_ai_common.so    128 900 B
0dbe48f5…573e9c  ivs_detect.bin        3 272 890 B
a7710fe7…93cd3a  soc-nna.ko               20 388 B  (license=GPL v2,
                 version=20190724a, vermagic 4.4.94, parm: nna_clk)
```

Symbolvergleich Stock-`libvenus.so` gegen öffentliche `libvenus.a` (nna1):
**106 von 126** exportierten `magik::venus`-Symbolen sind GEMANGELT IDENTISCH
in der öffentlichen Bibliothek; Kern-API (venus_init, net_create,
venus_deinit) in beiden, die 20 Nur-Stock-Symbole sind überwiegend
Signaturvarianten. Klassifikation im Sinne des AP1-Rasters:
**PUBLIC_RUNTIME_LIKELY_COMPATIBLE** — für unseren Weg ohnehin nachrangig,
weil der Helfer VOLLSTÄNDIG aus der öffentlichen Bibliothek gebaut wird und
kein Stock-Userland lädt.

**Der wichtigste Review-Fund — Treiber-Versions-Gate:** die ÖFFENTLICHE
libvenus prüft beim Start die soc-nna-Treiberversion und bricht bei
Abweichung ab ("The soc-nna version is %08x drivers_version is %p Don't
match"). Der Stock-Treiber meldet version=20190724a. Ob die öffentliche
nna1-Runtime (Changelog deutlich jünger) genau diese Version akzeptiert,
ist **UNKNOWN und der erste Messpunkt von AP-NNA6** — schlimmstenfalls
braucht der öffentliche Weg einen passenderen soc-nna.ko, den das Toolkit
nicht mitliefert (Quelle weiterhin offen, OpenIPC firmware#2031).
Zweites Gate derselben Art: Modell↔Runtime-Versionscheck ("model version
… venus version … mismatch") — für uns entschärft, weil Modell und
Runtime aus DEMSELBEN Toolkit-Stand kommen (CI baut beide zusammen).

## Lizenz / Shipping

- magik-toolkit: **kein LICENSE-File** im Repo; Quellheader tragen
  "(C) COPYRIGHT Ingenic Limited ALL RIGHT RESERVED". Formal:
  UNCLEAR_LICENSE. Faktisch ist es Ingenics öffentlich verteiltes
  Board-Support-Toolkit für genau diesen Zweck.
- Einordnung fürs Produkt: machino linkt HEUTE SCHON Ingenics libimp 1.3.1
  statisch (vendor libs aus ingenic-lib). Ein statisch gegen libvenus.a
  gelinktes machino-nna hat exakt denselben Shipping-Status wie machinod
  selbst — **CAN_BUILD_FROM_PUBLIC_SOURCE**, Redistribution im selben Rahmen
  wie bisher. KEINE Stock-Blobs (`ivs_detect.bin`, `libants_*`) im Produkt:
  STOCK_REFERENCE_ONLY.
- Beispielcode: der Inferenzpfad in tools/nna/machino-nna.cpp ist aus
  Ingenics venus_sample_yolov5s adaptiert (Herkunft im Dateikopf).

## Abgleich mit dem AP-Plan (Stand nach dieser Nacht)

- AP-NNA1 (Unterbau/Entscheidung): DONE — nna.md + dieses Dokument.
- AP-NNA3 (Detector `person`): DONE softwareseitig — nna_detector (Core,
  Hosttests), LinuxNnaProcess, IngenicNnaSource, Fabrik mit
  Voraussetzungs-Gründen; `ai.person` bleibt Unknown bis AP-NNA6.
- AP-NNA4 (Modellpfad): DONE softwareseitig — /etc/machino/models,
  ai.model_path wird durchgereicht, Modell nie im Binary; CI konvertiert
  yolov5s.onnx → T40 (best effort).
- AP-NNA5 (UI/API): Seite machino-ai.cgi (Voraussetzungen, Modelle,
  Backup-Purge), detectors-Liste gated auf Supported.
- AP-NNA2 (Tool: nmem-Patch) und Payload-Installation: siehe Folge-Commits.
- AP-NNA6 (Hardware): PENDING_PHYSICAL, bewusst nicht heute Nacht.

Abweichung von GPTs AP1-Regel "Produktcode nicht verändern": AP1–AP5 wurden
in einer Nacht zusammen umgesetzt; die AP1-Fragen sind trotzdem alle oben
beantwortet, mit UNKNOWN wo unbewiesen.
