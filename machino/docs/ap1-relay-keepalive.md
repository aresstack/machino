# AP1 — Relay Keep-Alive / TCP Connection Churn

**Status: implementiert, hardwareverifiziert, ab jetzt Regression-Gate.**

Commits: `7d7df58` (Keep-Alive), `40ac305` (gepufferte Folge-Requests),
`c1edd92` (bare-LF-CGI). Gate-Skript: `tools/relay-gate.ps1`.

## Das Ausgangsproblem

Machinos eigene Pfade gewährten Keep-Alive schon immer. Der **Relay-Pfad**
schloss jede Verbindung — auf beiden Seiten, also Browser→Machino **und**
Machino→busybox. Am Gerät gemessen:

| Pfad | Request 1 | Request 2 auf demselben Socket |
|---|---|---|
| nativ `/api/v1/state` | 200, `keep-alive` | **200** |
| relayed `/a/main.js` | 200, `close` | **nichts** |

Die Live-Seite zieht 38 Assets. Jedes kostete damit eine eigene
Browser-Verbindung plus eine eigene Localhost-Verbindung zu busybox.

## Warum das nicht trivial zu beheben war

Der Relay benutzt **EOF als Framing**: er schiebt die Bytes von busybox
durchgehend weiter und erfährt erst am Verbindungsabbau, dass die Antwort zu
Ende ist. `Connection: close` einfach zu entfernen hätte Antworten abgeschnitten
oder aneinandergehängt.

## Die Regel

Der Antwortkopf wird zurückgehalten, bis er vollständig ist, dann beurteilt.
Downstream bleibt **nur** offen, wenn das Ende des Körpers ohne den
Verbindungsabbau bekannt ist:

| Fall | Downstream |
|---|---|
| `Content-Length` vorhanden | **keep-alive** |
| `204`, `304`, `1xx` (kein Körper) | **keep-alive** |
| CGI ohne `Content-Length` | **close** |
| `Transfer-Encoding: chunked` | **close** — Chunk-Syntax wird nicht geparst |
| Kopf unvollständig oder kaputt | **close** |
| Körper endet vor der zugesagten Länge | **Verbindung kappen** |

**Bei jedem Zweifel: close.** Keine Antwort darf durch falsches Framing mit dem
nächsten Request vermischt werden.

Hop-by-Hop-Header vom Upstream (`Connection`, `Keep-Alive`,
`Proxy-Connection`) werden verworfen und durch `Connection: keep-alive`
ersetzt — ein durchgereichtes `close` würde den Browser dazu bringen, einen
Socket zu schließen, den Machino bewusst offenhält.

## Zwei Defekte, die dieser Fix selbst eingebaut hat

Beide gefunden, bevor ein Browser sie zu sehen bekam — und beide sind als
Testfall erhalten.

### 1. Bare-LF-CGI-Köpfe → 502 auf der ganzen WebUI

Von der Kamera abgegriffen:

```
statisch: HTTP/1.1 200 OK\r\nDate: …\r\nConnection: close\r\n…\r\n\r\n
CGI:      HTTP/1.1 200 OK\r\nContent-type: …\nPragma: no-cache\n\n
```

busybox schreibt für statische Dateien CRLF, reicht die **bare-LF-Header eines
CGI** aber unverändert durch. Ein `find("\r\n\r\n")` fand nie ein Ende, der
Kopfpuffer lief auf 8192 Bytes und schlug gegen die Oversized-Grenze. Die
Kopf-Erkennung nimmt jetzt den **zuerst kommenden** der beiden Terminatoren,
und der Zeilenparser akzeptiert beide Endungen innerhalb eines Kopfes.

### 2. Gepufferte Folge-Requests hingen bis zum Idle-Timeout

Die Parse-Schleife lief nur bei frischem `POLLIN` und nur solange kein Relay
aktiv war. Ein Request, der **während** einer relayed Antwort eintraf, lag
danach im Puffer — sein `POLLIN` war verbraucht. Vor dem Keep-Alive harmlos,
weil die Verbindung ohnehin geschlossen wurde. Danach ein hängender Tab.
`pump_requests()` wird jetzt an beiden Stellen aufgerufen.

