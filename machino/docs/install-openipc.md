
## USB-Nutzung: WLAN oder Mobilfunk

Die Kamera hat genau **einen** USB-Port. Deshalb gibt es genau **eine**
Einstellung:

    usb.mode = off        Default. In machino.conf.
    usb.mode = wifi
    usb.mode = cellular

Das Paket bringt **beide** Nutzlasten mit -- fuer WLAN Treiber, Firmware,
hostapd und den Rollen-Supervisor, fuer Mobilfunk `option`, `usb_wwan`,
`usbnet`, `cdc_ether` und den Datenpfad-Helfer -- und `install.sh` legt beide
per Default ab. **Beide sind aus.**

Beides zusammen ist Absicht. Der Schalter sitzt in der Machino-Oberflaeche
unter *Netzwerk & USB → USB-Nutzung*, und ein Schalter, der erst wirkt, nachdem
jemand per SSH Kernelmodule nachkopiert hat, waere keiner.

Warum EIN Feld und nicht zwei Haken: zwei Haken koennten "beides an" ausdruecken,
was die Hardware nicht kann. Welcher der beiden dann gewinnt, entschiede die
Reihenfolge der Init-Skripte -- und das liest niemand nach, bevor er ein
Haekchen setzt.

Bei **off** tut `machino-usb-helper` beim Boot nichts: kein Modul, kein
Portstrom auf PB18, kein wpa_supplicant, kein hostapd, kein Modem-Helfer, kein
DHCP. Der Port bleibt vollstaendig frei.

Bei **wifi** laeuft die auf Hardware erarbeitete Reihenfolge:

    cfg80211 -> aic_load_fw -> aic8800 -> GPIO 50 (PB18) -> wlan0 -> Supervisor

Bei **cellular** diese:

    usbnet -> cdc_ether -> usbserial -> usb_wwan -> option -> GPIO 50 -> Helfer

Die Netzwerkmodule kommen zuerst, und das ist kein Zufall: `option` bindet ueber
`new_id` ALLE Interfaces eines Geraets. Waere es zuerst da, verschluckte der
Notnagel auch das ECM-Interface -- ein Modem mit tadellosem AT-Port und ohne
Datenpfad.

Eine Aenderung wirkt beim **naechsten Neustart**, und die Seite sagt das auch
so, samt dem Modus, der gerade tatsaechlich laeuft. Kernelmodule bei laufender
IMP-Pipeline zu tauschen ist genau der Weg, den dieser Entwurf vermeidet.

Ein Upgrade von einer Installation vor AP-M6 uebernimmt `usb.wifi.enabled`
einmalig: `true` wird zu `wifi`, `false` zu `off`. Danach entscheidet `usb.mode`.
Der alte Schluessel wird weiter *geschrieben*, aber nicht mehr gelesen -- er ist
der Rueckfall fuer ein Boot-Skript, das ein halbes Upgrade stehengelassen hat.

Die Mobilfunk-Konfiguration (APN, PDP-Typ, Auth, SIM-PIN) haengt **nicht** am
Modus. `usb.mode = wifi` loescht keinen gespeicherten APN.

### Datenverbindung: ECM oder PPP

Innerhalb von `usb.mode = cellular` gibt es noch eine Wahl:

    cellular.data_link = ecm        Default
    cellular.data_link = ppp

**ECM** ist der Normalfall: das Modem meldet sich als Netzwerkkarte, der Kernel
legt ein `usb0`/`eth1` an, die Adresse kommt per DHCP oder aus `AT+CGCONTRDP`.

**PPP** ist die Ausweichmoeglichkeit fuer Modems und Netze, in denen das nicht
geht. Die Strecke laeuft dann ueber den seriellen Modem-Port; `pppd` waehlt mit
`ATD*99***1#` und handelt die Adresse per IPCP aus.

Es wird **nicht** automatisch gewechselt. Scheitert ECM, bleibt es bei ECM und
sagt warum. Ein stiller Fallback haette zur Folge, dass die Kamera auf einem
Weg laeuft, den niemand gewaehlt hat -- und dass der Fehler im gewaehlten Weg
nie auffaellt.

