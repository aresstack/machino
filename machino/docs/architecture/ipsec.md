# IPsec/VPN (Feature 2, AP3): machinod-Seite

## Prozessgrenze

machinod linkt **kein** WeirdIKE (GPL-Trennung, PLATFORM.md im
weirdike-openipc-Repo). Die gesamte IKEv2/ESP-Wahrheit lebt in `weirdiked`;
machinod spricht mit ihm ausschliesslich ueber drei Vertraege:

1. **Konfig-Datei** `/etc/weirdike/weirdike.conf` (0600) — von machinod
   GENERIERT, vom Daemon gelesen. Jeder emittierte Key ist im Daemon-Parser
   (`wd_config.c`) verifiziert; der Parser lehnt unbekannte Keys hart ab,
   deshalb emittiert `to_weirdike_conf()` exakt die Schnittmenge.
2. **Init-Skript** `S99weirdike start|stop` — derselbe Weg wie beim Boot.
   Kein eigener fork des Daemons, keine zweite Startlogik.
3. **Control-Socket** `/var/run/weirdike.sock` — weirdikectl-Protokoll
   (Kommando, `SHUT_WR`, Antwort). `status` liefert `key=value`-Zeilen, die
   `parse_status()` 1:1 spiegelt. "Daemon laeuft" heisst: der Socket nimmt
   eine Verbindung AN — ein verwaister Socket-File (`ECONNREFUSED`) zaehlt
   korrekt als "laeuft nicht", es gibt kein pidfile-Raten.

Ein VPN-Fehler beruehrt das Video nie: der Daemon crasht fuer sich, machinod
meldet nur Status.

## Zwei Dateien, ein Secret

- `/etc/machino/ipsec.conf` — die typisierte machino-Wahrheit, **ohne PSK**.
- `/etc/weirdike/weirdike.conf` — generiert, traegt zusaetzlich die
  `psk`-Zeile.

Der PSK ist **write-only**: `PUT /api/v1/ipsec/config` nimmt `psk` an,
keine Antwort, kein GET, kein Status und keine Fehlermeldung gibt ihn je
zurueck (`pskSet` ist die einzige Auskunft). Ein Speichern ohne neuen PSK
traegt die vorhandene `psk`-Zeile unveraendert weiter. Schreibreihenfolge:
erst die Daemon-Datei, dann die machino-Datei — schlaegt Schritt 2 fehl,
behauptet kein persistierter Stand `enabled=true` ohne hinterlegtes Secret.
Beide Schreibvorgaenge sind atomar (tmp + fsync + rename, 0600).

## Geschlossene Algorithmen-Menge

AP3 erlaubt exakt die in AP2 gegen strongSwan bewiesene Suite:
`aes256cbc / sha256 / dh14` (IKE) und `aes256cbc / sha256` (ESP). Alles
andere wird **mit Namen** abgelehnt (`ikeEnc: ... abgelehnt: 'chacha20'`),
nie still ignoriert oder heruntergestuft.

## Zustaende und Fehlerklassen

`weirdike_state_str` → API (`GET /api/v1/ipsec/status`):

| Daemon                 | API-`state`        |
|------------------------|--------------------|
| (Daemon aus, enabled)  | `disconnected`     |
| (Daemon aus, disabled) | `disabled`         |
| `IDLE`, `CLOSED`       | `disconnected`     |
| `SA_INIT_SENT/DONE`, `AUTH_SENT` | `connecting` |
| `IKE_SA_ESTABLISHED`   | `ikeEstablished`   |
| `CHILD_SA_ESTABLISHED` | `childEstablished` |
| `FAILED` / unbekannt   | `failed`           |

`connected` ist bewusst **noch nicht vergeben**: das ist der AP4-Beweis
(Datenpfad ueber ipsec0), nicht die Control-Plane. Ein unbekannter
Daemon-Zustand faellt auf `failed`, nie auf etwas Gruenes.

Fehlerklassifikation NUR bei `failed`, aus WeirdIKEs eigener Diag
(`last_notify`): 24 → `authenticationFailed`, 14 → `noProposalChosen`,
38 → `tsUnacceptable`, 0 → `transportTimeout`, sonst `other`. weirdiked
bleibt nach einem Fehlschlag am Leben und serviert diesen Status (siehe
Daemon-Fix im weirdike-openipc-Repo) — der ctl-Socket ist die Quelle, nicht
eine machinod-Vermutung.

## Routen

