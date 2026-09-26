# NNA/Venus auf dem T40NN: Befund und Architekturentscheidung (AP-NNA1)

Stand 2026-09-26. Alles hier ist gemessen (Stock-Dump `/c/tmp/ap6/appfs`,
öffentliches magik-toolkit), nichts vermutet.

## Was die Hardware kann und die Stock-Firmware tut

Die Stock-Firmware betreibt echte hardwarebeschleunigte Objekterkennung auf
Ingenics NNA (nna1-Generation, xburst2):

| Komponente | Größe | Rolle |
|---|---|---|
| `ivs_detect.bin` | 3,27 MB | quantisiertes Magik-Modell (praktisch unkomprimierbar) |
| `libvenus.so` | 2,78 MB | Venus-Inferenz-Runtime, spricht `/dev/soc-nna` |
| `libants_ai_common.so` | 129 kB | YOLO-Postprocessing (generateBBox, NMS) |
| `libants_ivs.so` | 258 kB | IVS-API (PersonDetectYuv420, get_result_list) |
| `soc-nna.ko` | klein | Kerneltreiber, "Ingenic soc nna driver" |

Bootargs Stock: `mem=81920k@0x0 rmem=40960k@0x5000000 nmem=8M@0x7800000`.
OpenIPC heute: `mem=48M rmem=64M@0x3000000` — endet bei 0x7000000 (112 MB).
**Das Stock-NNA-Fenster 0x7800000–0x8000000 (120–128 MB) liegt im aktuell
ungenutzten Bereich**; `nmem=8M@0x7800000` kollidiert mit nichts. Der
128-MB-Chip ist durch die Bootargs-Summe des Stock-Images belegt.

## Die ABI-Wand (warum kein dlopen)

`readelf -d` der Stock-Libs: `NEEDED libc.so.0`, `ld-uClibc-mipsn8.so.0` —
der gesamte Stock-AI-Stack ist **uClibc, dynamisch**. machino ist ein
**musl**-Binary. Ein dlopen über diese Grenze scheitert an der libc-ABI;
das ist keine Frage der Sorgfalt, sondern der Loader-Realität.

Der IMP-Weg (uclibc-`.a` statisch + libmuslshim) trägt hier nicht ohne
weiteres: `libvenus.a` ist 14 MB C++ mit `std::string`/`std::vector`/
`std::unique_ptr` in der API-Oberfläche (venus.h) — gebaut mit Ingenics
gcc 7.2. Statisch in ein gcc-15-musl-Binary gelinkt hinge die Korrektheit
an der libstdc++-ABI zweier Welten. Möglich, aber nicht die Architektur,
auf die man ein Produkt stellt.

## Der öffentliche Weg (magik-toolkit)

github.com/wispytrace/magik-toolkit (Spiegel des Ingenic-Toolkits):

- `InferenceKit/nna1/mips720-glibc229/` — **T40 ist nna1** (Changelog:
  "first release venus for t40n"). Enthält `include/venus.h` und
  `lib/uclibc/libvenus.a` (14 MB, statisch) + `.so`-Varianten.
- `Models/post/yolov5s/venus_sample_yolov5s/` — kompletter Beispielcode
  (inference.cpp, NV12-Variante, Pre/Post inkl. NMS) und `Makefile_t40`:
  `mips-linux-gnu-g++ -muclibc -std=c++11 -mfp64 -mnan=2008 ...
  -L lib/uclibc -lvenus`. Das ist Ingenics sanktioniertes Build-Rezept
  für T40-Venus-Binaries.
- `Models/training/pytorch/Txx_Xs2/persondet/` — Trainings-/Konvertier-
  pfad für einen kleinen Personendetektor; TransformKit serialisiert
  ONNX/TF/TFLite quantisiert für Txx/T40 (`run_t40.sh` zeigt den Aufruf).

Damit ist der alte AP23-Schluss ("SDK hat keine NNA-Header, also keine
Schnittstelle") widerlegt: die NNA gehört nicht zur IMP-API, sondern zum
separaten Magik/Venus-Stack, und der ist öffentlich verfügbar.

## Entscheidung

**Venus läuft NICHT in machinod, sondern in einem eigenen Helferprozess
(`machino-nna`), gebaut mit Ingenics mips-linux-gnu-Toolchain
(`-muclibc`, statisch gegen `libvenus.a`, statische Runtime — keine
uClibc-`.so` im musl-Rootfs nötig).** machinod spricht mit ihm über ein
zeilenbasiertes Protokoll (Frames über shm/Datei, Ergebnisse als Zeilen).

Warum:
1. **Keine ABI-Mischung.** Der Helfer ist durchgehend die Welt, für die
   libvenus gebaut wurde; machinod bleibt durchgehend musl.
2. **AI-Crash ≠ Videoausfall.** Der Detector ist ohnehin ein Consumer;
   ein toter Helfer wird neu gestartet, der Stream merkt nichts. Das ist
   dieselbe Isolationsentscheidung wie beim Cellular-Helfer.
3. **Optionalität.** Helfer + Modell sind ein Payload wie WLAN/Modem:
   wer keine KI will, installiert nichts und verliert keinen Platz.
4. Es ist Ingenics eigenes Rezept (Makefile_t40 baut genau solche
   Standalone-Binaries).

Stock-Blobs (`ivs_detect.bin`, `libants_*`) bleiben lokale Referenz für
die Forschung; ins Produkt gehen nur magik-toolkit-Komponenten und ein
selbst konvertiertes Modell (TransformKit, ONNX→T40).

## Platzrechnung (gemessen auf der Kamera 2026-09-26)

Overlay 8896 kB, frei 3216 kB. Größter toter Posten:
`/etc/machino/backup` = 2896 kB — das majestic-Backup für den
Uninstall-Rückweg. Auf dem T40NN ist das gesicherte majestic nachweislich
defekt (ABI-Mismatch, RC 5461/1310758), und der echte Rückweg ist ein
OpenIPC-Reflash (U-Boot bleibt unangetastet). Das Backup ist löschbar;
die UI bietet das an (KI-Seite), nichts löscht still.

Nach Purge: ~6,1 MB frei. NNA-Payload (Helfer statisch, jffs2-komprimiert
~2–3 MB + Modell ~3,3 MB unkomprimierbar) passt damit knapp; ein kleineres
eigenes persondet-Modell entspannt die Rechnung deutlich.

RAM: `nmem=8M@0x7800000` als U-Boot-Bootarg (Cam-Tool-Hardwarepatch,
reversibel). Offenes Risiko für AP-NNA6: Stock gab dem Userland 80 MB,
OpenIPC 48 — der Venus-Userland-Heap (get_forward_memory_size) muss auf
der Hardware gemessen werden, bevor `person` als Supported gilt.

## Offene Punkte je Arbeitspaket

- AP-NNA2 (Tool): nmem-Bootarg-Patch, reversibel, PENDING_PHYSICAL.
- AP-NNA3 (machino): Detector-Backend `person` = Prozess-Supervisor um
  `machino-nna`; Capability nur Supported wenn Helfer+Modell+/dev/soc-nna.
- AP-NNA4: Modell-Payload-Struktur /etc/machino/models/, TransformKit-
  Workflow dokumentiert; kein Modell im Binary.
- AP-NNA5: API/UI (KI-Seite: Detector, Modellpfad, Backup-Purge), Telemetrie.
- AP-NNA6: Hardware-Abnahme (nmem, soc-nna.ko, /dev/soc-nna, Inferenz,
  CPU/RAM/Latenz unter WebRTC-Last). Erst danach Supported.
