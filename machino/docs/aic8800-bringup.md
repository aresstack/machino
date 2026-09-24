
### Rueckweg AP -> Station: drei Defekte, alle auf Hardware aufgefallen

Der Weg hin lief auf Anhieb, der Weg zurueck nicht. Im Supervisor-Log stand

    wifi-role: wpa_supplicant startete nicht

und dasselbe Kommando lief eine Minute spaeter von Hand mit rc=0. Also kein
Konfigurations-, sondern ein Zeitfehler. Der Beweis steht im Kernel-Log:

    [6807.085433] usb 1-1 wlan0: AP Stopped
    [6807.133940] change_if: 3 to 2, 8, 2

`stop_ap()` hatte auf das Verschwinden des hostapd-ctrl-Sockets gewartet. Der
ist sofort weg. Der Treiber baut das Interface danach noch um, und genau in
dieses Fenster hinein startete der Supplicant.

1. **kill(1) wartet nicht.** `kill_pidfile` hat SIGTERM geschickt und ist
   weitergelaufen. Jetzt pollt es `kill -0`, bis der Prozess wirklich weg ist,
   und eskaliert nach 5 s auf SIGKILL. Der Socket-Wartelauf ist damit
   ueberfluessig und entfernt.

2. **Der Start braucht Wiederholung, keine Wartezeit.** `retry_start` versucht
   den Daemon zehnmal im Sekundenabstand. Eine feste Pause waere entweder zu
   kurz oder verschenkte Sekunden bei jedem Wechsel.

3. **Zwei DHCP-Prozesse ohne PID-Datei.** BusyBox 1.36 `udhcpd` kennt kein -P,
   und die Direktive `pidfile` in udhcpd.conf schreibt dieser Build
   stillschweigend nicht -- nachgemessen: die Datei entstand nie. Der
   DHCP-Server des Access Points hat den Rollenwechsel ueberlebt und weiter
   Adressen aus einem Netz verteilt, das es nicht mehr gab. Jetzt laeuft er mit
   `-f` und wird selbst in den Hintergrund gelegt, damit `$!` exakt stimmt.
   Derselbe Fehler steckte spiegelbildlich im Station-Pfad: `udhcpc` wurde ohne
   `-p` gestartet.

Ausserdem hat die erste Fassung ihre Daemons mit `>/dev/null 2>&1` gestartet.
Der Fehlschlag war damit nicht diagnostizierbar -- es stand da, dass der
Supplicant nicht startete, und warum stand nirgends. Der Supervisor schreibt
jetzt nach `/tmp/machino-wifi-role.log` (tmpfs, nicht Flash: das ist Diagnose,
kein Zustand).

Nach dem Fix, auf der Kamera gemessen:

    wpa_supplicant -B -P /var/run/wpa_supplicant.wlan0.pid ...   laeuft
    udhcpc -i wlan0 -b -t 0 -S -p /var/run/udhcpc.wlan0.pid      laeuft
    /var/run/udhcpd.pid, /var/run/hostapd.pid                    weg
    wpa_state=SCANNING

Die Assoziation selbst ist NICHT nachgewiesen: der Test-Hotspot war zu diesem
Zeitpunkt aus (`iwlist scan` zeigt zehn andere Netze, "Viva Espana" nicht
darunter). Der Supervisor tut das Richtige und sucht.

### Der AP-Test selbst ist bestanden

Ein Telefon hat sich mit `Machino-Test` verbunden, eine Adresse bekommen, die
WebUI ueber `http://192.168.24.1/` geladen und den Live-H.264-Stream ueber den
Access Point gesehen. Im Kernel-Log ist der Client als assoziierte Station
belegt:

    usb 1-1 wlan0: Del sta 9 (be:43:e6:78:0b:63)
�lt den Watchdog** (argv[0] ist `majestic`, wegen der
Drop-in-Kompatibilität — deshalb liest sich der OOM-Log so, als sei ein
fremdes Majestic gestorben). Stirbt es, wird `/dev/watchdog` nicht mehr
gefüttert und die Hardware setzt zurück. Der Treiber meldet beim Schließen
`watchdog did not stop!`, also nowayout-Verhalten.

