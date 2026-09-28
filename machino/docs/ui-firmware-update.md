# Firmware-Update über die WebUI (W5)

Stand: 2026-09-28. Commit `fda4082`. CI grün, Host-Tests 4947/0.
Hardware-abgenommen auf T40NN (`192.168.1.11`): echter Flash
`04db148/2026-09-17` → `60d4cbe/2026-09-27` durchgeführt, machino überlebte.

## Ziel

Der OpenIPC-„Firmware update"-Banner/-Knopf soll das Update **durch die WebUI**
ausführen — und dabei machino aktiv und majestic deaktiviert lassen, ohne
majestic neu zu installieren und ohne das KI-Modell zu opfern.

Vorher verweigerte machino `/ws/upgrade` (AP21: „dieser Build flasht nicht").
Das war ehrlich, aber es machte den Knopf nutzlos.

## Der Vertrag (aus majestic-webui `update.js`)

Der Client öffnet eine WebSocket zu `/ws/upgrade` und schickt **einen** JSON-
Start-Frame:

```json
{"source":"github","kernel":true,"rootfs":true,"reset":false,"force":false}
```

- `source`: `"github"`/leer = Online-Update (Banner-Weg, holt das neueste Build
  für den SoC); `http(s)://…` = eigene URL; `/tmp/firmware.tgz` = lokaler
  Upload; sonst = benannter Release-Channel.
- `kernel`/`rootfs`: welche Partitionen.
- `reset`: **Overlay-Wipe** (= `sysupgrade -n`).
- `force`: Validierung überspringen (`-f`).

Der Server führt `sysupgrade` aus und **streamt dessen stdout+stderr** zurück.
Alle Marker, auf die die Seite reagiert (`Protected: flashing…`,
`Kernel updated`, `RootFS updated`, `Unconditional reboot`,
`<Grund> Aborting.`), kommen direkt von `sysupgrade` — machino erfindet nichts.

## Implementierung

- **`compat::upgrade_plan(params_json)`** (`src/app/compat/majestic_webui.cpp`):
  reine, host-getestete Funktion, die den Start-Frame auf ein `sysupgrade`-argv
  abbildet — oder mit einer Ablehnung in der verankerten Vokabel-Sprache der
  Seite antwortet (`ERROR: invalid upgrade parameters` in Zeile 1).
- **`/ws/upgrade`** (`src/app/http/http_server.cpp`): Handshake annehmen,
  Start-Frame lesen (`ws_upgrade_input`), `sysupgrade` forken mit stdout+stderr
  auf einer nicht-blockierenden Pipe (`spawn_upgrade`, **kein Shell** → keine
  Injektion), und die Ausgabe aus der Poll-Schleife als WS-Text-Frames
  streamen (`pump_upgrade`, neue PollRef-`kind 3`).
- **`ws::ping_frame()`**: Server-Ping hält den Socket in den stillen Download-/
  Zeitsync-Phasen warm; ein laufender Flash ist vom Idle-Timeout ausgenommen.
- Der Flash-Kindprozess wird bei Socket-Schluss **nie gekillt** — `sysupgrade`
  jenseits des Flash-Punkts soll eine Trennung bewusst überleben.
- `/ws/upgrade` ist auth-gegated (nur `login.html`/`favicon.ico`/`POST /login`
  sind öffentlich).

## Zwei Invarianten (per Host-Test festgenagelt)

1. **`--web` wird IMMER übergeben.** Ohne `--web` schießt `sysupgrade` den
   Web-Daemon vor dem Flashen ab (`killall -q -3 majestic`, sysupgrade-Skript
   ~Zeile 347) — und das Log stirbt mit ihm. Mit `--web` überlebt machino die
   stillen Phasen und streamt bis zum Point of no Return.
2. **Overlay-Wipe (`reset`/`-n`) wird NIE übergeben** und wird abgelehnt.
   machino, seine Config und das KI-Modell liegen alle im Overlay; ein wipender
   Flash würde genau den Daemon löschen, der gerade das Log streamt.

## Warum majestic deaktiviert bleibt und das Modell überlebt

Ein normaler `-r`-Flash (ohne `-n`) **bewahrt das Overlay**. Damit überlebt der
overlayfs-Whiteout auf `/overlay/root/etc/init.d/S95majestic` — nach dem Reboot
startet machino, nicht majestic. Kein Wipe → das KI-Modell (Overlay) bleibt.

`sysupgrade` verschiebt sich für den Flash per `exec` in eine RAM-Kopie (gleiche
PID, dieselben fds) → die stdout-Pipe bleibt offen, das Streaming läuft
durchgehend bis zum Reboot.

## Live-Beleg (Hardware-Abnahme 2026-09-28)

argv war exakt `sysupgrade --web -r -k` (also `--web` immer, kernel+rootfs,
**kein `-n`**). Transkript-Auszug:

```
Received and unpacked
Protected: flashing continues even if this terminal disconnects.
Flashing from RAM (pid 10934)
Verifying rootfs … SoC OK
Erasing … 58/58 (100%)   Writing kernel …   Verifying kb 4688/4688 (100%)
RootFS updated to master+60d4cbe, 2026-09-27
Unconditional reboot
```

Nach dem Reboot:

| Prüfung | Ergebnis |
|---|---|
| Firmware | `04db148/2026-09-17` → `60d4cbe/2026-09-27` |
| Medien-Besitzer | machino (nicht majestic; ein Prozess mit argv0 „majestic") |
| Whiteout auf S95majestic | überlebte |
| Overlay-Nutzung | byte-identisch (7,9M/784K frei) vor+nach → Modell+Binary intakt |
| WebUI | `login.html` = 200 |

## Bewusst zurückgestellt

`/upload` (lokaler `.tgz`-Offline-Flash): mehrere MB im RAM einer 48-MB-Kamera
zu puffern braucht Streaming-auf-Platte, ein eigenes Paket. Der Online-/Banner-
Weg braucht es nicht. `upgrade_plan` bildet `source=/tmp/firmware.tgz` bereits
ab, sobald der Endpunkt kommt.
