
---

## Review dieses Dokuments

### Ein echter Fehler: die Modulinventur war unvollständig

Die erste Fassung nannte **11 Module** und führte sie als „die vollständige
Modulliste des Images". Tatsächlich sind es **28**. Mein `find` war auf
`/lib/modules/4.4.94/kernel` verwurzelt und übersah damit zwei ganze
Verzeichnisse:

```
ingenic/   17 Module   (u. a. tx-isp-t40, soc-nna, dtrng_dev, motor, 5 Sensoren)
extra/      1 Modul    (wireguard)
```

Aufgefallen ist es beim Gegenlesen: `/proc/modules` zeigt `gpio`, `audio`,
`sensor_imx307_t40`, `tx_isp_t40`, `avpu` und `sinfo` als **geladen** — und
keines davon stand in meiner angeblich vollständigen Liste. Sechs geladene
Module ohne Datei hätten mir sofort auffallen müssen.

**Die Schlussfolgerung ändert sich nicht**, und das ist hier wichtig: die
gezielte Suche nach `cdc*`, `option`, `usbnet` und `rndis*` lief zusätzlich
über das **gesamte Dateisystem** (`find / -xdev`) und blieb leer. Die
Klassentreiber fehlen wirklich. Aber die Zahl war falsch und die Liste
unvollständig, und eine Aussage wie „die vollständige Liste" trägt genau dann,
wenn sie stimmt.

**Das Werkzeug hatte denselben Bug** und ist mitkorrigiert: `usb-inventory.sh`
durchsucht jetzt ganz `/lib/modules`, meldet zusätzlich die Gesamtzahl und
findet den `regdump` per Glob statt über die fest verdrahtete Adresse
`13500000.otg`. Neu gegen die Kamera gelaufen: 28 Module, Klassentreiber
weiterhin sämtlich `MISSING`, 0 WLAN-Gerätetreiber.

### Zwei Formulierungen korrigiert

* Ein Satz sagte, OpenIPC sei „nicht hinter Stock zurück, sondern davor —
  jedenfalls **hinter** dem Stand, den der AP-Auszug beschreibt". Das
  widerspricht sich in der eigenen Zeile.
* Ich hatte das `#` vor `ingenic,drvvbus-gpio` im AP-Auszug als
  „auskommentiert" gelesen. In DTS ist `#` **kein** Kommentarzeichen. Die
  Lesart bleibt naheliegend, aber sie ist eine Deutung fremden Textes und
  steht jetzt als solche da. Die Messung am laufenden Gerät hängt nicht daran.

### Nachgeprüft und bestätigt

| Behauptung | Ergebnis |
|---|---|
| Machino hat keinerlei USB-Bezug | **bestätigt** — `grep` über `src/` und `openipc/` nach usb, ttyUSB, cdc_, rndis, modem: null Treffer |
| `machino/tools/` ist der richtige Ort | **bestätigt** — dort liegen bereits zehn Werkzeuge; kein CI-Job fasst sie generisch an |
| keine `.ko` ausserhalb `/lib/modules` | **bestätigt** — `find / -xdev` |
| Klassentreiber fehlen | **bestätigt**, jetzt über den vollständigen Baum |
erte trägt, ist bereits Host-Semantik — das Register existiert nur
in dieser Rolle.

Der von Thingino beschriebene Rollenwechsel ist damit **gegenstandslos**: er
stellt her, was hier schon steht. Zur Kontrolle gelesen:

```
0x10000040   ist 0x08000004,  Thingino schreibt dort 0x0b000096
```

Dieser Wert wird **nicht** interpretiert und **nicht** geschrieben. Das AP sagt
"Nicht einfach blind Registerwerte übernehmen", und die Zuordnung dieses
Offsets zu einem benannten CPM-Register ist für T40 nicht belegt. Da die Rolle
über GUSBCFG bereits nachweislich Host ist, gibt es auch keinen Anlass.

> **Eigener Fehler, protokolliert:** ich hatte zunächst `devmem 0x13500008` als
> GUSBCFG gelesen und 0x27 erhalten. Der `regdump` zeigt, dass 0x13500008
> **GAHBCFG** ist (= 0x27) und GUSBCFG woanders liegt. Rohe Offsets von Hand
> zuzuordnen ist genau die Fehlerquelle, vor der AP35.3 warnt; die Werte oben
> stammen deshalb ausschließlich aus dem benannten `regdump` des Treibers.
>
> Ebenso zurückgenommen: aus einem IRQ-Zähler von 0 hatte ich geschlossen, es
> sei "nie ein Interrupt gefallen". Eine spätere Messung zeigte 225. Der
> Schluss war falsch; dass nichts enumeriert ist, steht auf `PRTCONNSTS = 0`
> und einem leeren sysfs, nicht auf dem Zähler.