Folge für Tests: **den Mediendaemon nicht „zur Sicherheit" stoppen.** Genau
das erzeugt den Reset, den man vermeiden will. Der richtige Weg ist, den OOM
gar nicht erst entstehen zu lassen.

### 3. `cfg80211` ist nicht eingebaut

`aic8800.ko` braucht 44 `cfg80211_*`-Symbole. `cfg80211.ko` und `mac80211.ko`
liegen unter `/lib/modules/4.4.94/kernel/net/`, werden aber nicht geladen.
Ohne vorheriges `modprobe cfg80211` scheitert `insmod` mit unbekannten
Symbolen — was wie „dieses Board kann kein WLAN" aussieht und reine
Ladereihenfolge ist.

## Ein Fallstrick beim Debuggen

Nach jedem Reboot ist **GPIO 50 nicht gesetzt**, also liegt kein Portstrom an
und das Gerät ist gar nicht da. Beim ersten Versuch nach den OOM-Reboots sah
das exakt aus wie „Modul lädt, bindet aber nicht":

```
lsmod            aic8800 geladen
/sys/class/net   kein wlan0
1-1:1.2/driver   No such file or directory
```

Der richtige nächste Blick ist `ls /sys/bus/usb/devices/` — steht dort nur
`usb1` und `1-0:1.0`, fehlt der Strom, nicht der Treiber.

## Was NICHT eingerichtet ist

Bewusst nichts davon ist persistent:

| | |
|---|---|
| Module | in `/tmp`, kein Autoload, nach Reboot weg |
| GPIO 50 | nicht im Boot, nach Reboot aus |
| Default-Route | unverändert auf Ethernet |
| Firmware | **liegt** persistent unter `/lib/firmware/aic8800DC` (17 Dateien, 362 KB) |

Die Trennung ist Absicht: die Firmware zu haben schadet nichts, ein
automatisch ladendes Modul in dieser Phase schon.

## Scan-Ergebnis 2026-09-23

14 Netze. Die stärksten:

| SSID | BSSID | Kanal | Signal | Sicherheit |
|------|-------|-------|--------|------------|
| FRITZ!Box_6860 | 80:3F:5D:FD:F2:BA | 1 | −27 dBm | WPA2 |
| FIU 11 | 50:E6:36:7A:EB:84 | 6 | −60 dBm | WPA2 |
| Fuehrungsetage | D8:0D:17:47:7F:97 | 3 | −78 dBm | WPA2 |
| FIU 8 | 1C:ED:6F:FB:64:68 | 6 | −80 dBm | WPA2 |
| DIRECT-46-HP OfficeJet Pro 8210 | E6:E7:49:75:05:46 | 6 | −80 dBm | WPA2 |
| WAVLINK-N | 80:3F:5D:F1:81:01 | 2 | −81 dBm | **offen** |
| ARRIS-E5A2 | C0:C5:22:EF:E5:A0 | 6 | −88 dBm | WPA2 |

Zustand danach unverändert: `uptime` durchgehend, `MemFree` 20 260 kB (vorher
20 520), Ethernet 0 % Paketverlust.

## Werkzeuglage auf diesem Image

```
/sbin/iwlist              vorhanden
/usr/sbin/wpa_supplicant  vorhanden
/usr/sbin/wpa_cli         vorhanden
iw                        FEHLT
hostapd                   noch zu prüfen
```

Das `iw`-Fehlen bestätigt die Annahme hinter `WifiCapabilities`: ohne
nl80211-Werkzeug kann dieses Image den Treiber nicht nach seinen
Interface-Typen fragen, also bleibt `driver_ap_known = false` und der
AP-Modus ist „versuchbar, nicht bestätigt".

## Station-Betrieb — hardwareverifiziert 2026-09-23

In zwei Stufen getrennt, damit ein Fehlschlag zuordenbar bleibt: erst
Assoziation, dann erst DHCP.

### Stufe 1 — Assoziation

