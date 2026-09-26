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
