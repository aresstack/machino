# AP6–AP8 — Substream, RTSP-Auth, ONVIF

Batchlauf 2026-09-22. Commits `bc69895` (AP6), `83b1f59` (AP7), `eed8530` (AP8).
Hosttests 2350 → **2404**, CI inkl. MIPS grün.

---

## AP6 — Substream

### Was schon da war

Die Annahme des Pakets, der Substream fehle weitgehend, war falsch, und das
gehört korrigiert statt nachgebaut. Geprüft im Code:

* RTSP `/ch1` **und** `/stream=1` bilden beide auf `UNIT_SUB` ab
* `unit_for_stream()` bedient `?stream=1` für **MSE und WebRTC**
* der Dashboard-Streamreport kannte `video0`/`video1` bereits

### Die tatsächliche Lücke

Der Substream konnte **gemeldet**, aber nicht **geändert** werden:

```
/api/v1/config          gab nur video.0 aus
PATCH-Pfad              akzeptierte nur video.0
Settings-Schema         baute nur eine video0-Sektion
```

Einschaltbar war er damit **ausschließlich** durch Editieren von
`machino.conf` über SSH — und das ist kein Drop-in.

### Behoben

`config_json()` gibt `video.1` **immer** aus, inklusive `enabled` — eine
Sektion, die erst nach dem Aktivieren erscheint, kann man nicht zum Aktivieren
benutzen. Der PATCH-Pfad nimmt `enabled/fps/width/height/bitrate_kbps/gop`, je
mit Bereichsprüfung. Die Geometrie wird **nicht** vorbelegt: M8s Regel ist, dass
ein Substream eine ausdrückliche Größe braucht, weil eine geratene einen Stream
erzeugt, den niemand bestellt hat.

Das WebUI-Schema bekommt eine `video1`-Sektion in derselben Media-Gruppe wie
`video0`, also rendert die Stock-Seite sie ohne eigenen Patch.

**Ein Fehler vermieden statt ausgeliefert:** `video0` und `video1` landen im
übersetzten Patch unter demselben `video`-Objekt — eine Seite, die beide in
einem Formular sendet, hätte die zweite die erste überschreiben lassen. Die
Übersetzung mischt jetzt, und ein Test sendet beide **in beiden Reihenfolgen**,
weil „funktioniert in einer Richtung" kein Vertrag ist.

---

## AP7 — RTSP-Auth

### Der Vertrag, aus dem ausgeführten JS

`stream-urls.cgi` trägt zwei sich ausschließende Hinweise, und `main.js:1007`
wählt zwischen ihnen an **einem** Schlüssel:

```js
const unsafe = mjGet(cfg, 'system.unsafe');
const note = $(unsafe === true || unsafe === 'true' ? '#ep-unsafe' : '#ep-auth');
```

**Upstream kennt kein `rtsp.auth`.** Die Regel ist: RTSP authentifiziert als
`root` mit dem WebUI-Passwort, **außer** `system.unsafe` ist gesetzt.

### Die Abweichung

Machino lieferte `rtsp.auth` mit Default `false` aus. Die Seite sagte dem
Betreiber „These endpoints authenticate as user `root`", während die Kamera an
jeden streamte, der fragte. Eine Drop-in-Abweichung, die man der Oberfläche
**nicht ansehen** konnte — und das ist die Sorte, auf die es ankommt.

### Behoben

`RtspAuthConfig::enabled` ist jetzt `true`. `system.unsafe` hat weiterhin
Vorrang, und eine **unclaimed** Kamera fordert unabhängig davon immer eine
Credential — es gibt noch keine, also könnte keine richtig sein. MAIN und SUB
wurden schon gleich behandelt; das ist jetzt durch Tests festgenagelt statt
angenommen.

### Bewusst NICHT getan

**`rtsp.auth` ins Settings-Schema aufnehmen.** Upstream hat kein solches Feld.
Eines zu rendern hieße, auf die Stock-Seite ein Bedienelement zu setzen, das
majestic nie hatte — die WebUI würde erfahren, dass Machino existiert, und
genau das darf sie nicht. Der Zustand ist dort sichtbar, wo Upstream ihn
hinlegt: in `system.unsafe`.

Digest bleibt ebenfalls unangetastet: der Drop-in-Vertrag verlangt ihn nicht,
und ohne gespeicherten Klartext kann `/etc/shadow` kein HA1 liefern.

---

## AP8 — ONVIF

