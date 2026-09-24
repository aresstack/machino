
## I — WLAN als Produkt (Batch WLAN/AP, 2026-09-24)

### Auf Hardware bewiesen

  * Access Point: Telefon verbunden, DHCP-Adresse, WebUI ueber
    `http://192.168.24.1/`, Live-H.264 ueber den AP. Im Kernel-Log als
    assoziierte Station belegt (`Del sta 9 (be:43:e6:78:0b:63)`).
  * `change_if: 2 to 3` und zurueck `3 to 2` -- der Treiber behandelt Station
    und AP als Typwechsel eines Interface, nicht als zwei Betriebsarten.
  * Rollenwechsel Station -> AP -> Station vollstaendig, ohne
    Wiederholungsversuch, eth0 durchgehend unberuehrt (01:53:44 bis 00:54:40,
    siehe aic8800-bringup.md).
  * Boot-Gate in allen drei Zustaenden: Schluessel fehlt / `false` / `true`.
    Bei den ersten beiden meldet S42wifi "aus" und laedt nichts.
  * `claim_interface` beendet einen verwaisten Supplicant auf wlan0 und laesst
    den udhcpc von eth0 in Ruhe.

### `PENDING_PHYSICAL` — offen

  * **Assoziation an ein konkretes Netz nach dem Rollenwechsel.** Der
    Test-Hotspot "Viva Espana" war abgeschaltet; `iwlist scan` zeigte zehn
    andere Netze, dieses nicht. Der Supplicant steht korrekt auf
    `wpa_state=SCANNING`. Bewiesen ist der Rollenwechsel, nicht das
    Wiederfinden eines bestimmten Netzes.
  * **Boot mit `usb.wifi.enabled=true` und leerem Zustand.** Alle Gate-Tests
    liefen auf einer Kamera, deren Module bereits geladen waren. Dass
    `load_modules` beim echten Kaltstart aus `/etc/machino/modules` laedt, ist
    aus dem Bring-up bekannt, in dieser Fassung des Skripts aber nicht erneut
    gemessen.
  * **Installation aus dem Release-Artefakt heraus.** Die Nutzlast im Bundle
    ist geprueft (2,09 MB, 2 Module, 19 Firmware-Dateien, hostapd), aber
    `install.sh` wurde damit noch nicht auf der Kamera ausgefuehrt -- die
    Kamera traegt die von Hand kopierten Dateien.

### `PENDING_BROWSER` — offen

  * Die Registerkarte *USB-WLAN* auf `/machino/net`: Schalter, Hinweistext,
    der Wechsel der Meldung zwischen "nicht gespeichert" und dem Zustandstext,
    und dass Station/AP bei ausgeschaltetem WLAN "aus (USB-WLAN ist nicht
    aktiviert)" zeigen statt "Funkmodul nicht vorhanden". Nur im Simulator
    (test_netui) geprueft, nie in einem echten Browser gerendert.
Request) und braucht niemanden am Gerät — nur den abgelösten Build | AP30 HIGH-1 |
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

---

## G — USB / VBUS / Modem (AP35)

Alle softwareseitigen Fragen sind beantwortet (`ap35-usb.md`): Host-Modus ist
erzwungen, Portspannung ist an, PB27 ist vom Treiber belegt und high. Was
bleibt, ist Elektrik und Einstecken. Baseline vor jedem Test aufnehmen mit
`machino/tools/usb-inventory.sh`, danach erneut — die Ausgabe ist dafür
gebaut.

| # | Was | Warum physisch |
|---|---|---|
| G1 | **Spannung am USB-VCC messen**, dazu direkt an PB27 und am Ausgang des Load-Switch | Entscheidet die vier verbliebenen Hypothesen: Switch unbestückt / active-low / Messpunkt / defekt. Software kann hier nichts mehr beitragen |
| G2 | Beliebiges **Kleinlast-USB-Gerät** einstecken (Maus, Stick) und `usb-inventory.sh` erneut laufen lassen | Trennt "VBUS fehlt" von "Enumeration fehlt". Der Hub-Treiber ist vorhanden, ein Gerät müsste also in `/sys/bus/usb/devices` auftauchen — auch ohne Klassentreiber |
| G3 | **WiFi-Platine** einstecken, VID/PID ablesen | Ohne echte Enumeration wird der Chipsatz nicht aus dem Boardlayout geraten (AP35.11) |
| G4 | **EC200A** einstecken, möglichst an einem aktiv versorgten Hub | Laut eigener früherer Messung zieht das Modul 1–2 A Bursts und bootet an einem Host ohne aktiven VBUS-Switch nicht durch |
| G5 | **Stromaufnahme messen**, wenn G4 läuft | Maximale Stromfähigkeit des Boardpfads ist aus Software nicht ableitbar |
| G6 | Nach einem Kernel mit Klassentreibern: **Lastmessung** WebRTC-Latenz / Paketverlust / IRQ-Last unter Modemverkehr | AP35.15; heute nicht möglich, weil nichts enumerieren kann |