Der Bootpfad bereitet nur den gewaehlten Link vor: bei `ppp` werden `usbnet`
und `cdc_ether` nicht geladen. Ein Wechsel wirkt deshalb erst nach einem
Neustart, wie beim USB-Modus selbst.

Die gemeinsame Control Plane bleibt dieselbe -- SIM, PIN, Registrierung, APN
und PDP-Kontext laufen bei beiden ueber denselben AT-Port und denselben
SimManager. Die PIN wird also auch beim Wechsel des Datenlinks je Lebenszyklus
hoechstens einmal gesendet.

`pppd` und `chat` liegen im Bundle (Workflow `build-ppp-t40`) und werden nach
`/usr/sbin` installiert; die Hooks gehen nach `/etc/ppp/ip-up` und
`/etc/ppp/ip-down`, weil pppd genau dort sucht. Bringt das Bundle kein `pppd`
mit, sagt der Installer das, statt eine Auswahl anzubieten, die nichts tut.

### Optionen

    ./install.sh                             beide Nutzlasten, usb.mode unveraendert
    ./install.sh --usb-mode=wifi             zusaetzlich usb.mode = wifi
    ./install.sh --usb-mode=cellular         zusaetzlich usb.mode = cellular
    ./install.sh --without-wifi-payload      Treiber/Firmware/hostapd weglassen
    ./install.sh --without-cellular-payload  Modem-Module und Helfer weglassen
    ./install.sh --with-device-page          Menueeintrag "Geraete (machino)"
    ./install.sh --with-network-page         Menueeintrag "Netzwerk & USB (machino)"

Ohne `--usb-mode` bleibt stehen, was in der Datei steht: eine Neuinstallation
ueber eine bestehende hinweg setzt die Wahl des Betreibers nicht zurueck.

`--without-wifi-payload` spart rund 2 MB des 8,7-MB-Overlays (aic8800.ko 550 K,
aic_load_fw.ko 87 K, Firmware 362 K, hostapd 996 K) und macht die WLAN-Auswahl
wirkungslos -- die Seite sagt dann, dass nichts zu schalten da ist, statt etwas
anzubieten, das nicht funktionieren kann. Einen Modus zu waehlen, dessen
Nutzlast weggelassen wurde, wird abgelehnt: das waere ein Geraet einschalten,
dessen Treiber nicht installiert ist.

`--with-wifi` ist der alte Name fuer `--usb-mode=wifi` und bleibt erhalten,
damit bestehende Installationsbefehle nicht brechen. `--with-access-point` wird
noch angenommen und ignoriert; hostapd gehoert zur Standard-Nutzlast.
`hostapd_cli` wird nicht mitgeliefert -- machino spricht den ctrl-Socket selbst.

Die beiden Seitenschalter sind **aus**, und zwar auf jedem Weg -- auch beim
Deploy ueber `machino-manager install`. Das ist eine Architekturgrenze:

> OpenIPC bleibt unveraendert. Majestic raus, Machino rein.

Eine `<li>`-Zeile in `p/header.cgi` ist eine Aenderung an der OpenIPC-WebUI,
auch wenn sie markiert ist und der Deinstallierer sie byteweise zuruecknimmt.
Machino liefert seine Seiten selbst aus und bleibt damit von den Dateien des
Wirtssystems getrennt:

    http://<kamera>/machino/devices     Geraetemanager
    http://<kamera>/machino/net         Netzwerk & USB

`--with-network-page` / `--with-device-page` (bzw. `machino-manager install
--with-pages`) bleiben als **ausdrueckliche Entscheidung** dessen erhalten, der
installiert. Sollte OpenIPC einmal einen echten Erweiterungspunkt fuer
Menueeintraege anbieten, gehoert die Integration dorthin -- nicht in ein `awk`
auf eine fremde Datei.

Fehlt die WLAN-Nutzlast, ohne dass `--without-wifi-payload` angegeben wurde,
**bricht der Installer ab**. Frueher lief er weiter und meldete "0 module(s)":
das Profil stand dann in `/etc/wireless/usb`, die Module fehlten, `adapter_scan`
verwarf es stillschweigend, und im Dropdown stand weiter "None".

