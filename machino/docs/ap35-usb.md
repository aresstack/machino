# AP35 — USB Host / VBUS / WiFi und Quectel EC200A auf T40NN

2026-09-23. Ergebnis vorweg, weil es die Prämisse des Arbeitspakets umkehrt:

> **Die Host-Rolle ist aktiv, und die Software fordert VBUS an.** Der
> Controller steht im erzwungenen Host-Modus, `PRTPWR` ist gesetzt, DRVVBUS
> (PB27) ist vom Treiber belegt und asserted, der Root-Hub läuft ab Boot.
> **Was daraus am Stecker ankommt, ist damit nicht gezeigt** — siehe den
> Kasten unten. Getrennt davon fehlen die USB-Klassentreiber im Image.

> ### Wichtige Abgrenzung: Anforderung ≠ Spannung
>
> ```
> belegt        DWC2 PRTPWR = 1, DRVVBUS/PB27 asserted
>               => Linux/T40 fordert Host-Modus und Portspannung an
> NICHT belegt  dass am Zusatzstecker tatsächlich 5 V anliegen
> Gegenbeleg    am Stecker wurden real 0 V gemessen
> Status        actual connector VBUS -> PENDING_PHYSICAL
> ```
>
> Ein Registerbit ist eine Anforderung an eine externe Schaltung, keine
> Messung an ihrem Ausgang. Der reale 0-V-Messwert wird durch die
> Registerinterpretation **nicht** überschrieben; beide Befunde stehen
> nebeneinander, und der Widerspruch zwischen ihnen ist genau das, was die
> physische Messung auflösen muss.

Alles unten ist am laufenden Gerät gemessen, rein lesend. Es wurde kein
Register geschrieben, kein Modul geladen, keine Datei auf der Kamera geändert.

Reproduzierbar mit `machino/tools/usb-inventory.sh` — das Werkzeug ist so
gebaut, dass es vor und nach dem Einstecken eines Geräts dieselbe Struktur
ausgibt, damit die physischen Tests vergleichbare Belege liefern statt Prosa.

---

## AP35.1 / AP35.2 — Inventar

```
otg platform dev      10000000.otg_phy, 13500000.otg
bound drivers         dwc-mac, dwc2, usb_phy
gadget udc            KEINE          -> also kein Device-/Gadget-Modus
root hub              usb1, 1d6b:0002 "DWC OTG Controller", 480 MBit, 1 Port
/dev/bus/usb/001/001  vorhanden
IRQ 29                13500000.otg, dwc2_hsotg:usb1
```

Der Device-Tree trägt die OTG-Knoten aktiv:

```
/proc/device-tree/apb/otg_phy
    compatible = ingenic,innophy + syscon
    dr_mode    = otg
    status     = okay
    ingenic,drvvbus-gpio = <phandle 7, 27, 0, 0>      <- AKTIV
/proc/device-tree/ahb2/otg@0x13500000
    compatible = ingenic,dwc2-hsotg
    dr_mode    = otg
    g-use-dma
    status     = okay
    ingenic,usbphy = <24>
```

**Abweichung zum AP-Text:** der dort zitierte Stock-Auszug führt die Zeile als
`#ingenic,drvvbus-gpio`. Was das `#` dort bedeutet, lasse ich offen — ein
DTS-Kommentar ist es nicht (die sind `/* */` oder `//`), und als Property-Name
wäre `#name` syntaktisch möglich, aber ungewöhnlich; die naheliegende Lesart
ist "deaktiviert".

Unabhängig davon steht die **Messung** fest: im laufenden OpenIPC ist die
Property vorhanden und wirksam, der Pin ist belegt und high. OpenIPC ist bei
diesem Punkt also mindestens so weit wie der Stand, den der AP-Auszug
beschreibt.

## AP35.3 — Rolle: Host, und zwar erzwungen

Aus dem `regdump` des Treibers (`/sys/kernel/debug/*.otg/regdump`):

```
GOTGCTL   0x0030000c
GUSBCFG   0x20001408      Bit 29 FORCEHSTMODE = 1, Bit 30 FORCEDEVMODE = 0
GINTSTS   0x04000021
HPRT0     0x00001000      Bit 12 PRTPWR = 1, Bit 0 PRTCONNSTS = 0
```

`PRTPWR = 1` heißt: der Controller **fordert Portspannung an**. Ob die externe
Schaltung sie liefert, steht in keinem Register der CPU.