**G2 ist der billigste und aussagekräftigste Test** — eine USB-Maus genügt,
und das Ergebnis halbiert den Suchraum sofort.

### Stand von G nach der Messung am 2026-09-23

**G1, G2, G3 sind erledigt** und damit keine offenen Punkte mehr:

| Nr | Ergebnis |
|----|----------|
| G1 | **3,3 V am USB-VCC**, geschaltet über GPIO 50 = **PB18** (nicht PB27, das war ein aus Ingenics `shark.dts` geerbter Irrläufer). Der Schalter ist ein A1SHB-P-MOSFET, ein Transistor davor invertiert den Sinn: HIGH schaltet ein |
| G2 | entfällt — G3 hat die Enumeration direkt gezeigt |
| G3 | **AIC8800DC, `a69c:88dc`**, 3 Interfaces (2× Bluetooth `cls=e0`, 1× vendor `cls=ff`), 480 Mbit, 500 mA, `HPRT0 = 0x00001005` |

Die Formulierung „Portspannung AN" bleibt trotzdem zu stark für alles, was
**nicht** gemessen wurde: G4, G5 und G6 stehen unverändert offen.

---

## H — AP36 Konnektivität: was dieser Batch NICHT bewiesen hat

Der Batch (Netz-API, Uplink-Adapter, hostapd, WebUI) ist host-getestet und
CI-gebaut. Keiner der folgenden Punkte ist damit belegt, und keiner lässt sich
auf dem Entwicklungsrechner belegen.

### `PENDING_CI` — nie gelaufen

| Nr | Was | Warum offen |
|----|-----|-------------|
| ~~H1~~ | ~~Cross-Compile von `main.cpp`, `http_server.cpp` und allen `adapters/linux/*`~~ | **ERLEDIGT 2026-09-23.** `build-machino-t40` grün auf `b85b126`. Und es hat sich gelohnt: der Lauf fand vier Fehler, die lokal unsichtbar waren — siehe unten |
| ~~H2~~ | ~~`build-aic8800-t40.yml`~~ | **Der ABI-Vertrag ist erfüllt, siehe unten.** Beide Module bauen; vermagic stimmt; alle 159 undefinierten Symbole sind im laufenden Kernel der Kamera exportiert |
| H3 | `release-machino.yml` | unverändert: geschrieben, nie ausgeführt |

### AIC8800: der ABI-Vertrag ist erfüllt (2026-09-23)

Aus AP35.19 war „module-only ist strukturell möglich" eine **Schlussfolgerung**.
Jetzt ist es ein gebautes Artefakt mit drei nachgewiesenen Eigenschaften:

```
aic_load_fw/aic_load_fw.ko      92 224 Bytes
aic8800_fdrv/aic8800.ko        552 512 Bytes
vermagic                        4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT
undefinierte Symbole            159, davon im Kernel der Kamera exportiert: 159
```

Die Symbolprüfung lief **gegen `/proc/kallsyms` der laufenden Kamera**, nicht
gegen einen nachgebauten Kernel — also gegen genau den Kernel, in den das Modul
geladen würde. Der CI-Job hatte hier zuerst Alarm geschlagen (`printk`, `kfree`,
`memcpy` angeblich nicht exportiert); das war ein **Fehlalarm** aus einer
unvollständigen `Module.symvers`: `make modules` ohne `vmlinux` liefert nur 249
Einträge statt der 5716, die die Kamera tatsächlich exportiert. Der Job baut
jetzt `vmlinux modules` und bricht ab, wenn die Liste unplausibel kurz ist.

