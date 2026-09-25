# Die WebUI der Kamera: was da wirklich liegt

Gemessen am 2026-09-25 auf der T40NN (OpenIPC *lite*, Kamera 1), nicht aus der
Dokumentation abgeschrieben. Jede Zeile hier ist mit `curl` gegen die laufende
Kamera geholt worden. Wer das anzweifelt, kann jede Angabe in einer Minute
nachprüfen — die Kommandos stehen dabei.

Warum das Dokument existiert: Machino liefert eigene Seiten unter `/machino/*`
aus, und die sollen aussehen und sich verhalten wie der Rest der Kamera. Ein
erster Anlauf hat dafür eine eigene Menüleiste nachgebaut — Fremdkörper, und
unnötig, weil alles Nötige auf derselben Herkunft bereitliegt. Diese Fakten
sind die Grundlage dafür, es nicht noch einmal falsch zu machen.

## Der Kern

**Bootstrap 5.3.3.** Aus dem Kopf der Datei selbst:

```console
$ curl -su root:PW http://<kamera>/a/bootstrap.min.css | head -c 120
@charset "UTF-8";/*!
 * Bootstrap  v5.3.3 (https://getbootstrap.com/)
 * Copyright 2011-2024 The Bootstrap Authors
```

**Es gibt kein Bootstrap-JavaScript.** Das ist der Punkt, der am meisten
Aufwand spart oder kostet, je nachdem ob man ihn kennt:

```console
$ for f in bootstrap.bundle.min.js bootstrap.min.js bootstrap.js; do
    curl -so /dev/null -w "$f %{http_code}\n" -u root:PW http://<kamera>/a/$f; done
bootstrap.bundle.min.js 404
bootstrap.min.js 404
bootstrap.js 404
```

Das Verhalten steckt stattdessen in `/a/main.js` (≈15 kB, eigener Code). Es
setzt `window.bootstrap = {Modal: …}` als Attrappe und implementiert selbst:

| Funktion | in main.js |
|---|---|
| `data-bs-toggle="dropdown"` | ja, inklusive Escape/Pfeiltasten |
| `data-bs-toggle="collapse"` | ja |
| `data-bs-dismiss` (alert, modal) | ja |
| Modal | ja, über `<dialog>` |
| Tooltip, Popover, Offcanvas, Tab | **nein** |

Wer also ein Dropdown auf einer Machino-Seite will, lädt `/a/main.js` — und wer
ein Tooltip will, bekommt keins, egal was die Bootstrap-Doku sagt.

## Die Dateien

Alle unter `/a/`, alle auf **derselben Herkunft** wie Machinos eigene Seiten.
Machino hört auf Port 80 und reicht durch, was es nicht selbst bedient; ein
`<link href="/a/…">` aus einer `/machino/…`-Seite funktioniert deshalb ohne
CORS und ohne Sonderbehandlung.

| Pfad | Größe | was |
|---|---|---|
| `/a/bootstrap.min.css` | 61 868 B | Bootstrap 5.3.3, MIT |
| `/a/bootstrap.override.css` | 99 337 B | Farben, Dark/Light, Schriften, eigene Komponenten |
| `/a/main.js` | 14 923 B | das gesamte UI-Verhalten |
| `/a/logo.svg` | — | Markenzeichen in der Navbar |

`bootstrap.override.css` ist **größer als Bootstrap selbst**. Dort stehen die
Schriften (`Montserrat`, `PT Mono`, nachgeladen von Google Fonts) und die
`mj-*`-Komponenten der Oberfläche. Wer sich an das Aussehen der Kamera hängen
will, braucht beide Dateien, nicht nur die erste.

### Bootstrap ist reduziert

62 kB statt der üblichen ~230 kB: das ist ein beschnittener Build. Stichprobe:

| vorhanden | fehlt |
|---|---|
| accordion, toast, dropdown-menu, navbar, card, table, badge, form-control, alert, progress, modal, spinner-border, btn-*, tooltip, popover | **carousel**, **offcanvas** |

Dass `tooltip` und `popover` im CSS stehen, heißt nicht, dass sie
funktionieren — dafür fehlt das JavaScript (siehe oben). Vor der Benutzung
einer Klasse also erst nachsehen:

```console
$ curl -su root:PW http://<kamera>/a/bootstrap.min.css | grep -c '\.offcanvas'
0
```

## Der Seitenaufbau

```html
<html lang="en" data-bs-theme="dark">
<head>
  <script>/* auto -> dark|light nach prefers-color-scheme */</script>
  <link rel="stylesheet" href="/a/bootstrap.min.css">
  <link rel="stylesheet" href="/a/bootstrap.override.css">
  <script src="/a/main.js"></script>
</head>
<body id="page-live" class="lite">
  <nav class="navbar navbar-expand-lg bg-body-tertiary"> … </nav>
  <main class="mj-fullbleed">
    <div class="container"> … Inhalt … </div>
  </main>
</body>
```

* Das Theme hängt an `data-bs-theme` auf `<html>`; ein Inline-Skript im `<head>`
  löst `auto` gegen `prefers-color-scheme` auf, **bevor** gerendert wird.
  Wer das nachbaut, bekommt das Aufblitzen der falschen Farben geschenkt.
* `<body class="lite">` — die Firmware-Variante steht am Body. Regeln in
  `bootstrap.override.css` hängen daran.
