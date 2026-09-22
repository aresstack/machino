# AP4 — Hardware-Watchdog / automatische Recovery

**Status: implementiert, CI/MIPS grün, Geräte-Smoke PASS, Reset-Abnahme
`PENDING_PHYSICAL`.**

Commits `3ad7d1e`, `f5af812`.

## AP4.1 — Was auf dieser Kamera wirklich da ist

Rein lesend ermittelt, bevor irgendetwas geöffnet wurde:

```
/dev/watchdog    char 10:130   (misc)
/dev/watchdog0   char 254:0    (watchdog-Klasse)
Treiber          ingenic,watchdog.16 auf /devices/platform/apb/10002000.tcu
Modul            keins geladen -> einkompiliert
sysfs            nur dev/device/power/subsystem/uevent
```

Kernel 4.4 exportiert die `timeout`/`nowayout`/`state`-Attribute noch nicht —
`nowayout` war aus sysfs also **nicht** lesbar, und Öffnen schärft.

## AP4.2 — Majestics Vertrag

Aus den Strings des Binaries auf dem Gerät:

```
/dev/watchdog
Watchdog device (%s) not found
Watchdog device open failed %s
WDIOC_GETSUPPORT error '%s'
Watchdog timeout set to %u seconds
WDIOC_SETTIMEOUT error '%s'
watchdog_start
```

Also: `open` → `WDIOC_GETSUPPORT` → `WDIOC_SETTIMEOUT` → füttern. Aus
`/etc/majestic.yaml`: `watchdog: enabled: true, timeout: 15`.

Weder `/etc/init.d/majestic` noch sonst ein Skript fasst den Watchdog an —
majestic macht es selbst. Machinos Migration hat die Sektion bisher mit
„handled by the OpenIPC init + streamerctl" **ignoriert**; diese Begründung war
auf dieser Kamera nachweislich falsch.

## Die `nowayout`-Frage — direkt gemessen

Am Gerät, mit reiner Shell und ohne den Daemon:

```
uptime vorher                     5243 s
exec 3>/dev/watchdog              (öffnen = schärfen)
printf "V" >&3                    (Magic Close)
exec 3>&-                         (schließen)
... 89 s gewartet ...
uptime nachher                    5333 s   -> KEIN Reset
```

**`nowayout` ist aus, und Magic Close entschärft wirklich.** Das bestätigt
unabhängig, was `streamerctl` aus einem früheren Hardwaretest notiert hatte.
`LinuxWatchdog::close(true)` darf sich darauf stützen — und tut es trotzdem
nicht blind: schlägt der `V`-Write fehl, sagt das Log, dass der Watchdog noch
scharf sein könnte, statt zu behaupten er sei aus.

## AP4.3–4.5 — Warum der Feeder nicht blind füttert

Das ist der einzige Punkt, an dem dieser AP scheitern könnte, ohne dass es
auffällt:

> Ein Watchdog, den ein unabhängiger Timerthread füttert, beweist nur, dass der
> Timerthread lebt. Hängt die Hauptschleife, während der Feeder weiterläuft, ist
> der Watchdog **schlechter als nutzlos** — er garantiert, dass die Kamera sich
> NICHT erholt.

Deshalb:

```
Hauptschleife, jede Iteration   -> heartbeat()   -> ++health_epoch
Feeder, alle 200 ms             -> tick()
    epoch seit letztem Feed bewegt ? feed() : skipped++
```

Die `epoll_wait`-Blockierung ist dafür auf **1 s** begrenzt statt unbegrenzt: so
beweist auch eine **leerlaufende** Kamera, dass sie lebt. Nichts hier hängt an
Frames, Sessions oder Encodern — Leerlauf ist gesund.

Feed-Intervall: ein Drittel des Timeouts (15 s → 5 s), das lässt zwei verpasste
Feeds Luft, bevor die Hardware feuert.

## AP4.8 — Shutdown-Reihenfolge

Entschärft wird **vor** dem Teardown, nicht danach. Encoder und Pipeline
abzubauen dauert Sekunden, und die Schleife hört in dem Moment auf zu schlagen,
in dem sie verlassen wird — ohne diese Reihenfolge würde ein geordneter Neustart
genau den Reset erzeugen, den er nicht erzeugen darf.

## AP4.6/4.7 — Fehlerfälle und Konfiguration

Fehlt `/dev/watchdog` oder lässt es sich nicht öffnen: **eine** Warnzeile, der
Daemon läuft weiter. Keine Retry-Schleife. Scheitert `WDIOC_SETTIMEOUT`, bleibt
der Watchdog scharf mit dem Treiber-Default.

