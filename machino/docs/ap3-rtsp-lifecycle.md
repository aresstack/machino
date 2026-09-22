# AP3 — RTSP-Runtime-Lifecycle: Enable, Disable, Port-Rebind

**Status: implementiert, hardwareverifiziert, ab jetzt Regression-Gate.**

Eine Ausnahme: **AP3.5 (atomarer Rebind) war nicht erfüllt** und wurde beim
Durchgehen dieses Pakets korrigiert — siehe unten. Die Korrektur ist
CI-grün, aber **noch nicht auf Hardware geprüft**.

## Der Vertrag

```
rtsp.enabled = true, :554 lauscht, 0 Sessions
  -> COLD_IDLE, Sensor aus, Encoder aus

DESCRIBE/SETUP/PLAY -> acquire -> STARTING -> ACTIVE
TEARDOWN / close    -> release -> GRACE_IDLE -> STOPPING -> COLD_IDLE
```

**Ein Listener ist kein Consumer.** Das steht auch so in der Startzeile:
`listening on :554 paths /ch0 (main) /ch1 (sub) (pipeline stays cold until PLAY)`.

## AP3.5 — der Punkt, der nicht erfüllt war

Vorher schloss `set_port` zuerst und band dann neu:

```cpp
close_listener();
if (open_listener(port)) { … }
if (open_listener(old_port)) return rejected("… kept old");
cfg_.enabled = false;
LOGE(MOD, "re-bind to :%d failed AND :%d could not be restored - rtsp is now down");
```

Der Code war über sein Scheitern ehrlich, aber es gab einen Zweig, in dem eine
**Konfigurationsänderung die Kamera ohne RTSP zurücklässt**. Das Paket verlangt
ausdrücklich „alten Listener **behalten**".

Zwei verschiedene Ports kollidieren nie, also lässt sich der neue zuerst
beweisen. `bind_listener()` bindet und lauscht, **ohne** einen laufenden
Listener anzufassen; `adopt_listener()` übernimmt einen bereits gebundenen fd.
`set_port` bindet neu, und **erst bei Erfolg** fällt der alte Listener:

```
bind_listener(neu)
  fehlgeschlagen -> nichts angefasst, alter Listener akzeptiert weiter,
                    Antwort: "cannot bind port N; kept M"
  erfolgreich    -> close_listener() -> adopt_listener(fd, neu)
```

Der Zweig „beide unten" existiert damit nicht mehr, statt nur besser gemeldet
zu werden.

**Nicht hosttestbar:** `rtsp_server.cpp` braucht POSIX-Sockets und ist nicht im
Host-Testbuild. Der zugehörige Hardwaretest steht unten und ist **offen**.

## Was hardwareverifiziert ist

| | |
|---|---|
| `/ch0` PLAY, RTP/AVP/TCP interleaved | **194 135 Bytes in 3 s**, erstes Frame 96 ms nach PLAY |
| `rtsp.enabled` true → false | `:554` verschwindet aus `netstat`, live |
| false → true | `:554` lauscht wieder |
| 554 → 8554 | bindet neu, antwortet auf OPTIONS am neuen Port |
| 8554 → 554 | sauber zurück |
| mit `rtsp.auth=true`: `/ch0` | 1 543 745 B ≈ 3,0 Mbit/s (konfiguriert 3000) |
| mit `rtsp.auth=true`: `/ch1` | 226 235 B ≈ 452 kbit/s (konfiguriert 512) |
| Lifecycle nach letztem Consumer | ACTIVE → GRACE_IDLE → STOPPING → COLD_IDLE, `pool exhausted 0` |

Alles ohne Daemon-Neustart.

## AP3.7 — Pfadvertrag

Belegt im Code: `/ch0` und `/stream=0` bilden beide auf `UNIT_MAIN` ab,
`/ch1` und `/stream=1` auf `UNIT_SUB` — und `/stream=1` nur, wenn der
Substream überhaupt konfiguriert ist. Alias-Pfade erzeugen **keine** zweite
Pipeline; sie sind Namen für denselben Stream-Index.

## AP3.11 — mehrere Clients teilen den Encoder

Jede Session hängt am selben `StreamHub`-Ausgang. Ein zweiter Zuschauer kostet
keinen zweiten Encoder. Hardwareseitig mitgeprüft, als drei parallele
Medien-Consumer 15 Minuten liefen (AP2).

## AP3.10 — Session- und Demand-Cleanup

Eine Session hält genau **einen** Demand: `acquire` beim Start, `release` beim
Unwind ihres Threads. `reap_finished()` verbindet und entfernt fertige Clients,
sonst bliebe pro Connect/Disconnect ein joinbarer `std::thread` samt Stack
liegen — auf einer Kamera, die den ganzen Tag reconnectet, fällt das lange vor
einem Neustart auf.

Nach dem letzten Client gemessen: `rtsp_sessions = 0`, `consumers = 0`,
0 Zombies, 0 CLOSE_WAIT.

## Regression-Gate

Bei Änderungen an `HttpServer`, dem RTSP-Server, dem Session-Management,
`DemandHandle`, `PipelineManager`, Config-Hot-Reload oder `StreamHub`:

```
 1. nur Listener laufend            -> COLD_IDLE, 0 Consumer
 2. /ch0 PLAY                        -> ACTIVE, RTP fliesst
 3. Session schliessen               -> GRACE_IDLE -> STOPPING -> COLD_IDLE
 4. /stream=0 PLAY                   -> derselbe Stream, KEINE zweite Pipeline
 5. rtsp.enabled = false             -> :554 weg, Sessions beendet
 6. rtsp.enabled = true              -> :554 wieder da, PLAY geht wieder
 7. 554 -> 8554 -> 554               -> beide Male ohne Neustart
 8. erneut PLAY                      -> Re-Init funktioniert
 9. rtsp_sessions = 0, consumers = 0, 0 Zombies, 0 CLOSE_WAIT
10. init_failures = 0, init_retries = 0, pool exhausted = 0
```

### Offen: der Rollback-Fall

Der 554 → 8554 → 554-Durchgang hat nur den **Erfolgspfad** geprüft. Der
Rollback wurde auf dem Gerät **nie** ausgelöst. Zu ergänzen:

```
11. rtsp.port = 22        (belegt von dropbear)
    -> Antwort "cannot bind port 22; kept 554"
    -> :554 lauscht WEITER
    -> PLAY auf /ch0 funktioniert unverändert
    -> rtsp.enabled bleibt true
```

Das ist der Test für die Korrektur oben und gehört in den nächsten
Hardwaredurchgang.

## Abgrenzung

Nicht Bestandteil: RTSP-Auth-Policy und Digest (eigenes AP), `system.unsafe`,
Substream-WebUI, ONVIF, Relay (AP1), WebRTC, MSE, Hardlock, Watchdog.

Zur Erinnerung aus der Acceptance: `rtsp.auth` ist **default `false`** und nur
über `machino.conf` setzbar, nicht über die WebUI-Schema. Ein bloßer
RTSP-Client bekommt damit standardmäßig einen Stream **ohne Credentials** —
eine bekannte Drop-in-Abweichung von majestic, dokumentiert in
`hardware-acceptance-2026-09-22.md`, und ausdrücklich Sache des Auth-APs.