**Der Controller läuft im erzwungenen Host-Modus.** Dass HPRT0 überhaupt
sinnvolle Werte trägt, ist bereits Host-Semantik — das Register existiert nur
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

## AP35.4 — Die Software fordert VBUS an; der Stecker ist ungeprüft

```
/sys/kernel/debug/gpio:
    gpio-59  ( |ingenic,drvvbus ) out hi
```

GPB beginnt bei 32, Pin 27 ergibt GPIO 59 — **das ist PB27**. Der Pin ist vom
Treiber beansprucht, als Ausgang konfiguriert und **HIGH getrieben**. Zusammen
mit `PRTPWR = 1` heißt das: **die Anforderungsseite ist vollständig** — CPU und
Controller verlangen beide Portspannung.

**Das ist nicht dasselbe wie Spannung am Stecker.** Beides sind Zustände
*innerhalb* des SoC; der Pfad dahinter — Pegelwandler, Load-Switch, Sicherung,
Stecker — ist aus Software nicht beobachtbar. Gegen die Registerlesung steht
ein realer Messwert von **0 V** am Zusatzstecker, und dieser Messwert wiegt
schwerer, weil er am Ziel der Kette genommen wurde.

Der Widerspruch hat mehrere mögliche Auflösungen:

1. der externe Load-Switch ist auf dieser Bestückungsvariante nicht bestückt,
2. er ist **active-low**, dann bedeutet "out hi" gerade *aus*,
3. gemessen wurde an einer Stelle hinter einem offenen Pfad,
4. der Schalter oder seine Versorgung ist defekt,
5. der Pin führt auf dieser Variante gar nicht an den vorgesehenen Schalter.

**Welche zutrifft, ist offen.** Software kann zwei Beiträge noch leisten — die
Polarität aus dem Treiberquelltext bestimmen und prüfen, ob eine andere
Bestückungsvariante im Device-Tree beschrieben ist; beides ist hier nicht
getan. Die Entscheidung fällt am Multimeter: PB27 selbst, Eingang und Ausgang
des Schalters, Steckerkontakt → **`PENDING_PHYSICAL` (G1)**.

Aus demselben Grund wurde **nichts** geschaltet: das AP verbietet "blind PB27
auf HIGH setzen" — der Pin steht ohnehin high, und ihn gegen den Treiber zu
manipulieren wäre genau der verbotene Eingriff.

## AP35.5 — Auf der Anforderungsseite bleibt nichts zu aktivieren

Der AP sieht einen nichtpersistenten Host-/VBUS-Test vor. Der entfällt für
das, was er erreichen sollte: Host-Modus ist bereits erzwungen, `PRTPWR` und
DRVVBUS sind bereits gesetzt, der Root-Hub steht. Ein Schreibzugriff könnte
nur wiederholen, was schon anliegt.

**Das heißt nicht, dass der VBUS-Punkt erledigt ist** — es heißt, dass er
nicht durch Schreiben in ein Register zu erledigen ist. Bleibt am Stecker
0 V, liegt die Ursache hinter dem SoC.

## AP35.6 — Auf der Anforderungsseite nichts zu integrieren

"Boot → Host-Modus aktiv → VBUS angefordert → Root Hub bereit, ohne
UART-Befehl" ist **der Ist-Zustand**. Kein Initskript, kein `usb-role`, kein
`devmem`, keine DTB-Änderung nötig.

Das ursprüngliche AP-Ziel lautet "VBUS korrekt". Ob das erreicht ist, hängt an
der Steckermessung und ist deshalb **offen**, nicht erfüllt.

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

Nebenbei aus der vollständigen Liste festgehalten, **ohne weitere Prüfung**:
es gibt `soc-nna.ko` (deckt sich mit AP23: Treiber da, alles Übrige fehlt) und
`dtrng_dev.ko`, dem Namen nach ein Hardware-Zufallsgenerator. Beide sind
**nicht geladen**, und was `dtrng_dev` tut, ist hier aus dem Dateinamen
geschlossen, nicht geprüft.

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
| PB27/DRVVBUS-Pfad geklärt | **TEILWEISE** — die Anforderungsseite ist geklärt (gpio-59 belegt, out hi, PRTPWR=1); der Pfad vom Pin zum Stecker ist es **nicht**, und dort wurden 0 V gemessen |
| dauerhafte Host-Aktivierung implementiert | **ENTFÄLLT** — war nie nötig, die Rolle ist ab Boot aktiv |
| VBUS-Steuerung softwareseitig korrekt | **ANFORDERUNG ERFÜLLT, WIRKUNG OFFEN** — `PRTPWR`/DRVVBUS gesetzt; ob 5 V am Stecker ankommen, ist `PENDING_PHYSICAL` (G1) |
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