Hardwareseitig ebenfalls belegt, per UART gemessen:

```
GPIO 50 (PB18) = 1   ->  DEV a69c:88dc  speed=480  mA=500  ifs=3
                         mfr=AICSemi  prod=AIC8800DC
                         1-1:1.0 cls=e0 drv=none   (Bluetooth)
                         1-1:1.1 cls=e0 drv=none   (Bluetooth)
                         1-1:1.2 cls=ff drv=none   (vendor, der WLAN-Teil)
```

**Was weiterhin offen ist:** ob das Modul das Gerät *bindet* und ob `wlan0`
erscheint. Ein Modul kann laden und trotzdem nicht binden — das sieht aus wie
ein totes Funkmodul. Das ist H5 und bleibt `PENDING_PHYSICAL`, bis das Modul
tatsächlich geladen wurde.

### Was der Cross-Build gefunden hat, das die Hosttests nicht sehen konnten

Der Grund, warum H1 als eigener Punkt geführt wurde, hat sich beim ersten
Hinsehen sofort bestätigt. `PENDING_CI` war das richtige Etikett; **nicht
hinzusehen** war der Fehler — der Cross-Build war schon vor diesem Batch rot,
ohne dass es jemandem aufgefallen wäre.

| Fund | Warum lokal unsichtbar |
|------|------------------------|
| fehlendes `#include <unistd.h>` für `::readlink` in `linux_usb_host.cpp` | Die Datei braucht Linux-Header und wird hier nicht übersetzt. Die `static_assert`s nageln Signaturen fest, nicht Übersetzbarkeit |
| `system(3)` ist unter glibc `warn_unused_result`, also `-Werror` | Der Host-Compiler markiert es nicht so. Ausgerechnet im neuen Durability-Test |
| `sun_path` ist 108 Bytes — der Socket-Pfad konnte still gekürzt werden | `-Wformat-truncation` ist im Cross-Build an. **Kein kosmetischer Hinweis:** eine gekürzte Unix-Socket-Adresse benennt einen *anderen* Socket |
| dieselbe Klasse bei `ifr_name` (16 Bytes) | dito; durch die vorhandene Längenprüfung nicht erreichbar, aber präventiv beseitigt |

### `PENDING_PHYSICAL` — braucht die Kamera

| Nr | Was |
|----|-----|
| H4 | Läuft `wpa_supplicant` auf diesem Image überhaupt, und liegt sein Control-Socket unter `/var/run/wpa_supplicant/wlan0`? Der ganze Stationspfad hängt daran |
| ~~H5~~ | **ERLEDIGT 2026-09-23.** Modul bindet `1-1:1.2`, `wlan0` erscheint, Scan liefert 14 Netze. Pfad und Fallstricke in `aic8800-bringup.md` |
| ~~H6~~ | **ERLEDIGT 2026-09-23.** WPA2/CCMP assoziiert (`wpa_state=COMPLETED`, RSSI −31), DHCP-Lease 172.21.115.79, Ethernet parallel erreichbar. Details in `aic8800-bringup.md` |
| H7 | Access Point: `hostapd` vorhanden, startet mit der erzeugten Konfiguration, ein Telefon assoziiert und bekommt eine Adresse |
| H7a | **Reconfiguration ohne Prozessneustart.** Laufender AP mit SSID A, neue Konfiguration SSID B, `RELOAD_CONFIG` (bzw. SIGHUP als Fallback), danach muss `GET_CONFIG` B melden und ein Scan B sehen. Der Adapter prüft das selbst und schlägt fehl, wenn es nicht stimmt — dass der Pfad auf diesem `hostapd`-Build überhaupt existiert, ist aber ungeprüft |
| H7b | **Station → AP auf demselben PHY.** `DISABLE_NETWORK all` + `DISCONNECT` müssen `wpa_supplicant` weit genug vom Funkmodul lösen, dass `hostapd` es übernehmen kann. Concurrency wird nirgends unterstellt (`driver_concurrent_sta_ap = false`); ob das Freigeben reicht, entscheidet die Hardware |
| H5a | Bleibt `wlan0` über einen Reboot-Zyklus stabil, wenn Module und GPIO wieder gesetzt werden? Bisher einmalig aufgebaut, nicht wiederholt |
| H7c | **Geänderter DHCP-Pool.** SSID und Schlüssel wirken über den Control-Socket sofort, der Pool erst nach `/etc/init.d/S41hostapd restart` — `udhcpd` liest seine Konfiguration nicht neu. Die Einschränkung ist im Log und im Skript benannt; dass sie genau so eintritt, ist ungeprüft |
| H7d | **`driver_ap_known` bleibt heute meist `false`.** Dieser Build fragt den Treiber nicht über nl80211, sondern schließt nur aus einem laufenden, aktivierten BSS auf AP-Fähigkeit. Bis die nl80211-Abfrage existiert, ist AP-Modus „versuchbar, nicht bestätigt" — die WebUI sagt das auch so |
| H8 | Failover Ethernet → WLAN und zurück, mit laufendem RTSP/WebRTC: bricht die Session, und erholt sie sich? |
| H9 | **Der Rollback-Pfad unter realem Verbindungsverlust.** Genau der Fall, für den `NetworkTxn` existiert: falsche WLAN-Konfiguration setzen, Verbindung verlieren, warten, und die Kamera muss von selbst zurückkommen. Host-Tests decken die Logik ab, nicht den Stromausfall mittendrin |
| H10 | Speicher- und CPU-Wirkung des 2-s-Netz-Ticks über einen COLD_IDLE-Soak. Der Trend aus AP2x (~+11 kB/Zyklus) ist ungeklärt, und hier kommt eine neue periodische Last dazu |