Ein `S42wifi` aus einer aelteren Installation wird beim Installieren
**entfernt**. Es liefe sonst neben `S42usb`, laese noch `usb.wifi.enabled` und
laedte auf einer auf Mobilfunk gestellten Kamera trotzdem den WLAN-Treiber.

### Station und Access Point

Beides sind ROLLEN desselben Funkmoduls, keine gleichzeitigen Betriebsarten.
Der AIC8800 macht daraus im Treiber einen `change_if`, und es gibt genau einen
Besitzer von wlan0 pro Rolle:

* **Station** gehoert OpenIPC. SSID, Passwort und Adresse konfiguriert die
  OpenIPC-Netzwerkseite (Karte "Wireless adapter"), `S40network`/`ifup`
  startet den wpa_supplicant. Machino stellt nur die Hardware bereit:
  Treiber registrieren (`machino-device`), Portstrom schalten, `wlan0`
  anlegen (`machino-usb-helper wifi-attach`, seit 2026-09-25 hardware-only).
* **Access Point** gehoert Machino, weil OpenIPC dafuer nichts hat:
  `/usr/sbin/machino-wifi-role` startet hostapd, wenn in
  `/etc/machino/wifi-role` `ap` steht. Nur dann laeuft der Supervisor
  ueberhaupt -- zwei Supplicants auf demselben Interface waren der
  Doppelbesitzer-Befund vom 2026-09-25.

machino schreibt seine Absicht nach `/etc/machino/wifi-role`, der Supervisor
setzt sie um. So startet der Prozess mit der grossen IMP-Pipeline niemals
selbst ein Programm.

### What gets installed

| Path | What it is |
|---|---|
| `/usr/bin/machino` | the daemon |
| `/usr/sbin/streamerctl` | the selector — the only thing that switches services |
| `/usr/sbin/machino-usb-helper` | reads `usb.mode` at boot and brings up the selected stack |
| `/usr/sbin/machino-wifi-role` | runs the access-point role (hostapd); station Wi-Fi belongs to OpenIPC's own network page |
| `/usr/sbin/machino-cellular-helper` | starts the modem's data path (DHCP or pppd) |
| `/usr/sbin/machino-device` | registers a device with the host: payload -> `/lib/modules`, profile -> `/etc/wireless/usb` |
| `/etc/machino/machino.conf` | your configuration (kept on upgrades) |
| `/etc/machino/modules/` | the WiFi and modem kernel modules |
| `/etc/machino/payload/<id>/` | the kernel modules this release carries. **Survives the device manager's "uninstall"** -- only `uninstall.sh` removes them |
| `/etc/machino/devices/<id>.manifest` | the one source for a device's profile name, module order and USB id |
| `/etc/init.d/S39machinodev` | executes a pending install/remove intent at boot, before `S40network` |
| `/etc/machino/ppp/` | generated pppd options and chat script (0600 — carries the APN password) |
| `/etc/ppp/ip-up`, `/etc/ppp/ip-down` | tell machino which interface a PPP call got |
| `/etc/machino/streamer` | which service is selected; survives a reboot |
| `/etc/machino/streamer.preinstall` | what the camera looked like before, used by the uninstaller |
| `/etc/machino/backup/` | untouched copies of the files the installer modified |
| `/etc/init.d/machino` | start/stop for Machino |
| `/etc/init.d/S95streamer` | starts the selected service at boot |
| `/etc/init.d/majestic` | Majestic's original init script, moved out of the boot slot |

### Der Gerätemanager und OpenIPC

`/machino/devices` richtet **Hardwareunterstützung** ein, es konfiguriert kein
WLAN. Der Unterschied ist der Befund vom 2026-09-25: der AIC8800-Treiber war
gebaut, ausgeliefert und lief auf der Kamera — und OpenIPCs Netzwerkseite bot
trotzdem nur „None" an. Sie sucht nämlich woanders:

* `www/cgi-bin/network.cgi` (`adapter_scan`) parst `/etc/wireless/{usb,sdio,modem}`
  auf Blöcke `if [ "$1" = "<id>" ]`, zieht die **`modprobe`-Namen** heraus und
  bietet ein Profil nur an, wenn jedes dieser Module als `.ko` unter
  `/lib/modules` liegt. Ein `insmod <pfad>` ist für den Scanner unsichtbar.
