# AP12 — Zeit auf einer Kamera ohne RTC und ohne NTP

Stand 2026-09-22. Alles hier ist am Gerät gemessen, nicht aus Code abgeleitet;
wo etwas nur gelesen und nicht beobachtet wurde, steht es dabei.

## Das Problem in einem Satz

Die WebUI meldete `⚠ camera -1865m`. Die Meldung ist **korrekt** — die Kamerauhr
lag wirklich 31 Stunden zurück.

## AP12.1 — Der bestehende Vertrag: **PASS**

OpenIPC beantwortet das für einen Browser bereits, und es funktioniert über
Machinos Front-Door:

```
System → Time → [Set from browser]
  → GET /cgi-bin/j/time.cgi?set=<epoch>     (relayed CGI, führt `date -s @…` aus)
```

Am Gerät gemessen, exakt der Aufruf den die WebUI macht, mit Session-Cookie:

| | |
|---|---|
| Drift vorher | **−1865 Minuten** |
| Antwort | `200 OK`, `{"result":"success","message":"Camera clock set from browser."}` |
| Drift nachher | **0 Sekunden** |
| `node_time_seconds` in `/metrics` | folgt sofort |
| `fake-hwclock.data` | 13 s später aktualisiert — die Korrektur überlebt einen Neustart |

**Die WebUI bleibt unverändert.** Es wurde nichts an ihr gebaut und nichts an
ihr gebraucht.

### Ein Nebenbefund, der die Plausibilitätsregeln begründet

`time.cgi` prüft ausschließlich `^[0-9]+$`. Während des Tests wurde durch einen
Locale-Fehler im Testtreiber der Wert `179010879081498` gesendet — rund
5,6 Millionen Jahre in der Zukunft. Das CGI antwortete
`{"result":"success"}`, während `date -s` den Wert stillschweigend ignorierte
und die Uhr unverändert blieb. **Ein „success", das nichts getan hat.**

Das ist stock-Verhalten und wird hier nicht geändert — aber es ist der Grund,
warum der ONVIF-Pfad unten schärfer prüft.

## AP12.2 — Bootverhalten ohne NTP, experimentell belegt

Beteiligt sind drei Dinge:

```
/etc/init.d/S02fakehwclock   → ruft beim Start `fake-hwclock load`,
                                beim Stop `fake-hwclock save`,
                                und startet den Daemon
/usr/sbin/fake-hwclock       → load / save / daemon
/etc/fake-hwclock.data       → der gespeicherte Epoch-Wert
```

### Woher die Zeit beim Boot kommt

`do_load` nimmt den **neuesten** dieser drei Werte und setzt die Uhr darauf,
falls er in der Zukunft der aktuellen Uhr liegt:

1. `/etc/fake-hwclock.data`
2. `/sys/class/rtc/rtc0/since_epoch` — **auf dieser Kamera nicht vorhanden**
3. die neueste `mtime` unterhalb von `/etc`

Gemessen nach der Korrektur: `fake-hwclock.data` = `1790101644` und die neueste
`/etc`-mtime = `1790101644` — identisch, weil das Schreiben der Datei selbst die
mtime setzt.

### Wann gespeichert wird

`do_daemon` prüft **alle 60 s**. Gespeichert wird:

* **sofort**, wenn ein Zeitsprung erkannt wird (`now > last+90` oder `now < last`)
* sonst **periodisch**, und das Intervall hängt am Dateisystem von `/overlay`:

```
ubifs                      →  900 s
ext4 / f2fs / vfat / exfat →  600 s
tmpfs                      →    0 s  (aus)
alles andere               → 3600 s
```

Unser `/overlay` ist **jffs2** und fällt in den letzten Zweig: **eine Stunde**.
Bestätigt: der Daemon läuft (`{fake-hwclock} /bin/sh /usr/sbin/fake-hwclock daemon`),
und die Sprungerkennung hat nach `date -s` innerhalb von 13 s gespeichert.

`do_save` weigert sich außerdem, einen Wert zu speichern, der älter ist als
`TIME_STAMP` aus `/etc/os-release` (hier `1789668802`) oder älter als der bereits
gespeicherte. Die Uhr kann über diesen Weg also nie rückwärts wandern.