### `PENDING_BROWSER` — nie gerendert

| Nr | Was |
|----|-----|
| H11 | `/machino/net` in einem Browser öffnen. Die Feldnamen sind gegen die echten JSON-Builder getestet (`tests/test_netui.cpp`), das Layout ist es nicht |
| H12 | Der Bestätigungs-Countdown im Ernstfall: Banner sichtbar, Zähler läuft, Bestätigung kommt an, Rollback wird als solcher angezeigt |
| H13 | Der Menüeintrag aus `install.sh --with-network-page` an einer echten `header.cgi` — die Tests benutzen einen nachgebauten Ausschnitt |

---

## AP-M5 — Mobilfunk als Uplink, Routing und Failover

Alles hier ist softwareseitig fertig und hostseitig geprüft. Was fehlt, ist die
Kamera. Kein Punkt dieser Liste wurde geschätzt, geraten oder aus einem grünen
Test abgeleitet — die Hosttests prüfen die Entscheidung, nicht den Kernel.

### `PENDING_PHYSICAL`

| Nr | Was |
|----|-----|
| M5-1 | **rtnetlink auf diesem Kernel.** `LinuxRouteBackend` setzt Default-Routen über `RTM_NEWROUTE`/`RTM_DELROUTE` statt über `ip route` — ein Socket, kein `fork()`. Dass 4.4.94 auf dieser Box die Nachricht so annimmt (`RTA_OIF`, `RTA_PRIORITY`, `RT_SCOPE_LINK` bei fehlendem Gateway) ist ungeprüft. Der ACK wird gelesen, ein Fehler landet also im Log statt still zu verschwinden |
| M5-2 | **Die Metrik-Umschreibung im Betrieb.** Boot-Skript und DHCP-Hooks setzen ihre Startwerte (eth0 0, wlan0 200, Mobilfunk 300); machino soll sie durch die Policy-Metriken ersetzen, und zwar ohne dass die Kamera zwischendurch ohne Default-Route dasteht. Neu-vor-alt ist getestet, die Lücke in der Praxis nicht |
| M5-3 | **Eine fremde Default-Route wirklich in Ruhe lassen.** Ein VPN oder eine zweite Route von Hand anlegen und prüfen, dass die Reconciliation sie nicht anfasst. Am Gerät ist das der Fall, in dem ein Fehler die Kamera unerreichbar macht |
| M5-4 | **Failover Ethernet → Mobilfunk mit laufendem RTSP/WebRTC.** Das `path_change`-Signal geht raus, die Quelladresse und der NAT-Pfad wechseln. Ob die Transporte sich erholen, entscheidet keine Zustandsmaschine |
| M5-5 | **Die Entprellung mit echtem Zittern.** 3 s zum Verlassen, 15 s zum Zurückkehren — die Zahlen sind aus dem Verhalten hergeleitet, das vermieden werden soll, nicht aus einer Messung an einem wackelnden Link |
| M5-6 | **DNS-Eigentum und die Rückgabe.** Mobilfunk aktiv, `resolv.conf` trägt die Resolver des Anbieters; zurück auf Ethernet, und die vorherigen müssen wieder dastehen. Genau dieser Fall war der Review-Befund, und die Momentaufnahme überlebt **keinen** Daemon-Neustart — stirbt machino, während Mobilfunk aktiv ist, bleiben die Resolver des Anbieters, bis ein Lease sie ersetzt |
| M5-7 | **Der AT-Port nach einer Re-Enumeration.** `rediscover_modem_port()` sucht neu, sobald der bisherige Pfad weg ist. Dass der neue `ttyUSB` dann schon da ist und nicht erst Sekunden später, ist eine Annahme |
| M5-8 | **Zwei Uplinks gleichzeitig oben.** Ethernet und Mobilfunk zusammen, ohne dass sich beide die Default-Route gegenseitig wegnehmen. Hostseitig deterministisch, am Kernel ungeprüft |
| M5-9 | **Last des Mobilfunk-Ticks.** Der 2-s-Takt schickt jetzt zusätzlich eine AT-Runde, solange Mobilfunk eingeschaltet ist. Wirkung auf RSS und CPU über einen Soak ist offen — derselbe offene Trend wie H10 |

