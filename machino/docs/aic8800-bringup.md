
## Der Rollen-Supervisor, dritter Anlauf: drei Fehler hintereinander

Alle drei lagen uebereinander -- jeder wurde erst sichtbar, als der davor weg
war. Und keiner davon zeigte sich beim Lesen des Codes; sichtbar wurden sie,
als die Station-Rolle auf der Kamera lief und kein Netz in Reichweite war.

### 1. `udhcpc -b -t 0` kehrt nie zurueck

BusyBox sagt es selbst: `-b  Background if lease is not obtained` und
`-t N  Send up to N discover packets`. Mit `-t 0` (unbegrenzt) tritt "not
obtained" nie ein. Gemessen:

    udhcpc  pid=13644 ppid=13632
    superv  pid=13632 wchan=do_wait

Der Supervisor stand ab dem Moment, in dem die Station-Rolle hochkam, in
`do_wait` und hat die Rollendatei nie wieder gelesen. Damit war genau der Weg
tot, fuer den dieser Entwurf existiert: *mein WLAN ist weg, schalt die Kamera
auf Access Point, damit ich wieder drankomme.*

Im Log stand das uebrigens die ganze Zeit:

    23:45:48 wifi-role: Wechsel  -> station

und danach **kein** `Rolle station aktiv`. Die fehlende Zeile war der ganze
Befund. Sie wurde beim ersten Lesen uebersehen.

Behoben mit `-f` und eigenem `&`, wie bei udhcpd.

### 2. Beim Start raeumt der Supervisor die Station-Rolle nicht ab

`apply_role station` macht `stop_ap; start_station`. `stop_station` wird also
ausgerechnet dann nicht gerufen, wenn noch ein Supplicant aus einem frueheren
Leben das Interface haelt. wpa_supplicant antwortet darauf mit

    ctrl_iface exists and seems to be in use - cannot override it

und das wird von allein nie besser; `retry_start` verbrennt zehn Versuche
dagegen. Der Supervisor behauptet, alleiniger Besitzer von wlan0 zu sein --
dann muss er den Besitz beim Start auch nehmen. Tut er jetzt.

### 3. Ein fehlgeschlagener Supplicant-Start loescht die PID-Datei des laufenden

Gleicher Pfad. Nach einem Fehlversuch zeigt kein Griff mehr auf den lebenden
Prozess, und der naechste Start ergibt zwei Supplicants auf einem Interface --
auf der Kamera genau so beobachtet.

`claim_interface` sucht deshalb ueber `/proc/<pid>/comm` **und** den
Interfacenamen in der Kommandozeile. Das ist kein killall: ein Supplicant auf
einer anderen Schnittstelle bleibt unberuehrt, und der udhcpc von eth0 lief
nachweislich ueber den ganzen Durchlauf weiter.

### Ergebnis auf der Kamera

    00:53:42 uebernehme wlan0
    00:53:43 fremder Halter von wlan0: wpa_supplicant (pid 13643) -- wird beendet
    00:53:44 Wechsel  -> station
    00:53:44 Rolle station aktiv
    00:54:19 Wechsel station -> ap
    00:54:20 Rolle ap aktiv            Mode:Master, 192.168.24.1/24
    00:54:40 Wechsel ap -> station
    00:54:40 Rolle station aktiv       hostapd und udhcpd weg

Ohne Wiederholungsversuch, eth0 durchgehend unberuehrt.

OFFEN (PENDING_PHYSICAL): die Assoziation selbst. Der Test-Hotspot war
abgeschaltet, der Supplicant scannt. Der Rollenwechsel ist damit bewiesen, das
Wiederfinden eines konkreten Netzes nicht.

### Was daran methodisch schiefging

Der Durchlauf davor sah aus wie ein bestandener Test und war keiner: nach
`echo ap` stand wlan0 weiter auf `Managed/off-any`, es lief kein hostapd, und
das Supervisor-Log war leer. Ein Rollenwechsel hatte schlicht nicht
stattgefunden -- weil der Supervisor in `do_wait` hing. Ein Test, der nichts
ausloest, meldet keinen Fehler. Deshalb wird hier jetzt jeder Durchlauf am LOG
geprueft und nicht nur am Endzustand: "keine Fehlermeldung" und "es ist etwas
passiert" sind verschiedene Aussagen.
nter `/lib/modules/4.4.94/kernel/net/`, werden aber nicht geladen.
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
