# AP18 — Night / IR-Cut / GPIO / Pins / Calibration

Batchlauf 2026-09-23. Bestandsaufnahme, rein lesend: **kein GPIO wurde
angefasst**, das steht auf der No-Go-Liste.

---

## Kurzfassung

Es gibt auf dieser Kamera **nichts zu implementieren und nichts zu
veröffentlichen**, und das ist kein Ausweichen, sondern das Messergebnis.
Machinos bestehende Haltung ist bereits die richtige — neu ist, dass sie jetzt
**belegt** ist, und zwar vom Upstream-Prüfer selbst.

---

## 1. Was die Hardware hergibt

Alles am Gerät gelesen, nichts geschaltet.

```
/sys/class/gpio          export, unexport, gpiochip0/32/64/96
                         Banks GPA GPB GPC GPD, je 32 Pads (0..127)
exportierte GPIOs        KEINE
/sys/class/leds          existiert nicht
/sys/class/pwm           leer
/sys/bus/iio/devices     leer      -> kein ADC
/sys/class/hwmon         leer      -> kein Fotosensor über den Kernel
Device Tree: ircut/led/infrared/motor   KEIN Knoten
Device Tree: gpio_keys   nur bootsel0, bootsel1, power
compatible               ingenic,shark / ingenic,t40
```

`ingenic,shark` ist ein Referenzboardname, kein Kameramodell. Der Kernel weiß
also selbst nicht, auf welchem Board er sitzt.

## 2. Was die Stock-Konfiguration hergibt

`/etc/majestic.yaml` auf dieser Kamera:

```yaml
nightMode:
  colorToGray: true
  irCutSingleInvert: false
  lightMonitor: false
  lightSensorInvert: false
```

Vier Schalter — und **kein einziger Pin**. Auch die Stock-Konfiguration dieser
Box hat nie eine Pinbelegung bekommen.

## 3. Was die belastbare Fremdquelle hergibt

