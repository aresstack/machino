# AP29 — Memory / FD / Process / Socket Audit

2026-09-23, 30-Minuten-Prüfung. Kein künstlicher Sampler: **eine** Momentaufnahme
vom Gerät plus drei Stichproben im Abstand von 10 s, dazu der ohnehin laufende
AP25-Soak als Zeitreihe und eine statische Paarungsprüfung im Quelltext.

---

## Was das Gerät sagt — unter Last gemessen

Aufgenommen, während der Soak MSE-Consumer fuhr:

```
Prozess 992        Threads 12   VmRSS 6764 kB   FDSize 256
FDs gesamt         25
  6  anon_inode    epoll / timerfd / eventfd
  5  socket
  3  pipe          logread-Leseende, Wake-Pipe, ...
  3  /dev/null
  4  /dev/tx-isp, /dev/rmem, /dev/isp-m0, /dev/log_main
  1  machino.log
Sockets            5 LISTEN, 2 ESTABLISHED, 4 TIME_WAIT
  CLOSE_WAIT       KEINE
/proc/net/sockstat TCP: inuse 6 orphan 0 tw 4 alloc 7 mem 45
Prozesszustände    83 S, 1 D  -  KEIN Zombie
logread            genau EINER: pid 995, PPid 992
Stabilität         25 FDs / 12 Threads über drei Stichproben unverändert
```

Drei Dinge sind hier die Antwort auf frühere Erfahrungen:

* **Kein CLOSE_WAIT.** Das ist der Zustand, der entsteht, wenn die Gegenseite
  schließt und wir den Deskriptor nicht schließen. Null davon heißt: jeder
  Abbau greift.
* **`tw 4`** gegen `tcp_max_tw_buckets = 512`. Und `tw` ist hier der
  *gleichzeitige* Wert aus `sockstat`, nicht der kumulative Zähler, den ich
  früher einmal mit ihm verwechselt habe.
* **Genau ein logread, Elternteil 992.** Der Fork passiert einmal beim Start,
  vor jedem IMP-Kontakt; `/ws/logs` fügt danach nur Abonnenten hinzu.

---

## Ressourcenmatrix

| Ressource | owned by | released at | failure cleanup |
|---|---|---|---|
| HTTP-Listener `listen_fd_` | `HttpServer` | `~HttpServer` / `stop()` | Bind schlägt fehl → Socket sofort geschlossen, `start()` meldet Fehler |
| HTTP-Client-FD | `Client` im `clients_`-Vektor | Poll-Schleife: `close(c.fd)`, dann `erase` | jeder Fehlerpfad setzt `ok=false` und läuft durch denselben Abbau |
| Relay-Socket `relay_fd` | derselbe `Client` | `fail()` (2×), Client-Abbau, `~HttpServer` | `relay_open`: `connect`-Fehler schließt sofort; `fail()` schließt und nullt |
| `Client::out` / `in` | `Client` | mit dem Client | `MAX_IN` 16 KiB, `max_out_buffer` 64 KiB, `ws_out_cap` 512 KiB — Überlauf trennt, staut nicht |
| `ws_sink` / `rtc_sink` | `Client` | Abbau: `close()` + `hub->unsubscribe()` | beide Teardown-Pfade (Poll-Schleife und `~HttpServer`) tun dasselbe |
| `ws_demand` / `rtc_demand` | `Client` | `~DemandHandle` beim `erase` | RAII; sicher, weil `httpd` **nach** dem `PipelineManager` deklariert und daher vorher zerstört wird |
| `PeerSession` (WebRTC) + UDP-Socket | `Client::rtc` (`unique_ptr`) | `c.rtc.reset()` im Abbau | zweiter Offer auf demselben Socket wird mit „busy" abgewiesen, überschreibt also nichts |
| RTSP-Listener | `RtspServer` | `close_listener()`, `~RtspServer` | atomarer Rebind: neuer Port bindet **vor** dem Schließen des alten (AP3.5) |
| RTSP-Session-FD + `udp_fd` | `Session` | Sessionende (`rtsp_server.cpp:315`) | Reaper joint den Thread **nie** unter `clients_m_` |
| RTSP `s.demand`, `s.sink` | `Session` | `demand.release()` explizit + Destruktor; `unsubscribe(s.sink)` | Auth wird **vor** jedem Pfad geprüft, der Demand nimmt oder einen Sink abonniert |
| logread-Kind + Pipe | `LogReader` (einmal in `main`) | SIGTERM → `waitpid(WNOHANG)` → SIGKILL → blockierendes `waitpid` | Start scheitert → nicht fatal, Kamera streamt ohne Logansicht |
| chpasswd-Kind + Pipe | `set_root_password` | `waitpid` mit EINTR-Wiederholung | `pipe`/`fork` scheitert → beide Enden geschlossen, Fehler gemeldet |
| Unit-Capture-Thread | `PipelineManager::Unit` | `stop_unit_locked`: `quit=true`, dann `join()` | Join unter `m_` ist sicher: der Thread nimmt nur `u.win_m` |
| `Unit::fs` / `enc` / Binding | `Unit` (`unique_ptr`) | `stop_unit_locked` | gibt **nur** frei, was `fs_enabled`/`enc_started`/`bound` als erworben ausweisen (AP2.6) |
| `AuPool` | `Unit` | mit der Unit | fester Pool, 16 × 64 KiB — kein Wachstum pro Frame |
| `Sink`-Warteschlange | Consumer | `close()` + `unsubscribe` | Tiefe 4, Drop-Oldest, Resync am nächsten Keyframe |
| Grace-Timer-FD | `LinuxGraceTimer` | Destruktor | — |
| Discovery-Socket + Wake-Pipe | `DiscoveryServer` | `stop()` schließt beide Pipe-Enden und den Socket | Routing-Probe-Socket (`discovery_server.cpp:45`) wird in derselben Funktion geschlossen |
| epoll / timerfd in `main` | `main` | `close(tfd)`, `close(ep)`, `close(sfd)` am Schleifenende | — |
| Heap allgemein | RAII | — | **kein einziges `delete` im Baum**; die zwei rohen `new` gehen direkt in einen `unique_ptr` |

