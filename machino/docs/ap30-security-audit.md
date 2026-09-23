# AP30 — Security / Auth / Input Validation Audit

2026-09-23, 30-Minuten-Prüfung vor dem RC. Jeder Befund wurde **vorgeführt**,
bevor er behoben wurde — gegen den echten Parser oder gegen die Kamera.

## Ergebnis

```
CRITICAL   0
HIGH       2   beide behoben
NORMAL     1   behoben (Tiefenverteidigung)
NONE       der Rest
```

---

## HIGH-1 — Header-Smuggling durch das Relay · behoben

Eine Headerzeile wird an `\r\n` getrennt. Ein **bare LF** darin überlebt
deshalb als Teil des *Werts*, und `forward_request` schreibt Header wörtlich
wieder hinaus. Gegen den echten Parser gefahren:

```
GET /x HTTP/1.1\r\nHost: h\r\nX-Foo: a\nContent-Length: 99\r\n\r\n

  parse = Ok
  header [x-foo] = [a\nContent-Length: 99]

  was ans Upstream ging:
    GET /x HTTP/1.0
    x-foo: a
    Content-Length: 99        <-- eigener Header, von Machino nie verbucht
    Host: 127.0.0.1
```

Die vorhandenen Wächter konnten das nicht sehen: die Prüfung auf **doppeltes**
`Content-Length` zählt geparste Header, und dieser steckte in einem Wert.

**Behoben:** jedes Steuerzeichen in einer Feldzeile wird abgewiesen (RFC 7230
3.2 — `field-value` ist VCHAR / SP / HTAB), Tab bleibt erlaubt. Dasselbe für das
Request-Target, das `forward_request` ebenfalls wörtlich zurückschreibt.

**Auf dem laufenden Build noch offen:** derselbe Request kam dort mit 404
zurück (also geparst und weitergereicht), nicht mit 400. Der Fix ist in HEAD.

## HIGH-2 — Unbegrenzte Allokation im RTSP-Leser · behoben

```cpp
s.inbuf.append(buf, n);
while ((end = s.inbuf.find("\r\n\r\n")) != npos) { ... }   // leert NUR hier
```

Ein Client, der verbindet und Bytes sendet, **ohne je eine Leerzeile zu
schicken**, lässt `s.inbuf` unbegrenzt wachsen. Und zwar **vor jeder
Authentifizierung**: die steckt in `handle_request`, das nie erreicht wird.

Auf einer Kamera mit 42 MB RAM und einem OOM in der eigenen Geschichte reicht
dafür ein Socket. Die HTTP-Seite hat für genau das `MAX_IN`; RTSP hatte nichts.

**Behoben:** `MAX_RTSP_REQUEST = 8192`, dieselbe Obergrenze wie der HTTP-Kopf.
Ein echtes DESCRIBE oder SETUP mit `Authorization` und `Transport` ist wenige
hundert Bytes groß — abgewiesen wird nur, was nie eine Anfrage war.

## NORMAL — Path Traversal wurde abgewehrt, aber nicht von uns · behoben

Am Gerät gemessen:

```
GET /cgi-bin/../../../etc/shadow   -> 400   (Body: busybox' "Unsupported method")
GET /%2e%2e/%2e%2e/etc/shadow      -> 400
ohne Cookie                         -> 401 von Machino (das Gate läuft zuerst)
```

Also: nicht ausnutzbar — aber die Eigenschaft gehörte **busybox**, nicht uns.
Machinos eigene Routen sind exakte Stringvergleiche, dorthin kommt `..` ohnehin
nicht; alles andere wurde wörtlich weitergereicht. Das hätte still aufgehört zu
gelten, sobald etwas anderes hinter dem Relay sitzt.

**Behoben:** Punkt-Segmente im **Pfad** werden abgewiesen, literal und
`%2e`-kodiert. Die **Query** bleibt unangetastet — `/api/v1/reset?key=video.bitrate`
ist eine echte Anfrage.

---

## Geprüft und in Ordnung

| Eingang | Befund |
|---|---|
| **WS-Frames** | 64-Bit-Länge wird **vor** jeder Allokation gegen `max_payload` geprüft; FIN verlangt, RSV-Bits abgewiesen, Client-Maskierung verlangt (RFC 6455 5.1). Grenzen: 4096 Default, 32768 für das WebRTC-Angebot. |
| **WS-Handshake** | hinter dem Session-Gate; `/ws/upgrade`, `/ws/video`, `/ws/webrtc`, `/ws/logs` sind **nicht** in `is_public`. |
| **Content-Length** | nur Ziffern (kein `-1`, kein führendes Leerzeichen/Vorzeichen), gegen `max_body` begrenzt, **doppeltes CL → Bad** (RFC 7230 3.3.3). |
| **Transfer-Encoding** | vorhanden → **Bad**. Damit ist die ganze CL/TE-Smuggling-Klasse zu. |
| **Header-Grenzen** | `max_head` 8192, `max_headers` 32, `max_body` 8192, Request-Zeile ≤ 2048, `MAX_IN` 16 KiB pro Client. Überlauf trennt. |
| **obs-fold** | Fortsetzungszeile hat kein `:` → Bad. |
| **Relay** | hop-by-hop-Header werden verworfen, `Content-Length` aus dem tatsächlich geparsten Body **neu berechnet**, pro Anfrage eine frische Upstream-Verbindung. |
| **Sessions** | 128-Bit-Token aus dem System-CSPRNG (`secure_hex(16)`; vorher mt19937_64, ersetzt und begründet). Ablauf bei **jedem** `authed()` geprüft **und der Eintrag gelöscht** — keine stale session. Tabelle durch `evict()` begrenzt. Keine Zufallsquelle → Login wird abgelehnt statt ein ratbares Token vergeben. |
| **Auth live** | erfundenes Cookie → 401, kein Cookie → 401, RTSP `DESCRIBE` auf `/ch0` **und** `/ch1` → 401. Kein Bypass zwischen MAIN und SUB. |
| **RTSP-Auth-Reihenfolge** | geprüft **vor** jedem Pfad, der Demand nimmt oder einen Sink abonniert — ein unauthentifizierter Client kann den Sensor nicht starten. |
| **`system.unsafe`** | konsistent an allen vier Auth-Flächen: HTTP-Gate, Setup-Gate, RTSP-Auth, ONVIF. Keine Fläche, die es ignoriert, und keine, die es zu weit auslegt. |
| **SOAP/XML** | `MAX_REQUEST`-Grenze vor dem Parsen. |
| **Config PATCH** | jeder numerische Wert mit explizitem Bereich (`1..65535`, `0..600000`, `1..16` …), Typprüfung vor der Bereichsprüfung. |
| **Secrets in Logs** | keine. Die Treffer auf `pass|secret|token` sind Sensorverdrahtung (i2c-Adressen, GPIOs). Das Passwort erreicht auf dem Claim-Pfad nur eine Pipe — nie eine Kommandozeile, ein Log oder eine Umgebungsvariable. |
| **Time-CGI** | `?set=` wird von busybox validiert (`grep -qE '^[0-9]+$'`), Machino reicht nur durch. Der NTP-Zweig ist ein 504 ohne Seiteneffekt. |

## Was ein RC-Blocker wäre und keiner ist

Beide HIGH-Funde sind kleine, abgeschlossene Fixes mit Regressionstests — kein
Umbau, keine Vertragsänderung. Sie sind **nicht** deployt: der laufende Prozess
ist `c1edd92`. Solange das so ist, trägt die Kamera beide Schwächen. Das gehört
in `pending-physical.md` und in die Ablösereihenfolge.
