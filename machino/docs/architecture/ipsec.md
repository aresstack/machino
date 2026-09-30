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

## AP8: Native OpenIPC-WebUI-Integration

IPsec erscheint als native Seite in der bestehenden OpenIPC-WebUI, nicht als
zweite Webapp. Der etablierte serverseitige Weg (kein Client-Chrome-Hack,
keine OpenIPC-Datei angefasst):

- **Seite** `openipc/www/machino-ipsec.cgi` — ein haserl-CGI wie
  `machino-ai.cgi`/`machino-cellular.cgi`: `common/header/footer`-Includes
  (Head, Navbar, Theme, main.js kommen daraus), `#mch`-gescoptes CSS, IIFE-
  Script, Bootstrap/`mj-*`-Klassen. Sie spricht ausschliesslich die
  vorhandenen `/api/v1/ipsec*`-Routen (absolute Pfade). Installiert von
  `install.sh` nach `/var/www/cgi-bin/`, entfernt von `uninstall.sh`
  (`weirdike.conf` bleibt).
- **Menuepunkt** via `inject_machino_nav()` in die Services-Dropdown, direkt
  nach dem `wireguard.cgi`-Anker — dieselbe Anker-/Fail-closed-Logik wie
  DynDNS: fehlt der Anker (andere OpenIPC-Variante), gibt es eben keinen
  Menuepunkt (die Seite bleibt unter `/cgi-bin/` erreichbar), nie ein
  Abbruch; Duplikat-Guard `machino-ipsec.cgi`.
- **WireGuard bleibt unangetastet** — eigener Menuepunkt, eigene Seite,
  eigene API, keine gemeinsamen IDs/Config-Keys.

