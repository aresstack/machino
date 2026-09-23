
---

## Gruppe F — USB / VBUS / Modem (AP35)

Alle softwareseitigen Fragen sind beantwortet (`ap35-usb.md`): Host-Modus ist
erzwungen, Portspannung ist an, PB27 ist vom Treiber belegt und high. Was
bleibt, ist Elektrik und Einstecken. Baseline vor jedem Test aufnehmen mit
`machino/tools/usb-inventory.sh`, danach erneut — die Ausgabe ist dafuer
gebaut.

| # | Was | Warum physisch |
|---|---|---|
| F1 | **Spannung am USB-VCC messen**, dazu direkt an PB27 und am Ausgang des Load-Switch | Entscheidet die vier verbliebenen Hypothesen: Switch unbestueckt / active-low / Messpunkt / defekt. Software kann hier nichts mehr beitragen |
| F2 | Beliebiges **Kleinlast-USB-Geraet** einstecken (Maus, Stick) und `usb-inventory.sh` erneut laufen lassen | Trennt "VBUS fehlt" von "Enumeration fehlt". Der Hub-Treiber ist vorhanden, ein Geraet muesste also in `/sys/bus/usb/devices` auftauchen — auch ohne Klassentreiber |
| F3 | **WiFi-Platine** einstecken, VID/PID ablesen | Ohne echte Enumeration wird der Chipsatz nicht aus dem Boardlayout geraten (AP35.11) |
| F4 | **EC200A** einstecken, moeglichst an einem aktiv versorgten Hub | Laut eigener frueherer Messung zieht das Modul 1-2 A Bursts und bootet an einem Host ohne aktiven VBUS-Switch nicht durch |
| F5 | **Stromaufnahme messen**, wenn F4 laeuft | Maximale Stromfaehigkeit des Boardpfads ist aus Software nicht ableitbar |
| F6 | Nach einem Kernel mit Klassentreibern: **Lastmessung** WebRTC-Latenz / Paketverlust / IRQ-Last unter Modemverkehr | AP35.15; heute nicht moeglich, weil nichts enumerieren kann |

**F2 ist der billigste und aussagekraeftigste Test** — eine USB-Maus genuegt,
und das Ergebnis halbiert den Suchraum sofort.
tatt Schweigen (`jpeg`-Sektion) | AP14 |
| A9 | Audio-Panel sagt „both … switched off" statt „has not said yet" | AP20 |

## S — Sicherheit: die Fixes wirken erst nach der Ablösung (AP30)

| Nr | Prüfung | Woher |
|---|---|---|
| S1 | Header-Smuggling: eine Headerzeile mit einem bare LF und einem zweiten `Content-Length` dahinter muss **400** ergeben, nicht 404. **Der Ausgangszustand ist seit dem Audit vom 2026-09-23 gemessen: `c1edd92` antwortet 401.** Der Test ist fernbedienbar (TCP-Socket, roher Request) und braucht niemanden am Gerät — nur den abgelösten Build | AP30 HIGH-1 |
| S2 | RTSP: 8 KiB ohne Leerzeile schicken — die Verbindung muss fallen, RSS darf nicht wachsen | AP30 HIGH-2 |
| S3 | `/cgi-bin/../../../etc/shadow` muss **400 von Machino** ergeben, nicht von busybox. Ausgangszustand gemessen: `%2e%2e`-Variante ergibt heute 401 | AP30 NORMAL |

**Bis zur Ablösung trägt die Kamera beide HIGH-Schwächen** — der laufende
Prozess ist `c1edd92`. Das ist der stärkste Grund, den nächsten Kaltstart nicht
lange aufzuschieben.

## B — Beim Kaltstart, mit Browser

| Nr | Prüfung | Woher |
|---|---|---|
| B1 | **WebRTC spielt weiterhin**, jetzt auf pt 102 statt 41 — das Annahme-Log nennt die Nutzlast im Klartext | AP16 |
| B2 | WebRTC MAIN ~50–100 ms, keine unerklärten DTLS/SRTP-Fehler, PLI → IDR | AP16-Gate |
| B3 | `prft` erscheint im Stats-Panel als „capture→arrival p50/p95" | AP15 |
| B4 | MSE-Latenz über längere Laufzeit im Auge behalten (Band `lagFloor + 1 s`) | AP15 |
| B5 | Bestehende RTSP-Clients bekommen jetzt 401 (Auth-Default umgestellt) | AP7 |
| B6 | AP6–AP10 Laufzeitprüfungen der Config-/Schema-Wege | AP6–10 |

