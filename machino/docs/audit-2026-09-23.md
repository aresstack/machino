# Audit 2026-09-23 — Nachprüfung des Release-Stands

Kein neues Arbeitspaket. Dieses Audit nimmt die tragenden Behauptungen des
RC1-Stands und prüft sie **gegen die Wirklichkeit**, statt die Dokumente
nachzuerzählen. Zwei davon ließen sich von dieser Maschine aus nicht prüfen;
das steht unten als Einschränkung und nicht als Haken.

---

## 1. Ein verhinderter Falschbefund, gleich zu Beginn

Der erste Testlauf meldete **Exit 0**, und der Hintergrundlauf bestätigte
"completed (exit code 0)". Gelaufen ist nichts:

```
/usr/bin/bash: line 1: make: command not found
[exited with code 0]
```

`make -C machino test 2>&1 | tail -25` gibt den Status von `tail` zurück, nicht
den von `make`. Ein Blick nur auf den Rückgabewert hätte hier "2582 Tests grün"
produziert. Dieselbe Familie wie der awk-Selektor in AP21, die
`git grep`-Maskierung in AP26 und das Schließmuster in AP29: **Prüfcode
scheitert still und meldet Erfolg.**

## 2. Was von hier aus NICHT prüfbar ist

| Gate | Behauptet | Status heute |
|---|---|---|
| C++-Hosttests | 2582 bestanden | **NICHT NACHVOLLZIEHBAR** — auf dieser Maschine existiert kein `make`. Weder MSYS2 (`usr/bin`, `mingw64`, `ucrt64`, `clang64`) noch PowerShell finden eines; `cmake` und `ninja` fehlen ebenfalls. Nur `g++` unter `ucrt64` ist da |
| CI grün | inkl. MIPS-Crossbuild | **NICHT NACHVOLLZIEHBAR** — `gh` ist nicht mehr angemeldet (`gh auth status`: not logged into any host) |

Beides sind Werkzeugausfälle dieser Umgebung, keine Befunde über den Code.
Aber sie bedeuten: **die beiden wichtigsten Gates in `RELEASE.md` sind heute
unbelegt.** Sie waren es beim Setzen des Tags nicht — nachprüfen kann ich das
jetzt nicht, und deshalb behaupte ich es auch nicht.

`git push` funktioniert weiterhin, die Git-Anmeldung ist also intakt; betroffen
ist nur der API-Zugang von `gh`.

## 3. Integrität des Release-Stands — geprüft, in Ordnung

```
Tag machino-rc1     -> 8f101b6      RELEASE.md behauptet 8f101b6      MATCH
Commits seit RC1    3, alle nur Dokumentation
Diff gegen HEAD ueber machino/src, machino/openipc, machino/Makefile
                    -> LEER
```

**Das Artefakt ist von allem, was seither passiert ist, unberührt.** Die Hashes
in `RELEASE.md` beschreiben weiterhin genau den Code, der `machino-rc1` ist.

## 4. Der AP34-Blocker, unabhängig gegengeprüft

Meine WeirdIKE-Suchen liefen über `gh` — das, wie oben, **nicht angemeldet
ist**. Damit stand die Frage, ob die Null-Treffer echt waren oder stille
Auth-Fehler. Über die öffentliche API wiederholt, mit Kontrolle:

```
HTTP-Status der Suchanfrage          200      (nicht 403, nicht rate-limited)
search/repositories?q=weirdike       total_count 0
search/repositories?q=weirdiked      total_count 0
search/repositories?q=ikev2+ingenic  total_count 0
search/repositories?q=openipc+ipsec  total_count 0
users/aresstack/repos                16 oeffentliche, keines IKE/IPsec
```

Die 16 gegen die 20 von vorhin sind stimmig: damals war `gh` noch angemeldet
und zählte private mit. **Der Blocker hält**, jetzt aus zwei unabhängigen
Richtungen und mit einer Kontrolle, die 0 von "abgewiesen" trennt.

## 5. Der Sicherheits-Gap ist jetzt gemessen, nicht angenommen

`ap33-backlog.md` führte S1–S3 als `PENDING_PHYSICAL`. Das war zu vorsichtig:
der Ausgangszustand ist fernbedienbar prüfbar, weil beide Fixes **vor** der
Auth greifen müssen. Zwei rohe Requests vom PC, mehr nicht:

```
bare-LF-Header + zweiter Header   -> HTTP/1.1 401 Unauthorized
GET /..%2f..%2fetc/passwd         -> HTTP/1.1 401 Unauthorized
```

Ein Build mit den AP30-Fixes müsste **400** liefern, bevor die Auth überhaupt
befragt wird. Der laufende Prozess tut das nicht — er nimmt beide Requests an.

Einordnung, ohne sie größer zu machen als sie ist: 401 heißt, dass der Parser
die missgebildeten Requests **akzeptiert**, nicht dass sie ausnutzbar sind. Die
Auth steht davor, und der Relay-Pfad zu busybox wird unauthentifiziert nicht
erreicht. Der Befund ist der Beweis des Ausgangszustands — und damit zugleich
der Beweis, dass der Gegentest nach der Ablösung ohne Hardware am Platz läuft.

## 6. Gerätezustand

```
pid 992, uptime 9,1 h, kein Neustart seit dem letzten Stand
laufende exe      /usr/bin/machino (deleted)     <- Platte wurde darunter ersetzt
sha256 Platte     ec2d9f84...  = Commit 341a8d4  (dokumentiert)
laufender Code    c1edd92, aelter                (dokumentiert)
Listener          22, 554, 80, 127.0.0.1:85      (exakt wie RELEASE.md)
Front Door        HTTP 401 in 2,5 ms
RSS               5136 kB
Threads / FDs     4 / 18                         (COLD_IDLE, kein Consumer)
MemAvailable      21 048 kB
```

Zwei Anmerkungen:

* **Threads 4 / FDs 18** widersprechen nicht den 12/25 aus AP29 — jene wurden
  unter Last gemessen, diese im Leerlauf. Beim Vergleichen von Ressourcenzahlen
  gehört der Lastzustand dazu, sonst entsteht genau die Sorte Scheinbefund, die
  dieses Projekt schon zweimal hatte.
* **RSS 5136 kB** liegt über dem letzten dokumentierten COLD_IDLE-Boden
  (5103 kB, `ap25-soak.md`). Das ist **eine** Stichprobe nach 9,1 h und
  entscheidet nichts — die offene Frage P1 bleibt offen. Es ist der bisher
  höchste beobachtete Leerlaufwert und widerspricht der Leck-Hypothese nicht.

## 7. Ergebnis

| | |
|---|---|
| **RELEASE_BLOCKER** | keiner neu gefunden |
| **Artefakt-Integrität** | **PASS** — Tag, Commit und Hashes stimmen, seit RC1 nur Doku |
| **AP34-Blocker** | **BESTÄTIGT** mit unabhängiger Gegenprobe |
| **Sicherheits-Gap S1/S3** | **von PENDING_PHYSICAL auf BELEGT hochgestuft**; der Gegentest bleibt offen |
| **Test- und CI-Gate** | **UNBELEGT von dieser Maschine** — kein `make`, kein `gh`-Login |
| **P1 (COLD_IDLE)** | unverändert offen, ein weiterer Datenpunkt dazu |

Das Ehrlichste, was dieses Audit sagen kann: der Release**stand** ist intakt und
die Dokumente stimmen dort, wo ich sie nachmessen konnte. Zwei Gates konnte ich
nicht nachmessen, und das ist ein Unterschied zu "sie sind grün".
