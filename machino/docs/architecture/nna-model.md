# Das Erkennungsmodell (AP-NNA4): Herkunft, Bau, Paketierung

Stand 2026-09-26. Deckt die drei geforderten Kapitel (person-model /
model-build / model-packaging) im Repo-Stil in einem Dokument ab.

## Modellwahl und Lizenz — die ehrliche Lage

Konvertiert und als CI-Artefakt bewiesen ist **yolov5s** aus dem
magik-toolkit (Models/post/yolov5s). Operatorenseitig ist das die sichere
Wahl: es ist DAS Referenzmodell des Toolkits für T40, mit mitgelieferter
Konvertierungs-Config (`cfg/magik_t40.cfg`: SOC=T40, INPUT 1x3x640x640 RGB,
NORMAL 255, quantisiert) und Kalibrierungs-Datensatz (`yolov5-20`, im
Toolkit gepinnt) — kein Operator-Raten nötig.

Lizenzstatus, Code und Gewichte getrennt:

| Teil | Befund | Klassifikation |
|---|---|---|
| yolov5s-GEWICHTE (via Toolkit-ONNX) | Upstream Ultralytics YOLOv5, AGPL-3.0; Ingenics Weiterverteilung im Toolkit ohne eigene Lizenzaussage | **DEVELOPMENT_ONLY** |
| Konvertierungs-Tooling (TransformKit) | Teil des öffentlich verteilten Ingenic-Toolkits, kein LICENSE-File | wie libvenus: derselbe Status wie machinos libimp-Praxis |
| **Auslieferbarer Weg** | `Models/training/pytorch/Txx_Xs2/persondet` im selben Toolkit: EIGENES Training → eigene Gewichte → ONNX → TransformKit | **CAN_SHIP (nach Training)** — Training ist ein eigener Arbeitstag, kein Nacht-AP |

Konsequenz: das CI-Artefakt trägt seine Einstufung IM Manifest
(`"license": "DEVELOPMENT_ONLY: …"`), das Bundle nimmt es nur über den
Opt-in-Weg (`--with-nna-payload`) mit, und der produktreife Schritt ist
dokumentiert statt vorgetäuscht. **Kein Stock-`ivs_detect.bin` irgendwo.**

## Reproduzierbarer Bau (model-build)

Ein Weg, in CI automatisiert (build-nna-t40.yml, Job `model`) und damit
auf jedem frischen Host nachvollziehbar:

```
git clone https://github.com/wispytrace/magik-toolkit
git checkout e511d370dd7ff84664c9140e0590c354947c7eac     # PIN, s.u.
cd Models/post/yolov5s
../../../TransformKit/magik-transform-tools \
    --inputpath yolov5s.onnx \
    --outputpath ./yolov5s_t40_magik.mk.h \
    --config cfg/magik_t40.cfg \
    --save_quantize_model true
# Ergebnis: yolov5s_t40_magik.bin
```

Gepinnt ist ALLES an einem Punkt — dem Toolkit-Commit `e511d370…`:
ONNX-Quelle, TransformKit-Binary, Config und Kalibrierungsbilder liegen
in diesem Stand. Runtime (libvenus.a im Helfer) und Modell kommen aus
DEMSELBEN Pin; das Versions-Gate der Runtime (Modell↔Venus-Check,
nna-evidence.md) ist damit konstruktiv entschärft. Byte-Determinismus des
Tools ist nicht bewiesen (x86-Blackbox) — deshalb protokolliert die CI je
Lauf `provenance.txt` mit den SHA-256 von Quelle, Tool, Config und
Ausgabe: welcher Bytestand woher kam, ist immer rekonstruierbar.

Input-/Output-Vertrag: aus der REALEN Config abgeleitet (640×640 RGB
NCHW, NORMAL 255) und im Manifest festgehalten; die Ausgabe sind die drei
YOLOv5-Köpfe, Decode+NMS macht der Helfer über Ingenics `generate_box`
(AP1-Beleg). Der Helfer liest die Eingangsgeometrie zusätzlich aus dem
Modell selbst — ein Modell mit anderer Größe braucht keinen Codeanfass.