## AP35.4 — Der VBUS-Pfad ist softwareseitig fertig

```
/sys/kernel/debug/gpio:
    gpio-59  ( |ingenic,drvvbus ) out hi
```

GPB beginnt bei 32, Pin 27 ergibt GPIO 59 — **das ist PB27**. Der Pin ist vom
Treiber beansprucht, als Ausgang konfiguriert und **HIGH getrieben**. Parallel
dazu meldet der Controller `PRTPWR = 1`. Beide Seiten, GPIO und
Host-Controller, sagen also: Portspannung ein.

**Damit ist die Leitfrage des AP beantwortet, aber anders als vermutet.** Wenn
am USB-VCC 0 V gemessen werden, liegt das *nicht* an Pinmux, Device-Tree,
Treiber oder Rolle — diese Kette ist vollständig und aktiv. Übrig bleiben
ausschließlich Hardwareursachen:

1. der externe Load-Switch ist auf dieser Bestückungsvariante nicht bestückt,
2. er ist **active-low**, dann bedeutet "out hi" gerade *aus*,
3. gemessen wurde an einer Stelle hinter einem offenen Pfad,
4. der Schalter ist defekt.

Welche davon zutrifft, entscheidet **keine** Software. Das ist eine Messung mit
dem Multimeter an PB27 und am Schalterausgang → `PENDING_PHYSICAL`.

Aus demselben Grund wurde **nichts** geschaltet: das AP verbietet "blind PB27
auf HIGH setzen" — der Pin steht ohnehin high, und ihn gegen den Treiber zu
manipulieren wäre genau der verbotene Eingriff.

## AP35.5 — Nichts zu aktivieren

Der AP sieht einen nichtpersistenten Host-/VBUS-Test vor. Der entfällt: Host
ist erzwungen, Portspannung ist an, der Root-Hub steht. Es gibt keinen
sicheren, nichtpersistenten Schritt, der etwas verbessern würde — also wurde
keiner ausgeführt.

## AP35.6 — Nichts zu integrieren

"Boot → USB Host aktiv → VBUS korrekt → Root Hub bereit ohne UART-Befehl" ist
**der Ist-Zustand**. Kein Initskript, kein `usb-role`, kein `devmem`, keine
DTB-Änderung. Das beste Ergebnis, das dieses Teil-AP haben konnte.

## AP35.7 / AP35.10 — Hier bricht es ab: die Klassentreiber fehlen

Das ist der eigentliche Blocker, und er ist hart.

```
built-in auf dem USB-Bus     hub  usb  usbfs          <- mehr nicht
/sys/bus/usb-serial/drivers  existiert nicht
Module unter /lib/modules    28
.ko ausserhalb /lib/modules  keine (find / -xdev)
```

Die vollständige Modulliste des Images, in drei Verzeichnissen:

```
ingenic/  (17)   audio  avpu  dtrng_dev  gpio  motor  mpsys_driver
                 sample_pwm_core  sample_pwm_hal  sinfo  soc-nna  tx-isp-t40
                 sensor_gc4653_t40  sensor_imx307_t40  sensor_imx334_t40
                 sensor_imx335_t40  sensor_imx415_t40
extra/     (1)   wireguard
kernel/   (10)   ccm  gcm  ghash-generic  i2c-algo-bit  i2c-gpio  tun
                 usbserial  fat  vfat  mac80211  cfg80211
```

Gesucht und **nicht vorhanden** — geprüft über den gesamten Modulbaum und
zusätzlich über das ganze Dateisystem (`find / -xdev`):

```
usbnet  cdc_ether  cdc_acm  rndis_host  cdc_ncm  option  usb_wwan
qcserial  usb-storage  cdc_subset
```

Nebenbei aus der vollständigen Liste, ohne weitere Prüfung festgehalten:
`soc-nna.ko` ist vorhanden (deckt sich mit AP23: Treiber da, alles Übrige
fehlt), ebenso `dtrng_dev.ko`, ein Hardware-Zufallsgenerator. Beide sind
**nicht geladen** und wurden hier nicht weiter untersucht.

`usbserial.ko` ist nur der Kern des seriellen Subsystems; ohne `option` bindet
er an keine Modem-PID. Ein eingestecktes Gerät würde also **enumerieren**
(Hub-Treiber ist da) und danach **ohne Treiber** liegenbleiben.

## AP35.11 — WiFi: Stack ja, Treiber nein

```
mac80211.ko, cfg80211.ko          vorhanden (2 von 2)
Gerätetreiber unter *wireless*    0   (über alle 28 Module geprüft)
```

