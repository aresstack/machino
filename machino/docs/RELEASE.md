# Machino — Release Candidate

Stand 2026-09-23. Dies ist der Abschluss der **Drop-in-Phase**: Machino ersetzt
Majestic auf einer OpenIPC-T40NN so, dass die unveränderte
`OpenIPC/majestic-webui` nicht merkt, dass sie mit etwas anderem spricht.

---

## Unterstützte Hardware

| | |
|---|---|
| SoC | Ingenic **T40** (T40NN), xburst2, MIPS32r2 little-endian, soft-float o32 |
| Board-Profil | `t40nn-imx307-board-a` |
| Sensor | Sony **IMX307** über MIPI-CSI, i2c0/0x37, rst=91, pwdn=0 |
| Kernel | OpenIPC 4.4.94, `vermagic 4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT` |
| SDK | Ingenic IMP **1.3.1**, statisch gelinkt (das System-`libimp.so` 1.2.0 wird nicht benutzt) |
| libc | musl (thingino xburst2 musl SDK, `libmuslshim`) |
| RAM-Budget | `mem=48M` — nutzbar rund 42 MB |
| Flash | SPI NOR, jffs2-Overlay auf `mtd4`, davon rund 4,6 MB frei |

Andere T40-Boards **sind nicht geprüft**. Das Board-Profil wird aufgelöst und
validiert; ein unbekanntes Board startet nicht mit geratener Verdrahtung.

## Was läuft

| Funktion | Stand |
|---|---|
| RTSP MAIN (`/ch0`) und SUB (`/ch1`) | hardwareabgenommen |
| MSE über `/ws/video` (fMP4, ein Fragment pro Frame) | hardwareabgenommen, ~276 ms |
| WebRTC über `/ws/webrtc` (eigener Stack, ICE-lite, DTLS via mbedTLS, SRTP) | hardwareabgenommen, **~100 ms** |
| Front-Door: Port 80, Session-Login, Relay an busybox `:85` | hardwareabgenommen |
| `/ws/logs` | hardwareabgenommen |
| Settings/API, Schema, PATCH, per-Zeile-Reset | hardwareabgenommen |
| Bildregler live (`/api/v1/image`) | im Code seit AP10, auf Hardware `PENDING_PHYSICAL` |
| ONVIF Device + Media, `SetSystemDateAndTime`, WS-Discovery | Code fertig, Default **aus** |
| Watchdog (`/dev/watchdog`, 15 s) | Code fertig, noch nie scharf gelaufen |
| Demand-getriebener Lifecycle (kein Consumer → kein Pipeline) | hardwareabgenommen, viele Kaltzyklen |

## Default-Ports und -Schalter

```
HTTP-Frontdoor     80        (Init startet mit --api-port 80 --api-upstream-port 85)
busybox WebUI      85        intern, nur 127.0.0.1
API stand-alone    8080      (nur ohne Front-Door)
RTSP               554       MAIN /ch0, SUB /ch1, max. 4 Clients
RTSP-Auth          AN        (Default seit AP7 umgestellt; upstream-kompatibel)
Session-Login      AN        gegen das Systemkonto (root), Majestic-Vertrag
ONVIF              AUS       Default weicht bewusst von Upstream ab
Watchdog           AN        15 s
JPEG/Snapshot      AUS       siehe Einschränkungen
Recording          nicht angeboten
```

## Bekannte Einschränkungen

Jede davon ist gemessen und in einem eigenen Dokument belegt.

* **JPEG/Snapshot ist aus.** Das Anlegen des zweiten FrameSource-/Encoder-Paares
  hängt den ganzen Daemon auf (`machino-t40nn-jpeg-wedge`). `/snapshot` und
  `/stream.mjpeg` antworten **501**, die Dashboard-Kachel zeigt ihre eigene
  Meldung. → `docs/ap13-ap14.md`
* **Kein OSD-Zeichnen.** Es gibt keinen Schriftrasterer auf der Kamera und
  keinen im Binary; `/api/v1/osd` antwortet 404, was der Port-Vertrag
  ausdrücklich als gültig vorsieht. → `docs/ap13-ap14.md`