* `/etc/init.d/S40network` liest `wlandev` aus dem U-Boot-Env und ruft
  `/etc/wireless/usb "$wlandev"`.

Deshalb trennt Machino **Nutzlast** und **Registrierung**:

```
/etc/machino/payload/aic8800/   die Module   -- überleben jedes Deinstallieren
/lib/modules/<ver>/machino/     Registrierung -- kommt und geht
/etc/wireless/usb               das Profil    -- kommt und geht
```

Ablauf: **Installieren** merkt die Absicht nur vor (`machinod` darf bei lebendem
IMP nicht forken, also kein `depmod` zur Laufzeit), `S39machinodev` führt sie
beim nächsten Boot aus, und danach steht der Adapter auf der **unveränderten**
OpenIPC-Seite *Network* unter „Wireless Adapter" zur Auswahl. Verbunden wird das
WLAN weiterhin dort.

**Deinstallieren im Gerätemanager entfernt nur die Registrierung.** Die Module
bleiben liegen, damit ein späteres Installieren ohne neues Bundle möglich ist —
eine frühere Fassung löschte sie mit und war damit eine Einbahnstraße.

**Why Majestic's init script is moved:** OpenIPC's `rcS` runs every
`/etc/init.d/S??*` without checking the executable bit, so `chmod -x` does not
disable a service — it only makes the start fail with a permission error. The
only reliable way to keep Majestic from auto-starting is to take it out of that
glob. It stays fully functional as `/etc/init.d/majestic`, and the uninstaller
moves it back.

---

## WebUI access

While **Majestic** is active it serves the WebUI with its own login, exactly as
before - nothing changes.

While **Machino** is active it is the front door on port 80 and behaves like
Majestic: the WebUI shows the normal OpenIPC login page and authenticates the
camera's root account (Machino's session /login and /logout). Camera-local
requests (127.0.0.1) pass without credentials, exactly like Majestic; CLI/API
callers can also use HTTP Basic auth.

This is not TLS. Do not expose such a camera directly to an untrusted network.

> The stock WebUI's Dashboard/Live/Camera pages are Majestic-specific and will
> report "Majestic not running" while Machino is active. A Machino adaptation of
> those pages is a separate work item; this bundle does not ship a standalone
> `machino.cgi` page and does not edit the WebUI navigation. Switch the service
> from the command line (below) or from the Cam-Tool.

---

## 5. Switch to Machino

Over SSH:

```sh
streamerctl set machino
```

What happens, in this order:

1. Majestic is stopped and the script waits until the process is really gone.
2. busybox `httpd` starts on 127.0.0.1:85 as the internal WebUI backend.
3. Machino is started and becomes the front door on port 80.
4. Machino's API on :80 is polled until it answers.
5. If it does not come up, everything is rolled back to Majestic.

`COLD_IDLE` is a healthy state. Machino follows "no consumer, no pipeline":
with nobody watching, there is deliberately no pipeline running.

Check it:

```sh
streamerctl status
```

```
selected:     machino
running:      machino
port 80:      machino
machino api:  cold_idle (port 80)
```

And the stream:

```sh
ffplay rtsp://CAMERA:554/ch0
```

---

## 6. Switch back to Majestic

Over SSH:

```sh
streamerctl set majestic
```

Machino is stopped, port 80 is handed back, and Majestic is started — it then
serves the WebUI itself again.

---

## 7. After a reboot

The selection is stored in `/etc/machino/streamer` and applied at boot by
`/etc/init.d/S95streamer`. A camera set to Machino comes back on Machino; a
camera set to Majestic comes back on Majestic. Exactly one media service is
started either way.

---

## 8. Upgrading

Copy the newer bundle over and run `./install.sh` again. It replaces the
binary and the helper scripts but keeps `/etc/machino/machino.conf`; the new
default config is written to `machino.conf.default` next to it so you can
compare.

If Machino is the active service, restart it to pick up the new binary:

```sh
streamerctl set majestic
streamerctl set machino
```

or simply reboot the camera.

---

## 9. Diagnosis

```sh
streamerctl status                 # selection, running service, who serves port 80
/etc/init.d/machino status         # is the daemon running?
tail -50 /var/log/machino.log      # what the daemon says
logread | tail -50                 # system log
wget -q -O- http://127.0.0.1:8080/api/v1/state     # Machino state as JSON
```

