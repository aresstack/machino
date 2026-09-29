# machinoctl — Terminal-Bedienung (UART / SSH)

WeirdOS (`quectel-ec200a-eu/esp32-modem-host/serial_console.cpp`) laesst sich
komplett ueber eine serielle Kommandokonsole bedienen, weil ein ESP32 keine
Shell hat. Auf der Kamera ist das anders: UART und SSH (dropbear) landen in
busybox `ash`. Das Machino-Gegenstueck zur WeirdOS-Konsole ist deshalb **ein
Kommando in dieser Shell** — `machinoctl` — und nicht ein eigener
Zeilenleser am UART.

```
machinoctl ipsec                       # VPN-Kurzansicht
machinoctl ipsec setup                 # gefuehrte VPN-Einrichtung
machinoctl ipsec set gateway vpn.example.org remoteSubnet 192.168.178.0/24
machinoctl ipsec psk                   # verdeckte Eingabe, nie ausgegeben
machinoctl ipsec connect
machinoctl                             # Konsole: machino> help / exit
```

## Was es ist — und was nicht

- **Ein Modus des machino-Binaries.** `/usr/sbin/machinoctl` ist ein
  Einzeiler (`exec /usr/bin/machino --ctl "$@"`), `src/app/ctl/` die
  Implementierung. Kein zweites Binary auf dem 4,6-MB-Overlay; die
  JSON-Bibliothek ist dieselbe wie im Daemon.
- **Ein Client der lokalen API.** Jeder Befehl ist ein HTTP-Request an
  `127.0.0.1` (`/api/v1/...`). Kamera-lokale Aufrufer sind dort ohne
  Anmeldung vertrauenswuerdig (`SessionGate::is_local_peer`, wie bei
  Majestic). Es gibt also **keinen zweiten Konfigurationspfad**: was die
  Konsole speichert, laeuft durch dieselbe Validierung wie die WebUI
  (Ablehnung mit Namen, write-only-Secrets, atomare Persistenz).
- **Neben dem Daemon, nie statt seiner.** Ohne laufendes machinod (Majestic
  ist der Streamer) sagt die Konsole das und verweist auf
  `streamerctl status`. Sie schreibt nichts an machinod vorbei — auch nicht
  `/etc/machino/ipsec.conf`.
- **Kein Fork im Daemon.** machinoctl ist ein eigener, kurzlebiger Prozess in
  der Shell des Bedienenden. Die Regel „machinod forkt bei lebendem IMP
  nicht" bleibt unberuehrt.

Port: Machino als aktiver Streamer lauscht auf `80` (Front-Door), sonst auf
`api.port` (Vorgabe `8080`). machinoctl probiert `80`, dann `8080`;
`--port N` oder `MACHINOCTL_PORT` legen einen fest.

## Befehle

```
machinoctl [--port N] [--json] <befehl> [argumente]
machinoctl                                  interaktive Konsole ('exit' beendet)

help | ?                       Befehlsliste
status                         GET /api/v1/state (Lifecycle, Consumer, Medien)
telemetry                      GET /api/v1/telemetry
config [get] [<pfad>]          GET /api/v1/config, optional ein Teilbaum (video.0)
config set <pfad>=<wert> ...   PATCH /api/v1/config, z.B. video.0.fps=15 ai.enabled=true
ipsec                          Kurzansicht: Zustand, Gateway, Underlay, IDs, Netze, Routen, Fehler
ipsec status                   voller Status (/api/v1/ipsec/status)
ipsec config                   Konfiguration (Secrets nur als gesetzt/nicht gesetzt)
ipsec fields                   alle Schluessel mit Typ und erlaubten Werten
ipsec get <schluessel>         einen Wert lesen (Secrets: nur ob gesetzt)
ipsec set <k> <v> [<k> <v>..]  speichern (ein PUT, Pruefung wie in der WebUI)
ipsec enable | disable         Kurzform fuer set enabled true|false
ipsec psk [<wert>]             PSK; ohne Wert verdeckte Eingabe
ipsec eap-password [<wert>]    EAP-Passwort (write-only wie der PSK)
ipsec ca-pem <datei>|-         Trust-Anchor/CA als PEM aus Datei oder stdin
ipsec extra-pem <datei>|-      zusaetzliches Kettenmaterial
ipsec setup                    gefuehrte Einrichtung (fragt ab, speichert, verbindet)
ipsec connect | disconnect     wie die Web-Buttons
ipsec reconnect                trennen + neu aufbauen (nach einer Aenderung)
api get <pfad>                 beliebige Route lesen (Nicht-JSON kommt roh)
api put|post|patch|delete <pfad> [<json>|@datei|-]
```

Schluessel sind die der API (`gateway`, `remoteSubnet`, `natT`, ...), die
Schreibweise ist egal: `remote-subnet`, `remote_subnet` und `RemoteSubnet`
meinen dasselbe Feld. `ipsec fields` zeigt die vollstaendige Tabelle — sie ist
exakt die Menge, die `PUT /api/v1/ipsec/config` kennt, und ein Hosttest
(`tests/test_ctl.cpp`) beweist das gegen die echte `ApiService`.

