# AP21 — Firmware Update / Upgrade

Batchlauf 2026-09-23. **Nichts wurde geflasht**, und Machino flasht auch nach
diesem Arbeitspaket nicht.

---

## 1. Der Vertrag, und wer eigentlich schreibt

Die entscheidende Erkenntnis zuerst: **majestic schreibt kein MTD.** Es
orchestriert nur. `www/a/update.js`, Zeile 1:

> Firmware update over the robust /ws/upgrade WebSocket. majestic stops video
> (frees RAM), streams download+verify here, then is killed at the flash step.

Geschrieben wird von **`sysupgrade`**, einem Shellskript auf der Kamera. Die
Oberfläche besteht aus:

```
WS  /ws/upgrade     Start ({source, kernel, rootfs, reset, force}) oder Anhängen;
                    streamt das sysupgrade-Log als Text-/Binärframes
POST /upload        eine lokale .tgz nach /tmp/firmware.tgz
```

Und `sysupgrade` prüft bereits genau das, was AP21 verlangt — am Gerät gelesen:

```
check_soc()            die "Wrong SoC! (image '$1' != build '$model')"
check_size_fits()      die "... does not fit its partition ... Nothing was written."
preflight_image_sizes()  alle Größenprüfungen VOR dem ersten Erase
                       rootfs wird gemountet und gegen /etc/hostname geprüft;
                       ist er nicht mountbar und trägt kein Kernel im selben
                       Lauf eine SoC-Angabe, bricht es ab statt zu raten
LOCK_FILE=/tmp/sysupgrade.lock
```

Das ist die richtige Stelle dafür, und Machino darf sie nicht nachbauen.

## 2. Der Defekt, den die Bestandsaufnahme gefunden hat

```
GET /cgi-bin/update.cgi   200   (die Update-Seite lädt, über die Front-Door)
WS  /ws/upgrade           404
POST /upload              404
```

Die Seite ist also erreichbar, der Mechanismus fehlt. Und was sie dann sagt,
steht in `update.js`:

```js
status('danger', 'Could not start the upgrade. Another session may be in
       progress, or the camera is unreachable.');
```

**Beide Hälften sind falsch.** Es läuft keine zweite Sitzung, und die Kamera
antwortet einwandfrei. Der Satz schickt einen Besitzer auf die Suche nach einem
Phantom.

### Behoben, ohne eine Zeile Flash-Code

Der Vertrag hat einen Ablehnungskanal, und `update.js` beschreibt ihn genau:

```js
const refusedMarker = /^ERROR: (?:invalid upgrade parameters|cannot start
    sysupgrade|cannot stream upgrade log|cannot watch the upgrade)/mi;
...
stopWaiting('danger', 'The camera could not start the upgrade — see the log
    below. Nothing was written to flash, so the camera is unchanged.');
```

Machino nimmt den WebSocket jetzt an, sendet **einen Textframe** mit der ersten
Zeile `ERROR: cannot start sysupgrade`, darunter die Begründung, und schließt.
Die Seite zeigt daraufhin ihren eigenen, **wahren** Satz — „Nothing was written
to flash, so the camera is unchanged" — und im Logfenster steht, was stattdessen
zu tun ist: `sysupgrade` über SSH für die Firmware, `machino-manager install`
für Machino selbst.

Die Wortwahl ist nicht meine: die Liste ist im Upstream ausdrücklich
**aufgezählt und verankert**, mit Begründung — ein lockeres „irgendeine Zeile
mit ERROR:" würde Fremdwerkzeugausgabe treffen und „would end a run while a
flash was under way". Ein Hosttest hält unsere Zeichenkette gegen genau diese
Regex und prüft zusätzlich, dass wir **keine** der anderen drei Formeln
borgen — jede bedeutet der Seite etwas anderes.

## 3. Die Artefakte, klar getrennt

| Artefakt | Wer schreibt | Machinos Rolle |
|---|---|---|
| Machino-Binary + Config | `machino-manager install` / `install.sh` | **zuständig**, siehe unten |
| RootFS / Overlay | `sysupgrade` | keine |
| Kernel | `sysupgrade` | keine |
| vollständiges Firmware-Image | `sysupgrade --archive` | keine |

