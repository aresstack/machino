# EC200A: was wir aus dem ESP32-Projekt uebernehmen (AP-M1)

Analyse, kein Code. Nichts am Modem, nichts an der Kamera ausser lesenden
Abfragen. Referenz ist `Miguel0888/quectel-ec200a-eu`, insbesondere
`esp32-modem-host/` -- rund 3400 Zeilen bereits erarbeitete und am echten Modem
erprobte Modemlogik.

Die Leitlinie dieses AP: **das ESP32-Projekt ist eine Referenzimplementierung,
keine Inspiration.** Was dort fachlich geloest ist, wird portiert. Ersetzt wird
nur die Plattformunterseite -- und die ist auf Linux deutlich kleiner, weil der
Kernel uebernimmt, was auf dem ESP32 von Hand gebaut werden musste.

---

## 1. Gemessene Ausgangslage auf der Kamera

Alles hier ist abgefragt, nicht angenommen.

    Kernel                4.4.94
    vermagic              4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT
    USB-Host-Kern         eingebaut (24 usb_hcd_/usb_register_driver-Symbole)

    /lib/modules/4.4.94:  usbserial.ko  VORHANDEN
                          option.ko     FEHLT
                          usb_wwan.ko   FEHLT
                          usbnet.ko     FEHLT
                          cdc_ether.ko  FEHLT
                          cdc_ncm.ko    FEHLT
                          rndis_host.ko FEHLT

Ein erster Treffer auf "option" in `/proc/kallsyms` war ein **Fehlalarm**:
die Symbole heissen `cgroup_show_options`, `save_mount_options` und so weiter.
Es gibt keinen option-Treiber in diesem Kernel.

Der vermagic ist Zeichen fuer Zeichen derselbe, den der Workflow
`build-aic8800-t40` bereits erzeugt. Derselbe Kernelbaum
(`OpenIPC/linux @ ingenic-t40`) traegt also auch die Modemmodule -- AP-M2 muss
keine neue Toolchain- und Kernelkette aufbauen, sondern die vorhandene
erweitern.

### Kennt der 4.4-`option`-Treiber den EC200A?

Nein. Nachgesehen in `drivers/usb/serial/option.c` dieses Kernels:

    #define QUECTEL_VENDOR_ID   0x2c7c     vorhanden
    QUECTEL_PRODUCT_EC21        0x0121     vorhanden
    QUECTEL_PRODUCT_EC25        0x0125     vorhanden
    0x6005                                 NICHT vorhanden

Und der Punkt, den man beim Abschreiben falsch machen wuerde: die EC21/EC25-
Eintraege benutzen `net_intf4_blacklist` mit `.reserved = BIT(4)`.

Dass `.reserved` wirklich das Binden verhindert (und nicht nur `send_setup`
beeinflusst, wie das zweite Feld der Struktur), ist am Quelltext geprueft --
`option_probe` hat **keinen Klassenfilter** ausser Mass Storage:

```c
/* Never bind to the CD-Rom emulation interface */
if (iface_desc->bInterfaceClass == 0x08)
        return -ENODEV;
/*
 * Don't bind reserved interfaces (like network ones) which often have
 * the same class/subclass/protocol as the serial interfaces.
 */
blacklist = (void *)id->driver_info;
if (blacklist && test_bit(iface_desc->bInterfaceNumber, &blacklist->reserved))
        return -ENODEV;
```

Ein blanker `USB_DEVICE(0x2c7c, 0x6005)`-Eintrag ohne `.reserved` wuerde also
auch die Netzwerk-Interfaces beanspruchen -- CDC-Klassen 0x02 und 0x0A werden
nirgends ausgesiebt. Der Kernelkommentar sagt genau das.

Beim EC200A ist das Netzwerk **MI_00**. Belegt in `README.md` Zeile 7:
"Interfaces: MI_00 ECM, MI_02 Diag, MI_03 AT, MI_04 Modem", und Zeile 89
bestaetigt, dass auch nach der Umstellung auf RNDIS dasselbe MI_00 bindet --
die Nummer scheint also ueber beide usbnet-Modi stabil.

Ein kopierter EC25-Eintrag wuerde Interface 4 reservieren und Interface 0 an
`option` binden. `net_intf0_blacklist` mit `.reserved = BIT(0)` existiert in
diesem Kernel bereits.