A `note: selection and running process differ` line means the stored selection
and reality disagree — usually because a start failed. `streamerctl set <the
one you want>` puts both back in sync.

---

## 10. If Machino fails to start

`streamerctl` rolls back on its own: it restarts the previous service and
tells you what failed. The camera keeps streaming with Majestic and the WebUI
stays reachable.

Find out why:

```sh
tail -50 /var/log/machino.log
```

Common causes:

* **`hardware resolution failed: ... refusing to guess`** — the board profile
  does not match this camera. Machino will not invent an I²C address or a
  reset pin. Set `board = <profile>` or the explicit `sensor.*` values in
  `/etc/machino/machino.conf`.
* **`bind/listen :554 failed`** — something else holds the RTSP port. With
  Majestic fully stopped this should not happen; check with
  `netstat -ltnp | grep 554`.
* **API not answering** — the daemon started but its HTTP API did not come up.
  Check `api.enabled`, `api.bind` and `api.port` in the config.

---

## Switching back to Majestic on broken-Majestic hardware

On a camera where Majestic's media stack does not work (e.g. the T40NN, where
Majestic logs `Cannot start SDK`), starting Majestic live again after Machino
can reset the board - Majestic's own start takes the SoC down. `streamerctl set
majestic` does the switch correctly, but Majestic itself fails at start.

Majestic **does** start cleanly at boot (a fresh ISP). So on such hardware the
reliable way back is a reboot rather than a live switch:

```sh
streamerctl select majestic   # sets the boot selection only - starts/stops nothing
reboot                        # Majestic starts cleanly from a fresh boot
```

`select` is used instead of `set` on purpose: `set majestic` only persists the
choice if the live start succeeds, and on this hardware it does not - the
rollback would restore `machino` and the reboot would bring Machino back up.
`select` writes the boot selection unconditionally and touches no process, so
after the reboot the camera comes up on Majestic. This is specific to cameras whose Majestic cannot re-init the media
hardware; where Majestic works, the live switch back is immediate.

## Recovery

**The WebUI is gone and you cannot switch back.** Use SSH:

```sh
ssh root@CAMERA
streamerctl set majestic
```

**Neither service is running.**

```sh
streamerctl status
streamerctl set majestic        # or: streamerctl set machino
```

**`streamerctl` itself is broken or missing.** Everything it does can be done
by hand:

```sh
/etc/init.d/machino stop         # make sure Machino is gone
pgrep -f /usr/bin/machino        # must print nothing
killall httpd                    # free port 80
/etc/init.d/majestic start       # Majestic takes port 80 and the camera back
```

**The camera boots without any media service.** Log in over SSH or the serial
console and check what is selected:

```sh
cat /etc/machino/streamer
/etc/init.d/S95streamer start
```

**Put everything back the way it was** — see the next section.

---

## 11. Uninstall

```sh
cd /tmp/machino-openipc-t40nn
./uninstall.sh
```

This stops Machino, removes its files, restores `/etc/init.d/S95majestic` into
the boot slot and removes the WebUI entry.

It restores the state recorded at install time. If Majestic was deliberately
disabled before you installed Machino, it stays disabled — the uninstaller
does not switch a service on that you had switched off.

Keep your configuration for a later reinstall:

```sh
./uninstall.sh --keep-config
```

---

## What this does not do yet

* No `.ipk` package and no package manager integration — this is a tar bundle
  with `install.sh`/`uninstall.sh`. A Buildroot package comes later.
* The bundle contains no proprietary Ingenic SDK libraries. Machino links them
  statically at build time; nothing extra is installed on the camera.
* While Machino is active, the WebUI is served by BusyBox `httpd` with HTTP
  Basic auth instead of Majestic's login and session handling - see
  [WebUI access](#webui-access). Basic auth is not TLS; do not expose such a
  camera directly to an untrusted network.
* The Majestic WebUI pages that talk to Majestic's own API (dashboard, live
  preview) do not work against Machino yet. The
  [API compatibility layer](api/v1.md) covers the configuration endpoints;
  the rest follows in a later milestone.