Der 802.11-Unterbau ist da, aber **kein einziger Gerätetreiber**. Welcher
gebraucht würde, hängt am Chipsatz der Zusatzplatine — und den leitet dieses
Dokument nicht aus dem Boardlayout ab, das verbietet AP35.11 ausdrücklich.
Erforderlich ist VID/PID aus einer echten Enumeration → `PENDING_PHYSICAL`.

Damit ist die WiFi-Platine sauber klassifiziert: **technisch nicht
unterstützt**, Ursache benannt, Weg benannt.

## AP35.8 / AP35.9 / AP35.13 — EC200A aus dem Referenzrepo

`Miguel0888/quectel-ec200a-eu` ist **privat**; die öffentliche API antwortet
404. Über den vorhandenen Git-Credential-Helper war es klonbar (603 Dateien).

Übernommen statt neu erarbeitet, aus `doc/linux.md` des Repos:

| Modus | `AT+QCFG="usbnet",x` | Kernelmodul | Interface |
|---|---|---|---|
| ECM | `1` | `cdc_ether` | `usb0` |
| RNDIS | `3` | `rndis_host` | `usb0` |
| NCM | `5` | `cdc_ncm` | `wwan0`/`usb0` |

```
Geraetekennung  2C7C:6005
Chipsatz        ASR-basiert -> ECM/RNDIS/NCM, KEIN QMI
AT-Ports        ueber option, /dev/ttyUSB0..3, AT-Port meist ttyUSB2
Reihenfolge     AT+CGDCONT=1,"IP","<APN>"  ->  AT+QNETDEVCTL=1,1,1  ->  udhcpc
alte Kernel     echo 2c7c 6005 > /sys/bus/usb-serial/drivers/option1/new_id
```

Die Präferenz des AP (ECM/NCM statt PPP) deckt sich mit der Repo-Doku. **Für
diese Kamera ist das derzeit alles nicht erreichbar:** `option`, `cdc_ether`,
`rndis_host` und `cdc_ncm` fehlen sämtlich. Auch der `new_id`-Trick für alte
Kernel scheitert — er setzt `option` voraus, und den Bus `usb-serial` gibt es
hier nicht einmal.

Zu AP35.13 "portable Aufteilung ec200a-core / platform/*": **nicht gemacht und
nicht nötig.** Der EC200A-Code im Repo ist ESP-IDF/Arduino-USB-Host
(`ec200a_ecm.cpp`, `modem_datalink.cpp`) und löst ein Problem, das Linux mit
`cdc_ether` im Kernel erledigt. Für OpenIPC ist die Wiederverwendung die
**Betriebsdoku und die AT-Sequenz**, nicht der Code. Ein Refactor wäre hier
Aufwand ohne Gegenwert, und das AP erlaubt ausdrücklich, ihn zu unterlassen.

## AP35.14 — Power: die Referenz widerspricht der Hoffnung

Aus `reverse-eng/69-esp32-modem-stromversorgung.md` desselben Repos, eigene
frühere Messung:

```
EC200A Sende-/Attach-Peak      ~1-2 A Bursts
an einem Host ohne aktiven VBUS-Switch:  Modem bootet NICHT durch
an einem aktiv versorgten Hub:           enumeriert sauber als 2C7C:6005
Empfehlung dort                5-V-Quelle >= 2,5 A, 470-1000 uF nahe am Modem
```

Der T40-Port ist ein **einzelner Port an einem Kameraboard**. Es gibt keinen
Beleg, dass sein VBUS-Pfad 2 A liefert — und das AP verlangt ausdrücklich,
genau das **nicht** zu behaupten. Die Erfahrung aus dem eigenen Repo deutet in
die Gegenrichtung.

**Praktische Folge:** selbst wenn die Treiberfrage gelöst wird, ist der
wahrscheinlichste Aufbau EC200A **an einem aktiv versorgten Hub** oder mit
getrennter VBUS-Einspeisung. Maximale Stromfähigkeit des Boardpfads: aus
Software nicht ableitbar → `PENDING_PHYSICAL`.

## AP35.15 — Koexistenz mit der Kamera

**Es wurde nichts verändert**, also kann sich nichts verschlechtert haben. Zur
Kontrolle trotzdem gemessen: Front Door HTTP 401 in 2,5 ms, Listener 22/554/80
und 127.0.0.1:85 unverändert, Machino pid 992 ohne Neustart.

Eine Lastmessung unter USB-Datenverkehr (IRQ-Last, WebRTC-Latenz, Paketverlust)
ist **nicht möglich**, solange kein Gerät enumerieren kann. Bleibt offen und
gehört an den Tag, an dem ein Modem läuft.