### Was bewusst NICHT geprüft werden muss

`usb.cellular` ist per Vorgabe aus. Ohne eingeschalteten Mobilfunk schickt
`CellularUplink::tick()` **kein einziges AT-Kommando** und der Uplink meldet
`absent`; eine Kamera ohne Modem verhält sich also exakt wie vor AP-M5. Das ist
hostseitig festgenagelt (`test_switched_off_is_absent_and_sends_nothing`).

---

## AP-M6 — USB-Modus, Boot, Packaging

### `PENDING_PHYSICAL`

| Nr | Was |
|----|-----|
| M6-1 | **`usb.mode=off` auf einer Kamera, an der etwas steckt.** Die ganze Zusage lautet: kein Modul, kein Portstrom, kein Daemon. Nachzuweisen ist sie nur negativ — `lsmod` ohne aic/option, GPIO 50 unten, kein ttyUSB, kein wlan0 |
| M6-2 | **`usb.mode=cellular` laedt die Module in der beabsichtigten Reihenfolge.** Insbesondere: bindet `cdc_ether` das ECM-Interface, BEVOR `option` (ggf. ueber `new_id`) an die Reihe kommt? Die Reihenfolge ist aus der Reuse-Map hergeleitet, nicht gemessen. Geht sie schief, entsteht ein Modem mit AT-Port und ohne Datenpfad |
| M6-3 | **Der `new_id`-Notnagel.** Er laeuft nur, wenn nach 20 s kein `ttyUSB` da ist. Ob `option.c` dieses Kernels ihn ueberhaupt anbietet (`option1` vs. `option`) und ob er dann das schon gebundene Netzwerkinterface in Ruhe laesst, ist ungeprueft |
| M6-4 | **Der Moduswechsel ueber einen echten Neustart.** wifi → cellular → off, jeweils speichern, neu starten, und `machino-usb-helper status` muss danach dasselbe sagen wie die Seite |
| M6-5 | **Das Entfernen von `S42wifi` bei einem echten Upgrade.** Hostseitig geprueft; am Geraet haengt daran, ob nach dem Upgrade wirklich nur noch ein Boot-Skript laeuft |
| M6-6 | **Flash-Budget in Zahlen vom Geraet.** CI misst das entpackte Bundle. Was nach dem Installieren tatsaechlich auf dem Overlay liegt (inklusive Konfiguration und Logs), sagt nur `df` auf der Kamera |
| M6-7 | **Bootzeit.** Der Helfer wartet bis zu 15 s auf `wlan0` bzw. bis zu 20 s (plus 10 s Notnagel) auf einen `ttyUSB`. Im Fehlerfall — Modul da, Geraet nicht — verlaengert das den Boot messbar, und niemand hat gemessen, wie stark |

### `PENDING_BROWSER`

