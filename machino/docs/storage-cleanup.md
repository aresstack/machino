# Speicherkonzept & Storage-Menü (W5b)

Stand: 2026-09-28. Gemessen an der T40NN (`192.168.1.11`), nicht nur aus der Doku.

## Das Speicherkonzept

Ein einziger 16-MB-SPI-NOR-Chip, fest in Partitionen zerschnitten (`mtdparts`
in der U-Boot-Env). overlayfs legt rootfs (ro) + rootfs_data (rw) übereinander
und zeigt das Ergebnis als `/`.

| Partition | Inhalt | Größe | belegt (Cam) | beschreibbar? |
|---|---|---|---|---|
| `mtd0` boot | U-Boot | 256 kB | – | nein |
| `mtd1` env | U-Boot-Env (enthält `mtdparts`) | 64 kB | – | nur mit Brick-Risiko |
| `mtd2` kernel | Linux-Kernel | 2048 kB | – | nur per Flash |
| `mtd3` rootfs | **ROM** (squashfs, Firmware) | 5120 kB | **4736 kB / 100 %** | **nein, read-only** |
| `mtd4` rootfs_data | **Overlay** (jffs2) | 8896 kB | **~8032 kB / 90 %** | **ja** |

### Die zwei überraschenden Eigenheiten

1. **Eine ROM-Datei ändern** kopiert sie in voller Größe ins Overlay (copy-up).
2. **Eine ROM-Datei „löschen"** legt nur einen overlayfs-**Whiteout** an: er
   *kostet* Overlay-Platz und gibt **kein** ROM frei. ROM ändert nur ein ganzer
   Reflash von `mtd3`.

### Zwei Missverständnisse, die das direkt aufklärt

- **„Flashen löscht majestic aus dem ROM."** Ein rootfs-Flash löscht `mtd3`
  komplett — aber **jedes OpenIPC-Image enthält majestic**, es kommt sofort
  wieder mit. Ein ROM ohne majestic bräuchte ein selbst gebautes Firmware-Image.
- **„majestic löschen schafft Platz für KI-Modelle."** Nein. majestic liegt in
  `mtd3`, die Modelle in `/etc/machino/models` im **Overlay (mtd4)**. `mtd3` und
  `mtd4` sind getrennte, fest begrenzte Partitionen — freier `mtd3`-Platz ist für
  `mtd4` unerreichbar. Platz für Modelle entsteht **nur im Overlay**.

### Wo der Platz auf dieser Cam wirklich hingeht (Overlay)

| Datei (Overlay) | ~Größe | Was |
|---|---|---|
| `/usr/bin/machino.old.*` | 3,0 MB | Rollback-Backup eines Binary-Tauschs |
| `/etc/machino/backup/machino.prev` | 2,9 MB | Vorversions-Backup (machino-manager) |
| `/usr/bin/machino.machino-new.*` | 1,0 MB | altes Staging-Update |
| `/root/m11a/…` | 1,5 MB | alte Test-Payload |

Das sind die Aufräum-Ziele — nicht majestic.

### Overlay vergrößern? (Repartitionieren)

Nur durch Verschieben der `mtd3`/`mtd4`-Grenze (U-Boot-Env umschreiben). **No-Go:**
löscht das Overlay, brickt bei einem Fehler (UART-Rettung nötig), steht auf
`docs/pending-physical.md`. Und es **lohnt hier nicht**: das squashfs füllt seine
5-MB-Partition schon zu ~92 % — Verkleinern brächte <0,5 MB. Alternativen:
Overlay aufräumen, oder große Modelle auf SD/USB auslagern.

## Das Storage-Menü (System → Storage)

`machino-cleanup.cgi` (haserl-Seite) + `machino-cleanup-dl.cgi` (plain-sh
Download-Helfer). Aufgebaut nach Speicherbereichen — wie „Dieser PC" mit
Laufwerken:

- **Overlay (beschreibbar):** Nutzungsbalken + die Liste der system-irrelevanten
  Dateien (Backups, Staging, alte Payloads, KI-Modelle). Pro Datei ein
  **Download**-Knopf (Sicherung zuerst) und ein **Delete**-Knopf. Der einzige
  Bereich, in dem echter Platz frei wird.
- **Firmware/ROM (read-only):** nur Ansicht (Größe, majestic-Größe/-Status,
  Media-Owner). **Kein** Löschknopf (siehe oben). majestic bekommt einen
  „Ensure majestic disabled"-Knopf, der den overlayfs-Whiteout auf
  `S95majestic` prüft/setzt (Deaktivierung, kein Platzgewinn).
- **Extern (SD/USB):** falls vorhanden gemountet angezeigt (Modelle auslagern),
  sonst der Hinweis + Link zur USB-Seite.
- **Partitionen (read-only Info):** `/proc/mtd` als Tabelle + die ehrliche
  No-Go-Erklärung. **Kein** Repartitionierungs-Knopf.

### Sicherheitskern

Beide Skripte prüfen jeden Pfad mit derselben strengen `is_reclaimable`-Allowlist:
kein `..`, nur reguläre Dateien, nur die bekannten Overlay-Fundstellen. Ein Pfad
aus dem Netz wird nie zu `/etc/shadow` o. ä. Das laufende `/usr/bin/machino`
(ohne Suffix), `webui-upstream.tgz` und aktive Binaries fallen durch die
Allowlist. Live geprüft: `/etc/shadow` → 404, Traversal → 404, Löschen von
`/usr/bin/machino` → „Not a cleanup file". Der Download-Helfer schreibt seine
eigene `HTTP/1.1`-Antwort (wie `machino-cgi-run.cgi`) und liefert als
`Content-Disposition: attachment`; machinos :80-Relay streamt sie mit
Backpressure (kein Ganz-in-den-RAM-Puffern auf der 48-MB-Cam).

Nav-Eintrag: `inject_machino_nav` fügt „Storage" in die System-Dropdown ein
(nach „AI"). Keine OpenIPC-Datei wird verändert; uninstall entfernt beide CGIs.