**Aber vermutlich reicht BIT(0) nicht.** Eine CDC-ECM-Funktion belegt ZWEI
Interfaces -- Communication (0x02/0x06) und Data (0x0A) -- und das Repo nennt
nur MI_00, MI_02, MI_03, MI_04 ("damit haben alle 4 Interfaces einen Treiber").
MI_01 taucht nirgends auf, was gut dazu passt, dass Windows das ECM-Paar als
ein Geraet fuehrt. Ist das so, muss der Eintrag beide reservieren:

    .reserved = BIT(0) | BIT(1)

Praezedenz dafuer steht in derselben Datei (`telit_le922_blacklist_usbcfg3`
reserviert BIT(0)|BIT(1)|BIT(3)). Das ist eine begruendete Vermutung aus der
CDC-Konvention, kein Messwert -- der Descriptor entscheidet. Siehe UNKNOWN 1.

Die Alternative aus `doc/linux.md` -- `echo 2c7c 6005 > .../option1/new_id` --
braucht keinen Patch, hat aber genau dieses Problem in verschaerfter Form:
`new_id` transportiert kein `driver_info`, also gibt es keine Reservierung, und
`option` nimmt jedes Interface.

`cdc_ether` dagegen braucht keinen geraetespezifischen Eintrag: es matcht
generisch ueber `USB_CLASS_COMM / USB_CDC_SUBCLASS_ETHERNET / PROTO_NONE`.
Sobald das Modem im ECM-Modus ist, bindet es von allein.

### Die Modulliste ist vollstaendig -- nachgerechnet

`USB_USBNET` hat in der Kconfig dieses Kernels ein `select MII`. Damit waere
`mii.ko` ein fuenftes Modul gewesen. Ist es nicht: MII ist in diesen Kernel
**eingebaut**, es gibt kein `mii.ko`, und die Symbole stehen als globaler
Kerneltext bereit:

    8026df78 T mii_ethtool_gset
    8026e19c T mii_ethtool_sset
    8026e4d4 T mii_link_ok

`usbnet.ko` findet sie also beim Laden. Vier Module reichen. Das stand hier
zuerst als Annahme und ist jetzt geprueft -- `select` in einer Kconfig heisst
nicht, dass das Ergebnis ein Modul ist.

Die Kconfig-Symbole, die AP-M2 im Build aktivieren muss:

    CONFIG_USB_SERIAL=m          (usbserial.ko existiert bereits im Image)
    CONFIG_USB_SERIAL_WWAN=m
    CONFIG_USB_SERIAL_OPTION=m
    CONFIG_USB_USBNET=m
    CONFIG_USB_NET_CDCETHER=m

Der Ausgangszustand der Board-Config (`br-ext-chip-ingenic/board/t40/
t40.generic.config`), im CI-Lauf abgelesen:

    # CONFIG_MODVERSIONS is not set          <- die Praemisse der ganzen
                                                Modul-Strategie, bestaetigt
    CONFIG_USB_SERIAL=m                      <- daher usbserial.ko im Image
    # CONFIG_USB_SERIAL_OPTION is not set
    # CONFIG_USB_USBNET is not set

USBNET ist also ausdruecklich abgeschaltet, nicht nur nicht mitgeliefert. Wir
schalten es ein und bauen die Module -- das Kernelimage bleibt unberuehrt.

Offen und in AP-M2 zu entscheiden: ob wir das vorhandene `usbserial.ko` des
Images benutzen oder ein eigenes mitliefern. Der vermagic passt, und
`CONFIG_MODVERSIONS=n` heisst, dass es keine Symbol-CRCs gibt -- unser
`option.ko` wuerde also gegen das Image-Modul laden. Verlassen wuerde man sich
damit darauf, dass das Image-Modul aus derselben Quellversion stammt, und das
ist wahrscheinlich, aber nicht geprueft.

### Referenzdaten zum Wiedererkennen des Geraets

Aus dem Repo, am echten Modem abgelesen:

    ATI          EC200A
    Firmware     EC200AEUV1HAR02A07M16
    VID:PID      2c7c:6005

### Das Modem steht aktuell nicht auf ECM

Aus `doc/ecm-host-plan.md`, am echten Modem verifiziert:

    AT+QCFG="usbnet"   -> 3   (RNDIS)
    AT+QNETDEVCTL?     -> 0,0,0,0   (Datenkanal aus)

Der erste Linux-Kontakt sieht also **kein** ECM. Die Umschaltung laeuft ueber
den AT-Port, und der existiert unabhaengig vom usbnet-Modus. Daraus folgt eine
harte Reihenfolge fuer AP-M2/M4: `option` + `usb_wwan` sind die Voraussetzung,
`cdc_ether` wird erst nach der Umschaltung und der Re-Enumeration gebraucht.