```sh
umask 077
mkdir -p /var/run/wpa_supplicant
{ echo "ctrl_interface=/var/run/wpa_supplicant"
  echo "update_config=0"
  wpa_passphrase "<SSID>" "<PSK>" | grep -v '#psk='
} > /tmp/wpa.conf
chmod 600 /tmp/wpa.conf
wpa_supplicant -B -i wlan0 -c /tmp/wpa.conf -D nl80211
```

`wpa_passphrase` und das `grep -v '#psk='` gehören zusammen: das Werkzeug
schreibt den Klartext als Kommentarzeile **neben** den abgeleiteten Schlüssel.
Ohne den Filter liegt das WLAN-Passwort im Klartext auf dem Overlay. Geprüft
wird das hinterher, nicht angenommen — `grep -c` auf den Klartext muss `0`
ergeben.

Ergebnis:

```
wpa_state          COMPLETED
ssid/bssid         <SSID> / 06:61:1d:a3:5c:19
freq               2462 MHz (Kanal 11)
mode               station, wifi_generation 4
pairwise/group     CCMP / CCMP
RSSI −31 dBm       LINKSPEED 65 Mb/s   NOISE −89   WIDTH 20 MHz
```

`rfkill: Cannot open RFKILL control device` erscheint dabei und ist harmlos —
dieses Image hat kein rfkill, die Assoziation läuft trotzdem.

### Stufe 2 — DHCP, mit Ethernet parallel

```sh
udhcpc -i wlan0 -n -q -t 6 -T 3
```

```
lease 172.21.115.79 von 172.21.115.8, 3599 s
dns 172.21.115.8
```

Routing danach, und das ist der Punkt, auf den es ankam:

```
default via 172.21.115.8 dev wlan0            <- die EINZIGE Default-Route
172.21.115.0/24 dev wlan0  src 172.21.115.79
192.168.1.0/24  dev eth0   src 192.168.1.10   <- Management-Pfad unberührt
```

Es gab keinen Routen-Konflikt, **weil eth0 nie eine Default-Route hatte**.
Die Kamera hing bis hierher ohne Gateway im Netz; das erklärt nebenbei den
Uhrzeit-Befund (`t40nn-uhr-ohne-rtc`) — mit der ersten echten
Internetverbindung korrigierte `ntpd` die Systemzeit sofort um +10 989 s.

Auf einem Board, das über eth0 *doch* eine Default-Route bekommt, greift
dieser Automatismus nicht mehr von selbst. Dann muss die WLAN-Default-Route
eine höhere Metrik bekommen, sonst wandert der Management-Pfad unbemerkt mit.

Beide Wege gleichzeitig nachgewiesen:

```
eth0  -> 192.168.1.222    0 % Verlust   0,9 ms
wlan0 -> 172.21.115.8     0 % Verlust   7,7 ms
wlan0 -> 8.8.8.8          0 % Verlust  49 ms
machino /api/v1 antwortet auf 192.168.1.10 UND auf 172.21.115.79
uptime 54 min durchgehend, MemFree 16 304 kB
```

### Nebenbefund: der Treiber schreibt Schlüsselmaterial ins Kernel-Log

Beim Vier-Wege-Handshake erscheinen auf der Konsole Zeilen der Form

```
key: 00000000: 04 58 5d 3d 03 49 6f fd 58 a1 e7 9f 26 b4 ab 70
```

Das ist Debug-Ausgabe des AIC-Treibers, kein Fehler unsererseits — aber es
heißt, dass **jeder, der `dmesg` lesen darf, an Sitzungsschlüssel kommt**.
Für den Bring-up ist das hinnehmbar; bevor WLAN produktiv wird, gehört die
Treiber-Loglevel heruntergesetzt oder diese Ausgabe entfernt. Eigener Punkt,
nicht mit den Machino-Secrets-Regeln zu verwechseln: die halten sich daran,
der Fremdtreiber nicht.

## Offen

* Machino-Build mit der Connectivity-API deployen, `/machino/net` im Browser
* Candidate/Confirm und der H9-Rollback
* AP-Modus
* Überlebt der Aufbau einen Reboot-Zyklus (H5a)?