## AP35.16 / AP35.17 — Boot und Fehlerfälle

Der persistente Boot-Test ist für den **Host-Teil** bereits erfüllt: die
Kamera bringt Host und Portspannung ohne Zutun hoch, seit jeher, ohne UART.
Für Geräteerkennung und Hotplug fehlt das Gerät.

Zu AP35.17: Machino hat heute **keinerlei USB-Bezug** — kein Code, kein
Config-Schlüssel, kein Thread. Die Forderung "USB-/Modem-Lifecycle getrennt vom
Media-Lifecycle" ist damit trivial erfüllt und soll es bleiben. Ein
ausfallendes USB-Gerät kann Machino nicht stören, weil Machino nichts davon
weiß.

## AP35.12 — WebUI

Bewusst nichts gebaut. Eine Seite "Services / Networking → USB Host / WiFi /
Cellular" hätte heute nur eines anzuzeigen: dass nichts geht. Sobald die
Treiber im Image sind, gehört sie in die **OpenIPC-WebUI**, nicht in Machinos
Kamera-Schema — das sagt AP35.12 selbst, und es deckt sich mit der in diesem
Projekt getesteten Invariante, dass Machinos Installer die Stock-WebUI nicht
anfasst.

## AP35.18 — Tests

Neu: `machino/tools/usb-inventory.sh`, ein rein lesendes Inventar, das
Controller, Rolle, Registerbits, VBUS-GPIO, enumerierte Geräte und die
Treiberverfügbarkeit in stabiler Form ausgibt.

**Bewusst keine weiteren Tests.** Es gibt keinen neuen Produktivcode, den sie
prüfen könnten — ein Parser für Modemantworten, die auf dieser Firmware nie
ankommen, wäre Testtheater. AP35.18 sagt selbst: "Keine Hardware-Behauptung aus
reinen Hosttests ableiten."

---

## Stand gegen "Fertig, wenn"

| Kriterium | Stand |
|---|---|
| Stock/OpenIPC USB-Pfad verglichen | **ERFÜLLT** — mit einer Abweichung: `drvvbus-gpio` ist in OpenIPC aktiv, im AP-Stockauszug auskommentiert |
| T40 Host-Rolle verstanden | **ERFÜLLT** — FORCEHSTMODE=1, aus dem benannten regdump |
| PB27/DRVVBUS-Pfad geklärt | **ERFÜLLT, softwareseitig** — gpio-59 vom Treiber belegt, out hi, PRTPWR=1. Der Rest ist Elektrik |
| dauerhafte Host-Aktivierung implementiert | **ENTFÄLLT** — war nie nötig, ist ab Boot aktiv |
| VBUS-Steuerung softwareseitig korrekt | **ERFÜLLT** |
| USB enumeration funktioniert | **UNGEPRÜFT** — kein Gerät verfügbar; der Hub-Treiber ist vorhanden |
| WiFi-Platine unterstützt oder klassifiziert | **KLASSIFIZIERT: nicht unterstützt** — mac80211/cfg80211 da, null Gerätetreiber |
| EC200A 2c7c:6005 erkannt | **BLOCKIERT** — `option`/`cdc_ether`/`rndis_host`/`cdc_ncm` fehlen im Image |
| AT-Port funktioniert | **BLOCKIERT** — dito |
| Linux USB networking vorbereitet | **BLOCKIERT** — dito |
| Repo wiederverwendet | **ERFÜLLT** — AT-Sequenz, Modustabelle, VID/PID und die Powerbelege übernommen |
| kein UART für normalen Betrieb nötig | **ERFÜLLT** |
| Kamera-/WebRTC-Pfad regressionsfrei | **ERFÜLLT durch Nichteingriff**; Lastmessung offen |
| physische Tests als PENDING_PHYSICAL | **ERFÜLLT** |

## Was jetzt wirklich fehlt

Ein einziger Punkt, und er ist groß:

> **Die USB-Klassentreiber müssen in den Kernel.** `option`, `cdc_ether`,
> `rndis_host`, `cdc_ncm` für den EC200A; der passende Gerätetreiber für die
> WiFi-Platine. Das ist eine Kernel-/Buildroot-Konfiguration plus ein neues
> Image — kein Paket, das man nachinstalliert, und das schließt AP35.10s
> Vorgabe "nicht mit Debian/OpenWrt-Paketinstallation arbeiten" korrekt ein.

Das berührt den stehenden No-Go-Punkt "ISP/Kernel/Treiber-Deploy" und verlangt
ein Gerät, an dem jemand sitzt. Es gehört damit vor die Umsetzung eine
Entscheidung, nicht ein Commit.