* `main.js` markiert den aktiven Menüeintrag selbst, anhand des Dateinamens aus
  `location.pathname` gegen `#navbarNav a[href]`. Für eine Seite unter
  `/machino/…` greift das nicht — dort muss man den aktiven Eintrag selbst
  setzen oder auf die Markierung verzichten.

## Das Menü ist FIRMWAREABHÄNGIG

Das `<nav>` wird serverseitig mit Bedingungen gerendert (haserl, `<% if %>`).
Welche Einträge erscheinen, hängt von Hardware und installierten Diensten ab.
Deshalb steht die Einträgeliste nirgends im Machino-Quelltext — die
Relay-Injektion ergänzt genau zwei `<li>` (Device Manager, Network & USB) in
der ausgelieferten Navbar und ist der EINZIGE Sonderfall, weil OpenIPC keinen
Erweiterungspunkt für Menüeinträge hat.

## Wie Machinos Seiten gebaut sind (Stand 2026-09-25, dritter Anlauf)

Machinos Seiten sind **echte OpenIPC-Seiten**: haserl-CGIs nach exakt dem
Muster von `wireguard.cgi`,

```sh
#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="Network & USB" %>
<%in p/header.cgi %>
… Inhalt als Cards, Daten per fetch von /api/v1/… …
<%in p/footer.cgi %>
```

installiert als `machino-network.cgi` und `machino-devices.cgi` nach
`/var/www/cgi-bin/` (Machino-eigene Dateien; OpenIPC-Dateien bleiben
byte-identisch, der Uninstall entfernt sie restlos). Damit stehen Head, Navbar,
Theme und `main.js` **im ersten HTML**, und relative Links haben denselben
Basiskontext wie jede Stock-Seite. Die alten Pfade `/machino/net` und
`/machino/devices` sind nur noch 302-Weiterleitungen.

Die zwei Anläufe davor sind absichtlich dokumentiert, damit sie niemand
wiederholt:

1. **Nachgebaute Kopfleiste** in eigenem Stil — Fremdkörper.
2. **Clientseitig übernommene Kopfleiste** (fetch einer Stock-Seite, `<nav>`
   importieren, Assets nachladen): Seite springt beim Einfügen, relative Links
   laufen unter `/machino/` ins Leere, und `main.js` starb am globalen
   `$`-Konflikt mit dem Seitenskript. Alles am Browser gemessen.

Regeln, die `tests/test_wwwpages.cpp` erzwingt: kein eigenes
`<html>/<head>/<body>`, eigenes CSS nur unter `#mch` gescoped (ein ungescoptes
`.row{display:flex}` zerlegt Bootstraps Grid), Seitenskript als IIFE (wegen
`function $` in main.js), API-Pfade absolut.

## Die Zerlegung (2026-09-25/26) und der Architekturschnitt WLAN

Die eine "Network & USB"-Seite (15 Karten, und ein zweites "Network" direkt
unter OpenIPCs eigenem Menuepunkt) ist in Seiten je Thema zerlegt: **USB**
(der eine Port: Rolle, Portstrom, Host, gesteckte Geraete), **Cellular**,
**Uplinks**, dazu der **Device Manager**. Vier Menueeintraege, injiziert nach
dem Stock-Eintrag "Network".

**Keine Machino-Wi-Fi-Seite.** Station-WLAN (SSID, Passwort, Adresse)
konfiguriert OpenIPCs eigene Netzwerkseite: `adapter_scan` in `network.cgi`
listet jedes Profil aus `/etc/wireless/usb`, dessen Module unter
`/lib/modules` liegen — genau dort registriert `machino-device` den AIC8800
(`openipc_profile` aus dem Manifest). Danach waehlt man den Adapter im
Stock-Dropdown, `S40network`/`ifup` startet den wpa_supplicant. Der fruehere
Weg (Machino-Seite mit eigenem Supplicant) war ein Doppelbesitzer von
`wlan0`: das `/etc/wireless/usb`-Profil startete den Rollen-Supervisor UND
S40network machte danach `ifup wlan0` — zwei Supplicants, gemessen als
Architekturbefund am 2026-09-25. Seitdem ist `machino-usb-helper wifi-attach`
hardware-only (Module, Portstrom PB18/GPIO50 = 3,3-V-Transistor auf VBUS,
`wlan0` hoch), und der Rollen-Supervisor startet nur noch fuer die AP-Rolle.

**Karten-Injektion statt Seiten-Kopie.** Auf `network.cgi` setzt der Relay
serverseitig eine native Karte "USB network hardware" neben "Wireless
adapter" (`inject_machino_network_cards` in `http_parse.cpp`): keine Datei
unter `/var/www` wird angefasst, nur die durchgereichte Antwort. Fail-closed:
unbekannte Struktur → Seite unveraendert, nur die Karte fehlt. Dafuer puffert
der Relay GENAU diese Seite ganz (`max_page_transform_bytes`), weil der Anker
(`<details class="mj-advanced">`) tief in der ~52-KB-Seite liegt — das
48-KB-Navbar-Fenster reicht dort nicht.

## Was hier NICHT steht

Ob andere OpenIPC-Varianten (ultimate, fpv) dieselben Pfade und dieselbe
Bootstrap-Version ausliefern. Gemessen wurde *lite* auf dieser einen Kamera.