### Der Multicast-Defekt

```
[WRN] WSDISC multicast join failed (No such device) - unicast probes only
[WRN] WSDISC hello failed: Network unreachable
```

`INADDR_ANY` überlässt dem Kernel die Interface-Wahl über die Routing-Tabelle.
Diese Kamera hat nur das lokale `/24` und **kein** Default-Gateway, also
routet nichts `239.255.255.250` und `IP_ADD_MEMBERSHIP` liefert `ENODEV`.

Ein flaches LAN ohne Gateway ist der **Normalfall** für eine Kamera, und
Multicast ist genau der Weg, auf dem ein ONVIF-Client sie findet. „Unicast
probes only" heißt damit: unauffindbar für alles, was die Adresse nicht schon
kennt. Unicast ist eine Testhilfe, keine Discovery.

### Behoben

Der Join versucht zuerst `INADDR_ANY` — richtig, wo eine Route existiert — und
geht dann jedes **aktive, multicastfähige, nicht-Loopback** IPv4-Interface
durch, bis eines die Mitgliedschaft annimmt. `IP_MULTICAST_IF` wird auf
dieselbe Adresse gesetzt, damit das eigene Hello über das Interface hinausgeht,
auf dem gelauscht wird.

Die zweite Warnung hatte dieselbe Wurzel und brauchte einen eigenen Fix:
`announce()` erfragt die Quelladresse per `connect()` zur Gruppe, was aus
demselben Grund scheitert und `0.0.0.0` lieferte — das Hello warb also mit
`http://0.0.0.0:80/onvif/device_service`. Es fällt jetzt auf die Adresse
zurück, auf der die Mitgliedschaft genommen wurde.

Die Fehlermeldung sagt außerdem jetzt ausdrücklich, dass die Kamera **nicht
gefunden werden wird**, statt wie bisher nach einer kleinen Einschränkung zu
klingen.

### Bestätigt, soweit ohne Neustart möglich

```
eth0   flags=0x1003   = UP | BROADCAST | MULTICAST   -> der Join wird es finden
lo     flags=0x9      = UP | LOOPBACK                -> korrekt uebersprungen
```

Die Interface-Flags belegen, dass der neue Pfad auf dieser Kamera greift.

---

## `PENDING_PHYSICAL`

Alle drei Pakete sind gebaut, getestet, CI-grün und auf der Kamera
**installiert** — aber der laufende Prozess ist noch der alte, und ein Install
tauscht ihn nicht. Laufzeitprüfung offen:

```
AP6   /api/v1/config zeigt video.1
      PATCH video.1 {enabled,width,height} -> persistiert
      WebUI-Settings rendert die video1-Sektion
      Stream-Selektor SUB liefert 640x360 (Sichtpruefung)

AP7   anonym /ch0 und /ch1 -> 401 OHNE Config-Aenderung
      mit Credentials      -> 200 + PLAY
      system.unsafe=true   -> beide ohne Credential
      (braucht je einen Neustart, also ein eigenes Fenster)

AP8   Startlog "multicast joined via eth0 192.168.1.10"
      Multicast-Probe von 239.255.255.250 -> Antwort
      Hello mit XAddr != 0.0.0.0
      GetProfiles liefert MAIN und SUB
```

Ein **warmer** Daemon-Neustart ist auf dieser Kamera der dokumentierte
Hardlock-Auslöser; unbeaufsichtigt ausgelöst riskiert er eine Box, die tot
liegen bleibt. Deshalb markiert und nicht erzwungen.

## Nebenbefund

Der AP7-Default ändert das Verhalten für bestehende RTSP-Clients: ohne
Credentials bekommen sie ab dem nächsten Start `401`. Das ist beabsichtigt und
entspricht dem Stock-Vertrag — aber es ist eine spürbare Änderung und gehört
beim nächsten Hardwarefenster als Erstes geprüft.

---

## Korrektur nach Review — AP6 war in zwei Punkten falsch

### 1. Der PATCH hätte gar nichts getan

Mein Commit behauptete, `video.1.*` werde "persistiert". Das war falsch. Die
Anwendungsschleife in `api_service.cpp` verzweigt auf `c.key`; für `video.1.*`
gab es **keinen Zweig**, also fiel der Schlüssel durch mit einem
default-konstruierten `ApplyResult`:

```cpp
struct ApplyResult { bool ok = false; ApplyMode mode = ApplyMode::Unsupported; ... };
```

und weiter unten:

```cpp
for (const auto& c : changes) if (c.r.ok) kv.emplace_back(c.key, c.value);
```

Die Änderung wäre also **validiert, dann stillschweigend verworfen** worden —
nicht angewendet, **nicht gespeichert**, und der Aufrufer hätte `ok:false` mit
**leerer Meldung** bekommen. Schlimmer als gar kein Feature.

Ursache dahinter: `PipelineManager::update_sub_stream()` existiert, hat aber
**keinen Aufrufer**. Der Substream wird beim Daemonstart gebaut.

**Behoben:** ein ausdrücklicher Zweig, der als `DaemonRestart` speichert und
das auch sagt — `"persisted; the substream is built at daemon start"`. Damit
ist die Änderung dauerhaft **und** der Aufrufer weiß, dass sie erst beim
nächsten Start greift.

### 2. Die Schema-Sektion verstieß gegen die Regel dieser Datei

Ich hatte `video1` mit denselben `live`-Semantiken wie `video0` ins
Settings-Schema gelegt. `majestic_webui.cpp` hat dafür aber eine eigene, klar
begründete Regel:

```cpp
// Fields whose class is daemon_restart/boot_only are NOT exposed at all:
// their POST only persists, and the stock Apply (`killall -HUP majestic`)
// cannot restart the daemon to deliver them.
```

`video.1.*` ist genau diese Klasse. Die Stock-Seite hätte eine
Substream-Änderung als angewendet gemeldet, während nichts passiert war — ein
Feld, das die Seite setzen, aber nicht wahr machen kann, ist schlechter als
keines.

**Zurückgenommen.** Der Substream bleibt erreichbar über `/api/v1/config`, das
ihn ehrlich als "persistiert bis zum Neustart" meldet, und über
`machino.conf`. Der Test prüft jetzt die **Abwesenheit** aus der Sektion und
begründet sie.

### Was von AP6 übrig bleibt

| Anspruch | Stand |
|---|---|
| RTSP `/ch1`, `/stream=1`, MSE/WebRTC `?stream=1` | war schon da, verifiziert |
| `/api/v1/config` **liest** `video.1` | **neu, korrekt** |
| PATCH **persistiert** `video.1` | **neu, korrekt — als DaemonRestart deklariert** |
| PATCH **wendet live an** | **nein**, und sagt es jetzt |
| WebUI-Settings rendert `video1` | **nein**, und das ist richtig so |

Der Anspruch „ohne Sonderpatch im Stream-Selektor nutzbar" bleibt damit
erfüllt — der Selektor liest die Streamliste, nicht das Settings-Schema, und
die kannte `video1` schon vorher.

### Folge für die Acceptance-Matrix