---

## Paarungsprüfung

| Paar | Befund |
|---|---|
| `open`/`socket`/`accept`/`pipe`/`timerfd`/`epoll` ↔ `close` | 15 Erzeugungsstellen, jede mit Abbaupfad; oben einzeln zugeordnet |
| `new` ↔ `delete` | 2 rohe `new`, beide sofort in einem Smart-Pointer. **0 `delete`.** |
| `fork` ↔ `waitpid` | 2 Forks, beide geerntet; live bestätigt: 0 Zombies |
| Thread-Start ↔ `join` | 7 Startstellen, 7 Joins; keine `detach()` |
| `acquire` ↔ `release` | 4 `acquire_unit`, alle über move-only RAII mit idempotentem `release()` |
| `subscribe` ↔ `unsubscribe` | 4 Paare |

---

## Heap-Retention gegen monotones Leck

Aus dem laufenden Soak (Stand Zyklus 4 von 6), die COLD_IDLE-**Böden** — nur
die zählen, denn ein Leck zeigt sich als Boden, der steigt, nicht als große
Zahl während drei Clients streamen:

```
4752  4800  4716  4908  4816  4976  4924  4964  4980 ... kB
```

Das schwankt in beide Richtungen (4908 → 4816, 4976 → 4924) und liegt in einer
Spanne von rund 260 kB. Kein monotoner Anstieg. Die Gauges kehren nach jeder
Phase auf 0 zurück, `pipeline_generation` zählt jede Kaltrunde hoch (13 → 22),
`init_failures` und `init_retries` bleiben **0**, `dropped_frames` bleibt **0**.

Die abschließende Bewertung steht in der AP25-Auswertung, wenn alle sechs
Zyklen durch sind.

---

## Ein Methodenfehler von mir, protokolliert

Meine erste Zählung der Schließstellen suchte nach `close($var` und meldete für
`relay_fd`, `udp_fd` und `wake_` jeweils **null** Treffer. Der Code schreibt
aber `close(c->relay_fd)`, `close(s.udp_fd)` und `close(*p)`. Das Muster war
falsch, nicht der Code — dieselbe Sorte Fehler wie der awk-Selektor in AP21 und
die `git grep`-Maskierung in AP26. Bei Prüfcode ist das Scheitern still: er
sagt „geprüft" und hat das Falsche geprüft.

## Ergebnis

Keine Leckstelle gefunden, kein unpaariger Abbau, kein CLOSE_WAIT, kein Zombie,
keine unbegrenzte Warteschlange. Die einzige offene Frage bleibt der seltene
Hardlock (`ap17-hardlock.md`), und der ist kein Ressourcenbefund: er tritt bei
25 stabilen FDs und 12 Threads auf.