Upstream hat genau die Tabelle, die AP18 verlangt („Zuordnung nur aus
belastbaren Stock-/Hardwaredaten"): `www/a/ircut-pads.js` ist **generiert** aus
`OpenIPC/wiki en/gpio-settings.md`, mit IR-Cut-Spulenpaaren und belegten Pads
pro SoC.

```
$ node tools/upstream-checks/ircut-verdict.js ../majestic-webui cfg.json
wiki pin table, t40 : {"coils":[],"busy":[],"known":false}
wiki pin table, t31 : {"coils":[[58,57]],"busy":[11,49,50,...],"known":true}
```

Die Tabelle kennt `t10, t20, t21, t31, t31l, t31n` — **kein t40**. Für diesen
SoC existiert keine belastbare Pinquelle. Damit endet die Kette: keine
Hardwarebelege, keine Stock-Pins, keine Wikitabelle.

Der einzige verbleibende Weg wäre der **Pin-Sweep** der Stock-WebUI
(`ircut-scan.js`), der Pads reihum bestromt, bis sich ein Filter bewegt. Das
ist wörtlich das „IR-cut GPIO-Experiment" der No-Go-Liste und wird nicht
gefahren — es kann eine Reset-Leitung treiben, und die Datei sagt das selbst:
„driving a reset line stops the camera."

## 4. Was Machino veröffentlicht — und warum genau das

`GET /api/v1/config.json` von der laufenden Kamera:

```json
"nightMode": { "irCut": "off" }
```

Eine Zeile. Ob das richtig ist, entscheidet nicht meine Absicht, sondern der
**unveränderte Upstream-Prüfer** — `ircut-check.js`, 1753 Zeilen. Er ist per
`module.exports` ansprechbar, also wird er direkt gefragt:

```
-- no sample, no observation:            0 finding(s)
-- a heartbeat with nothing to say:      0 finding(s)

-- control, nightMode absent entirely:   1 finding(s)
   [danger] no-pins - Majestic cannot move the IR-cut filter
-- control, if a pin were invented:      2 finding(s)
   [info] single-coil - The filter is driven from one pad
   [info] manual-only - Day/night switching is manual
```

Die zwei Kontrollen sind der eigentliche Beleg: die eine Zeile trägt **in
beide Richtungen**. Ohne sie zeigt die Stock-Seite einen roten Hardwarefehler
über eine Kamera, die vollkommen in Ordnung ist. Mit einem erfundenen Pin
fängt die Seite an, einen Filter zu beschreiben, für den es auf diesem Board
keinerlei Beleg gibt.

Das Werkzeug liegt als `tools/upstream-checks/ircut-verdict.js` im Repo und
lässt sich gegen jede zukünftige Konfiguration wiederholen.

## 5. Eine Korrektur am Vertrag selbst

Ich hatte zuerst aus den Kommentaren gelesen, der Pin-Editor spreche
`j/gpio.cgi`. Das ist **falsch** — und zwar nach der Regel dieses Projekts, dass
das ausgeführte JS die veraltete Doku schlägt. Hier schlägt es sogar veraltete
Kommentare im Upstream-Quelltext:

```
www/a/mj-pins.js:811      FETCH('/api/v1/gpio', ...)
www/a/mj-settings.js:8534 apiFetch('/api/v1/gpio', ...)
www/a/pin-hunt.js:155     FETCH('/api/v1/gpio', ...)
www/a/mj-settings.js:7628 "It replaced a `j/gpio.cgi?unset=` call ..."   <- Vergangenheit
```

Der Endpunkt heißt `/api/v1/gpio`.

## 6. Die vier unimplementierten Endpunkte, gemessen

```
GET /api/v1/gpio                   404
GET /api/v1/calibration/coverage   404
GET /api/v1/calibration/pair       404
GET /api/v1/calibration/peer       404
```

Und das ist **genau der Zustand, den die Stock-Seite vorsieht**, nicht ein
Loch. `mj-settings.js` schreibt es selbst hin:

```js
if (!r.ok) throw new Error('HTTP ' + r.status);
...
// No pad list, no map. The hidden number fields are still there, so
// nothing is unreachable - say which door is shut and unhide them.
```

`mj-pins.js` macht dasselbe mit `.catch(() => null)`. Wichtig ist allein, dass
die Antwort **nicht 2xx** ist: derselbe Kommentar warnt, dass eine Firmware,
die stattdessen „a perfectly parseable JSON error" mit Erfolgsstatus liefert,
als Paddaten eingehängt wird und „an empty chip" zeichnet, statt auf die
Zahlenfelder zurückzufallen. 404 ist hier also die richtige Antwort, nicht
bloß eine hinnehmbare.

Randnotiz: `/api/v1/gpio` und `/api/v1/calibration/*` fallen heute durch die
Front-Door an busybox durch und liefern dessen HTML-404, während `/api/v1/osd`
Machinos JSON-404 liefert. Für die UI ist beides gleichwertig (`!r.ok`);
geändert wurde nichts, weil eine Routing-Änderung mehr Risiko trägt als der
Ordnungsgewinn.

`calibration/*` gehört im Upstream ohnehin zu Kennzeichenerkennung und
Peer-Homographie (`raw.js`, `raw-plates.js`, `preview-peer.js`), nicht zu einer
Kamerakalibrierung — dahinter steht hier keine reale Funktion, also wird auch
nichts angeboten.

## 7. Was gegen den Test gesichert wurde

`test_compat.cpp` prüfte bisher `irCut == "off"` und die Abwesenheit von
`irCutPin1`. Jetzt prüft es **alle** Schlüssel, die die Stock-UI als
Verdrahtungstatsache liest — aus den Seiten extrahiert, die sie benutzen, nicht
aus einer Doku:

```
irCutPin1  irCutPin2  lightSensorPin  backlightPin  backlightPwmChannel
```

und zusätzlich, dass `nightMode` **genau einen** Schlüssel trägt. Eine spätere
Änderung kann hier also keine Fähigkeit mehr einschmuggeln, ohne dass der Test
anschlägt.

2506 Hosttests, 0 failed.

## Stand gegen „Fertig wenn"

> Hardwarefähigkeiten korrekt erkannt und keine erfundenen Pins/Funktionen
> publiziert.

* **Erkannt:** kein IR-Cut, keine IR-LED, kein Fotosensor, kein ADC, kein PWM,
  keine LED-Klasse, kein passender Wikieintrag. Vier GPIO-Bänke existieren,
  ihre Belegung ist unbekannt und softwareseitig nicht ermittelbar.
* **Publiziert:** eine Zeile, deren Wirkung mit dem Upstream-Prüfer in beide
  Richtungen belegt ist. Null erfundene Pins, null Dummy-Controls, null
  Endpunkte, die Erfolg melden.
* **`PENDING_PHYSICAL`:** ob dieses Exemplar überhaupt einen IR-Cut-Filter
  besitzt, ist eine Frage an das Gerät, nicht an die Software — sichtbar nur
  am Bild bei Dunkelheit oder an der Platine. Solange das offen ist, wäre jede
  Ansteuerung geraten.
