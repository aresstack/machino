
## USB-WLAN

Das Paket bringt alles mit, was das WLAN braucht -- Treiber, Firmware, hostapd
und den Rollen-Supervisor -- und `install.sh` legt es per Default ab. **WLAN
selbst ist aus.**

Beides zusammen ist Absicht. Der Schalter sitzt in der Machino-Oberflaeche
unter *Netzwerk & USB → USB-WLAN*, und ein Schalter, der erst wirkt, nachdem
jemand per SSH Kernelmodule nachkopiert hat, waere keiner. Gleichzeitig hat die
Kamera genau EINEN USB-Port, und eingeschaltetes WLAN belegt ihn: cfg80211,
aic_load_fw, aic8800, Portstrom auf PB18. Wer dort ein Modem betreiben will,
soll dafuer nichts bezahlen muessen.

    usb.wifi.enabled = false      Default. In machino.conf.

Bei **aus** tut `S42wifi` beim Boot nichts: kein Modul, kein Portstrom, kein
wpa_supplicant, kein hostapd, kein DHCP auf wlan0. Der Port bleibt frei.

Bei **an** laeuft die auf Hardware erarbeitete Reihenfolge:

    cfg80211 -> aic_load_fw -> aic8800 -> GPIO 50 (PB18) -> wlan0 -> Supervisor

Eine Aenderung wirkt beim **naechsten Neustart**, und die Seite sagt das auch
so. Kernelmodule bei laufender IMP-Pipeline nachzuladen ist genau der Weg, den
dieser Entwurf vermeidet.

### Optionen

    ./install.sh                          Nutzlast installieren, WLAN aus
    ./install.sh --with-wifi              zusaetzlich usb.wifi.enabled=true
    ./install.sh --without-wifi-payload   Treiber/Firmware/hostapd weglassen

`--without-wifi-payload` spart rund 2 MB des 8,7-MB-Overlays (aic8800.ko 550 K,
aic_load_fw.ko 87 K, Firmware 362 K, hostapd 996 K) und macht den Schalter
wirkungslos -- die Seite sagt dann, dass nichts zu schalten da ist, statt etwas
anzubieten, das nicht funktionieren kann. `--with-wifi` zusammen mit
`--without-wifi-payload` wird abgelehnt: das waere ein Funkmodul einschalten,
dessen Treiber nicht installiert ist.

`--with-access-point` wird noch angenommen und ignoriert; hostapd gehoert jetzt
zur Standard-Nutzlast. `hostapd_cli` wird nicht mitgeliefert -- machino spricht
den ctrl-Socket selbst.

### Station und Access Point

Beides sind ROLLEN desselben Funkmoduls, keine gleichzeitigen Betriebsarten.
Der AIC8800 macht daraus im Treiber einen `change_if`, und es gibt genau einen
Besitzer von wlan0: `/usr/sbin/machino-wifi-role`. machino schreibt seine
Absicht nach `/etc/machino/wifi-role` (`station` | `ap` | `off`), der
Supervisor setzt sie um. So startet der Prozess mit der grossen IMP-Pipeline
niemals selbst ein Programm.
 selected; survives a reboot |
| `/etc/machino/streamer.preinstall` | what the camera looked like before, used by the uninstaller |
| `/etc/machino/backup/` | untouched copies of the files the installer modified |
| `/etc/init.d/machino` | start/stop for Machino |
| `/etc/init.d/S95streamer` | starts the selected service at boot |
| `/etc/init.d/majestic` | Majestic's original init script, moved out of the boot slot |

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