**B1 ist der Punkt mit dem größten Rückrollwert.** Er verschiebt die
ausgehandelte Nutzlast auf einem bereits hardwareabgenommenen Pfad. Spielt
WebRTC nicht mehr, ist Commit `37d1619` das Erste, was zurückgedreht wird — er
ist isoliert.

## C — Gezielte Fehlerfälle (Kamera zeitweise ohne Dienst)

| Nr | Prüfung | Woher |
|---|---|---|
| C1 | RTSP-Rebind-Rollback mit `rtsp.port = 22` | AP3 Schritt 11 |
| C2 | Watchdog löst wirklich aus (Feed anhalten) | AP4.13 |
| C3 | Shutdown-Reaping | AP5.15 |
| C4 | `machino-manager install` mit dem neuen Installer, echtes Gerät | AP21 |
| C5 | Rollback-Pfad: absichtlich unbrauchbarer Build → `machino.prev` kommt zurück | AP21 |

## D — `NEEDS_HUMAN` (Fehlschlag kostet die Erreichbarkeit)

| Nr | Prüfung | Warum | Woher |
|---|---|---|---|
| D1 | Factory-/Unclaimed-Test | leert den root-Hash in `/etc/shadow` und beendet damit SSH | AP11 |
| D2 | `nmem=` in die U-Boot-Umgebung für die NNA | Schreibvorgang in `mtd1 (env)`; eine kaputte Boot-Umgebung braucht UART und einen Menschen | AP23 |
| D3 | Modultausch `tx-isp` / Sensor / `avpu` | `CONFIG_MODVERSIONS=n`: der Kernel lädt auch ein inkompatibles Modul klaglos und stürzt später ab | AP22 |
| D4 | Echter `sysupgrade`-Lauf | Firmware-Upload steht auf der No-Go-Liste | AP21 |

## E — Fragen an die Platine, nicht an die Software

| Nr | Frage | Woher |
|---|---|---|
| E1 | Hat dieses Exemplar überhaupt einen IR-Cut-Filter? | AP18 |
| E2 | Ist ein Mikrofon angelötet? (`aic_enable=1` sagt nur, dass der Controller an ist) | AP20 |
| E3 | Gibt es einen Lautsprecheranschluss, den der Treiber nur nicht kennt? (`spk_gpio=-1` ist Treiberkonfiguration, keine Aussage über Kupfer) | AP20 |
| E4 | Ist der SD-Slot verdrahtet? Der Controller steht im DT, eine Karte war nie drin | AP19 |

Fällt E4 positiv aus, ist **Recording neu zu bewerten**: die Entscheidung
„nicht angeboten" beruht ausdrücklich darauf, dass es kein Ziel gibt.

## F — Weiterhin offen, ohne Prüfplan

* **Der Hardlock (Befund B in `ap17-hardlock.md`).** Auslöser gepinnt, Ursache
  offen, kein Reproducer in ~12 synthetischen Versuchen. Instrumentiert ist er
  jetzt: Konsole spricht wieder (`printk 3 3 1 3`), und
  `tools/hardlock-watch.ps1` friert bei Kontaktverlust einen begrenzten
  Mitschnitt ein. **Beim nächsten Ereignis zuerst UART lesen, dann erst Strom
  ziehen** — tmpfs stirbt mit dem Power-Cycle.
* **AP13 (OSD-Backend).** Blockiert an einer Architekturentscheidung: TTF-
  Rasterer ja/nein, und die Bindetopologie des funktionierenden Videopfads.

---

## Regressionsstand zu diesem Zeitpunkt

```
C++ Hosttests                 2544 bestanden, 0 fehlgeschlagen
Installer-Shelltests            91 bestanden, 0 fehlgeschlagen, 1 übersprungen
streamerctl-Tests               29 bestanden, 0 fehlgeschlagen
check-linux-only                ok
CI inkl. MIPS-Crossbuild        grün
```

Und die drei Upstream-Prüfer gegen die Konfiguration, die der **ausgelieferte**
Build erzeugt (nicht der laufende):

```
{"jpeg":{"enabled":false},"nightMode":{"irCut":"off"},
 "audio":{"enabled":false,"outputEnabled":false}}

ircut-check.js    0 Befunde
audio-check.js    "both its microphone and its speaker switched off"
storage-verdict   "There is no SD card in the camera"
```