---

## 2. Reuse-Matrix

| ESP32-Komponente | fachliche Verantwortung | Entscheidung | Machino-Ziel | Linux-Abhaengigkeit |
|---|---|---|---|---|
| `ec200a_modem.*` (2059 Z.) | AT-Kanal, Registrierung, RAT/Baender, RF-Snapshot, Recovery, Observer | **PORTIEREN** (Fachlogik) | `QuectelEc200a` / `CellularService` | ttyUSB statt USB-Bulk |
| `modem_sim.*` (167 Z.) | SIM-Bereitschaft, PIN-Sicherheit, PIN-Verwaltung | **PORTIEREN, nahezu 1:1** | `SimManager` | keine |
| `ec200a_ecm.*` (812 Z.) | ECM-Control-State-Machine, `QCFG usbnet`, `QNETDEVCTL`, Re-Enumeration, Fallback-Regel | **FACHLOGIK PORTIEREN** | `LinuxEcmBackend` | `cdc_ether`/`usb0` ersetzt den Datenpfad |
| `ec200a_ecm.*` Bulk-/netif-Teil | rohe Ethernet-Frames ueber USB-Bulk, lwIP-netif | **ERSETZEN** | -- | Kernel `usbnet` |
| `modem_datalink.*` | genau EINE Stelle entscheidet PPP vs. ECM | **PORTIEREN (Idee)** | `CellularService` | keine |
| `wan_policy.*` | logische Uplink-Praeferenz auto/cellular/wifi | **NICHT portieren, ABBILDEN** | vorhandene `net::UplinkPolicy` | keine |
| `wan_service.*` | WAN-Aufloesung + echter Internet-Check | **NICHT portieren, ABBILDEN** | vorhandene `ConnectivityService` | keine |
| AT-Parser (ATI, CGSN, QCCID, CIMI, CPIN, CEREG, CSQ, COPS, QNWINFO, QENG, CGPADDR, CGCONTRDP) | Statusmodell | **PORTIEREN** | `CellularStatus` | keine |
| Provider-Profile (o2 `netpublic`, Telekom `internet.t-d1.de`) | bekannte funktionierende Konfigurationen | **UEBERNEHMEN als Defaults** | Presets | keine |
| `modem_clock.*` | Netzzeit ueber `AT+CTZU=1` / `AT+CCLK?` | **SPAETER** (eigener AP) | -- | keine |
| PPP (`pppStart`/`pppStop`, lwIP-Pumpe) | PPP-Datenpfad | **SPAETER (AP-M7)** | -- | `pppd` statt lwIP |
| `modemSpeedtest*`, `modemStartBandScan`, `modemNeighbourDump` | Diagnose-Extras | **SPAETER** | -- | keine |
| ESP32 `usb_host`-API, Claim/Endpoints, `modemUsbRootPortCycle` | USB-Transport | **ERSETZEN** | Kernel + vorhandene GPIO-50-Portsteuerung | `option`, `usb_wwan`, `usbnet` |
| ESP-IDF-Tasks, NVS/Preferences | Nebenlaeufigkeit, Persistenz | **ERSETZEN** | machino-Eventloop + `ConfigStore` | keine |
| `network_registry.*`, `net_scan.*`, `network_mode.*` | ESP32-Netzzonen/Diagnose | **NICHT portieren** | -- | ausserhalb des Auftrags |
| `ui_wan.cpp`, `ui_internet.cpp`, `ui_netscan.cpp` | ESP32-Web-UI | **Datenmodell wiederverwenden, Markup nicht** | `/machino/modem` | keine |

### Kurzlisten

    REUSE    ec200a_modem (Logik), modem_sim, ec200a_ecm (Control),
             modem_datalink (Idee), alle AT-Parser, Provider-Profile,
             Recovery-Semantik, ECM->PPP-Fallbackregel

    REPLACE  ESP32 usb_host, Interface-Claim, Bulk-Pumpe, lwIP-netif,
             ESP-IDF-Tasks, NVS, modemUsbRootPortCycle

    DEFER    PPP (AP-M7), modem_clock, Speedtest, Bandscan,
             Nachbarzellen, IPv6

    UNKNOWN  siehe Abschnitt 5

---

## 3. Was beim Portieren leicht kaputtgeht

Drei Stellen, an denen eine naive Uebernahme die Semantik verliert.

### 3.1 Der PIN-Schutz haengt am Prozessleben, nicht am Boot

`modem_sim.cpp` sendet die konfigurierte PIN **genau einmal je Boot**:

```cpp
static bool   s_pinTried   = false;
static String s_pinTriedFor = "";
...
if (s_pinTried && s_pinTriedFor == modemSimPin)
    return setStatus("PIN-Versuch bereits fehlgeschlagen - kein weiterer Versuch (PUK-Schutz)");
```

Der Zaehler haengt zusaetzlich am PIN-WERT: eine korrigierte PIN darf wieder
versuchen, dieselbe abgelehnte nicht. Und ein erfolgreiches Entsperren setzt
ihn zurueck.

Auf dem ESP32 ist "je Boot" dasselbe wie "je Prozess" -- es gibt nur einen und
er wird nicht neu gestartet. **Auf Linux nicht.** Ein Supervisor, der abstuerzt
und neu startet, faengt mit `s_pinTried = false` an und schickt dieselbe
abgelehnte PIN erneut. Drei solche Neustarts, und die SIM ist im PUK.

Der Port muss diesen Zaehler also ausserhalb des Prozesses halten, und zwar an
einem Ort, der einen Prozessneustart ueberlebt, aber einen Reboot **nicht** --
also `/var/run` (tmpfs), nicht das Overlay. Das ist keine Kleinigkeit, sondern
der Unterschied zwischen "SIM funktioniert" und "SIM ist gesperrt und der
Kunde braucht seinen PUK".

### 3.2 `AT+QCFG="nat"` entscheidet, wer die oeffentliche IP bekommt

`modemNatMode` in `ec200a_modem.h`:

    "nic"      Default, oeffentliche IP direkt am Host
    "routing"  Modem-NAT, 192.168.43.x, kein Inbound

Das ist leicht zu uebersehen und entscheidet, ob die Kamera von aussen
erreichbar ist. Gehoert in den Port, nicht in eine spaetere Feinschliff-Runde.

### 3.3 Der ECM-Fallback darf bei SIM-Problemen nicht feuern

`ec200a_ecm.cpp`:

```cpp
if (!simProblem && ++g_ecmFailStreak >= 3 && !g_ecmFallback) {
    g_ecmFallback = true;   // -> PPP
```

Ein SIM-Problem loest **keinen** Fallback aus -- richtig so, denn PPP wuerde es
nicht beheben und jeder Versuch kostet potenziell einen PIN-Versuch. Diese
Bedingung muss mitwandern, auch wenn PPP selbst erst in AP-M7 kommt.

---

## 4. Wie das in Machinos vorhandene Architektur passt

Der wichtigste Befund hier ist, was wir **nicht** bauen.

`wan_policy.*` und `wan_service.*` loesen ein Problem, das machino bereits
geloest hat. Die laufende API liefert heute schon:

```json
"policy":{"order":["ethernet","wifi","cellular"],
          "autoFailover":true,"returnToPreferred":true,
          "pinned":false,"pinnedUplink":""}
```

`cellular` steht dort bereits drin. Es gibt einen `ConnectivityService` mit
Uplink-Bewertung, Pfadwechsel-Benachrichtigung und einer Candidate/Confirm-
Transaktion mit Rollback -- auf Hardware bewiesen. Eine zweite
Netzwerkarchitektur daneben zu stellen waere der Fehler, vor dem der Auftrag
ausdruecklich warnt.

Was aus dem ESP32-Modell trotzdem mitmuss, ist die **Aufloesungsregel**: der
Bedienende waehlt "Mobilfunk", nie `usb0`, `ECM`, `PPP` oder `ttyUSB4`.
Intern loest das auf `cellular-ecm` und spaeter optional `cellular-ppp` auf.
Fuer machino heisst das: ein `CellularUplink` implementiert
`net::INetworkUplink` genau so, wie `WifiStationUplink` es tut, und der
Datenpfad dahinter ist eine interne Entscheidung.

Zielstruktur, an machinos Namensgebung angelehnt:

    src/core/cellular/cellular_service.{hpp,cpp}    Lebenszyklus, Zustand
    src/core/cellular/at_parser.{hpp,cpp}           reine Parser, host-testbar
    src/core/cellular/sim_manager.{hpp,cpp}         PIN-Semantik aus modem_sim
    src/adapters/linux/at_transport.{hpp,cpp}       ttyUSB, termios
    src/adapters/linux/quectel_ec200a.{hpp,cpp}     AT-Sequenzen, QCFG/QNETDEVCTL
    src/adapters/linux/cellular_uplink.{hpp,cpp}    INetworkUplink ueber usb0