Golden-Vector-Grenze, ehrlich: der eigentliche Tensor-Decode lebt in der
Venus-Bibliothek (uClibc, MIPS) und ist hostseitig nicht ausführbar; ein
Host-Evaluator des Toolkits ist nicht belegt. Hostgetestet ist der
GESAMTE machinod-seitige Vertrag (det-Zeilenformat, Normalisierung,
Clipping, Ablehnung ungültiger Boxen) plus das Manifest-Gate; die erste
Ende-zu-Ende-Inferenz ist ausdrücklich AP-NNA6.

## Manifest und Kompatibilitäts-Gate

Die CI legt neben das Modell ein `manifest.json` (schemaVersion 1: id,
backend `venus-nna`, nnaGeneration `nna1`, soc, modelFile, modelSha256,
Input-Vertrag, Klassen, Schwellen, Lizenz, sourceRevision).

machinod prüft beim Detector-Start (core/detection/nna_manifest, Fabrik
im Ingenic-Adapter): liegt neben `ai.model_path` ein manifest.json, MUSS
es passen — sonst kein Detector, mit **stabilem Reason-Code**:

```
AI_MANIFEST_INVALID            kaputt / falsches Schema
AI_MODEL_INCOMPATIBLE_BACKEND  fuer eine andere Runtime gebaut
AI_MODEL_INCOMPATIBLE_NNA      falsche NNA-Generation
AI_MODEL_INCOMPATIBLE_SOC      fuer einen anderen SoC
AI_MODEL_FILE_MISMATCH         Manifest gehoert zu einer anderen Datei
```

Dazu die Voraussetzungs-Codes der Fabrik: `NNA_DEVICE_MISSING`,
`NNA_RUNTIME_MISSING`, `AI_MODEL_MISSING`. Ohne Manifest bleibt eine
nackte .bin nutzbar (Entwicklungsmodus) — geloggt, nie still. Die
SHA-Prüfung des Modells passiert am Installationsweg (das Cam-Tool
verifiziert das GANZE Bundle per sha256 vor der Übertragung); ein
Start-zeitlicher Re-Hash von 3,3 MB auf der Kamera wäre Doppelarbeit
ohne neuen Beweis.

## Paketierung (model-packaging)

- Installpfad: `/etc/machino/models/` — EIN Ort, keine dritte
  Payload-Wahrheit. Manifest und Modell nebeneinander.
- Opt-in: `install.sh --with-nna-payload` (Manager reicht durch,
  Cam-Tool-Checkbox, Default AUS — Overlay-Platz ist eine Entscheidung).
- Atomarität: jede Datei geht über `put()` (staging-Datei + `mv -f`);
  ein abgebrochener Install lässt das alte Modell unversehrt. Ein
  Modelltausch bei laufender KI greift erst beim nächsten Detector-
  (Neu-)Start — machinod hält keine offene Modelldatei, der Helfer lädt
  beim Spawn.
- Uninstall-Asymmetrie: der Helfer geht, die **Modelle bleiben**
  (Betreiberdaten; uninstall räumt /etc/machino jetzt gezielt UM models/
  herum ab — der Pauschal-`rm -rf` hatte genau das verletzt, vom
  Install-Test gefangen).
- CI-Artefakte: `machino-nna-model` = Modell + manifest.json +
  provenance.txt; `machino-nna-t40` = Helfer. Das Haupt-Bundle zieht
  beide best effort; rot wird es dadurch nie.

## PENDING / offen

- **Größenbefund (Audit 2026-09-26):** das konvertierte yolov5s wiegt
  7,6 MB (7,2M Gewichte in INT8 — Parameterzahl, nicht Auflösung) und ist
  damit größer als das GESAMTE Overlay. Für den Hardware-Test gehört es
  nach /tmp (tmpfs, ai.model_path dorthin, Manifest daneben); produktreif
  wird erst ein kleines Netz (persondet, 1–2M Parameter) — derselbe
  Schritt, der auch die AGPL-Frage löst.
- PENDING_PHYSICAL: erste echte Inferenz, Genauigkeits-Sichtprüfung,
  RAM-/CPU-Realität (keine erfundenen Zahlen — nirgends steht eine
  fps- oder ms-Behauptung).
- OFFEN (bewusst): persondet-Training für ein CAN_SHIP-Modell;
  Host-Referenzvergleich Original↔konvertiert, falls sich ein
  Toolkit-Evaluator findet. Beides ändert nichts an Schnittstellen —
  genau dafür sind Manifest und Helfer-Grenze da.
