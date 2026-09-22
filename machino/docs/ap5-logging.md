# AP5 — `/ws/logs` abgenommen, Logging-Lifecycle gehärtet

**Status: Acceptance vollständig automatisiert durchgeführt. Rows 6.1–6.4 sind
geschlossen. Ein echter Defekt gefunden und behoben. AP5.15 `PENDING_PHYSICAL`.**

Commit `13f998f`. Testskripte: `C:\tmp\ap5-logs.ps1`, `C:\tmp\ap5-part2.ps1`.

## Methodik

Jede Zusicherung benutzt eine **Nonce**, die per `logger -t machino-ap5` in
syslog geschrieben und im WebSocket-Strom gesucht wird. Nichts hängt davon ab,
dass zufällig eine Zeile auftaucht — die Zeilen 6.1–6.4 standen offen, **weil**
ein früherer Lauf einen nebenbei offenen Tab als Beleg gelesen hatte.

## Der gefundene Defekt: `ws_logs_clients` zählte nie hoch

```
dec(&RuntimeCounters::ws_logs_clients)   an ZWEI Teardown-Stellen
inc(...)                                 nirgends
```

Da `dec()` bei null klemmt, stand der Zähler **für die gesamte Prozesslaufzeit
auf 0**, während Clients verbunden waren und Logs empfingen.

**Das war nicht kosmetisch.** Genau dieser Zähler wurde am selben Tag als Beleg
dafür gelesen, dass die Logs-Seite des Nutzers während des 10.4-Laufs
geschlossen war — und die Zeilen 6.1–6.4 blieben teilweise deshalb offen. Der
Zähler konnte gar nichts anderes sagen. **Diese Schlussfolgerung ist
zurückgezogen** (siehe `hardware-acceptance-2026-09-22.md`).

Die anderen drei Gauges wurden auf dieselbe Form geprüft: `ws_video_clients`,
`rtsp_sessions` und `webrtc_sessions` haben je genau ein `inc` und passende
`dec`. Nur dieser eine war unausgeglichen.

Kein Hosttest möglich: `http_server.cpp` braucht POSIX-Sockets und ist nicht im
Host-Testbuild. Geprüft wird er durch das AP5-Skript, das zwei echte
WebSocket-Clients öffnet und den Zähler zurückliest.

## Ergebnisse

| AP | Prüfung | Ergebnis |
|---|---|---|
| 5.1/5.2 | genau 1 `logread` bei 0 Clients; 120 Zeilen ohne Subscriber | **PASS**, PID unverändert — die Pipe wird auch ohne Abnehmer geleert |
| 5.3 | ein Client, Nonce | **PASS**, `101`, Nonce empfangen |
| 5.4 | zwei Clients, Fan-out | **PASS**, beide empfangen dieselbe Nonce, **ein** Reader |
| 5.5 | A schließen | **PASS**, B empfängt weiter, `logread` unverändert |
| 5.6 | letzten Client schließen | **PASS**, Reader **überlebt** (PID 995) |
| 5.7 | Reconnect | **PASS**, **gleiche** PID — Disconnect ist kein Prozess-Lebenszyklus |
| 5.8 | harter Abbruch (RST, kein Close-Frame) | **PASS**, Subscriber entfernt, 0 CLOSE_WAIT |
| 5.9 | Client liest nicht mehr, 400 Zeilen | **PASS**, VmRSS +28 kB — beschränkt; ein gesunder Client daneben lief weiter |
| 5.10 | Sturm, 300 Zeilen aus **einer** Schleife | **PASS**, 300/300 empfangen, danach weiter nutzbar |
| 5.11 | kein Media-Demand | **PASS**, COLD_IDLE vor, während und nach dem Log-Client; 0 Consumer |
| 5.12 | Logs während ACTIVE | **PASS**, Logs fließen, **kein** neuer `logread` bei lebendem IMP |
| 5.13 | Teardown + Re-Init | **PASS**, **1 796 154 Bytes** beim zweiten Start, `init_failures 0`, `init_retries 0` |
| 5.14 | drei Connect/Disconnect-Zyklen | **PASS**, identische PID, FDs 19–20, keine Zunahme |
| 5.16 | Reader stirbt | **per Inspektion**: `reader_died()` loggt einmal, `mark_dead()`, schließt Subscriber, **kein** Re-Fork |
| 5.17 | FDs / Prozesse / Speicher | **PASS**, FDs 20 → 19, 0 Zombies, 1 `logread` |
| 5.18 | File-Sink | **PASS**, 54 993 Bytes, wächst; Rotation bei 256 KiB noch nicht erreicht |
| 5.19 | Syslog-Identität | unverändert `majestic` |

`AP5.13` ist der Punkt, der zählt: das ist exakt die Sequenz, die im
Ursprungsvorfall den nächsten `IMP_System_Init` scheitern ließ.

## Rows 6.1–6.4 geschlossen

| Zeile | Verdikt |
|---|---|
| 6.1 `/ws/logs`, ein Viewer | **PASS** — `101`, Nonce empfangen |
| 6.2 zwei Viewer | **PASS** — beide empfangen dieselbe Nonce aus **einem** `logread` |
| 6.3 einen schließen | **PASS** — der andere läuft weiter |
| 6.4 beide schließen | **PASS** — Child bleibt bestehen (so gewollt), **keine Zombies** |

Zu 6.4 ausdrücklich: die Zeile fragte ursprünglich „child gone, no zombie". Der
Vertrag hat sich seit Fix A geändert — der Reader ist **absichtlich**
persistent und stirbt erst mit dem Daemon. „Child gone" wäre heute ein
**Fehler**. Abgenommen wird gegen den aktuellen Vertrag: ein Reader, null
Zombies.

## AP5.15 — `PENDING_PHYSICAL`

Das Reaping beim Daemon-Shutdown ist **nicht** geprüft. Es bräuchte einen
Daemon-Neustart, und ein **warmer** Neustart ist auf dieser Kamera der
dokumentierte Hardlock-Auslöser. Unbeaufsichtigt ausgelöst riskiert er eine
Box, die tot liegen bleibt — und blockiert damit den restlichen Batch.

Offen:

```
Machino beenden
  -> SIGTERM an logread
  -> bounded waitpid
  -> notfalls SIGKILL
  -> abschliessendes waitpid
  -> kein alter logread, kein Zombie
Neustart
  -> genau 1 neuer logread
```

Der Code dafür existiert (`LogReader::stop()`: SIGTERM → 20×50 ms `waitpid` →
SIGKILL → blockierendes `waitpid`) und ist ungeprüft.

## Nebenfunde

Nur notiert, nicht bearbeitet: die Rotation des File-Sinks bei 256 KiB konnte
nicht ausgelöst werden, ohne absichtlich tmpfs zu fluten — dafür fehlen bei
55 kB noch rund 200 kB Logaufkommen. Bleibt offen.