### Was in den vier realen Fällen passiert

| Fall | Ergebnis |
|---|---|
| **sauberer Reboot** | `S02fakehwclock stop` ruft `save` → Zeit geht nur die Dauer des Neustarts verloren |
| **Power-Loss** | kein `save`; beim Boot wird der letzte periodische Stand geladen → **bis zu eine Stunde** plus die gesamte Ausschaltzeit verloren |
| **Watchdog-Reset** | identisch zum Power-Loss: ein Watchdog garantiert **kein** geordnetes Shutdown |
| **Hardlock + Stecker** | identisch zum Power-Loss |

**Damit ist die beobachtete Drift erklärt, ohne etwas zu behaupten, das nicht
gemessen wurde:** die Uhr läuft nur, solange die Kamera läuft. Jede
Ausschaltzeit — über Nacht, zwischen Tests, nach jedem der sechs Hardlocks
dieses Tages — geht vollständig verloren, und ohne erreichbaren NTP-Server holt
sie das nie wieder auf. 31 Stunden sind die Summe daraus, nicht ein Defekt.

### Die Netzwerkseite

```
ntpd:        läuft (PID 870)
/etc/ntp.conf: vorhanden, 4 × pool.ntp.org
ip route:    192.168.1.0/24 dev eth0 scope link  src 192.168.1.10
             → KEIN Default-Gateway
```

Das ist die Testverkabelung (Direktlink), kein Defekt der Kamera. Die
Netzwerkkonfiguration wird nicht angefasst.

## AP12.3 — Der definierte Fallback

Die Priorität aus dem Arbeitspaket war: vorhandenes Verhalten wiederverwenden,
dann `time.cgi`, und **keine neue proprietäre Zeit-API**.

### Geprüft: gibt es einen automatischen Stock-Mechanismus?

**Nein.** Im Upstream-Klon ruft nur `www/a/time.js` das CGI auf, und
ausschließlich aus zwei Klick-Handlern:

```js
sync.addEventListener('click', … '/cgi-bin/j/time.cgi')
set .addEventListener('click', … '/cgi-bin/j/time.cgi?set=' + Math.floor(Date.now()/1000))
```

Es gibt keinen Pfad, der beim Laden einer Seite automatisch synchronisiert.

### Geprüft: könnte Machino die Browserzeit von selbst lernen?

**Nein.** Ein Browser sendet keine Zeit. Die Header des tödlichen
Chrome-Requests aus dem Paketmitschnitt, vollständig:

```
Host, Connection, Upgrade-Insecure-Requests, User-Agent,
Accept, Referer, Accept-Encoding, Accept-Language, Cookie
```

Kein `Date`. `If-Modified-Since` trägt die `Last-Modified` der Datei, die von
der Kamera selbst stammt — zirkulär, also keine Quelle.

„Automatisch beim ersten WebUI-Kontakt" wäre daher **nur** mit einer Änderung
an der WebUI möglich, und die ist ausgeschlossen.

### Umgesetzt: ONVIF `SetSystemDateAndTime`

Das ist der **standardisierte** automatische Weg und keine Erfindung: ein NVR
stellt die Kamerauhr bei der Inbetriebnahme. Machino beantwortete bisher
`GetSystemDateAndTime`, hatte aber kein `Set` — eine echte Lücke der
ONVIF-Oberfläche, unabhängig von diesem Problem.

```
POST /onvif/device_service   SetSystemDateAndTime
  → authentifiziert (nur Get ist per Spezifikation ausgenommen)
  → <tt:UTCDateTime><tt:Date>…</tt:Date><tt:Time>…</tt:Time></tt:UTCDateTime>
```

### Damit ergibt sich diese Ordnung

| Situation | Weg | Status |
|---|---|---|
| NTP erreichbar | `ntpd` wie gehabt | unverändert |
| Browser, kein NTP | `[Set from browser]` | **verifiziert (AP12.1)** |
| ONVIF-Client/NVR, kein NTP | `SetSystemDateAndTime` | **neu, CI-grün, Hardware offen** |
| nichts davon | Uhr bleibt beim letzten gespeicherten Stand | dokumentiert |

