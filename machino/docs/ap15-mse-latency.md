# AP15 — MSE-Latenz und Rebuffering

Batchlauf 2026-09-22/23. Gemessen gegen die laufende Kamera (Prozess
`c1edd92`), Code committet als `8313bf3`.

---

## Die Frage, sauber gestellt

Der Befund war: „bei MSE ist der Delay wieder auf 1 sec angestiegen, bei WebRTC
nicht." Zwei Erklärungen sind damit vereinbar und schließen sich aus:

1. **Der Server puffert.** Die fMP4-Zeitachse läuft schneller als die Wanduhr,
   der Browserpuffer wächst stetig, die Latenz steigt monoton.
2. **Der Server ist sauber**, und die Sekunde ist das, was der Player selbst
   zulässt, bevor er an die Live-Kante springt.

Das ist messbar zu trennen, und zwar **ohne Browser**:

```
drift(t) = (tfdt(t) - tfdt(0)) / 90000  -  (ankunft(t) - ankunft(0))
```

`tfdt` ist die Dekodierzeit, die der Server in jedes Fragment schreibt. Läuft
sie der Wanduhr davon, wächst der Puffer im Browser — unabhängig davon, was der
Browser sonst tut. Läuft sie mit, kommt das Wachstum nicht von hier.

`tools/mse-drift.ps1` macht genau das: es spricht `/ws/video` als echter
WebSocket-Client, liest `tfdt`, `sequence`, Dauer und Sample-Flags an festen
Offsets aus jedem `moof` und stellt sie der Ankunftszeit gegenüber — für N
parallele Clients gleichzeitig.

### Ein Fehler im Messgerät zuerst

Der erste Lauf meldete −4150 ms Drift über 45 s, 18,2 statt 20 fps und
21 Ankunftslücken über 250 ms. Das war **das Messgerät**: der Client hängte
15 MB byteweise an eine `List[byte]`, wurde selbst zum langsamen Consumer, und
die Kamera warf für ihn Frames weg. Mit `MemoryStream` statt `List.Add` ist der
Effekt vollständig verschwunden. Ein Instrument, das sich selbst misst, ist
kein Befund — die Zahlen unten stammen alle aus der korrigierten Fassung.

---

## Messung

**Ein Client, 60 s**

```
frags  key  span_s  fps    media_s  drift_ms  drift_ppm  gap>250ms  maxgap_ms  MB
1200   32   59.9    20.03  59.9     +15       +246       0          76         21.7
```

**Drei Clients parallel, je 600 s** (648 MB insgesamt über die Kamera)

```
client  frags  key  span_s  fps    media_s  drift_ms  drift_ppm  gap>250ms  maxgap_ms  MB
1       12003  302  599.9   20.01  599.9    -16       -27        0          107        215.9
2       12003  302  599.9   20.01  599.9    -15       -25        1          347        215.9
3       12005  303  600.0   20.01  600.0     -7       -12        0          110        216.1
```

Das beantwortet die Frage.

* Über zehn Minuten unter Dreifachlast liegt die Drift bei **−27 bis −12 ppm**
  — und zwar **negativ**: die Zeitachse läuft der Wanduhr minimal *hinterher*,
  der Player holt also eher auf, als dass er puffert.
* `12003 / 599,9 s = 20,01 fps` bei 302 Keyframes: **kein Frameverlust**, keine
  Resyncs, keine Drosselung.
* Ankunftsmuster sauber: eine einzige Lücke über 250 ms in 36 000 Fragmenten.
* Die **+246 ppm** aus dem 60-s-Lauf sind 15 ms auf 60 s — ein einziges Frame.
  Über zehn Minuten löst sich das auf. Es war Rauschen, kein Trend.

**Befund: die Sekunde entsteht nicht auf der Kamera.** Der Server liefert eine
Zeitachse, die der Aufnahmeuhr folgt, in gleichmäßigem Takt, ohne Rückstau.

### Wo sie dann entsteht

Im Player, und zwar **absichtlich**. `www/a/preview.js` (Upstream, unverändert):

```js
const LIVE_EDGE = 1.0;
...
const lag = end - ct;
if (lagLearn) { lagFloor = lag; lagLearn = false; }
else if (lag < lagFloor) lagFloor = lag;
if (lag - lagFloor > LIVE_EDGE) seekLive(start, end);
```

Der Player springt erst an die Live-Kante, wenn der Rückstand **eine Sekunde
über** dem gelernten Boden liegt. Alles darunter lässt er stehen — MSE holt von
sich aus nie auf, jede Dekodier- oder Renderpause ist dauerhaft verloren, und
genau deshalb gibt es diesen Sprung überhaupt. „Wieder auf 1 s" ist der obere
Rand dieses Bandes, nicht unbegrenztes Wachstum. Was das Band füllt, sind
Browserereignisse — drei offene Browser mit mehreren Tabs, wie beim Befund —
nicht die Kamera.

WebRTC bleibt davon unberührt, weil dort die Wiedergabe an der Ankunft hängt
und nicht an einem Medienpuffer: es *kann* nicht stetig puffern.

---

