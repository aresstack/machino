# Offene physische Prüfungen — Stand 2026-09-23

Erzeugt am Ende des Batchlaufs AP21–AP23, konsolidiert über AP0–AP23.

Alles hier ist **nicht** durch Hosttests oder CI abzudecken. Vieles davon
braucht nur einen Kaltstart; einiges braucht einen Menschen am Gerät.

---

## Die Klammer um fast alles: der Daemon läuft noch auf `c1edd92`

```
laufender Prozess     machino c1edd92   (pid 992, uptime > 5 h)
auf /usr/bin/machino  machino 341a8d4
im Repo               alles bis 53b08f2 (AP21) ist NICHT deployt
```

Ein **Warmstart des Daemons ist der dokumentierte Hardlock-Auslöser**
(`t40nn-freeze-nach-install`). Deshalb liegt seit AP14 ein neueres Binary auf
der Platte, ohne dass der Prozess abgelöst wurde, und deshalb hängt fast jede
offene Prüfung am nächsten **Cold Power-Cycle**.

Die Reihenfolge nach dem Kaltstart ist nicht beliebig: erst die Dinge, die
ohne Last messbar sind, dann Last, dann Browser.

---

## A — Beim nächsten Kaltstart, ohne zusätzliches Risiko

| Nr | Prüfung | Woher |
|---|---|---|
| A1 | `printk` steht nach dem Boot auf `3 3 1 3` (rc.local greift erst beim Start) | AP17 |
| A2 | `/api/v1/telemetry`: `watchdog_available: true`, `watchdog_enabled: true` | AP4 |
| A3 | `init_retries` ist **0** und bleibt es | Fix B |
| A4 | Baseline-Messung AP0.4–0.6 (Boot, ein MAIN-Zyklus, Relay-Smoke) | AP0 |
| A5 | `ws_video_*`-Zähler erscheinen in der Telemetrie und bewegen sich | AP15 |
| A6 | Driftmessung `tools/mse-drift.ps1` gegen den **neuen** Build wiederholen | AP15 |
| A7 | `/ws/upgrade` antwortet mit der Ablehnung, und die Update-Seite zeigt „Nothing was written to flash" | AP21 |
| A8 | Dashboard-Kachel: eigene Snapshot-Meldung statt Schweigen (`jpeg`-Sektion) | AP14 |
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
| H2 | `build-aic8800-t40.yml` | **Läuft inzwischen.** Beide Module bauen (`aic_load_fw.ko` 92 224 B, `aic8800.ko` 552 512 B) mit dem vermagic der Kamera. Offen sind nur noch Symbol-Gate und Alias-Report |
| H3 | `release-machino.yml` | unverändert: geschrieben, nie ausgeführt |

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
| H5 | Bindet das AIC8800-Modul das Gerät und erscheint `wlan0`? Ein Modul kann laden und trotzdem nicht binden — das sieht aus wie ein totes Funkmodul |
| H6 | Tatsächliches Assoziieren mit einem WPA2-Netz, inklusive DHCP-Lease |
| H7 | Access Point: `hostapd` vorhanden, startet mit der erzeugten Konfiguration, ein Telefon assoziiert und bekommt eine Adresse |
| H7a | **Reconfiguration ohne Prozessneustart.** Laufender AP mit SSID A, neue Konfiguration SSID B, `RELOAD_CONFIG` (bzw. SIGHUP als Fallback), danach muss `GET_CONFIG` B melden und ein Scan B sehen. Der Adapter prüft das selbst und schlägt fehl, wenn es nicht stimmt — dass der Pfad auf diesem `hostapd`-Build überhaupt existiert, ist aber ungeprüft |
| H7b | **Station → AP auf demselben PHY.** `DISABLE_NETWORK all` + `DISCONNECT` müssen `wpa_supplicant` weit genug vom Funkmodul lösen, dass `hostapd` es übernehmen kann. Concurrency wird nirgends unterstellt (`driver_concurrent_sta_ap = false`); ob das Freigeben reicht, entscheidet die Hardware |
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
