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
* **Audio ohne Hardwareabnahme.** `/audio.pcm`, `/audio.alaw`, `/audio.g711a`,
  `/audio.ulaw`, `/audio.opus`, `/audio.m4a`, Audio im WebUI-Player
  (`/ws/video`), `/play_audio`, RTSP-/ONVIF-Backchannel und WebRTC-Talkback
  (aus bis `audio.enabled = true`). `spk_gpio = -1` heißt nicht
  „kein Ausgang" — die Stock-Firmware spielt damit. Hardwareabnahme über
  `machino --audio-test`. → `docs/ap20-audio-talkback.md`
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

## Automatische Releases

Jeder Push auf `main` erzeugt ein GitHub-Release — derselbe Workflow, der
baut und prüft (`.github/workflows/build-machino-t40.yml`, Job `release`).
Es gibt keinen zweiten Build: das Release-Asset ist byteidentisch mit dem
CI-Artefakt `machino-openipc-t40nn` desselben Laufs.

| | |
|---|---|
| Push auf `main` | Release `main-<Commitdatum>-<sha7>`, z.B. `main-20260930-ca9b444`. Die zehn neuesten bleiben, ältere werden samt Tag entfernt. |
| Push eines Tags `v*` | versioniertes Release, das bleibt (`v1.0`, `v1.1-rc1`). |
| `workflow_dispatch` mit `release: true` | ein `main-…`-Release aus dem gewählten Lauf — nur auf `main`. |
| Build rot oder Bundle unvollständig | **kein** Release, kein Tag. |

Der Tag wird **vor dem Build lokal gesetzt** und erst nach grünem Lauf
gepusht. So ist `git describe` = Tag = `-DMACHINO_VERSION` im Binary =
`Machino commit: … (<tag>)` in `BUILDINFO` = `version` im Manifest. Genau
diesen String vergleicht der Cam-Tool (`ipcam-lan-discovery`, Reiter
*Patches*) mit `machino --version` auf der Kamera; er lädt das Asset
`machino-openipc-t40nn.tar.gz` aus `/releases/latest`. Ein Prerelease
oder Draft würde dort nie erscheinen — deshalb ist jedes automatische
Release ein normales, als *latest* markiertes Release.

Assets pro Release: `machino-openipc-t40nn.tar.gz`, `manifest.json`
(Cam-Tool-Schema 1, mit der sha256 des Bundles), `SHA256SUMS`, `BUILDINFO`
und `machino-nna-model-t40nn.tgz` (AGPL, deshalb nie im Bundle). Die
Release-Notes nennen die Testzähler des Laufs und zitieren
`docs/pending-physical.md` — was Hardware braucht, wird nicht behauptet.

Strenger als das Entwicklungs-Artefakt: dem Release fehlt keine Nutzlast.
Fehlen WLAN-Treiber, hostapd, Modem-Module, pppd, NNA-Helfer oder
WeirdIKE, gibt es kein Release, und die Fehlermeldung nennt den
Nutzlast-Workflow (`build-aic8800-t40`, `build-hostapd-t40`,
`build-modem-modules-t40`, `build-ppp-t40`, `build-nna-t40`,
`build-weirdike-t40`), dessen Artefakt fehlt — meist, weil es nach 90
Tagen abgelaufen ist: den Workflow neu starten, dann den Release-Lauf.
Ändert ein Push `weirdike-openipc/` oder `machino/tools/nna/`, wartet der
Build auf den Nutzlast-Lauf **desselben Commits**, statt den alten Daemon
unter dem neuen Namen zu veröffentlichen.

Berechtigungen: das Release entsteht mit dem `GITHUB_TOKEN` des Laufs
(`permissions: contents: write`, wie in `aresstack/firmware-tool`). Ein
Organisations-Secret ist nicht nötig. Der Tag-Push mit diesem Token löst
keinen weiteren Workflow aus.

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

### Release Candidate 1

```
Tag        machino-rc1
Commit     8f101b6 (8f101b638bee6d0286ed49d7cd1c3d702bbfe68a)
CI-Lauf    35812288768, gebaut 2026-09-23T02:58:49Z

machino                        3a553c13cf646730d248454ca6ed9938abe3b91bac0c31e23e26f45b5ddbfe76
machino-openipc-t40nn.tar.gz   12ae7ad4b26776033fc96a49305548c48fb8f287bb835e61acd897cb924324e9
```

Zuvor auf die Kamera gelegt (nicht RC1): Commit `341a8d4`, machino
`ec2d9f84cd418cc2350bc6a503d603b354d91f4723c92262ab68f3c94bd6c8ae`.

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