Zeile **4.3** („RTSP ohne Credentials, Defaults → Stream spielt") ist durch
AP7 **ungültig geworden**: mit dem neuen Default antwortet die Kamera `401`.
Das ist die beabsichtigte Korrektur des Drop-in-Vertrags, aber die Zeile muss
beim nächsten Hardwarefenster umgeschrieben statt nachgetestet werden.

---

# AP9–AP11

Commits `c41c17c` (AP9), `995cd7b` (AP10). AP11 war bereits gebaut.
Hosttests 2372 → **2435**, CI inkl. MIPS grün.

## AP9 — Schema-Parität

Die Frage war nicht „was könnten wir exponieren", sondern **was erwartet
Upstream**. `mj-settings.js` nennt genau vier Sektionen:

```js
'image'  'sensor'  'video0'  'video1'
```

Daraus folgt beides:

* **`video1` gehört ins Schema** — es wegzulassen war die Abweichung, und mein
  vorheriger Revert hat eine Sektion versteckt, nach der die Stock-Seite fragt.
* **`rtsp.*` und `onvif.*` gehören NICHT hinein** — sie kommen in
  `mj-settings.js` nirgends vor. Sie zu rendern hieße, Bedienelemente auf die
  Stock-Seite zu setzen, die majestic nie hatte.

### Der Kompromiss, den es nicht braucht

`video1` ist jetzt als **`x-reload: "pipeline"`** exponiert, nicht als `live`.
Aus dem ausgeführten `changeCost()`:

```js
if (spec === 'none' || spec === 'live') return 'none';
...
return 'pipeline';
```

Die Seite sagt dem Betreiber dann „After Save, a reload restarts the video
streams" und bietet Apply an — und **Apply ist ein SIGHUP**. Also musste die
zweite Hälfte dazu: `main.cpp`s SIGHUP-Handler konfiguriert jetzt auch die
Substream-Unit. Ohne sie hätte die Seite ein Apply angeboten, das nichts
liefert.

Beide meiner früheren Positionen waren falsch: `live` hätte eine Änderung als
angewendet gemeldet, obwohl nichts passiert; das Weglassen versteckte eine
erwartete Sektion.

Der Schema-Contract-Test erzwang „nur `live`". Diese Invariante galt, solange
jedes exponierte Feld im POST angewendet wurde — sie gilt nicht mehr. Er
akzeptiert jetzt `pipeline` und beschränkt es auf `video1`, damit nichts
anderes es stillschweigend beansprucht.

## AP10 — `/api/v1/image`

Die Acceptance führte den 404 hier als „die eine sichtbar betroffene
Drop-in-Lücke". Der Vertrag stammt aus `mj-settings.js`, nicht aus einem
Entwurf:

```
POST /api/v1/image?brightness=128&contrast=100&hflip=1
```

Nur Blattnamen (`f.dot.split('.').pop()`), **alle** Live-Felder bei **jedem**
Push — „sending them together is what lets the backend apply combined settings
(mirror and flip need each other)" — und ausgewertet wird nur `r.ok`.
`sendBeacon` beim `pagehide` postet dieselbe Form, weshalb die Werte im Query
stehen und nicht im Body.

**Nichts wird persistiert.** Das ist die Regel, um die herum implementiert
wurde, kein Nachgedanke: ein Zug am Regler erzeugt einen Write pro
Zeigerbewegung, und die auf Flash zu schreiben wäre ein Regler, der die Kamera
verschleißt. Bewusst **nicht** `patch_config` mit einem Flag — zwei Pfade, die
sich keine Persistenzentscheidung teilen dürfen. Ein Test prüft, dass die
Config-Revision über einen Push hinweg unverändert bleibt.

Ein **nicht unterstützter** Regler wird übersprungen statt den Push abzulehnen:
die Seite sendet jedes gerenderte Live-Feld, und wegen eines fehlenden Knopfes
die anderen mitzureißen wäre falsch. Ein **fehlerhafter** wird abgelehnt, und
zwar der ganze Push — ein Tippfehler darf keine halbe Reglerreihe anwenden.

Dazu markiert das Schema die Bildfelder als `x-live`; ohne diese Hälfte gäbe es
den Endpunkt und niemand riefe ihn.

## AP11 — Setup / First-Run / Claiming

**War bereits gebaut**, und die Prüfung bestätigt es statt es nachzubauen.
`tests/test_setup.cpp` trägt **102 Zusicherungen**, darunter genau die Punkte
des Pakets:

| Anforderung | Stand |
|---|---|
| Shadow-Hash ist die einzige Wahrheit | `shadow_check()` und `claim_state()` lesen beide `/etc/shadow`, sonst nichts |
| kein zweiter Credential-Store | auf dem Gerät geprüft: keiner vorhanden |
| leerer Hash → Setup verfügbar | abgedeckt |
| Passwort setzen → claimed | abgedeckt, inkl. „Schreiben behauptet Erfolg und tat nichts" |
| danach normales Login | durch die **echte** `SessionGate` getestet |
| Restart-Persistenz | abgedeckt |
| `system.unsafe` konsistent | Setup-Gate (`http_server.cpp:364`), Session-Auth (420), ONVIF (348) und `RtspAuth::required()` hängen alle daran |
| Entropie | `/dev/urandom` unverändert, kein schwacher Fallback |

Die `SetupGate` selbst kennt `unsafe` bewusst nicht — die Entscheidung liegt
beim Aufrufer, was die Schichtung sauber hält.

### `PENDING_PHYSICAL` — und warum ich es nicht erzwungen habe

Der echte Factory-/Unclaimed-Test fehlt. Ihn hier zu fahren hieße, den
root-Hash in `/etc/shadow` zu leeren — und das bricht **sofort** die
Passwort-Authentifizierung von dropbear. Ich hätte in derselben Sekunde keinen
SSH-Zugang mehr, um den Zustand wiederherzustellen. Das ist exakt der
`NEEDS_HUMAN`-Fall, und unbeaufsichtigt auszulösen wäre unverantwortlich.

Er gehört an eine Kamera, die neu geflasht werden kann, oder ans Ende einer
Sitzung mit physischem Zugang.