## Hardware-Acceptance

| | vorher | nachher |
|---|---|---|
| nativ, req2 | 200 | **200** |
| **relayed Asset, req2** | **keine Antwort** | **200, 15 155 B** |
| relayed CGI | 200, 18 923 B | **200, 18 923 B**, schließt korrekt |

## Churn-Messung

**`TW` und `tcp_max_tw_buckets` sind verschiedene Größen** und dürfen nicht
verrechnet werden:

```
TW                 = kumulativer Zähler erzeugter TIME_WAIT-Übergänge
tcp_max_tw_buckets = Obergrenze GLEICHZEITIG vorhandener TIME_WAIT-Sockets
```

Aus „TW +515" folgt **nicht** „515 Slots gleichzeitig belegt". Eine frühere
Auswertung, die daraus eine 90-%-Verbesserung machte, ist zurückgezogen: das
Vorher-Fenster lief neun Minuten mit drei Browsern und sechs Medien-Consumern,
das Nachher-Fenster zwei Minuten auf frischem Boot mit einem Consumer.

Belastbar ist der Mitschnittvergleich, gleicher Reiz und gleiche Metrik:

| | vorher | nachher |
|---|---|---|
| TCP-SYNs zu `:80` | 45 | **4** |
| HTTP-Requests | 57 | 46 |
| **Verbindungen pro Request** | **0,79** | **0,09** |

**Vorbehalt:** vorher ein frischer Login, nachher ein F5 mit warmem Cache — die
Reize sind ähnlich, nicht identisch. Deshalb ist die normierte Zahl die
belastbare, nicht die absoluten Zählwerte.

## Der Upstream bleibt bewusst einfach

```
Browser → Machino    persistentes Keep-Alive
Machino → busybox    weiterhin ein Socket pro Request
```

Kein Verbindungspool. Das entfernt den größten browserseitigen Churn, ohne den
Relay komplexer zu machen als nötig.

## Abgrenzung zum Hardlock

Dieser AP behebt einen **nachgewiesenen HTTP-/TCP-Effizienzdefekt**. Er behebt
**nicht** den seltenen T40NN-Hardlock, und das darf auch nicht behauptet werden.

Zulässig ist genau:

> Der Relay verursachte unnötig hohen TCP-Churn. Dieser Churn wurde deutlich
> reduziert. Ob er zum Hardlock beiträgt, ist **nicht bewiesen**.

Zwölf Reproduktionsversuche haben den Hardlock an einem Tag nicht ausgelöst —
eine Abwesenheit nach diesem Fix würde deshalb nichts beweisen.

Ebenfalls ausdrücklich **nicht** getan: `tcp_max_tw_buckets` erhöhen. Ein
Kernel-Limit anzuheben, um unnötigen Churn zu verdecken, behandelt das Symptom.

## Regression-Gate

`tools/relay-gate.ps1` prüft automatisiert:

```
1  GET /login.html                                 -> 200
2  POST /login                                     -> 200 + Session
3  GET /cgi-bin/live.cgi                           -> 200, voller Body, KEIN 502
4  relayed Asset req1                              -> 200
5  relayed Asset req2 auf DEMSELBEN Socket         -> 200
6  CGI ohne Content-Length bleibt kompatibel       -> darf danach schließen
7  nativer Pfad req2 auf demselben Socket          -> 200
```

Punkt 5 und 7 lassen sich **nicht** als Hosttest schreiben:
`http_server.cpp` braucht POSIX-Sockets und ist nicht im Host-Testbuild. Das
Gate ist der einzige Ort, an dem dieses Verhalten geprüft wird.

Nicht automatisiert und weiterhin nur mit Browser prüfbar:

```
8  Live-WebUI lädt vollständig
9  kein hängen gebliebener HTTP-Client
10 kein Anstieg unbeantworteter Requests unter realer Last
```

Punkt 10 hat einen Präzedenzfall: unter hohem Churn blieb einmal
`/a/mj-luma.js` unbeantwortet, **ohne dass Machino eine Zeile dazu geloggt
hätte** — der Ausfall lag unterhalb der Anwendung. Das ist bis heute nicht
erklärt und gehört beobachtet.