## Was trotzdem geändert wurde

Drei Dinge, alle mit eigener Begründung, keines als Spekulation gegen die
Sekunde.

### 1. Die Zeitachse wird abgeleitet, nicht aufsummiert

Vorher:

```cpp
dur = (uint32_t)(d_us * 90000 / 1000000);   // abgeschnitten
...
c.ws_dts += dur;                            // aufsummiert
```

Zwanzig Abschneidungen pro Sekunde, immer in dieselbe Richtung. Bei einer
Bildrate, die 90 000 nicht glatt teilt (29,97 fps → 3003,03 Einheiten), sind
das rund 0,4 Einheiten pro Frame — über eine Stunde gut 0,5 s Drift, die
niemand je gemessen hat, weil auf dieser Kamera 20 fps zufällig glatt aufgeht.

Jetzt ist `dts` **abgeleitet**: `pts - origin - skew`. Eine durchgehende
Strecke kann damit gar nicht driften. `skew` ist die Zeit, die an einer
Diskontinuität **absichtlich entfernt** wird — das erhält das bisherige,
richtige Verhalten: eine fünf Sekunden lange Pause rückt die Zeitachse um *ein*
Frame vor, statt ein Loch zu schlagen, in dem der Playhead stehen bliebe.

Hosttests pinnen das: eine Stunde bei 29,97 fps (Abweichung ≤ 1 Tick, und
ausdrücklich der Nachweis, dass dieselbe Strecke aufsummiert *mehr als* 100
Ticks danebenläge), ein stehender und ein rückwärts laufender Zeitstempel
(Monotonie), eine 5-s-Lücke (genau ein Frame), eine 900-ms-Lücke (bleibt echt,
81 000 Ticks), das allererste Frame (Ursprung, Fallback-Dauer) und zehn Minuten
mit ±200 µs Jitter gegen die Aufnahmeuhr.

**Korrektur am eigenen Test.** Die erste Fassung dieses Tests trug die
Arithmetik als **Kopie** im Test und prüfte die. Das beweist die Kopie, nicht
den Code — und eine Kopie läuft von ihrem Original weg, ohne dass es jemand
merkt. Die Zeitachse ist deshalb jetzt `fmp4::Timeline`, eine eigene Einheit
neben dem Muxer, und der Server benutzt sie. Der Test ruft dieselbe Einheit.
Der Grund, warum sie überhaupt dort liegt und nicht inline im Server:
`http_server.cpp` ist nicht im Hosttest-Build (POSIX-Sockets, von
`tools/check-linux-only.sh` bewacht) — eine Arithmetik, die nur von einem Test
*nachgebaut* wird, ist von ihm nicht getestet.

### 2. `prft` — die Lücke, die niemand gesehen hat

`preview.js` hat `readPrft()`: eine `ProducerReferenceTimeBox` vor dem `moof`
nennt dem Player den Wanduhr-Zeitpunkt der Aufnahme, und daraus baut das
Stats-Panel seine Latenzanzeige:

```js
lagMs.push(Date.now() - wallMs);
```

Machino hat nie eine geschickt. Die Anzeige war damit **immer leer** — die
einzige Stelle, an der die WebUI die echte Ende-zu-Ende-Latenz zeigen kann.
Jetzt wird sie geschickt, aber **nur wenn die Kamera die Uhrzeit kennt**
(dieselbe Plausibilitätsgrenze wie AP12): eine Kamera ohne gestellte Uhr würde
sonst eine erfundene Latenz melden. Ohne RTC ist das kein Randfall
(`t40nn-uhr-ohne-rtc`).