| Nr | Was |
|----|-----|
| M6-8 | Die Auswahl *USB-Nutzung* in einem Browser: drei Radios, Speichern, und der Hinweis muss danach „Aktiv ist noch …" sagen und nicht „Gespeichert" allein |
| M6-9 | Die Mobilfunkseite mit einem echten Modem: dass „–" wirklich dort steht, wo nichts gemessen wurde, und keine 0 |
| M6-10 | Die Seite bei `usb.mode != cellular`: der Hinweis oben muss erscheinen, die Zugangsdaten aber weiter ausfuellbar sein |

---

## AP-M7 — PPP als Alternativpfad

ECM bleibt der Hauptweg. PPP ist hostseitig fertig und am Geraet vollstaendig
ungeprueft — mehr als bei den vorigen Paketen, denn hier haengt fast alles an
einem fremden Programm (pppd) auf einem Port, den noch nie jemand belegt hat.

### `PENDING_PHYSICAL`

| Nr | Was |
|----|-----|
| M7-1 | **Laeuft das gebaute pppd auf der Kamera ueberhaupt?** Es ist gegen dieselbe musl-Toolchain gebaut wie machino und im CI auf `NEEDED` geprueft, aber gestartet hat es dort niemand |
| M7-2 | **Welcher `ttyUSB` ist der Modem-Port?** Die Zuordnung MI_03 = AT, MI_04 = Modem stammt aus der Reuse-Map und ist am EC200A nicht nachgemessen. Zeigt `p.modem` auf den falschen Port, waehlt pppd auf einem AT-Kanal und bekommt nie ein CONNECT |
| M7-3 | **Das Chat-Skript.** `\d\d+++\d\d` / `ATH` / `AT` / `ATD*99***1#` / `CONNECT` — jede Zeile davon ist aus der Referenz uebernommen, keine an diesem Modem mit chat(8) gelaufen. Insbesondere: braucht dieses Modem die Escape-Sequenz ueberhaupt, und antwortet es danach mit OK? |
| M7-4 | **Die Exit-Code-Tabelle.** `8 = kein CONNECT`, `11/19 = Authentifizierung`, `10 = Aushandlung`, `15/16 = Gegenstelle` stammen aus pppd(8). Dass dieses pppd sie in genau diesen Faellen liefert, ist Papier |
| M7-5 | **`AT+CGACT=0,1` auf dem AT-Port loest wirklich eine haengende Datensitzung.** Das ist der Kniff, mit dem die Referenz das "vorher half nur Neu-Anstecken" beseitigt hat — an ihrer Hardware, nicht an dieser |
| M7-6 | **`/etc/ppp/ip-up` wird aufgerufen und bekommt `DNS1`/`DNS2`.** Beides haengt daran, wie dieses pppd gebaut ist. Ohne den Hook sieht machino nie ein Interface, und der Anruf laeuft ins Aushandlungs-Timeout |
| M7-7 | **`nodefaultroute` haelt pppd wirklich von der Default-Route fern.** Tut es das nicht, uebernimmt der Mobilfunk den Management-Pfad, sobald jemand PPP waehlt |
| M7-8 | **Der Moduswechsel ECM → PPP ueber einen echten Neustart**, samt der Frage, ob `cdc_ether` danach wirklich nicht geladen ist und `option` alle Interfaces bekommt |
| M7-9 | **Die Rechte der Optionsdatei am Geraet.** Sie traegt das APN-Passwort und wird mit 0600 angelegt; dass das ueber dem overlayfs dieser Box so bleibt, ist ungeprueft |
| M7-10 | **Bundle-Groesse mit pppd.** Der Report misst sie im CI, sobald `build-ppp-t40` einmal gelaufen ist. Bis dahin ist die Zeile leer, und ob beide Nutzlasten plus pppd noch ins Overlay passen, ist eine Rechnung ohne die Zahl |

### Was hostseitig festgenagelt ist

Der Datenlink wird **nicht** automatisch gewechselt: scheitert ECM, bleibt es
bei ECM. Bei `dataLink=ppp` startet kein DHCP-Client, bei `ecm` kein pppd; der
Uplink heisst in beiden Faellen `cellular`; kein `QNETDEVCTL` und kein
`QCFG="usbnet"` verlaesst den PPP-Pfad; und weder PIN noch APN-Passwort stehen
in einem Dokument, einem Log oder einer Kommandozeile.