Die vier Felder aus der WeirdOS-Profilparitaet (AP10 in
`architecture/ipsec.md`): `localIdType`/`remoteIdType`
(`fqdn|rfc822|ipv4|keyid`, „Senden als"/„Erwarten als"), `pfs` (PFS beim
Child-Rekey) und `autoConnect` (beim Start von machinod verbinden). Die
Tunnel-Adresse ist kein eigenes Feld: `localSubnet` leer = vom Gateway per
Configuration Payload, so wie WeirdOS' „leer = automatisch"; `ipsec` zeigt
dann `local=automatisch vom Gateway (CP)` und, sobald der Tunnel steht,
`tunnel=<adresse>`.

Werte: `true|false` (auch `ja|nein`, `on|off`, `1|0`), Ganzzahlen mit den
API-Grenzen, Listen mit Komma (`ikeEnc aes256cbc,aes256cbc`). Ein falscher
Wert wird **vor** dem Request mit Feldname abgelehnt; was die API dann noch
ablehnt (etwa `chacha20`), wird woertlich weitergegeben.

Rueckgabewerte: `0` ok, `1` API- oder Transportfehler, `2` Bedienfehler.
`--json` gibt die Antwort des Daemons unformatiert aus — fuer Skripte.

## VPN einrichten in vier Zeilen

```
machinoctl ipsec set gateway vpn.example.org remoteSubnet 192.168.178.0/24 \
    localId cam@example.org localIdType rfc822 underlay cellular pfs true
machinoctl ipsec psk            # fragt verdeckt
machinoctl ipsec enable         # autoConnect ist Vorgabe: nach einem Reboot kommt der Tunnel von selbst
machinoctl ipsec connect        # danach: machinoctl ipsec
```

Ohne `localSubnet` holt sich die Kamera ihre Tunnel-Adresse vom Gateway;
eine feste Adresse (`localSubnet 10.77.0.2/32`) schaltet das ab.

Oder gefuehrt: `machinoctl ipsec setup` fragt Gateway, Port, Underlay, IDs
samt Typ, Tunnel-Adresse (leer = vom Gateway), Remote-Netz, Auth (PSK oder
EAP-MSCHAPv2 mit Trust-Modus), NAT-T, PFS, `enabled` und `autoConnect` ab,
zeigt den heutigen Wert als Vorgabe (Enter uebernimmt, `-` leert), speichert
mit **einem** PUT und bietet an, sofort zu verbinden. Strg-D bricht ab, ohne
etwas zu speichern.

`ipsec` danach:

```
ipsec    : enabled=ja  autoConnect=ja  state=childEstablished  runtime=dataPlaneUp  daemon=laeuft
gateway  : vpn.example.org:500  auth=psk
underlay : cellular -> cellular (usb0)
ids      : local=cam@example.org (rfc822)  remote=(leer = jede)
nets     : local=automatisch vom Gateway (CP)  remote=192.168.178.0/24  ausgehandelt=192.168.178.0/24  tunnel=10.9.0.7  cp=10.9.0.7
routes   : 192.168.178.0/24 (tsr, ipsec0)
transport: natT=ja  natDetected=ja  ike=udp4500  esp=udp4500  pfs=ja (dh14)  if=ipsec0
secrets  : psk=gesetzt  eapPassword=nicht gesetzt  caPem=nicht gesetzt
traffic  : tx 12 Pakete/1024 B  rx 10 Pakete/900 B  uptime 120 s  child-gen 1  ike-gen 1
```

Bei `state=failed` steht die Fehlerklasse aus WeirdIKEs Diag in der zweiten
Zeile (`FEHLER   : authenticationFailed (notify 24)`), nicht ein generisches
„Error" — dieselbe Trennung wie in der WebUI.

## Secrets

PSK und EAP-Passwort sind write-only: keine Antwort, kein Status, keine
Fehlermeldung und keine Konsolenausgabe traegt sie je. `ipsec get psk`
antwortet `psk: write-only (gesetzt: ja)`. `ipsec psk` ohne Argument liest
mit abgeschaltetem Echo (termios), sobald stdin ein Terminal ist — am UART und
in SSH also immer. Mit Argument (`ipsec psk <wert>`) landet der Wert in der
Shell-History; das ist eine bewusste Entscheidung des Bedienenden, keine der
Konsole.

## Die Konsole (REPL)

`machinoctl` ohne Befehl oeffnet `machino> ` und liest Zeilen bis `exit`
oder Strg-D — das Aequivalent zum WeirdOS-UART-Monitor. Aus einer Pipe
(`echo "ipsec connect" | machinoctl`) gibt es weder Banner noch Prompt, nur
die Antworten; so laesst sich eine Befehlsfolge per Skript einspielen.

## Alles andere

`api get /api/v1/network`, `api post /api/v1/network/change '{"uplink":"cellular"}'`
und `config set` decken den Rest der API ab, ohne dass jede Route einen eigenen
Befehl braucht. Nur `/api/v1/events` (SSE, endet nie) lehnt die Konsole ab;
dafuer ist `curl -N` das richtige Werkzeug.

## Tests

`make -C machino test` — `tests/test_ctl.cpp`: Zeilenzerlegung, Feldtabelle
und Typisierung, Darstellung, HTTP-Zerlegung, jeder Befehl gegen einen
Fake-Client (Requests, Ausgaben, Rueckgabewerte, dass kein Secret je in einer
Ausgabe steht), der Setup-Dialog mit Skript-Eingabe, die REPL — und der
Vertragsbeweis gegen die echte `ApiService` mit Fake-IPsec-Backend.
`tests/test_openipc_install.sh` prueft, dass `install.sh` den Wrapper legt und
`uninstall.sh` ihn wieder entfernt.