Nur die erste Zeile ist Machinos Sache. Die anderen drei schreiben auf MTD, und
das tut dieser Daemon nicht.

## 4. Der gehärtete Installpfad

`install.sh` prüfte bisher nur den freien Platz. Jetzt, **alles vor dem ersten
Schreibvorgang**:

**Hash.** Das Bundle liefert `SHA256SUMS`; der Eintrag für den Daemon wird
gegen die Datei geprüft. Fehlt die Datei oder `sha256sum`, wird installiert und
ausdrücklich gesagt, dass **nicht** geprüft wurde — ein „NOT verified" ist eine
Aussage, ein Schweigen wäre eine Andeutung.

**Format.** `\x7fELF`, 32 Bit, little-endian, und `e_machine == 8` (MIPS) direkt
aus dem Header. Eine Cross-Build, die still auf den Host-Compiler zurückgefallen
ist, scheitert hier statt als nicht ausführbarer Daemon auf der Kamera zu landen.

**Zielplattform.** `BUILDINFO` gegen `/proc/device-tree/compatible`. Zwei
Kameras auf einem Tisch sind die leichteste Verwechslung mit dem höchsten
Preis. Ein **anderer Hersteller** im Gerätebaum wird abgelehnt — ein T40-Bündel
ist auf einer SigmaStar nicht einmal ausführbar. Ein anderes **Ingenic**-Board
installiert und meldet „platform NOT verified": dafür gibt es keine Tabelle,
und eine Ablehnung wäre dort geraten.

Nachtrag (AP26): der erste Wurf deckte nur eine Richtung ab — er verlangte T40
im `BUILDINFO`, *wenn* der Gerätebaum t40 sagte, ließ aber einen fremden
Hersteller mit einem Achselzucken durch. Gefunden, indem das **echte**
CI-Artefakt gegen einen Gerätebaum `sigmastar,ssc335` installiert wurde.

**Atomarer Austausch.** `put()` kopiert in eine temporäre Datei **im
Zielverzeichnis** und benennt sie darüber. Ein `cp` auf einen lebenden Pfad
schreibt in place: ein voller Datenträger, ein Stromausfall oder ein getötetes
Skript hinterlässt eine **abgeschnittene** Datei dort, wo eine funktionierende
war — und für `/usr/bin/machino` ist das eine Kamera, die nicht mehr streamt.

**Rollback.** Der vorherige Daemon wird vor dem Ersetzen als
`$STATE_DIR/backup/machino.prev` gesichert. Schlägt die Nachprüfung in
`machino-manager` fehl (installiert, aber nicht aktiv), wird er
zurückgeschrieben und neu gestartet — und das Ergebnis wird **berichtet**, in
jedem der drei Fälle: zurückgerollt und läuft, zurückgerollt und läuft immer
noch nicht, oder kein Rollback-Ziel vorhanden. Ein stilles Rollback ließe den
Nächsten das falsche Binary debuggen.

Nur der Daemon wird gesichert: er ist die eine Datei, deren Ausfall den Stream
kostet, und bei 4,6 MB freiem Flash die eine, von der mehrere Kopien wehtun.

## 5. Drei Fehler in meinem eigenen Code, alle durch Ausprobieren gefunden

Diese Prüfungen sind genau die Sorte Code, die plausibel aussieht und falsch
ist. Alle drei hätten **jede** Installation blockiert:

1. **Der Hash-Selektor.** Mein erster awk-Ausdruck nahm jeden Pfad, der auf
   `/machino` endet. Das Bundle liefert aber auch `./init/machino`, und das
   steht in `SHA256SUMS` **vor** `./machino` — jede Installation hätte den
   Daemon gegen den Hash des Init-Skripts verglichen und ein einwandfreies
   Bundle „korrupt" genannt. Gefunden beim Test gegen das echte Artefakt.