## AP12.4 — Plausibilitäts- und Sicherheitsregeln

Umgesetzt im ONVIF-Pfad, nicht im CGI (das bleibt stock):

| Regel | Umsetzung |
|---|---|
| syntaktisch gültig | Felder werden **nach Namen** gelesen, nicht nach Position — Clients ordnen `Date` und `Time` unterschiedlich |
| nur Ziffern | ein Wert wie `20x6` wird **abgelehnt**, nicht abgeschnitten |
| echtes Datum | 31. Februar und 29. Februar im Nicht-Schaltjahr werden abgelehnt |
| plausible Größenordnung | `1700000000 ≤ epoch ≤ 4102444800` (2023-11-14 … 2100-01-01) |
| keine unnötigen `date -s` | Abweichung `≤ 2 s` ⇒ **kein Syscall**, Antwort trotzdem `200` |
| kein NTP-Ersatz | es gibt keine Nachregelung, keinen Timer, keinen Poll — nur die eine Operation auf Anfrage |
| Fehler werden gemeldet | kein Setter installiert ⇒ `ter:ActionNotSupported`; Setter scheitert ⇒ `ter:ActionFailed`. Nie ein stilles „success" |

Der Syscall selbst liegt in `main.cpp` hinter einem injizierten Setter, damit die
SOAP-Schicht hosttestbar bleibt. Er protokolliert jeden Sprung mit Vorher- und
Nachher-Wert — eine gestellte Uhr macht jede Uptime-Rechnung davor bedeutungslos,
und das Log muss sagen, wann das passiert ist.

**37 Zusicherungen** decken Authentifizierung, Happy Path, Slack-Fenster, beide
Plausibilitätsgrenzen, fünf fehlerhafte Körper, Schaltjahre, fehlende `Time`,
fehlenden Setter und scheiternden Setter ab. Hosttests 2024 → 2061.

## AP12.5 — Regressionstest

### Erledigt

* `[Set from browser]` über den Relay: **PASS** (AP12.1)
* Drift nach der Synchronisation: **0 s**, die Warnung verschwindet
* `fake-hwclock` persistiert die Korrektur: **bestätigt**
* Hosttests und CI inkl. MIPS-Build: **grün**

### Offen — braucht einen Cold Power-Cycle

Der Daemon läuft noch mit dem alten Binary; ein Install tauscht den laufenden
Prozess nicht. Nach dem nächsten Power-Cycle:

```
1. Cold Boot ohne Gateway
   → falsche Startzeit nachvollziehbar (aus fake-hwclock.data)
2. SetSystemDateAndTime mit gültiger Zeit   → 200, Uhr gestellt, Log-Zeile
3. SetSystemDateAndTime erneut, gleiche Zeit → 200, KEIN zweiter Sprung
4. SetSystemDateAndTime mit 1999            → 400 ter:InvalidDateTime
5. Warnung in der WebUI verschwunden
6. Logs danach mit richtiger Zeit
7. MSE / WebRTC / RTSP laufen weiter
8. kein zusätzlicher Media-Demand (consumers unverändert)
9. kein zusätzlicher persistenter Kindprozess (logread bleibt genau 1)
```

### Ausdrücklich NICHT getestet

**Cold Boot mit funktionierendem NTP.** Dafür bräuchte die Kamera ein
Default-Gateway, und die Testnetzwerk-Konfiguration ist laut Arbeitspaket
ausgeschlossen. Der Code fasst `ntpd` nicht an und registriert keinen Timer —
das ist ein Argument, keine Messung, und es steht hier als solches.

## Nicht Bestandteil

Watchdog, WebUI-Darstellung, RTC-Hardware, eigener NTP-Server,
Netzwerkkonfiguration. Zum Watchdog gilt weiterhin, was in
`dropin-gaps.md` steht: er würde die Häufigkeit der harten Abstürze senken und
damit auch die Uhrendrift — aber er **löst das Zeitproblem nicht**, weil auch ein
Watchdog-Reset kein geordnetes Shutdown ist und `fake-hwclock save` nicht läuft.
