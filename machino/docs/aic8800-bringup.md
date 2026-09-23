# AIC8800 WLAN auf dem T40NN — der Bring-up-Pfad

Stand 2026-09-23. **Hardwareverifiziert bis einschließlich Scan.** Alles hier
ist gemessen, nicht abgeleitet; die Kommandos sind so gelaufen.

## Der Pfad

Die Reihenfolge ist nicht beliebig — jeder Schritt hat einen Grund, der weiter
unten steht.

```sh
modprobe cfg80211                    # NICHT eingebaut, liegt als Modul
insmod aic_load_fw.ko                # muss vor aic8800 (depends=aic_load_fw)
insmod aic8800.ko
echo 50 > /sys/class/gpio/export     # PB18: Portstrom ERST JETZT
echo out > /sys/class/gpio/gpio50/direction
echo 1 > /sys/class/gpio/gpio50/value
                                     # -> USB-Hotplug -> bind -> wlan0
ip link set wlan0 up
iwlist wlan0 scan
```

Der Portstrom kommt **zuletzt**. Andersherum geht es auch, aber so ist es
robuster: das Gerät taucht auf, wenn der Treiber schon registriert ist, und
Enumeration und Binding passieren in einem Zug.

## Was dabei herauskommt

```
aicwf_usb_chipmatch USE AIC8800DC
rwnx_load_firmware: /lib/firmware/aic8800DC/fmacfw_patch_8800dc_h_u02.bin
Firmware Version: zh Aug 08 2023 20:31:22 - g41bc49e
is 5g support = 0                    <- nur 2,4 GHz
HT supp 1, VHT supp 1, HE supp 0
support channel: 1 2 3 4 5 6 7 8 9 10 11 12 13 14
New interface create wlan0

wlan0            5: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500
MAC              cc:b8:5e:87:c9:1d
1-1:1.2 driver   -> bus/usb/drivers/aic8800
iwlist wlan0 scan -> 14 Netze
```

## Die drei Dinge, an denen es gescheitert ist

### 1. `CONFIG_PREALLOC_RX_SKB` muss AUS

Mit dem Upstream-Default (`y`, vom Top-Makefile erzwungen, obwohl der
Unter-Makefile `?= n` vorsieht) fordert `aicwf_prealloc_init()` beim
Modul-Init 847 Empfangspuffer als **order-3**-Blöcke an. Das kostete zwei
Reboots, bis die Ursache sichtbar war:

```
insmod invoked oom-killer: order=3 ... aicwf_prealloc_init
Out of memory: Kill process 992 (majestic)
```

Warum das nie gehen konnte, steht in `/proc/buddyinfo`:

```
Node 0, zone Normal    41  55  30  20  11  1  2  0  1  2  1  1  0  0  0
                        ^order-0        ^order-3 = 20 Blöcke frei
```

**20 verfügbar, 847 gefordert.** `free` meldet dabei ~20 MB und ist für diese
Frage irrelevant — es geht um *zusammenhängende* Seiten. Bei jeder künftigen
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

## Offen

* Station-Assoziation (SSID/PSK stehen noch aus)
* DHCP parallel zu Ethernet
* Machino-Build mit der Connectivity-API deployen, `/machino/net` im Browser
* Candidate/Confirm und der H9-Rollback
* AP-Modus