Konfiguration unter majestics eigenen Namen: `watchdog.enabled` (Default `true`)
und `watchdog.timeout` (Default `15`, Grenzen 1–300). Die Migration **mappt** die
Sektion jetzt, statt sie zu verwerfen.

**Bewusst nicht im WebUI-Schema.** Upstream bietet den Schalter auch nicht an,
und ein Knopf, der die einzige automatische Recovery abschaltet, gehört nicht
einen Klick weit weg.

## AP4.9 — Telemetrie

Unter `watchdog` in `/api/v1/telemetry`: `available`, `enabled`,
`timeout_seconds`, `feeds`, `skipped`, `feed_errors`, `health_epoch`,
`last_feed_age_ms`.

**`skipped` ist das Feld für nach einem unerklärten Neustart.** Es zählt die
Ticks, die absichtlich **nicht** gefüttert haben, weil die Hauptschleife stand —
der einzige legitime Grund, aus dem die Hardware feuern darf.

## AP4.10 — Hosttests

140 Zusicherungen gegen ein Fake-Gerät. Der wichtigste Test ist der, der den
ganzen AP trägt:

```
Feeder tickt 20 s lang auf seinem Zeitplan
Hauptschleife schlägt kein einziges Mal
-> KEIN Feed, skipped = 20, last_feed_age = 20000 ms
```

Dazu: gesunde Schleife füttert; ein Ausfall-Intervall und Erholung; 50 Herzschläge
zwischen zwei Ticks ergeben **einen** Feed; zu früher Tick zählt nicht als
Stillstand; 60 s Leerlauf ohne einen einzigen Skip; fehlendes Gerät stoppt den
Daemon nicht; abgelehntes `SETTIMEOUT`; ein Treiber, der einen **anderen**
Timeout nimmt als verlangt; scheiternde Feeds ohne Absturz; `stop()` idempotent
und mit angefordertem Magic Close; `start()` zweimal ohne Doppel-Arm.

Hosttests 2210 → 2350. CI inkl. MIPS-Cross-Build grün.

## AP4.12 — Geräte-Smoke, soweit ohne Nutzer möglich

| | |
|---|---|
| `/dev/watchdog` vorhanden | **PASS** |
| Öffnen schärft | **PASS** |
| Magic Close entschärft, kein Reset nach 89 s | **PASS** |
| Binary installiert | `machino f5af812` auf der Platte |
| Laufender Prozess | noch `c1edd92` — ein Install tauscht ihn nicht |

## AP4.13 — Reset-Abnahme: `PENDING_PHYSICAL`

Der definitive Test — Herzschlag absichtlich anhalten, Watchdog feuern lassen,
Kamera bootet selbst — ist **nicht** durchgeführt.

Begründung, nicht Bequemlichkeit: der Watchdog wird erst aktiv, wenn der neue
Daemon läuft, und dafür braucht es einen Neustart. Ein **warmer** Daemon-Neustart
ist auf dieser Kamera der dokumentierte Hardlock-Auslöser
(`t40nn-freeze-nach-install`). Ihn unbeaufsichtigt auszulösen riskiert eine Box,
die tot liegen bleibt, bis jemand den Stecker zieht — und das würde **alle**
folgenden Arbeitspakete blockieren. Der Preis wäre höher als der Erkenntnisgewinn.

Offen bleibt damit:

```
1. Cold Power-Cycle
2. Startlog: "armed: <identity>, timeout N s, feeding every M ms"
3. telemetry.watchdog: enabled=true, feeds steigt, skipped=0
4. /etc/init.d/machino restart  -> KEIN Reset waehrend des Neustarts
5. Herzschlag anhalten          -> Reset nach ~timeout, Kamera kommt allein zurueck
```

Schritt 5 braucht zusätzlich eine Möglichkeit, den Herzschlag gezielt
anzuhalten, ohne den Kernel zu wedgen — z. B. ein Debug-Signal. Das ist bewusst
**nicht** gebaut: ein Schalter, der den Watchdog auslöst, ist ein Schalter, der
die Kamera neu startet, und er gehört nicht ohne Not ins Produktionsbinary.

## AP4.14 — Abgrenzung

Der Watchdog ist **Recovery, keine Ursachenbehebung**.

Zulässig:

> Der Watchdog begrenzt die Auswirkung eines Hängers, der sich nicht selbst
> erholt.

Nicht zulässig:

> Der Watchdog behebt den Hardlock.

Der seltene T40NN-Hardlock bleibt ein eigener, offener Diagnosepunkt. Und selbst
ein Watchdog-Reset ist **kein** geordnetes Shutdown: `fake-hwclock save` läuft
dabei nicht, die Uhr verliert also weiterhin die Ausschaltzeit (siehe
`ap12-time.md`).