Karten: Status+Aktionen (Connect/Disconnect/Reconnect), Basis-Config
(Gateway/Port/Underlay/PSK), Identitaeten+Remote-Netze, Algorithmen (die
feste AP2-Suite, sichtbar als „kein stiller Downgrade"), Diagnose. Der PSK
ist ein `type=password`-Feld, das nur bei Eingabe sendet; die Anzeige ist
`stored`/`not set`, nie der Wert. **Configured / Negotiated / Installed**
werden in der Diagnose getrennt gezeigt (angeforderte remoteSubnet vs.
`remoteTs` aus dem Status vs. `routes[]`). Ein API-Fehler wird WORTWOERTLICH
gezeigt (AUTHENTICATION_FAILED, unsupported algorithm, cellular unavailable,
route conflict), nicht als generisches „Error". Bei `underlay=cellular` nur
ein Link auf die Cellular-Seite, keine zweite Modemconfig. 401 → zurueck auf
`/login.html?next=/cgi-bin/machino-ipsec.cgi`.

Hardware-Abnahme (Kamera + EC200A, Connect per UI, echter Remote-Ping/TCP,
sauberer Disconnect): PENDING_PHYSICAL.

## AP9: EAP-MSCHAPv2 + Linux-Trust-Store

Neben PSK kann der Tunnel per IKEv2/EAP-MSCHAPv2 aufgebaut werden: die Kamera
weist sich mit Benutzername/Passwort aus, der Server mit einem Zertifikat,
das gegen den gewaehlten Trust-Modus geprueft wird. Der EAP-Protokollteil und
die Zertifikatspruefung liegen im WeirdIKE-Core; AP9 ist nur die
Linux-Host-Integration (kein nachgebautes MSCHAPv2, keine zweite X.509-Kette).

**Auth-Modell:** `auth = psk | eap-mschapv2`. Bei EAP: `eapUser` (Identity,
kein Secret), `eapPassword` (write-only wie der PSK — nie GET, nie in einer
Fehlermeldung; Anzeige `eapPasswordSet`). Das noetige Credential haengt am
Modus: `connect()` verlangt PSK ODER (eapUser + eapPassword), nie beides,
kein stiller PSK-Fallback.

**Trust-Modi** (1:1 auf `weirdike_trust_mode_t` gemappt, UI-Labels lesbar):
`anchor-pem` (Own CA — `caPem` Pflicht), `host-store` (System CA store),
`host-store-plus-pem` (System store + `extraPem` als Chain-Material, KEIN
zusaetzlicher Anchor), `none` (No CA validation, nur explizit waehlbar).
`caPem`/`extraPem` sind oeffentlich, liegen aber als 0600-Dateien
(`/etc/weirdike/ca.pem`, `extra.pem`) in Machinos Config, referenziert per
`ca_pem_file`/`extra_pem_file` in der Daemon-Datei (der Parser bleibt eine
reine Pufferfunktion; der Daemon liest die PEM-Dateien in `load_config`).

**Linux-Host-Store:** der Daemon parst den ERSTEN vorhandenen Store dieses
RootFS (`/etc/ssl/certs/ca-certificates.crt`, `/etc/ssl/cert.pem`,
`/etc/ssl/certs/`) via mbedTLS in `weirdike_crypto_mbedtls_set_host_store`.
Ist keiner da, wird der Store NICHT gesetzt — und der Core verweigert einen
HOST_STORE-Modus beim Start (kein Rueckfall auf PEM-Anchor oder „keine
Pruefung"). machinod meldet `hostStoreAvailable`, die UI graut die
HOST_STORE-Optionen aus. Nichts wird heruntergeladen.

**Server-Identitaet vs. IKE-ID:** die Zertifikatskette (Trust-Modus) und die
IKE-`remoteId` sind getrennte Pruefungen mit getrennten Fehlern — eine
gueltige Kette mit falscher ID scheitert an der ID, nicht „irgendwie".

**CP bei EAP:** `request_cp = 1` — INTERNAL_IP4_ADDRESS/NETMASK/DNS/Subnets
werden uebernommen, aber hostseitig nur unter den AP6-Routing-/Selector-
Regeln (CP-Netz muss ganz in einem akzeptierten TSr liegen).

Hardware-Abnahme (Cellular-Underlay, echter EAP-Peer): PENDING_PHYSICAL.

## AP10: WeirdOS-Profilparitaet — ID-Typ, Tunnel-Adresse per CP, PFS, Autostart

Anlass: das WeirdOS-Profil einer realen Gegenstelle (IKEv2/PSK, Identitaet
als E-Mail-Adresse, „Tunnel-IP leer = automatisch vom Gateway", PFS ja,
„Automatisch verbinden beim Start"). Die Engine ist byteidentisch mit der in
WeirdOS (`vendor/weirdike` = `esp32-modem-host/src/weirdike`, Pin d3c5d1e);
alle vier Luecken lagen in machinods Verdrahtung. Kein Engine-Code geaendert.

**ID-Typ** (`localIdType`, `remoteIdType`: `fqdn|rfc822|ipv4|keyid`,
Vorgabe `fqdn` = Verhalten vor AP10). Explizit wie in WeirdOS („Senden als"
/ „Erwarten als"), keine Inhaltserkennung: ein Gateway prueft Typ UND Wert,
und `cam@intern` als FQDN ist fuer ein LANCOM eine andere Identitaet als
dieselben Bytes als RFC822. `ipv4` verlangt ein Literal (Validierung mit
Namen); der Daemon sendet dann die vier Bytes. Eine leere ID bleibt NONE
(eigene Quell-IP bzw. jede Server-ID akzeptieren) — unabhaengig vom Typ.
Daemon-Datei: `local_id_type`/`remote_id_type`, nur emittiert, wenn eine ID
gesetzt ist und der Typ nicht `fqdn` ist.

**Tunnel-Adresse vom Gateway** (IKEv2 Configuration Payload, RFC 7296 2.19).
`requestCp` ist ABGELEITET, nicht gespeichert — die WeirdOS-Regel „leer =
automatisch": PSK fordert CP genau dann an, wenn `localSubnet` leer ist; EAP
immer (wie bisher). Die API zeigt `requestCp` nur lesend; ein PUT damit wird
mit Namen abgelehnt. Der Daemon bekommt `request_cp = yes` explizit (Vorgabe
`no`, damit eine Daemon-Datei ohne die Zeile dasselbe bedeutet wie vor
AP10, auch fuer den Interop-CI). Die zugewiesene Adresse wird die
ipsec0-Adresse, sofern keine `local_subnet` konfiguriert ist — auch wenn
der Responder TSi nicht auf sie verengt hat (manche Gateways lassen es bei
any). Status: `tunnelIpv4`, `requestCp`, `cpAddress`.

**PFS** (`pfs`, Vorgabe `false`). Der Child-Rekey traegt dann eine neue
D-H-Gruppe (`pfs_group = 14`, die einzige erlaubte IKE-Gruppe). Aus bleibt
das im Namespace-CI unter Dauer-Ping bewiesene Verhalten; an entspricht der
LANCOM-/FRITZ!Box-Vorgabe. Wichtig fuer die Reihenfolge: WeirdIKEs
Child-Lebensdauer (55 min) ist kuerzer als die LANCOM-Vorgabe (8 h), also
initiiert Machinos Seite den ersten Rekey — ein Gateway, das PFS erzwingt,
lehnt ihn ohne KE ab. Status: `pfsGroup`.

**Autostart** (`autoConnect`, Vorgabe `true`). Der erste `tick()` nach dem
Start von machinod plant bei `enabled` + `autoConnect` einen Connect als
Reconnect-Versuch 1 (sofort). Scheitert er — typisch: das Mobilfunk-Underlay
ist Sekunden nach dem Boot noch nicht da — laeuft die AP7-Backoff-Kette
(2 s, 5 s, 10 s, dann 30 s) weiter, bis der Tunnel steht; `schedule_reconnect_`
verlangt weiterhin gueltige Config, Gateway und Credential. Ein bereits
laufender weirdiked (machinod-Neustart bei lebendem Daemon) wird nicht
angefasst. Ein manueller `disconnect` gilt wie bisher bis zum naechsten
`connect`; einen Boot ueberlebt er nicht (manualStop ist Prozesszustand).

Kompatibilitaet: eine unveraenderte Konfiguration erzeugt ausser den AP11-
Listenzeilen dieselbe Daemon-Datei; eine alte `ipsec.conf` ohne die Keys
liest sich mit den Vorgaben. Hosttests: `tests/test_ipsec.cpp` (`test_ap10_profile_parity`),
`tests/test_api.cpp` (`test_ap10_ipsec_api_fields`), `tests/test_ctl.cpp`,
`weirdike-openipc/tests/test_config.c` (`t_profile_fields`).

Hardware-Abnahme gegen die LANCOM-Gegenstelle (RFC822-ID, CP-Adresse auf
ipsec0, Rekey mit PFS nach 55 min, Tunnel nach Reboot ohne Klick):
PENDING_PHYSICAL.

## AP11: die LANCOM-Matrix — Algorithmen, Liveness, Rekey, Diagnose

Befund vor AP11: die Config liess genau EINE Suite zu, der Daemon reichte aber
gar keine Policy an die Engine — die bot ihre Vorgabe an (AES-256-CBC,
SHA-256 *und* SHA-512, DH14). Config und Kabel stimmten nicht ueberein, die
Behauptung „exakt die AP2-Suite auf dem Kabel" war fuer IKE falsch. Und die
Engine kann laengst mehr (ike_suite.c): AES-CBC 128/192/256, SHA-1/256/384/512
als PRF und Integritaet, DH 14/15/16/19/20/21/28/29/30/31.

**Katalog** (`core/net/ipsec_algos.cpp`): das komplette Raster des LANCOM
Advanced VPN Client in Anzeige-Reihenfolge — 13 D-H-Gruppen, 8 IKE-Chiffren,
5 Hashes, 9 ESP-Chiffren inkl. NULL, 6 ESP-Hashes — je Eintrag Host-Name (wie
WeirdOS), IANA-ID, Schluessellaenge, `implemented` (Spiegel des gepinnten
Engine-Builds) und `lancomDefault`. Die API liefert ihn als `algorithms` in
`GET /api/v1/ipsec`; WebUI-Kaestchen und `machinoctl ipsec algos` rendern
daraus, nichts ist dort hartkodiert. Nicht Implementiertes bleibt sichtbar
(grau bzw. `[-]`) und wird beim Speichern MIT NAMEN abgelehnt: kein Angebot,
das der Build nicht halten kann. Engine-Arbeit fuer AES-GCM, ChaCha20, 3DES,
MD5, NULL, DH2/5/32 gehoert in WeirdIKE, nicht in Machinos vendored Kopie.

**Allow-Listen** `ikeDh`, `ikeEnc`, `ikeHash`, `espEnc`, `espHash` (RFC 7296
3.3.1): ein Eintrag heisst „darf angeboten UND angenommen werden", keine
Prioritaet; die Engine normalisiert. Config = Kabel: `to_weirdike_conf`
schreibt alle fuenf Listen immer, der Daemon (`wd_config.c`: Namen → IANA,
alle fuenf oder keine) baut daraus `weirdike_ike_policy_t`/`child_policy_t`
und prueft sie beim Start mit `weirdike_policy_check()` — verweigert die
Engine, startet er nicht und benennt die Liste. Ohne Listen (Interop-CI)
gilt weiter die Engine-Vorgabe. PFS nutzt die kleinste erlaubte D-H-Gruppe
(`pfs_group = min(ikeDh)`), dieselbe, mit der die Engine das KE baut.

**Liveness/MTU**: `dpd` (an/aus; Peer-Proben werden immer beantwortet),
`dpdRetries` (0 = Vorgabe 5), `nattKeepaliveS` (0 = Vorgabe 20), 
`childLifetimeMb` (Byte-Lifetime, 0 = keine; Zeit UND Bytes gelten), `mtu`
(576..9000, Vorgabe 1400). Daemon-Zeilen nur, wenn nicht Vorgabe.

**Rekey auf Zuruf**: `POST /api/v1/ipsec/rekey` (Child) und `/rekey-ike`
(IKE-SA) → `IIpsecBackend::rekey` → `weirdikectl rekey|ikerekey` (ctl
`ikerekey` ist neu, `weirdike_rekey_ike`). Der PFS-Nachweis gegen ein
Gateway: `machinoctl ipsec rekey`, dann `ipsec log` zeigt „rekey sent (PFS)".

**Status**: `ikeSuite` (enc/prf/integ/dh) und `childSuite` (enc/integ) — die
AUSGEHANDELTEN Suiten aus der Engine-Diag, nicht die konfigurierten.

**Diagnose in der Konsole**: `ipsec fetch <ip[:port]>` und `ipsec log [n]`
fuehren `curl` und `logread` in der Shell des Bedienenden aus (machinoctl ist
ein eigener Prozess; machinod forkt weiter nicht). Nur IPv4-Literale und
Zahlen gelangen in die Kommandozeile. `ipsec ping` war hier zuerst ein
Shell-`ping` und ist seit AP12 der Test-Ping des Daemons (unten).

Nicht uebernommen aus WeirdOS: die Server-Rolle (dort selbst nur Konfig ohne
Runtime), L2TP und Zertifikats-Auth (dort beim Speichern abgelehnt), die
ESP32-spezifische AES-Backend-Wahl. Hosttests: `test_ap11_algorithm_grid`,
`test_ap11_ipsec_api_grid`, `test_ctl.cpp`, `weirdike-openipc` `t_policy_lists`.
Hardware-Abnahme (breitere Suite gegen das LANCOM, Rekey mit PFS, Suite im
Status): PENDING_PHYSICAL.

## AP12: Test-Ping durch den Tunnel

WeirdOS hat unter der VPN-Seite den „IPsec Test-Ping": eine Zieladresse,
ein Knopf, das Ergebnis als RTT, und ein Dropdown mit den letzten Zielen —
gespeichert auf dem Geraet (NVS), nicht im Browser. Das ist der Beweis, ob
ein Rechner im privaten LAN hinter dem Gateway wirklich erreichbar ist, und
er fehlte in Machino: `machinoctl ipsec ping` war ein Shell-`ping` ueber
generisches Routing, die WebUI hatte nichts.

**Messung im Daemon, ohne fork.** `IIpsecBackend::ping(PingRequest,
PingResult)`; Linux (`linux_ipsec_backend.cpp`): `SOCK_RAW`/`IPPROTO_ICMP`
(machinod ist root), `SO_BINDTODEVICE` auf das Tunnel-Interface aus dem
Daemon-Status, Echo-Request mit id=PID und Sequenz 1..n, Antwort nur, wenn
Typ 0 mit unserer id/Sequenz vom Ziel kommt; ein `DEST_UNREACH`/
`TIME_EXCEEDED`, das den Kopf unseres Echos traegt, wird als Grund gemeldet
(`unreachable (ICMP 3/1 von 10.66.0.1)`). Blockierend mit Timeout — im
API-Thread, nie im Medienpfad; `count` 1..5, `timeoutMs` 100..5000, zusammen
hoechstens 10 s. Kein `ping`-Prozess, denn machinod forkt bei lebendem IMP
nicht.

**Durch den Tunnel, nicht irgendwohin.** `IpsecService::ping` verweigert
mit Grund, bevor ein Paket entsteht: kein Daemon, Child steht nicht
(`Tunnel steht nicht (ikeEstablished)`), kein Interface, oder das Ziel liegt
in keiner **installierten** Tunnelroute (`10.77.0.1 liegt in keiner
installierten Tunnelroute (10.66.0.0/24)`). Ein Echo ueber den Uplink, das
dann „keine Antwort" hiesse, waere eine Luege ueber den Tunnel. „Keine
Antwort" ist dagegen ein Messergebnis: HTTP 200 mit `ok=false` und dem
Grund. Nur IPv4-Literale — DNS laeuft nicht durch den Tunnel.

**Historie auf dem Geraet.** `/etc/machino/ipsec-ping-targets` (neben
`ipsec.conf`, atomar, 0600): die letzten acht Ziele, neuestes zuerst, ohne
Doppelte; ein gueltiges Ziel wird gemerkt, sobald der Tunnel steht — auch
eines, das gerade nicht antwortet, denn genau das wiederholt man. Eine
kaputte Zeile faellt still raus, der Rest bleibt.

**API**: `POST /api/v1/ipsec/ping` `{target, count?, timeoutMs?}` →
`{ok, target, via, sent, received, rttMs, rttMinMs, rttMaxMs, error?,
targets}`; 400 MIT NAMEN fuer Eingaben, 409 mit Grund ohne Tunnel.
`GET /api/v1/ipsec/ping` → `{targets, maxCount, maxTimeoutMs}`;
`POST /api/v1/ipsec/ping/forget` `{target}`. **WebUI**: Karte „Tunnel test
ping" (`machino-ipsec.cgi`): Eingabe mit `<datalist>` aus der Historie,
Echos 1/3/5, Ergebniszeile, „Forget target". **Konsole**: `ipsec ping <ip>
[n]`, `ipsec ping` (Liste), `ipsec ping forget <ip>` — dieselbe Route, kein
Shell-`ping` mehr.

Hosttests: `test_ap12_test_ping` (Ablehnung mit Grund, Bindung an das
Interface, Historie: Reihenfolge, Doppelte, Deckel, Persistenz, Vergessen),
`test_ap12_ipsec_api_ping`, `test_ctl.cpp` (Fake-API und echte ApiService).
Der Raw-Socket selbst laeuft nur auf der Kamera: PENDING_PHYSICAL
(`docs/pending-physical.md`).

## Terminal: machinoctl

Dieselben Routen sind vom Terminal aus bedienbar — UART wie SSH — ueber
`machinoctl` (`docs/cli.md`): `ipsec setup` (gefuehrt), `ipsec set <k> <v>
...`, `ipsec psk` (verdeckt), `ipsec connect|disconnect|reconnect`, `ipsec`
(Kurzansicht mit Fehlerklasse). Es ist ein Client von `/api/v1/ipsec*` auf
127.0.0.1, kein zweiter Konfigurationspfad: die Feldtabelle der Konsole ist
exakt die `known[]`-Menge von `PUT /api/v1/ipsec/config`, und
`tests/test_ctl.cpp` beweist das gegen die echte ApiService. Der PSK bleibt
write-only — auch die Konsole gibt ihn nie aus.