**Belegt ist:** die Klassentreiber `option`, `cdc_ether`, `rndis_host`,
`cdc_ncm`, `usbnet`, `cdc_acm`, `usb_wwan` sind im aktuellen Image nicht
vorhanden — weder als Modul noch eingebaut auf dem USB-Bus.

**Nicht belegt ist**, dass daraus ein neues Kernel-Image folgt. Eine frühere
Fassung dieses Dokuments hat genau das behauptet; das war ein Sprung von
"fehlen" zu "nur per Image nachrüstbar". Diese Kette hat ein fehlendes Glied:

> Es sind **In-Tree-Treiber** des Kernels 4.4.94. Lassen sie sich gegen
> exakt dieselbe Kernelquelle und -konfiguration als `.ko` bauen, und passt
> das `vermagic`, dann genügt es, sie ins Overlay zu legen und per
> `/etc/modules` laden zu lassen. Das wäre erheblich risikoärmer als ein
> Image-Tausch.

Diese Frage wird in **AP35.19** untersucht (eigener Abschnitt weiter unten).
Erst wenn Module-only scheitert, ist ein Image die Antwort — und erst dann ist
der No-Go-Punkt "ISP/Kernel/Treiber-Deploy" überhaupt berührt.

Für die WiFi-Platine bleibt es ohnehin nachgelagert: ohne VID/PID aus einer
echten Enumeration ist nicht bekannt, welcher Treiber gebraucht wird.

---

## AP35.20 — Der DRVVBUS-Pfad, aus dem Kernelquelltext beider Firmwares

Anlass: ich hatte im Gespräch behauptet, das Board habe **keinen
software-schaltbaren 5-V-VBUS**. Diese Behauptung ist **zurückgezogen**. Sie
stützte sich auf zwei Argumente, von denen eines schlicht falsch war:

> Ich schrieb selbst, dass `spi0@0x10043000` im Stock auf `status = "disable"`
> steht — und leitete daraus trotzdem einen *aktiven* SPI-Pin-Konflikt auf PB27
> ab. Eine Pindefinition in einem deaktivierten Controller belegt gar nichts.