2. **Der ELF-Präfix.** Ich hatte `7f454c46010100` erwartet. Das echte Binary
   beginnt `7f454c46 01 01 01 00`: Byte 6 ist `EI_VERSION` (1), nicht 0. Die
   Prüfung hätte jeden gültigen Build abgelehnt. Gefunden, indem ich sie gegen
   `machino` aus dem CI-Bundle laufen ließ statt über das Layout nachzudenken.

3. **Der atomare Austausch gegen ein kaputtes `mv`.** Test 13b simuliert ein
   fehlschlagendes `mv` — und das ist kein künstlicher Fall: `move_file()`
   dokumentiert ein EINVAL dieses Overlayfs, im Feld gesehen. Mein `mv` ließ
   die Installation sterben. Jetzt fällt `put()` bei einem gescheiterten
   Rename auf die alte In-Place-Kopie zurück **und sagt es** — eine nicht
   atomare Schreibweise ist besser als eine Installation, die gar nicht läuft,
   eine unbemerkte nicht.

## 6. Tests

`tests/test_openipc_install.sh` (CI, Linux): **91 bestanden, 0 fehlgeschlagen,
1 übersprungen.** Neu:

* ein x86-64-ELF wird mit „not a MIPS binary" abgelehnt, **und**
  `/usr/bin/machino` entsteht nicht — „Nothing was written" muss wahr sein,
  nicht bloß gedruckt
* ein MIPS-Header passiert die Formatprüfung
* ein falscher Hash bricht ab, nichts wird geschrieben
* ein richtiger Hash installiert — mit `./init/machino` **vor** `./machino` in
  der Prüfsummendatei, also genau der Reihenfolge, die den Selektorfehler
  ausgelöst hat
* nach der Installation bleibt keine `*.machino-new.*`-Datei übrig
* der ersetzte Daemon liegt als `machino.prev` bereit, und es ist derselbe
  Inhalt, der ersetzt wurde

Gegenprobe gemacht: mit absichtlich verdorbenem ELF-Präfix schlagen die zwei
Formattests fehl — sie prüfen also wirklich etwas.

Der Format-Check kennt einen Hook `MACHINO_INSTALL_SKIP_FORMAT`, den **nur** die
Testumgebung setzt (ihr Daemon-Platzhalter ist ein Shellskript). Dieselbe
Bauart wie das vorhandene `MACHINO_MANAGER_NO_ACTIVATE`. Die Prüfung selbst
läuft in den zwei Fällen oben ohne Hook.

Hosttests C++: 2544 bestanden, 0 fehlgeschlagen.

## Stand gegen „Fertig wenn"

> normale Machino-Updates laufen remote sicher durch, und ein fehlerhaftes
> Paket überschreibt nicht blind die Kamera.

* **Normale Machino-Updates:** Hash, Format und Zielplattform werden vor dem
  ersten Schreibvorgang geprüft, der Austausch ist atomar (mit gemeldetem
  Rückfall), und ein Build, der installiert aber nicht läuft, wird
  zurückgerollt.
* **Fehlerhaftes Paket:** abgelehnt, mit einem Satz, der sagt was fehlt, und
  ohne dass eine Datei angefasst wurde. Durch Tests gedeckt, die beides prüfen.
* **Firmware:** unverändert Sache von `sysupgrade`. Machino sagt das jetzt in
  Worten, die die Stock-Seite versteht, statt sie eine Unwahrheit über eine
  Phantomsitzung anzeigen zu lassen.

### `PENDING_PHYSICAL`

* Ein echtes `machino-manager install` auf der Kamera mit dem neuen Installer
  — er verlangt einen Daemon-Neustart, und der ist der dokumentierte
  Hardlock-Auslöser.
* Der Rollback-Pfad, ausgelöst durch einen absichtlich unbrauchbaren Build.
  Das ist ein Test, der die Kamera zeitweise ohne laufenden Daemon lässt; er
  gehört an ein Gerät, an dem jemand sitzt.
* Ein echter `sysupgrade`-Lauf: **nicht** von hier aus, und nicht in diesem
  Arbeitspaket. Firmware-Upload steht auf der No-Go-Liste.