Nachgeprüft im Review, wo die Zahl landet: `preview-stats.js` rendert daraus
„capture→arrival p50 … · p95 … (n frames)" — und korrigiert sie sogar um einen
bekannten Uhrenversatz („clock-corrected p50"). Es gibt im MSE-Pfad keine
zweite Quelle dafür; ohne `prft` war diese Zeile schlicht tot. Die
Uhr-Korrektur entschärft zusätzlich den Fall einer schief stehenden
Kamerauhr — die Plausibilitätsgrenze bleibt trotzdem, weil sie billiger ist
als eine falsche Zahl.

Der Test liest die Box exakt so zurück, wie Upstream sie liest — über absolute
Offsets, nicht durch Boxwalking, denn das ist der Leser, der zählt.

### 3. Zähler statt Vermutungen

`/api/v1/telemetry` meldet jetzt `ws_video_frames`, `ws_video_bytes`,
`ws_video_resyncs`, `ws_video_overruns`, `ws_video_out_peak`.

`ws_video_resyncs` zählt **Episoden, keine Ticks**: solange ein Socket
hinterherhängt, greift die Grenze bei jedem Schleifendurchlauf, und die zu
zählen hätte hundert Aussetzer gemeldet, wo der Zuschauer einen gesehen hat.
`ws_video_out_peak` ist der serverseitige Rückstau in Bytes — die eine Zahl,
die sagt, ob ein wachsender Browserpuffer hier angefangen hat oder dort.

---

## Was geprüft und **nicht** gebaut wurde

| Idee | Warum nicht |
|---|---|
| `ws_out_cap` kleiner (heute 512 KiB, Pause bei 256 KiB ≈ 0,7 s bei 2,9 Mbit/s) | Der Rückstau tritt in der Messung **nicht auf** (0 Lücken > 250 ms in 36 000 Fragmenten). Kleiner heißt: ein kurzer TCP-Hänger wird zum sichtbaren Resync statt zu 0,3 s Aufholen. Ohne Evidenz kein Tausch. |
| Bei Überlast den Sendepuffer verwerfen und am nächsten Keyframe neu ansetzen | Der Kopf von `c.out` kann **mitten in einem WebSocket-Frame** stehen (Teilschreibvorgang). Sicher wäre nur das Abschneiden am Ende, und das verwirft die *neuesten* Frames — das Gegenteil der Echtzeitregel. Bräuchte eine Rahmengrenzen-Buchführung im gemeinsamen Ausgabepuffer. Kein Nutzen ohne gemessenen Rückstau. |
| Am WebRTC-Pfad etwas ändern | AP15 verbietet es ausdrücklich, und WebRTC ist hardwareabgenommen. Nicht angefasst. |
| SPS-Rewrite anfassen | `sps_with_bitstream_restriction()` (`num_reorder_frames=0`, `max_dec_frame_buffering=1`) steht unverändert im MSE-Pfad, RTSP bleibt unberührt. Ausdrücklich beibehalten. |

---

## Stand gegen „Fertig wenn"

> MSE bleibt über längere Laufzeit bounded und kehrt nicht durch stetiges
> Puffern in Sekundenlatenz zurück.

* **Serverseitig belegt:** 3 Clients × 10 min, 648 MB, Drift −27…−12 ppm
  (negativ), kein Frameverlust, ein Ausreißer über 250 ms in 36 000 Fragmenten.
  Stetiges Puffern findet auf der Kamera nicht statt.
* **Browserseitig** ist die Latenz durch `lagFloor + LIVE_EDGE` des
  Upstream-Players begrenzt, nicht unbegrenzt. Der beobachtete „1 s" ist der
  obere Rand dieses Bandes.
* **Offen und `PENDING_PHYSICAL`:** die Sichtprüfung im Browser mit dem
  **neuen** Build — `prft` in der Stats-Anzeige, `ws_video_*` in der Telemetrie
  und eine Wiederholung der Driftmessung gegen `8313bf3`. Alle drei brauchen
  einen Daemon-Neustart, und ein Warmstart ist der dokumentierte
  Hardlock-Auslöser. Die Messungen oben stammen aus dem **laufenden** Build
  `c1edd92`; der neue Code ist durch Hosttests und CI gedeckt, nicht durch
  Hardware.

CI grün (Hosttests 2501/0, MIPS-Crossbuild).

---

## Nachtrag aus dem Review: die Ausgabe durch einen echten Decoder

Die ehrliche Lücke blieb: `http_server.cpp` ist nicht im Hosttest-Build, die
Kamera lässt sich nicht neu starten, also war der Muxer durch Unit-Tests und
einen Crossbuild gedeckt — und durch nichts, das seine Ausgabe je **dekodiert**
hätte. `tools/mse-replay/` schließt das, ohne den laufenden Daemon anzufassen:
echte Frames vom Live-Feed, neu gemuxt durch den **ausgelieferten** Code, und
in headless Chrome durch MSE abgespielt — auf demselben Weg wie `preview.js`,
einschließlich dessen `readPrft()`.

```
                         durchgehend     mit 2-s-Aussetzer
Fragmente                150             110
prft entfernt aus        150             110
Buffered Ranges          1               1
buffered                 0.000 .. 7.497  0.000 .. 5.498
videoWidth x Height      1920 x 1080     1920 x 1080
readyState               4               4
error                    keiner          keiner
```

150 Frames bei 20 fps sind 7,5 s, und die Range endet bei 7,497 — die
abgeleitete Zeitachse ist **richtig skaliert**, nicht bloß monoton. Und in
beiden Läufen **eine einzige** Buffered Range: der zweite ist der, auf den es
ankam, denn genau dort absorbiert `Timeline` einen langen Aussetzer in den
Skew, statt ein Loch zu lassen, in dem der Playhead stehen bliebe. Der Browser
bestätigt, dass keins da ist.

`totalVideoFrames` meldet in beiden Läufen 2. Das ist ein Headless-Artefakt —
ohne Compositor und mit virtueller Zeit gibt es nichts zu präsentieren.
Dekodiert **wurde** nachweislich: `videoWidth` wird erst nach einem dekodierten
Frame gesetzt, `readyState` ist 4, `currentTime` läuft. Als Durchsatzmessung
taugt die Zahl nicht und wird hier nicht als eine geführt.

Was das **nicht** beweist: den Servierpfad der Kamera selbst — Sockets,
Backpressure, Drop-until-Key, Demand-Handle. Das bleibt `PENDING_PHYSICAL`.
Geklärt ist, dass die Bytes, die der Muxer erzeugt, Bytes sind, die ein Browser
annimmt.