Die Parser gehoeren bewusst in `core` und ohne I/O: `+QENG`, `+CEREG`, `+CSQ`
sind reine Textverarbeitung und damit vollstaendig auf dem Host testbar --
genau die Sorte Code, bei der Hardwaretests nichts beweisen, was ein Hosttest
nicht besser beweist.

### USB-Verwendung wird exklusiv

Es gibt einen Port. Zwei unabhaengige Haken waeren eine Falle. Der heutige
Schluessel

    usb.wifi.enabled = false

wird damit zu

    usb.function = off | wifi | cellular

Das ist eine bewusste Aenderung an einer Einstellung, die es seit heute Nacht
gibt und die noch nirgends produktiv gesetzt ist -- der Zeitpunkt dafuer ist
jetzt und nicht spaeter. Die Migration ist trivial (`usb.wifi.enabled = true`
-> `usb.function = wifi`), muss aber tatsaechlich stattfinden, weil
`S42wifi` sonst still auf AUS faellt. Das gehoert in AP-M6, nicht frueher.

---

## 5. Echte offene Fragen fuer AP-M2

Nicht geraten, nicht gefuellt.

1. **Wieviele Interfaces belegt die Netzwerkfunktion, und welche Nummern?**
   Das Repo dokumentiert MI_00 ECM, MI_02 Diag, MI_03 AT, MI_04 Modem und
   spricht von "allen 4 Interfaces". Eine CDC-ECM-Funktion braucht aber zwei
   (Communication + Data), was MI_01 nahelegt -- nur nennt es niemand. Belegt
   ist das Ganze ausserdem am WINDOWS-Treiber, nicht am Linux-Descriptor. Davon
   haengt ab, ob der `option.c`-Eintrag `BIT(0)` oder `BIT(0)|BIT(1)` (oder
   etwas anderes) reservieren muss -- und ein falscher Wert heisst entweder
   "kein AT-Port" oder "kein Netzwerk". Erste Handlung in AP-M2: den
   Config-Descriptor auslesen, in BEIDEN usbnet-Modi.

   Nebenbefund fuer die Reihenfolge: die V1.4-INF deckt MI_02/03/**06**/**20**
   ab. Es gibt also Konfigurationen mit mehr Interfaces (MI_06 ist der
   NMEA/GNSS-Port). In der aktuellen Konfiguration des Geraets treten sie nicht
   auf, aber die Interfacenummern sind nichts, worauf man sich blind verlaesst.

2. **Braucht `option` fuer diesen ASR-Chip ueberhaupt einen Eintrag, oder
   genuegt `usb_wwan` generisch?** Nicht geprueft.

3. **Wieviel Strom zieht das EC200A im Sendebetrieb, und traegt PB18 das?**
   Der AIC8800 laeuft am Port, das beweist nur, dass die Schiene HS-USB kann.
   Die Vorgabe aus dem Vorprojekt lautet aktiver Hub bzw. externe Versorgung,
   und dabei bleibt es beim ersten Anschluss.

4. **Verhaelt sich die Re-Enumeration nach `AT+CFUN=1,1` auf diesem
   USB-Host-Kern sauber?** Auf dem ESP32 brauchte es dafuer eine eigene
   Recovery-Stufenleiter samt Root-Port-Cycle. Linux macht das normalerweise
   von selbst; "normalerweise" ist auf dieser Kamera aber schon mehrfach falsch
   gewesen. Machino hat mit der GPIO-50-Portsteuerung bereits das Werkzeug fuer
   einen echten Power-Cycle -- ob es gebraucht wird, ist offen.

5. **Reicht `cdc_ether`, oder braucht der ASR-basierte EC200A `cdc_ncm`?**
   Der Repo-Pfad ist ECM. NCM ist dokumentiert, aber nicht erprobt.

6. **Overlay-Platz.** Die WLAN-Nutzlast belegt bereits rund 2 MB von 8,7. Vier
   weitere Module kommen dazu. Da `usb.function` exklusiv ist, koennte der
   Installer beide Nutzlasten anbieten und nur eine installieren -- zu
   entscheiden, wenn die Modulgroessen bekannt sind.

---

## 6. Was dieser AP ausdruecklich nicht getan hat

Keine Kernelmodule gebaut, kein Modem angeschlossen, kein GPIO geschaltet,
kein AT-Kommando an Hardware gesendet, kein ttyUSB, kein ECM, kein DHCP, keine
Routenaenderung, keine UI, kein PPP, kein OpenIPC-Patch. Die Abfragen an der
Kamera waren ausschliesslich lesend (`uname`, `find`, `strings`,
`/proc/kallsyms`).
