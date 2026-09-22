# AP19 — Recording / Analytics / Peers

Batchlauf 2026-09-23. Alle drei enden bei „bewusst nicht angeboten" — aber
nicht per Verzicht, sondern weil der Vertrag jeweils einen definierten
Nicht-Angeboten-Zustand hat und die Kamera ihn erfüllt.

---

## Peers — vom Upstream selbst geklärt

Zweck zuerst, wie AP19 verlangt. `cameras-switch.js` sagt es in seinem eigenen
Kopf:

> Fed by GET /api/v1/peers, the same roster the openipc.local selector uses. It
> is subnet-gated (404 off-link) and absent on older majestic (404), and either
> way the switcher simply stays hidden: no new disclosure, no hard dependency.

Das ist **kein Medien-Subsystem**, sondern ein Flotten-Umschalter: eine Liste
anderer Kameras im selben Subnetz, damit man von einer IP aus zu den übrigen
springen kann. Der Upstream behandelt 404 ausdrücklich als „gibt es hier
nicht", und dann verschwindet der Umschalter.

```
GET /api/v1/peers   ->  404
```

Verdikt: **sauber nicht angeboten.** Kein Code nötig, kein Schema-Eintrag, und
nichts, was verschwiegen würde.

---

## Recording — es gibt nichts, wohin

### Die Hardware

```
/proc/partitions        nur mtdblock0..4 (SPI NOR). Kein mmcblk, kein sd*.
/sys/class/mmc_host     mmc0  ->  DRIVER=ingenic,sdhci, OF_NAME=msc @0x13060000
                        mmc0:*  ->  KEINE Karte registriert
beschreibbare FS        /dev/mtdblock4 auf /overlay, jffs2, 4,6 MB frei
```

Ein SD-Host-Controller ist also **vorhanden**, eine Karte **nicht**. Das ist
ein wichtiger Unterschied: die Kamera *könnte* aufnehmen, sobald eine Karte
steckt — sie kann es heute nur nirgendwohin.

Der einzige beschreibbare Speicher ist das jffs2-Overlay auf SPI-NOR mit
4,6 MB. Bei den gemessenen 2,9 Mbit/s ist das rund **13 Sekunden Video**, und
danach steht die Kamera mit vollem Flash da — von der Abnutzung eines
NOR-Bausteins durch Dauerschreiben ganz abgesehen. Dorthin aufzunehmen wäre
kein Feature, sondern ein Defekt.

Die Stock-Konfiguration sagt dasselbe: `records.path: /mnt/mmcblk0p1/%F` —
sie zeigt auf genau die Karte, die nicht da ist.

### Was die Kamera heute antwortet

```
GET /cgi-bin/j/sdcard.cgi      200  {"present":false,"health":"absent",
                                     "mountpoint":"/mnt/mmcblk0p1",...}
GET /cgi-bin/j/recordings.cgi  200  {"error":"no recording path is configured"}
GET /metrics/records           404
GET /api/v1/records/resume     404
GET /api/v1/records/standdown  404
```

Die beiden `j/`-CGIs sind **busybox' eigene** und laufen über die Front-Door —
sie melden die Dateisystem-Hälfte. `/metrics/records` mit 404 setzt in
`recordings.js` den Rekorder auf `{ absent: true }`, einen definierten Zustand,
keinen Fehler.

### Was der Nutzer davon zu lesen bekommt

