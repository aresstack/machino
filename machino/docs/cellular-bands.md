# Mobilfunk: Frequenzen, Nachbarzellen, PIN-Sperre, Neustart

Abgleich der Cellular-Seite (`machino-cellular.cgi`) mit dem WeirdOS-ESP32-Stand
(`Miguel0888/quectel-ec200a-eu`, `esp32-modem-host/`: WAN > Modem und
Diagnose > Modem) vom 2026-09-29. Die dort vorhandenen, in Machino bisher
fehlenden Karten sind portiert; was bewusst nicht portiert wurde, steht am
Ende mit Begruendung.

## Was fehlte, was jetzt da ist

| ESP32 (WeirdOS)                               | Machino vorher | Machino jetzt |
|---|---|---|
| WAN > Modem > **Frequenzen**: Netzmodus (auto / nur LTE / nur 2G) | fehlte | Karte **Frequencies**, `netMode` |
| Bandprofil (auto / 1-2 GHz / <1 GHz / benutzerdefiniert) + Bandliste mit Frequenz | fehlte | dieselben Profile, Bandraster aus `bands.supported` |
| **SINR-Bandscan** (jedes Band sperren, Median messen, bestes waehlen) | fehlte | Karte **Best band (SINR scan)**; Lock als eigener Schritt (s.u.) |
| Diagnose > Modem > **Nachbarzellen** (`AT+QENG="neighbourcell"`) | fehlte | Karte **Neighbour cells**, geparst + Rohtext |
| Zugangsdaten > **SIM-PIN-Sperre auf der Karte** (Status / aktivieren / deaktivieren / aendern, Restversuche) | fehlte | Abschnitt **PIN lock on the card** in der SIM-Karte |
| Zugangsdaten > **Einwahlnummer** (PPP) | Konfig vorhanden (`dial`), kein Feld | Feld in der Data-link-Karte, sichtbar bei PPP |
| Verbindung > **Modem neu starten** (`AT+CFUN=1,1`) | fehlte | Knopf in der Diagnose-Karte |
| Funkwerte: MCC/MNC, RSSI | fehlte in der Anzeige | Zeilen in der Diagnose-Karte |

Vorher schon abgedeckt: Datenschicht ECM/PPP, NIC-/Routing-Modus, APN-Presets,
Zugangsdaten, PDP-Typ, Auth, Autoconnect, SIM-PIN (einmalig je Boot), Status,
Funkwerte der Serving-Zelle, Internet-Check, Uplink-Wahl (Uplinks-Seite).

## Wie die Bandwahl in Machino funktioniert

Quelle: `modemApplyBands()` / `modemLockBandMask()` / `bandScanTask()` in
`ec200a_modem.cpp`. Die EC200A-Eigenheiten sind uebernommen und in
`core/cellular/band_plan.hpp` festgehalten:

    AT+QCFG="band",d3,<lte-hex>          bare-Hex ohne 0x, ZWEI Parameter, "d3" = GSM behalten
    AT+QCFG="nwscanmode",<0|3|1>,1       0 auto, 3 nur LTE, 1 nur GSM (kein 3G auf dem EC200A)
    AT+CFUN=0 ... AT+CFUN=1              RF-Zyklus, keine USB-Re-Enumeration

Unterschiede zum ESP32, alle aus der Plattform bzw. den Regeln dieser Kamera:

1. **Ein Sprecher auf dem AT-Port.** Der HTTP-Thread schickt nichts; er
   hinterlegt eine Absicht, `CellularUplink::tick()` fuehrt sie im
   Hauptthread aus (`core/cellular/radio_tuner`, `core/cellular/modem_actions`).
2. **Abgleich statt Knopf.** Bandmaske und Netzmodus sind im Modem
   persistent. Machino schreibt bei einer Konfigurationsaenderung genau
   einmal, liest danach zurueck und meldet Abweichungen als Fehler ohne
   zweiten Versuch. Beim Start wird nur gelesen; ein lesbar abweichendes
   Modem (vorher woanders gelockt) wird einmal in Linie gebracht, eine
   nicht lesbare Rueckmeldung fuehrt zu **keinem** Schreibvorgang
   (`sync = unverified`). Bei Profil "auto" gilt eine Werksmaske, die
   unsere Baender enthaelt, als passend -- sie wird nie beschnitten.
3. **Bestaetigungsfenster.** Ein Band-Lock kann die Kamera unerreichbar
   machen. Die Aenderung laeuft deshalb wie jede Netzaenderung ueber
   `PATCH /api/v1/network/cellular` mit Rollback; das Rollback schreibt die
   alten Werte ins Modem zurueck.
4. **Der Scan lockt nicht selbst.** Er misst (Median aus vier
   QENG-Lesungen je Band, Messwert nur wenn QENG wirklich das gesperrte
   Band meldet), stellt die konfigurierte Maske wieder her und meldet das
   beste Band. Der Lock ist der Knopf "Lock band Bx" -- ein normaler PATCH
   mit Bestaetigungsfenster. Waehrend des Scans ist der Datenlink
   heruntergenommen (die Kamera ist ueber Mobilfunk so lange nicht
   erreichbar; die Seite sagt das vor dem Start).
