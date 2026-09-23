# AP23 — AI / NNA / Analytics-Ausbau

Batchlauf 2026-09-23. Bestandsaufnahme, rein lesend: **kein Kernelmodul
geladen**, nichts an der Kommandozeile geändert.

---

## Kurzfassung

Das Arbeitspaket ist **auf dieser Kamera nicht erfüllbar**, und zwar an einer
Stelle, die weiter unten liegt als alles, was Machino entscheiden kann. Die
Beweiskette ist vollständig und hat vier Glieder — jedes einzelne reicht.

Das ist kein Ausweichen: die vorhandene Detection (IMP-IVS-Motion, M9) bleibt,
und AP23 verlangt ausdrücklich, sie zu benutzen statt daneben eine zweite
Architektur zu bauen.

---

## 1. Die vier Glieder

### (a) Der Treiber ist da, aber nicht geladen

```
/lib/modules/4.4.94/ingenic/soc-nna.ko     20 968 B
lsmod                                      gpio audio sensor_imx307_t40
                                           tx_isp_t40 avpu sinfo vfat fat
/dev/nna, /dev/soc_nna                     existieren nicht
```

### (b) Er verlangt eine Speicherreservierung, die der Boot nicht macht

Aus den Zeichenketten des Moduls, ohne es zu laden:

```
soc_nna_probe / soc_nna_malloc / soc_nna_mmap / soc_nna_flushcache
nna_clk
nmem
copy_from_user nmem_extension_size error %d
@@@@ soc nna probe sucess (Board: %s, Version: %s) @@@
```

`nmem` ist der Ingenic-Parameter für den der NNA vorbehaltenen Speicher. Die
Kommandozeile dieser Kamera:

```
mem=48M rmem=64M@0x3000000 console=ttyS1,115200n8 panic=20
root=/dev/mtdblock3 rootfstype=squashfs init=/init mtdparts=jz_sfc:...
```

**Kein `nmem=`.** Es ist kein Speicher für die NNA reserviert. Das zu ändern
heißt, die Kernel-Kommandozeile zu ändern, und die steht in der U-Boot-Umgebung
— ein Schreibvorgang in `mtd1 (env)`, der auf der No-Go-Liste steht und bei
einem Fehler die Kamera nicht mehr booten lässt.

Nebenbei: `mem=48M` von 42 816 kB nutzbarem RAM. Selbst mit einer
`nmem`-Reservierung müsste sie aus diesem Budget kommen.

### (c) Das SDK, das Machino linkt, kennt die NNA nicht

`include/T40/1.3.1/en/imp` enthält genau sechzehn Header:

```
imp_audio.h   imp_common.h  imp_decoder.h  imp_dmic.h
imp_emu_framesource.h   imp_encoder.h   imp_framesource.h
imp_isp.h     imp_ivs.h     imp_ivs_base_move.h   imp_ivs_move.h
imp_log.h     imp_osd.h     imp_system.h   imp_utils.h   isp_osd.h
```

Keiner davon ist ein NNA-Header, und `grep -rl "IMP_NN\|nna"` über den ganzen
SDK-Baum findet nichts. Es gibt also **keine API**, gegen die man
programmieren könnte.

Die System-`libimp.so` (1.2.0) ebenfalls nicht: `strings | grep '^IMP_NN'`
liefert nichts.

### (d) Es gibt keine Userspace-Bibliothek und kein Modell

```
ls /usr/lib /lib /usr/share | grep -iE "nna|magik|venus|npu|tflite|caffe"
  nichts
Modelldateien
  keine
```

Ingenics NNA-Userspace kommt normalerweise als eigene Bibliothek mit einem
Modellformat. Auf dieser Kamera ist davon nichts vorhanden.

## 2. Was daraus folgt

Um AP23 zu erfüllen, müssten **alle vier** Glieder beschafft werden:

1. `nmem=` in die U-Boot-Umgebung — **No-Go** (Flash-Env-Schreibvorgang).
2. `soc-nna.ko` laden — Kernelmoduleingriff, ohne (1) ohnehin sinnlos.
3. Eine NNA-fähige SDK-Variante samt Headern beschaffen — existiert im
   verwendeten 1.3.1-Satz nicht.
4. Ein Modell beschaffen und ein Inferenzformat unterstützen.

Punkt 1 allein beendet die Sache für einen unbeaufsichtigten Batchlauf. Und
selbst wenn alle vier da wären, gälte weiterhin die AP23-Bedingung „den
akzeptierten Streamingpfad nicht merklich verschlechtern" — auf einer Box mit
48 MB Gesamtbudget, die bereits an ihrem RAM gemessen wird.

**Der Stock-Audit (AP6) sagte „kein nmem/NNA".** Das war richtig und ist jetzt
genauer: der *Treiber* ist da, reserviert ist nichts, und der Userspace fehlt
vollständig.

## 3. Was stattdessen bleibt und weiterhin gilt

`imp_ivs.h`, `imp_ivs_base_move.h` und `imp_ivs_move.h` sind im SDK — das ist
der Unterbau von Machinos `DetectionService` (M9): Detector als Consumer,
IMP-IVS-Motion als Backend, über `ai.*` konfigurierbar, im Schema als eigene
Sektion geführt (`config.schema.json` listet `ai`).

Dieser Pfad ist **nicht** von AP23 blockiert und bleibt unverändert. Er ist
auch genau das, was AP19 als „vorhandene Detection-Infrastruktur" benannt hat.

Was AP23 an Zielen nennt und was davon erreichbar wäre:

| Ziel | Mit NNA | Mit IMP-IVS (vorhanden) |
|---|---|---|
| Person Detection | blockiert | nein — IVS erkennt Bewegung, keine Klassen |
| einfache Objektklassen | blockiert | nein |
| Bounding Boxes | blockiert | IVS liefert Bewegungsregionen, keine Objekte |
| Enable/Disable im bestehenden Schema | — | **ja, vorhanden** (`ai.enabled`) |
| Inferenz vom Media-Hotpath entkoppelt | — | **ja, vorhanden** (Consumer-Modell) |

Eine Personenerkennung ohne NNA auf dieser CPU zu bauen, wäre die „zweite
parallele Analysearchitektur", die AP23 verbietet — und auf einem
MIPS32-Kern ohne Vektorwerk auch ohne jede Aussicht auf brauchbare
Bildraten.

## Stand gegen „Fertig wenn"

> mindestens ein NNA-basierter Detector läuft stabil, ist abschaltbar und
> verschlechtert den akzeptierten Streamingpfad nicht merklich.

**Nicht erfüllt, und nicht erfüllbar**, ohne die Kernel-Kommandozeile zu ändern
— was ausdrücklich verboten ist. Gemessen statt vermutet: Treiber vorhanden,
`nmem` nicht reserviert, keine SDK-API, keine Bibliothek, kein Modell.

Die Metrikliste aus AP23 (Inferenzzeit, NNA-Auslastung, CPU, RAM,
FPS-Einfluss, WebRTC-Latenz, Leistungsaufnahme) wurde **nicht** erhoben. Es
gibt nichts, das sie erzeugen könnte; sie zu schätzen wäre erfunden.

### `PENDING_PHYSICAL` / `NEEDS_HUMAN`

* `nmem=` in die U-Boot-Umgebung schreiben: **`NEEDS_HUMAN`**, nicht bloß
  physisch. Eine kaputte Boot-Umgebung kostet die Kamera vollständig, und
  Recovery braucht UART und einen Menschen.
* Ob dieses Board überhaupt eine nutzbare NNA-Anbindung hat, ist damit offen —
  die Frage ist bis dahin nicht beantwortbar, nicht bloß unbeantwortet.