Das zweite Argument („Stock deaktiviert die GPIO-Property, also gibt es keinen
VBUS-Enable") war ebenfalls zu schnell: bei Ingenic *könnte* DRVVBUS eine
dedizierte Pinfunktion des OTG-Blocks sein. Also nachgesehen, statt zu raten.

### Die Kette im OpenIPC-Kernel ist vollständig und feuert

`OpenIPC/linux`, Branch `ingenic-t40`, `drivers/usb/dwc2/hcd.c:2505`:

```c
usb_phy_vbus_on(hsotg->uphy);                     /* -> iphy_set_vbus(x, 1) */
dwc2_writel(HPRT0_PWR | HPRT0_CONNDET | ... );    /* -> PRTPWR = 1          */
```

und `drivers/usb/phy/phy-ingenic-inno.c:208`:

```c
static int iphy_set_vbus(struct usb_phy *x, int on) {
    if (!(IS_ERR_OR_NULL(iphy->gpiod_drvvbus))) {
        printk("OTG VBUS %s\n", on ? "ON" : "OFF");
        gpiod_set_value(iphy->gpiod_drvvbus, on);
    }
    return 0;
}
```

Das erklärt die Messung exakt: **derselbe Aufruf** setzt `PRTPWR = 1` *und*
den GPIO — und genau beides haben wir am Gerät gesehen. Der Pin wird mit
`GPIOD_OUT_LOW` angefordert und steht trotzdem auf `hi`, also **ist
`set_vbus(1)` tatsächlich gelaufen**.

### Eine dedizierte DRVVBUS-Pinfunktion gibt es bei Ingenic nicht

```
drivers/pinctrl/pinctrl-ingenic.{c,h}        keine USB-Funktion
include/dt-bindings/pinctrl/ingenic-pinctrl.h keine USB-Funktion
arch/mips/boot/dts/ingenic/t40-pinctrl.dtsi   keine usb/otg-Pingruppe
```

Die Treffer auf „DRVVBUS" im Baum stammen sämtlich von **anderen Herstellern**
(MediaTek, SiRF, TI). In diesem Kernel ist DRVVBUS bei Ingenic
ausschließlich ein GPIO.

### Und Ingenics eigene Referenzboards machen es genauso

```
shark.dts       ingenic,drvvbus-gpio = <&gpb 27 GPIO_ACTIVE_HIGH ...>
shark_fast.dts  ingenic,drvvbus-gpio = <&gpb 27 GPIO_ACTIVE_HIGH ...>
halley2v20.dts  ingenic,drvvbus-gpio = <&gpb 25 GPIO_ACTIVE_HIGH ...>
seal.dts        ingenic,drvvbus-gpio = <&gpf 26 GPIO_ACTIVE_HIGH ...>
```

**Gleicher Pin, gleiche Polarität wie OpenIPC hier konfiguriert.** OpenIPCs
Einstellung ist also keine Erfindung, sondern Ingenics Referenzdesign.

### Der Stock-Kernel hat denselben Treiber — und treibt trotzdem nicht

Aus dem Stock-Image (`kernel_0xd0000.bin`, unkomprimiert lesbar):

```
Linux version 4.4.94 (jiangtaixu@...) (Ingenic gcc 5.4.0) #11 SMP Fri Apr 1 2022
"OTG VBUS %s"            <- derselbe printk aus iphy_set_vbus
"ingenic,drvvbus"        <- derselbe GPIO-Consumer-Name
"#ingenic,drvvbus-gpio"  <- die deaktivierte Property
"ingenic,innophy", "ingenic,dwc2-hsotg"
```

**Stock benutzt exakt denselben Mechanismus und hat ihn abgeschaltet.** Es gibt
in Stock keinen zweiten, dedizierten Pfad — der Kernel enthält schlicht keinen.

Damit ist die Frage „wie versorgt Stock den Anschluss mit VBUS?" beantwortet:
**gar nicht.** Und OpenIPC tut hier *mehr* als Stock, nicht weniger.

### Der Port ist als Host gedacht

```
usbcore, usb_hub_wq, hub_port_connect, usb_hub_claim_port   vorhanden
g_ether / android_usb / configfs-gadget / f_acm / mass_storage  KEINE
```

Der Stock-Kernel hat **Host**-Unterstützung und **keine** Gadget-Funktionen.
Der Anschluss ist also nicht als USB-Device-Port gedacht.

### Was daraus folgt — und was ausdrücklich nicht

**Belegt:** Software-seitig ist die Anforderungskette in OpenIPC vollständig,
korrekt gegen Ingenics Referenz konfiguriert und nachweislich ausgeführt.
Stock fordert VBUS nie an. Eine dedizierte Pinfunktion existiert nicht.

**Nicht belegt:** ob auf dieser Platine ein Load-Switch an PB27 hängt. Dafür
hilft kein Quelltext mehr.

**Der eine Messwert, der das jetzt aufspaltet — direkt an PB27:**

```
PB27 fuehrt ~3,3 V   -> der SoC treibt, der Fehler liegt dahinter
                        (Switch fehlt, ist active-low, oder keine 5-V-Schiene)
PB27 fuehrt 0 V      -> der Pin wird trotz "out hi" nicht getrieben
                        -> dann ist es doch ein Software-/Pinmux-Befund
```

Erst danach ist zu entscheiden, welcher VBUS-Init richtig ist. Bis dahin wird
weder PB27 getoggelt noch etwas geflasht.

## AP35.19 — Module-only: lässt sich das ohne neues Image lösen?

Die Frage vor dem Image-Tausch: es sind **In-Tree-Treiber** des 4.4.94. Können
sie als einzelne `.ko` gegen genau diesen Kernel gebaut und ins Overlay gelegt
werden?

### Der Abhängigkeitstest — und er fällt positiv aus

Alles, was die fehlenden Module an Fremdsymbolen brauchen, ist **fest
eingebaut und exportiert**. Geprüft über `/proc/kallsyms` (`kptr_restrict = 0`,
5716 Exporte) auf das Vorhandensein von `__ksymtab_<symbol>`:

| Gruppe | Beispielsymbole | Status |
|---|---|---|
| usbcore | `usb_register_driver`, `usb_deregister`, `usb_submit_urb`, `usb_control_msg`, `usb_alloc_urb`, `usb_get_dev`, `usb_ifnum_to_if` | **alle exportiert** |
| Netz | `alloc_etherdev_mqs`, `register_netdev`, `netif_rx`, `eth_type_trans`, `skb_put`, `__netdev_alloc_skb` | **alle exportiert** |
| **MII** | `mii_ethtool_gset`, `mii_nway_restart`, `generic_mii_ioctl`, `mii_link_ok` | **alle exportiert** |
| TTY | `tty_register_driver`, `tty_port_init`, `tty_standard_install`, `tty_port_open` | **alle exportiert** |

Die MII-Zeile ist die wichtigste: `usbnet` hängt hart an `mii`, und `mii` ist
kein Modul, sondern **eingebaut**. Bestätigt durch `modules.builtin`:

```
drivers/usb/common/usb-common.ko     eingebaut
drivers/usb/core/usbcore.ko          eingebaut
drivers/usb/dwc2/dwc2.ko             eingebaut
drivers/usb/phy/phy-ingenic-inno.ko  eingebaut
drivers/net/mii.ko                   eingebaut      <- entscheidend
drivers/net/phy/libphy.ko            eingebaut
drivers/tty/serial/serial_core.ko    eingebaut
```

`usbnet`, `cdc_*`, `rndis*` und `option` kommen in `modules.builtin`
**nicht** vor — sie sind also weder eingebaut noch als Modul vorhanden, und
genau das sollen sie werden.

### Damit löst sich die Kette vollständig gegen Vorhandenes auf

```
usbnet       -> usbcore (eingebaut) + mii (eingebaut)        OK
cdc_ether    -> usbnet                                        OK
cdc_ncm      -> usbnet                                        OK
rndis_host   -> usbnet + cdc_ether                            OK
cdc_acm      -> usbcore + tty (eingebaut)                     OK
option       -> usbserial   <- liegt bereits als .ko im Image OK
usb_wwan     -> usbserial                                     OK
```

Zu `usb_serial_register_drivers`, das im Exporttest als **FEHLT** erschien:
das ist kein Mangel, sondern Ladereihenfolge. Das Symbol exportiert
`usbserial.ko`, und das ist **nicht geladen**. Sobald es geladen ist, steht es
zur Verfügung. Deshalb ist es in der Tabelle oben auch nicht als Blocker
geführt.

### Der ABI-Vertrag ist erfüllbar

`CONFIG_MODVERSIONS=n` (aus AP8): es gibt **keine Symbol-CRCs**, der gesamte
Vertrag ist der vermagic-String. Der lautet hier

```
4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT
```

und wurde bereits an `tun.ko` als exakt passend nachgewiesen (AP34). Ein aus
derselben Quelle mit derselben Konfiguration gebautes Modul trägt denselben
String.

### Woher Quelle und Konfiguration kämen

```
Kernel      Linux 4.4.94, gebaut 2026-09-17 von einem GitHub-Runner
Compiler    buildroot-gcc-13.3.0        (aus /proc/version)
Quelle      OpenIPC/linux, Branch ingenic-t40
Konfig      aus dem OpenIPC-Firmware-Buildroot fuer dieses Board
```

### Verdikt

> **Module-only ist strukturell möglich.** Es spricht kein technischer Befund
> dagegen: alle Fremdsymbole sind exportiert, die harte `mii`-Abhängigkeit ist
> eingebaut, der ABI-Vertrag ist ein String ohne CRCs, und die Quelle ist
> benannt. Die frühere Aussage "es braucht ein neues Kernel-Image" ist damit
> **zurückgezogen**.

**Gebaut wurde nichts**, und das ist ehrlich zu benennen: auf dieser Maschine
existiert **kein `make`** — weder in MSYS2 (`usr/bin`, `mingw64`, `ucrt64`,
`clang64`) noch in PowerShell, `cmake` und `ninja` ebenso wenig. Der Beweis
durch einen echten Build steht deshalb aus und braucht eine Linux-Maschine
oder einen CI-Job mit der Buildroot-Toolchain.

Auf der Kamera wurde weisungsgemäß **nichts geladen**.

### Nebenfund für die WiFi-Platine

Die OpenIPC-Firmware führt ein Paket **`aic8800-openipc`** — ein USB-WLAN-
Treiber. Der AIC8800 ist in Kamera-Zusatzplatinen verbreitet, und das wäre der
naheliegende Kandidat. **Behauptet wird das nicht**: AP35.11 verbietet
ausdrücklich, den Chipsatz aus dem Boardlayout zu erraten, und ohne VID/PID
aus einer echten Enumeration bleibt es eine Spur, kein Befund. Sie ist
notiert, damit G3 weiß, wonach es sucht.

---

## Review dieses Dokuments

### Befund 1 — die Modulinventur war unvollständig

Die erste Fassung nannte **11 Module** und bezeichnete sie als "die
vollständige Modulliste des Images". Es sind **28**. Mein `find` war auf
`/lib/modules/4.4.94/kernel` verwurzelt und übersah `ingenic/` (17 Module) und
`extra/` (wireguard).

Aufgefallen beim Gegenlesen: `/proc/modules` führt `gpio`, `audio`,
`sensor_imx307_t40`, `tx_isp_t40`, `avpu` und `sinfo` als **geladen** — und
keines davon stand in meiner angeblich vollständigen Liste. Sechs laufende
Module ohne zugehörige Datei hätten beim Schreiben auffallen müssen.

**Die Schlussfolgerung ändert sich nicht:** die gezielte Suche nach `cdc*`,
`option`, `usbnet` und `rndis*` lief zusätzlich über das gesamte Dateisystem
(`find / -xdev`) und blieb leer. Falsch waren Zahl und Liste, nicht der Befund.

Das Werkzeug hatte denselben Bug und ist mitkorrigiert: `usb-inventory.sh`
durchsucht jetzt ganz `/lib/modules`, meldet die Gesamtzahl und findet den
`regdump` per Glob statt über die fest verdrahtete Adresse `13500000.otg`.
Erneut gegen die Kamera gelaufen: 28 Module, Klassentreiber weiterhin sämtlich
`MISSING`, 0 WLAN-Gerätetreiber.

### Befund 2 — mein Korrekturwerkzeug hat dieses Dokument zerstört

Der schwerwiegendere Fehler, und er betraf nicht den Inhalt, sondern das
Schreiben.

Rund 50 Zeilen Kopf — Titel, Zusammenfassung, das gesamte Inventar aus
AP35.1/35.2 und der Anfang von AP35.3 — wurden gelöscht und durch den
Review-Abschnitt ersetzt. Der Schnitt fiel **mitten in ein Wort**: aus
"sinnvolle Werte trägt" wurde "erte trägt". **Das war bereits committet und
gepusht**, bevor es auffiel.

Die Ursache liegt bei mir und war von Anfang an bekannt: für diese Umgebung
gilt die Vorgabe, Dateioperationen **mit absoluten Windows-Pfaden**
auszuführen. Ich habe in der Shell durchgehend relative Pfade benutzt. Dazu kam
ein selbstgebauter perl-Ersetzer mit einem doppelten Trennmarker, dessen
`substr` daraufhin mit falschem Offset schrieb.

Wiederhergestellt aus dem letzten guten Stand (`git show HEAD~1:...`), danach
alle fünf Korrekturen einzeln über das Edit-Werkzeug mit absolutem Pfad
angewandt — jede mit Trefferprüfung, keine Sammelersetzung mehr.

Das ist dieselbe Lehre wie beim awk-Selektor in AP21, der `git grep`-Maskierung
in AP26 und dem `make`-Aufruf im Audit — mit einem Unterschied: dort hat das
Werkzeug still das Falsche **geprüft**, hier still das Falsche **geschrieben**.
Die zweite Variante ist gefährlicher, weil das Ergebnis danach unauffällig
aussieht und der Fehler erst beim Lesen auffällt.

### Zwei Deutungen entschärft

* Ein Satz sagte, OpenIPC sei "nicht hinter Stock zurück, sondern davor —
  jedenfalls **hinter** dem Stand, den der AP-Auszug beschreibt". Das
  widerspricht sich in der eigenen Zeile.
* Das `#` vor `ingenic,drvvbus-gpio` hatte ich als "auskommentiert" gelesen; in
  DTS ist `#` kein Kommentarzeichen. Steht jetzt als Deutung fremden Textes da.
* `dtrng_dev.ko` hatte ich "Hardware-Zufallsgenerator" genannt — das ist aus
  dem Dateinamen geschlossen, nicht geprüft.

### Nachgeprüft und bestätigt

| Behauptung | Ergebnis |
|---|---|
| Machino hat keinerlei USB-Bezug | **bestätigt** — `grep` über `src/` und `openipc/` nach usb, ttyUSB, cdc_, rndis, modem: null Treffer |
| `machino/tools/` ist der richtige Ort | **bestätigt** — dort liegen zehn weitere Werkzeuge; kein CI-Job fasst sie generisch an |
| keine `.ko` ausserhalb `/lib/modules` | **bestätigt** — `find / -xdev` |
| Klassentreiber fehlen | **bestätigt**, jetzt über den vollständigen Baum |
| FORCEHSTMODE / PRTPWR | unverändert — beide aus dem benannten `regdump`, nicht aus Hand-Offsets |
