
## AP-Modus (2026-09-23, Kamera 192.168.1.10, beobachtet ueber Ethernet)

Der AIC8800 kann Access Point. Das war bis hierher eine Annahme; jetzt ist es
gemessen. Der Uebergang Station -> AP im Kernel-Log:

    rwnx_cfg80211_disconnect drv_vif_index:0 disconnect reason:3
    rwnx_cfg80211_unlink_bss(): cfg80211_unlink Viva Espana!!
    change_if: 2 to 3, 8, 2
    usb 1-1 wlan0: AP started: ch=0, bcmc_idx=33 channel=2437 bw=1

`change_if: 2 to 3` ist der Rollenwechsel des Interface-Typs im Treiber, und er
laeuft sauber durch: erst die Station-Verbindung abbauen, dann den Typ aendern,
dann der AP. Genau deshalb sind es getrennte Rollen und keine gleichzeitigen
Daemons -- der Treiber selbst behandelt sie als Umschaltung.

Zustand danach:

    wlan0  Mode:Master, 192.168.24.1/24
    hostapd -B -P /var/run/hostapd.pid   laeuft, ctrl-Socket /var/run/hostapd/wlan0
    udhcpd /etc/machino/udhcpd.conf      laeuft
    eth0   192.168.1.10 unveraendert
    machino laeuft weiter, MemAvailable 24824 kB, buddyinfo unauffaellig

hostapd 2.10, statisch gegen libnl 3.7.0, 996 KB gestrippt. `hostapd_cli` wird
NICHT mitgeliefert: machino spricht den ctrl-Socket ueber wpa_ctrl.cpp selbst
an, und der Supervisor braucht ihn nicht. Das sind 150 KB, die auf einem
Overlay mit 4,3 MB frei nichts zu suchen haben.

Fuer diesen Test liegt das Binary auf tmpfs mit einem Symlink aus /usr/sbin --
absichtlich: haette der Treiber AP verweigert, waere kein Flash verbraucht
worden. Die dauerhafte Installation kommt ueber den Installer.

HTTP antwortet auf der AP-Adresse: `GET http://192.168.24.1/api/v1/state` gibt
401. Das ist der Beweis, um den es geht -- der Server ist auf dem Interface
erreichbar und verlangt Anmeldung. Ein 401 ueber die AP-Adresse ist ein
erreichbarer Server, keine kaputte Route.

NOCH NICHT GEMESSEN (braucht ein zweites Geraet, PENDING_PHYSICAL):
sieht ein Telefon die SSID, kommt WPA2 zustande, vergibt udhcpd eine Adresse,
und ist /machino/net dann ueber die Luft bedienbar.

### Ein Nebenbefund, der nicht vergessen werden darf

Der AIC-Treiber schreibt weiterhin Schluesselmaterial ins Kernel-Log
("key: 00000000: ea a5 2c 65 ..." direkt nach dem AP-Start). Das ist der
Treiber, nicht machino, aber `dmesg` ist damit auf dieser Kamera ein
Geheimnistraeger.
der künftigen
Speicherfrage zu diesem Treiber ist `buddyinfo` die Messung, nicht `free`.

Gebaut wird deshalb mit:

```
CONFIG_PREALLOC_RX_SKB=n CONFIG_PREALLOC_TXQ=n
```

als Make-Variablen auf der Kommandozeile — die schlagen das `export` im
Top-Makefile. Das ist kein Patch, sondern ein vorgesehener Betriebsmodus.

### 2. Der OOM trifft den Watchdog-Fütterer

```
PID 992 majestic -> /dev/watchdog
  992 {majestic} /usr/bin/machino -c /etc/machino/machino.conf --api-port 80
```

**machino selbst hält den Watchdog** (argv[0] ist `majestic`, wegen der
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