- `GET  /api/v1/ipsec` — Config ohne Secret, plus `pskSet`.
- `PUT  /api/v1/ipsec/config` — Overlay-Semantik (fehlende Felder bleiben),
  unbekannte Felder → 400 mit Namen, `psk` optional.
- `POST /api/v1/ipsec/connect` / `disconnect` — idempotent; Vorbedingungen
  (enabled, Gateway, PSK) werden benannt, 409 bei Verstoss.
- `GET  /api/v1/ipsec/status` — der gespiegelte Daemon-Status.

Ohne verdrahteten Service (fremde Plattform, Hosttests ohne Wiring)
antworten alle Routen ehrlich 404.

## Abnahmepunkt "der AP2-Handshake laeuft ueber den neuen Service"

Woertlich genommen hiesse das: machinod startet weirdiked im Namespace-CI.
Das tut der Interop-Test bewusst NICHT — er faehrt weirdiked direkt, denn
die AP2-Abnahme ist die IKE-Interop, nicht machinods Prozess-Wiring. Der
Service-Anteil ist stattdessen exakt an der Prozessgrenze bewiesen:

- `to_weirdike_conf()` erzeugt dieselben Keys, die der Interop-Test in
  seine `weirdike.conf` schreibt (Parser-Schnittmenge, siehe oben);
- `parse_status()` ist gegen die realen weirdikectl-Zeilenformate getestet
  (inklusive der drei Notify-Klassen aus den Interop-Negativfaellen);
- Start/Stopp gehen ueber dasselbe `S99weirdike`, das auch der Boot nutzt.

Was zwischen diesen Vertraegen liegt (der Daemon selbst), deckt der
Namespace-CI ab. Der erste Ende-zu-Ende-Lauf machinod→weirdiked auf echter
Hardware ist Teil der AP5-Abnahme (Cellular-Underlay, mit Modem).

## AP5: Cellular/Underlay-Bindung

Der IPsec-Service waehlt das Underlay aus der BESTEHENDEN Uplink-Wahrheit
(`ConnectivityManager` via `ConnectivityIpsecUplinks`) — keine zweite
Failover-Policy, kein Modemwissen. WeirdIKE selbst kennt weiterhin nur
konkrete IP, UDP-Transport, Peer-Endpunkt (kein EC200A/ttyUSB/PPP/ECM/APN).

Ablauf von `connect()`:
1. **Underlay waehlen** (`underlay = auto|ethernet|wifi|cellular`): `auto` =
   der aktive Uplink der Machino-Policy; ein konkreter Wunsch = genau dieser
   Uplink, sonst **VERWEIGERT** (kein stiller Wechsel).
2. **DNS einmal** vor dem Tunnel (`resolve4`) — die aufgeloeste Peer-IP wird
   als Literal in die Daemon-Datei geschrieben, nie loest der Daemon nach
   Tunnelstart ueber `ipsec0` auf; Rekey nutzt dieselbe Adresse.
3. **Peer-Hostroute zuerst** (`/32` auf dem gewaehlten Underlay, `SIOCADDRT`).
   Sie gehoert dem Service und ueberlebt jede spaeter verhandelte Tunnelroute
   (§8-Invariante: IKE/ESP-Verkehr bleibt IMMER auf dem Underlay).
4. **Session-Bindung**: `bind_ip` (konkrete Underlay-IPv4) + `bind_dev`
   (`SO_BINDTODEVICE`) in die Daemon-Datei; die Sockets binden daran, ein
   Routing-Flip kann den Tunnel nicht mehr verschieben.
5. Daemon starten.

`tick()` (Hauptthread, nach der Uplink-Neubewertung): faellt das
SESSION-Interface/-Adresse weg, wird abgebaut und `failed/underlayLost`
gemeldet — **kein** MOBIKE-Vortaeuschen, kein Umhaengen der SA. Ein
Reconnect mit neuer Adresse ist ein NEUER `connect()`.

`disconnect()` entfernt die Peer-Route auch dann, wenn der Daemon-Stopp
scheiterte (eine Route zu einem toten Tunnel ist nur ein benanntes
Blackhole).

Status (`GET /api/v1/ipsec/status`) traegt zusaetzlich `requestedUnderlay`
(Config), `actualUnderlay`/`underlayInterface`/`underlayIpv4`/`peerIpv4`
(Session) und `ikeTransport`/`espTransport` (GEMESSEN vom Daemon:
`natT`/`natDetected` sind das Messergebnis der NAT-D, die Config-`natT`
erlaubt/verbietet die Funktion, ersetzt aber nie die Messung). Keine
Cellular-Secrets (APN, SIM-PIN) in diesem Status.

