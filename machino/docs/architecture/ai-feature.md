# Das KI-Feature (AP-NNA5): Zustaende, API, Grenzen — und die AP6-Checkliste

Stand 2026-09-26, Ende der Software-Nacht. Dies ist die Zusammenfuehrung
von AP1–AP5 zu EINEM Feature-Vertrag; Details je Schicht: nna.md,
nna-evidence.md, person-detector.md, nna-model.md,
docs/nna-boot-environment.md (Cam-Tool-Repo).

## Sichtbare Zustaende (Benutzer-Wahrheit)

| Zustand | Woran erkennbar |
|---|---|
| KI aus | ai.enabled=false — kein Helfer, kein Analyse-Kanal, keine Kosten |
| motion aktiv | state=active, backend=imp_ivs_move |
| person gewaehlt, nicht verfuegbar | state=error, telemetry.ai.error={code,message}; die Wahl BLEIBT gespeichert, Video laeuft |
| person aktiv (nach AP6) | state=active, backend=venus_nna, Dauer-Telemetrie gefuellt |

## Der Availability-Vertrag

Eine Bewertung, zwei Verbraucher: `core/detection/detector_availability`
(reine Funktion ueber `NnaFacts`) gated die Ingenic-Detector-Fabrik UND
speist die API. Reason-Codes, stabil und vollstaendig in
Abhaengigkeitsreihenfolge:

```
NNA_PLATFORM_UNSUPPORTED   SoC nicht vermessen (nur t40nn)
NNA_BOOT_MEMORY_MISSING    kein nmem in der GEBOOTETEN Cmdline
NNA_DEVICE_MISSING         /dev/soc-nna fehlt (soc-nna.ko)
NNA_RUNTIME_MISSING        /usr/sbin/machino-nna fehlt (Payload)
AI_MODEL_MISSING           ai.model_path nicht lesbar
AI_MANIFEST_INVALID        manifest.json kaputt/falsches Schema
AI_MODEL_INCOMPATIBLE_*    BACKEND | NNA | SOC (gegen den LAUFENDEN SoC)
AI_MODEL_FILE_MISMATCH     Manifest gehoert zu anderer Datei
```

Fehlendes Manifest ist bewusst KEIN Grund (Entwicklungsmodus, geloggt).

## API

- `GET /api/v1/ai/detectors` → `{selected, detectors:[{id,label,available,
  selectable,reasons:[{code,message}]}]}` — live aus dem Store
  (model_path/detector sind patchbar), Provider aus der Plattform; ohne
  Provider ehrlich nur motion.
- `GET /api/v1/telemetry` ai-Block: +skipped (bewusst ausgelassene
  Perioden), +last/avg/max_infer_duration_ms (NUR wo gemessen; IVS bleibt
  null — keine erfundene NNA-Auslastung, nirgends), +`error:{code,message}`
  im Error-Zustand (erster Reason des gewaehlten Detectors).
- `PATCH ai.detector` akzeptiert motion|person; person auch wenn gerade
  unavailable: **stored statt rejected** — Phase 3 persistiert nur
  ok-Keys, eine Ablehnung haette die Wahl verschluckt (§16-Fund). Kein
  stiller Rueckfall auf motion, je.
- Capabilities: `ai.person` bleibt Unknown bis AP-NNA6 — Supported ist
  eine Hardware-Aussage. `detectors` (Strings) bleibt kompatibel; die
  Laufzeitwahrheit wohnt in der neuen Route.
- Health: KI-Fehler beruehren nur den ai-Block; der Dienst und das Video
  melden weiter running (war schon immer die Architektur: Detector =
  Consumer).

## Events

`detection`-Ereignis traegt je Detektion label/class_id/confidence und —
wenn echt — `box{x,y,w,h}` (normalisiert, 4 Nachkommastellen); mehrere
Personen = mehrere Eintraege in detections[]. Motion bleibt boxlos und
formatkompatibel; keine Ereignisse fuer leere Frames.

## UI

- KI-Plattform-Seite (machino-ai.cgi): Voraussetzungen einzeln, Modelle,
  Backup-Purge. KEIN Bootenv-Knopf — der lebt ausschliesslich im
  Cam-Tool-NNA-Dialog (AP2-Sicherheitsflow, nie unattended).
- Detector-Dropdown der ai-Sektion speist sich aus der detectors-Liste;
  person erscheint dort erst mit Supported (nach AP6). Die Reason-Codes
  sind bis dahin ueber /api/v1/ai/detectors und die KI-Seite sichtbar.
  (Bewusste Restluecke: „ausgegraut mit Grund IM Dropdown" ist
  JS-Arbeit in der ai-Sektion — nach AP6 sinnvoll, wenn person real
  erscheinen kann.)

## Paketierung / Cam-Tool

Core-Bundle laeuft ohne jede KI-Beigabe. Optional, je eigene
Entscheidung: `--with-nna-payload` (Helfer + Modell + Manifest +
Provenienz nach /etc/machino/models, ueberlebt Uninstall),
`--with-weirdike` analog fuers VPN. Cam-Tool: Checkboxen (Default aus)
+ NNA-Dialog (Befund/Plan/Apply mit doppelter Bestaetigung).

## Fehlerverhalten (Invariante, getestet)

Helfer fehlt/stirbt/schweigt/antwortet Muell → Timeout/Fehler + Backoff-
Respawn; wiederholtes Scheitern bleibt im ai-Block sichtbar; niemals
Daemon-Restart, niemals Pipeline-Anfassen. Detector-Wechsel
motion→person→motion: sauberer Neustart je Wechsel, die Sensor-Basis
blinzelt nie (ein bring_up ueber alle Wechsel, Test).

## Matrix

```
SOFTWARE VERIFIED (Host 4633/0, Cross-CI gruen)
-----------------------------------------------
Availability-Vertrag + Reason-Codes (Matrix-Tests)
person als zweiter Detector (Prozess-Helfer, Pacing, Backoff, skipped)
Manifest-/Kompatibilitaets-Gate (inkl. laufender-SoC-Vergleich)
API: /ai/detectors, Fehlercode in Telemetrie, person konfigurierbar
Events mit Box; Dauer-Telemetrie nur-wo-gemessen
Payload-Installation atomar; Modelle ueberleben Uninstall
Helfer-Build + Modellkonvertierung + Manifest/Provenienz in CI
Bootenv-Pfad im Tool (Plan/Apply/Rollback, 49 Checks), Schranke intakt

PENDING_PHYSICAL (AP-NNA6, morgen, durch einen Menschen)
--------------------------------------------------------
nmem setzen (NNA-Dialog) + Neustart + verifyAfterReboot
soc-nna.ko laden (GPL-v2-Stock-Modul, vermagic 4.4.94) -> /dev/soc-nna
MESSPUNKT 1: akzeptiert die oeffentliche Runtime die Treiberversion
             20190724a? (ihr "Don't match"-Gate, nna-evidence.md)
Payload: Helfer installieren (--with-nna-payload; vorher KI-Seite:
majestic-Backup purgen). AUDIT-BEFUND: das dev-Modell (7,6 MB) passt
NICHT aufs Overlay — fuer den Test nach /tmp kopieren, Manifest daneben,
ai.model_path=/tmp/... setzen
erste Inferenz: ready -> result; Person vor der Kamera
Venus-Heap in mem=48M (Stock hatte 80M) — DAS Restrisiko
CPU/Latenz unter WebRTC-Last; Langlauf
danach: ai.person=Supported setzen (Commit), person erscheint im Dropdown
```