Nicht geraten: `storage-verdict.js` ist das Upstream-Modul, das genau diesen
Satz formuliert („the wording lives here and the callers choose a length, not a
phrasing"). Mit den Antworten von oben gefüttert:

```
kind:   "absent"
level:  "danger"
short:  "There is no SD card in the camera - nothing is being recorded."
```

Das ist der richtige Satz über eine Kamera ohne Karte. **Keine leere Seite,
keine Erfolgsmeldung ohne Wirkung.** Und er kommt zustande, weil Machino
*nichts* behauptet: ohne `records`-Sektion ist `records.enabled !== true`,
womit auch `sdcard.js` (`return false`) und der Rekorder-Zweig in
`recordings.js` einheitlich auf „aus" stehen.

### Warum keine `records`-Sektion veröffentlicht wird

Die AP14-Lehre lautete: eine Sektion, auf die das Dashboard *gated*, soll
ausdrücklich gemeldet werden statt zufällig zu fehlen. Für `jpeg` war das
richtig — Machino **hat** eine JPEG-Konfiguration. Für `records` gilt das
Gegenteil: Machino hat kein Aufnahme-Subsystem. `records: {enabled:false}` zu
melden hieße, einen Schalter zu behaupten, den niemand umlegen kann. Das ist
genau der „Endpunkt, der Erfolg meldet, obwohl nichts passiert", den AP19
verbietet.

Verdikt: **sauber nicht angeboten**, und die Seite sagt trotzdem den wahren
Grund — weil ihn die Hardware-Hälfte liefert, nicht wir.

---

## Analytics — es gibt nichts zu indizieren

`/api/v1/analytics/day?d=…` ist **kein** eigenes Analysesystem, sondern die
Bewegungsspur **über Aufnahmen**, erreichbar nur von der Recordings-Zeitleiste:

> What the camera reports on /api/v1/analytics/day is presence: when it saw
> movement, never where in frame. (`timeline.js`)

Und der Ausfall ist vorgesehen:

```js
if (r.status === 404) {
    state.motionWhy = 'Motion - this camera is too old to keep an index';
    return null;
}
// A refusal or a fault is NOT evidence that the camera keeps no index -
// 403 says this account may not read it and 500 says the camera broke.
// Only the 404 above is a statement
```

Machino antwortet 404. Genau das ist die Aussage „hier gibt es keinen Index" —
und sie ist wahr, denn es gibt keine Aufnahmen, über die ein Index geführt
werden könnte.

AP19 verlangt ausdrücklich: „vorhandene Detection-/AI-Infrastruktur verwenden;
keine zweite parallele Analysearchitektur bauen." Machinos `DetectionService`
(M9, IMP-IVS-Motion) existiert und ist über `ai.*` konfigurierbar. Einen
Tagesindex darüber zu bauen hieße, ein Aufnahmeverzeichnis zu erfinden, das es
nicht gibt — also eine zweite Architektur ohne Gegenstand.

Verdikt: **sauber nicht angeboten.** Die Detection bleibt, wo sie ist.

---

## Ein Defekt, den diese Bestandsaufnahme gefunden hat

Beim Prüfen, was ein Schreibversuch auf diese Sektionen bewirkt, fiel auf:

```
POST records / analytics / peers   ->  400 unknown_field   (richtig)
POST nightMode                     ->  400 unknown_field   (FALSCH)
```

`nightMode` wird von Machino **veröffentlicht** (`{"irCut":"off"}`, siehe
AP18). Eine Sektion zu melden und einen Schreibversuch darauf dann „unknown
majestic-webui section" zu nennen, ist wortwörtlich der AP14-Defekt: es liest
sich wie ein Tippfehler des Aufrufers, obwohl die Ursache eine Eigenschaft der
Kamera ist.

Behoben, mit demselben Muster wie AP14:

```
403 unsupported_control
"this camera has no IR-cut hardware this build can drive: no ircut, led or
 infrared node in the device tree, no /sys/class/leds, no PWM, no ADC, and
 upstream's wiki-harvested pin table has no entry for t40. Pins are not
 accepted because nothing would act on them."
```

Der Unterschied ist bewusst: `records`, `analytics` und `peers` erwähnt die
Kamera nie, sie sind ihr also tatsächlich unbekannt. `nightMode` erwähnt sie.
Beide gleich zu beantworten wäre der Fehler.

## Tests

`test_unoffered_subsystems()`:

* `records`, `analytics`, `peers` → `unknown_field`, mit dem Sektionsnamen im
  `path`.
* `nightMode` → `403 unsupported_control`, Meldung nennt IR-Cut und t40.
* und für alle vier: der übersetzte Patch bleibt **leer**. Eine Ablehnung, die
  trotzdem etwas übersetzt hätte, wäre schlimmer als jede der beiden Antworten
  — das ist der AP6-Defekt, der hier nicht wiederkommen darf.

2525 Hosttests, 0 failed.

## Stand gegen „Fertig wenn"

> alle drei Bereiche entweder funktionsfähig oder bewusst und sauber nicht
> angeboten.

| | Verdikt | Beleg |
|---|---|---|
| Recording | nicht angeboten | keine Karte im vorhandenen SD-Host, 4,6 MB Flash als einziger beschreibbarer Speicher; die Seite sagt den wahren Grund |
| Analytics | nicht angeboten | Index über Aufnahmen, die es nicht gibt; 404 ist die vertragliche Aussage dafür |
| Peers | nicht angeboten | Flotten-Umschalter; Upstream behandelt 404 selbst als „gibt es hier nicht" |

Keine leere UI-Seite, kein Endpunkt, der Erfolg meldet, ohne dass etwas
passiert, und keine erfundene Fähigkeit im Schema.

### `PENDING_PHYSICAL`

* Eine microSD einlegen und nachsehen, ob der Slot überhaupt verdrahtet ist —
  der Controller steht im Device Tree, ob ein Kartenhalter daran hängt, sagt
  nur die Platine.
* Sollte eine Karte auftauchen, ist Recording **erneut zu bewerten**: dann gibt
  es ein Ziel, und die Entscheidung oben beruht ausdrücklich darauf, dass es
  keines gibt.