**Hardware-Gate (AP5 §12):** PENDING_PHYSICAL — Kamera + EC200A, `actualUnderlay=cellular`,
CHILD established, `natDetected` korrekt, echter Ping/TCP durchs VPN,
Ethernet bleibt parallel als Management. Keine Aenderung an APN-/Modemlogik.

## AP7: Lifecycle-Haertung (DPD, Rekey, Reconnect, Fehlerfaelle)

Arbeitsteilung: die **Protokoll**-Lebenszyklen (DPD, NAT-T-Keepalive,
Child-Rekey, IKE-SA-Rekey, Sequence-Exhaustion) macht der Daemon bzw. der
WeirdIKE-Core; der **Host**-Lebenszyklus (expliziter Runtime-Zustand,
Reconnect-Policy, geordneter Abbau, manualStop) lebt in machinods
IpsecService. machinod baut KEINE zweite IKE-Zustandsmaschine — die
Protokollwahrheit kommt aus `weirdike_state/get_diag/get_child_sa` (im
Daemon) und darueber aus dem ctl-Status.

**Runtime-Zustand** (`runtimeState` im API-Status), abgeleitet aus
Daemon-Status + Session: `disabled → idle → resolving → binding →
ikeConnecting → ikeEstablished → childEstablished → dataPlaneUp`, dazu
`rekeying` (Child-Generation gerade gewechselt), `disconnecting`, `failed`.
Ein Child mit mindestens einer installierten Route ist `dataPlaneUp`.

**Fail closed (§3/§8):** ein `FAILED`, das NACH dem Aufbau eintrifft (DPD
hat aufgegeben), reisst im Daemon den Datenpfad ab — Routen zurueckgezogen,
ipsec0 down, ESP-Keys zeroisiert — der Daemon lebt aber weiter und meldet
`FAILED`. ESP-Sequence-Exhaustion erzwingt einen Rekey (nie
Sequence-Reuse). Ein DPD-Verlust bleibt nie „UI Connected".

**Child-Rekey (§5):** neue Generation → neue esp_session auf den neuen
Keys, ipsec0/Routen bleiben, kein Traffic-Loch (add-before-remove im
Route-Manager). Im Namespace-CI unter Dauer-Ping bewiesen.

**Reconnect-Policy (§10):** nach einem WIEDERHERSTELLBAREN Verlust plant
`tick()` einen Reconnect mit Backoff (1: sofort, 2: 2 s, 3: 5 s, 4: 10 s,
danach 30 s) plus Jitter. Ein stabiler Connect (Child steht) setzt den
Zaehler zurueck. **Kein** Reconnect bei manualStop, ungueltiger Config,
fehlendem PSK. Ein neuer Uplink wird nur bei `underlay=auto` gewaehlt; bei
`underlay=cellular` nie heimlich gewechselt.

**Geordneter Disconnect (§11):** ctl `down` → der Daemon sendet ein
RFC-7296-DELETE (bounded — ein haengender Peer blockiert den Stopp nicht),
dann `S99weirdike stop`; die Peer-Route entfernt machinod auch dann, wenn
der Stopp scheitert.

**Crash/Restart (§12):** es wird NUR Konfiguration persistiert, nie SPIs,
Message-IDs, Keys oder Replay-Fenster. Der Daemon-TUN ist non-persistent —
ein `SIGKILL` laesst ipsec0 und seine Routen vom Kernel automatisch
verschwinden. machinods Peer-`/32` ist die einzige persistente Eigenroute;
`connect()` raeumt eine stale Peer-Route vor dem Neuanlegen ab.

### Ehrlich dokumentierte WeirdIKE-Grenzen

- **Simultaner IKE-SA-Rekey:** WeirdIKE loest eine gleichzeitige, beidseitig
  initiierte IKE-SA-Rekey-Kollision NICHT vollstaendig auf (keine
  Nonce-Collision-Resolution); der aktuelle Stand ist `TEMPORARY_FAILURE` +
  Retry. Status/Docs behaupten NICHT, das sei fertig.
- **Kein MOBIKE:** eine bestehende IKE-SA wird nicht auf ein anderes
  Interface umgehaengt; Underlay-Wechsel = sauberer Abbau + kompletter
  Neuaufbau.
