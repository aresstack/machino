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