* **Kein IR-Cut, kein Nachtmodus.** Keine Hardwarebelege und keine belastbare
  Pinquelle für t40 — es werden **keine Pins erfunden**. → `docs/ap18-night-gpio.md`
* **Kein Recording, keine Analytics-Indizes, keine Peers.** Es steckt keine
  SD-Karte im vorhandenen Slot; der einzige beschreibbare Speicher sind 4,6 MB
  Flash. → `docs/ap19-recording-analytics-peers.md`
* **Kein Audio, kein Talkback.** `spk_gpio = -1`, kein externer Codec. Capture
  wäre möglich, ist aber nicht gebaut. → `docs/ap20-audio-talkback.md`
* **Keine NNA.** Treiber vorhanden, aber kein `nmem=` reserviert, keine SDK-API,
  keine Bibliothek, kein Modell. → `docs/ap23-nna-analytics.md`
* **Machino flasht keine Firmware.** `/ws/upgrade` lehnt mit Begründung ab;
  `sysupgrade` über SSH ist der Weg. → `docs/ap21-update.md`
* **„Synchronize now" der Zeit-Seite läuft in ein 504**, wenn die Kamera keine
  Route nach draußen hat: `ntpd` scheitert 41 s lang an DNS, die
  Inaktivitätsgrenze des Relays liegt bei 6 s. „Set from browser" und ONVIF
  funktionieren. → `docs/dropin-matrix.md`
* **Der seltene Hardlock ist ungelöst.** Auslöser gepinnt (erster
  authentifizierter `GET /cgi-bin/live.cgi`), Ursache offen, kein Reproducer.
  Instrumentiert: Konsole spricht wieder, begrenzter PCAP-Ring liegt bereit.
  → `docs/ap17-hardlock.md`

## MAIN und SUB

```
MAIN   1920x1080 @ 20 fps, H.264 High 4:2:0 Level 5.1 (avc1.640033), ~3000 kbit/s
SUB     640x360  @ 20 fps, dieselbe Kodierung, ~500 kbit/s
```

Beide über RTSP, MSE und WebRTC erreichbar. Der Substream wird nachfragegesteuert
aufgebaut; `video1` im Schema trägt `x-reload: "pipeline"`, und die Stock-Seite
bietet danach „Apply" an, dessen SIGHUP die Einheit rekonfiguriert.

## Zeit

Die T40NN hat **keine RTC**. Ohne Gateway ist jede Ausschaltzeit verloren.
`fake-hwclock` speichert stündlich auf jffs2 (sofort bei einem erkannten
Sprung). Stellen lässt sich die Uhr über den WebUI-Knopf „Set from browser"
(38 ms) oder über ONVIF `SetSystemDateAndTime`. Der NTP-Knopf braucht eine
Route nach draußen.

## Installation

```sh
tar xzf machino-openipc-t40nn.tar.gz
cd machino-openipc-t40nn
sh ./install.sh                  # oder: sh ./sbin/machino-manager install --owner cam-tool
```

Der Installer prüft **vor dem ersten Schreibvorgang**: freien Platz, die
sha256-Summe des Daemons gegen `SHA256SUMS`, den ELF-Header (32 Bit LE,
`e_machine 8` = MIPS) und `BUILDINFO` gegen `/proc/device-tree/compatible`.
Fehlt ein Prüfwerkzeug, wird installiert und ausdrücklich gesagt, dass **nicht**
geprüft wurde.

Der Austausch ist atomar (temporäre Datei im Zielverzeichnis, dann `rename`);
wo das Overlay ein `rename` verweigert, fällt er auf eine In-Place-Kopie
zurück und meldet das.

**Nach dem Installieren: Power-Cycle.** Ein Warmstart ist der dokumentierte
Hardlock-Auslöser.

## Rückweg

Drei Ebenen, von klein nach groß:

1. **Automatisch.** Schlägt die Nachprüfung fehl (installiert, aber nicht
   aktiv), stellt `machino-manager` den vorherigen Daemon aus
   `/etc/machino/backup/machino.prev` wieder her, startet ihn und schreibt das
   Manifest neu, damit es nicht die gescheiterte Version behauptet.
2. **Von Hand zurück auf die vorherige Machino-Version:**
   ```sh
   cp /etc/machino/backup/machino.prev /usr/bin/machino && /etc/init.d/machino restart
   ```
3. **Zurück auf Majestic:**
   ```sh
   machino-manager uninstall --owner cam-tool
   ```
   Das stellt genau den Zustand wieder her, der **bei der Installation
   aufgezeichnet** wurde — einschließlich „Majestic war absichtlich
   abgeschaltet". `--keep-config` behält `machino.conf`.

Die Installation legt **keine** persistenten Änderungen außerhalb von
`/usr/bin/machino`, `/usr/sbin/{streamerctl,machino-manager}`,
`/etc/init.d/{S95streamer,machino,majestic}` und `/etc/machino/` an. Die
Stock-WebUI wird nicht angefasst: kein `machino.cgi`, keine Menüzeile — der
Test vergleicht `header.cgi` byteweise vor und nach Installation **und**
Deinstallation.

## Konfiguration und Upgrade

`machino.conf` ist eine `key = value`-Datei. Ein Upgrade

* **behält** eine vorhandene `machino.conf` unverändert (die mitgelieferte
  landet daneben als `machino.conf.default`),
* **erhält** Kommentare, Reihenfolge und **jeden Schlüssel, den dieser Build
  nicht kennt** — geprüft mit einem Test, der fremde Schlüssel, eingerückte
  Zeilen und doppelte Durchläufe abdeckt,
* **migriert** bei einer Erstinstallation einmalig `majestic.yaml`, mit einem
  Protokoll pro Schlüssel (mapped/converted/ignored/unsupported/invalid).

## Artefakt

Das CI erzeugt zwei Bündel pro Commit:

```
machino-m2-t40/                  machino, machino.conf, required-libs,
                                 SHA256SUMS, BUILDINFO
machino-openipc-t40nn.tar.gz     das Installationspaket
```

`BUILDINFO` nennt Commit, Zeitpunkt, SDK, Toolchain, libc, Ziel und die
vollständigen Compiler-Flags. `SHA256SUMS` deckt jede Datei des Pakets ab und
wird vom Installer geprüft.

### Der Build ist reproduzierbar — gemessen, nicht behauptet

Derselbe Commit (`de7b189`) wurde zweimal gebaut: der reguläre CI-Lauf und ein
`gh run rerun` desselben Laufs (`attempt=2`, Artefakt neu erzeugt um
01:35:27 Z).

```
Versuch 1   bf72da8c02968c720b75cf5e6c876a5a41f718df966e9d81d11b72a654eb9241
Versuch 2   bf72da8c02968c720b75cf5e6c876a5a41f718df966e9d81d11b72a654eb9241
```

**Bitidentisch.** Das gilt für denselben Commit — der Versionsstring steckt per
`-DMACHINO_VERSION` im Binary, zwei *verschiedene* Commits ergeben also
notwendigerweise verschiedene Hashes.

Beispiel für den zuletzt auf die Kamera gelegten Build:

```
Commit   341a8d4
machino  ec2d9f84cd418cc2350bc6a503d603b354d91f4723c92262ab68f3c94bd6c8ae
```

## Gates zu diesem Stand

```
Hosttests (C++)              2561 bestanden, 0 fehlgeschlagen
Installer-Shelltests           94 bestanden, 0 fehlgeschlagen, 1 übersprungen
streamerctl-Tests              29 bestanden, 0 fehlgeschlagen
check-linux-only               ok
CI inkl. MIPS-Crossbuild       grün
Drop-in-Matrix                 58 Endpunkte klassifiziert, 0 offene FAIL
```

Offene physische Prüfungen: `docs/pending-physical.md`.