5. **Einmal-Aktionen** (Nachbarzellen, PIN-Sperre, Neustart) werden genau
   einmal ausgefuehrt und verbraucht -- auch bei Fehlschlag, auch wenn das
   Modem gerade nicht antwortet. Eine PIN-Aktion, die Minuten spaeter von
   selbst losgeht, gibt es nicht.
6. **PIN-Persistenz.** Nimmt die Karte "Sperre an" oder "PIN aendern" an,
   wird die PIN sofort in die Konfigurationsdatei geschrieben (Haken
   `CellularUplink::set_persist`, in `main.cpp` an den Store gebunden);
   "Sperre aus" loescht sie. Ohne das staende beim naechsten Start eine
   gesperrte Karte da. Waehrend einer ausstehenden Netzaenderung sind
   PIN-Aktionen abgelehnt (409).

## API

Alles unter `/api/v1/network/cellular`:

| Route | Methode | Bedeutung |
|---|---|---|
| `/api/v1/network/cellular` | GET | wie bisher, plus `bands`, `neighbourCells`, `simLock`; `radio.mcc`/`radio.mnc` |
| `/api/v1/network/cellular` | PATCH | zusaetzlich `netMode` (`auto|lte|gsm`), `bandProfile` (`auto|mid|low|custom`), `bands` (Liste von Bandnummern) -- gestaged wie alles andere |
| `/api/v1/network/cellular/bandscan` | POST | Scan starten; 202, 409 mit Grund |
| `/api/v1/network/cellular/neighbours` | POST | Nachbarzellen einmal abfragen; Ergebnis in `neighbourCells` |
| `/api/v1/network/cellular/restart` | POST | `AT+CFUN=1,1` einmal |
| `/api/v1/network/cellular/sim/lock` | POST | `{"action":"status|enable|disable|change","pin":"..","newPin":".."}`; Ergebnis in `simLock`, nie die PIN |

`bands` im GET:

```json
"bands": {
  "supported": [{"band":1,"mhz":2100,"tdd":false}, ...],
  "netMode": "auto", "bandProfile": "custom", "bands": [3,20], "lteMaskHex": "80004",
  "sync": "in-sync", "detail": "the modem matches the configuration", "writes": 0,
  "modem": {"lteMaskHex": "80004", "bands": [3,20], "nwscanmode": 0, "netMode": "auto"},
  "scan": {"state": "done", "detail": "best band: B20 (...)", "current": null, "best": 20,
           "rows": [{"band":20,"mhz":800,"ok":true,"samples":4,"sinrDb":9,"rsrpDbm":-95,"rsrqDb":-11}]}
}
```

Neue Konfigurationsschluessel: `cellular.net_mode`, `cellular.band_profile`,
`cellular.band_mask` (Hex wie im Modem). Eine Datei ohne sie bedeutet
auto/auto.

## Offen an der Hardware

Das Format der Rueckmeldung `AT+QCFG="band"` ist im Referenzprojekt nicht
aufgezeichnet (dort wird nie zurueckgelesen). Der Parser nimmt
`+QCFG: "band",<gsm>,<lte>[,<tds>]` mit Hex mit oder ohne `0x`. Meldet die
Firmware etwas anderes, zeigt die Karte `sync = unverified` mit dem Hinweis
"band report not readable"; geschrieben wird trotzdem, wenn der Benutzer
etwas aendert. Dann bitte die Rohantwort melden.

## Bewusst nicht portiert

| ESP32 | Grund |
|---|---|
| Freie AT-Konsole (Entwickler-Tab) | Regel dieser Seite seit AP-M5: ein falsches Kommando stellt die USB-Komposition dauerhaft um. Bleibt draussen. |
| Speedtest (`modemSpeedtest*`) | braucht einen Gegenserver und mehrere Worker; auf der Kamera misst der RTSP-/WebRTC-Verkehr selbst. Steht im Reuse-Map als DEFER. |
| `modem_clock` (Netzzeit per `AT+CCLK?`) | die Kamera hat NTP ueber den Uplink; eine zweite Uhrquelle waere ein zweiter Besitzer der Systemzeit. |
| Verbinden/Trennen-Knoepfe | den Uplink waehlt die Policy (Uplinks-Seite); ein manuelles Trennen neben der Policy waere ein zweiter Schalter fuer dieselbe Sache. |
| USB-Port-Zuordnung, "USB-Modem aktiv" | ist die USB-Rolle auf der USB-Seite (ein Port, ein Selektor). |
| IPv6- und DNS-Tab | reine Hinweistexte auf dem ESP32; DNS entscheidet `route_plan` zentral. |
| Pipeline-/Durchsatz-Tab | ESP-spezifische USB-Bulk-Pipeline; auf Linux gibt es sie nicht. |
