# AP36 — USB-Massenspeicher: SD-Kartenleser oder Stick am Hub

Stand 2026-09-30. Code fertig, Hosttests grün, **auf Hardware nicht
geprüft** (pending-physical.md B8).

## Ausgangslage

Am USB-Port hängt ein aktiver Hub, daran das 4G-Modem — und es funktioniert.
Der Wunsch: am selben Hub ein SD-Kartenleser, um Daten von der Karte zu
lesen, vor allem das KI-Modell. Das ist keine Kür: `yolov5s_t40_magik.bin`
hat 7,6 MB, das Overlay hat 3,3 MB frei. Ohne externes Medium passt das
Modell nicht auf die Kamera.

Was die Kamera dafür schon hat, und was nicht (Kernelkonfiguration
`br-ext-chip-ingenic/board/t40/t40.generic.config`, Befund AP35.7):

```
Hub-Unterstützung      fest im Kernel (usbcore) — der Hub mit Modem beweist es
fat.ko, vfat.ko        im Image (/lib/modules/kernel), NLS 437/8859-1/utf8 eingebaut
mdev.conf              sd[a-z][0-9] und mmcblk*p* -> /lib/mdev/automount.sh -> /mnt/<dev>
CONFIG_SCSI            NICHT gesetzt
usb-storage            fehlt
exFAT, ext4, NTFS      nicht im Kernel — die Karte muss FAT32 sein
```

Es fehlt also genau die Treiberkette Block ← SCSI ← USB Mass Storage.

## Was gebaut wurde

**Kernelmodule** (`.github/workflows/build-storage-modules-t40.yml`): der
Modul-only-Weg aus AP35.19, derselbe wie beim Modem. Kernelbaum
`OpenIPC/linux@ingenic-t40`, Board-Config, `CONFIG_SCSI`, `CONFIG_BLK_DEV_SD`
und `CONFIG_USB_STORAGE` als `=m`, alles Übrige an SCSI ausdrücklich aus
(sg, sr, UAS, Low-Level, Debug), dann `M=drivers/scsi` und
`M=drivers/usb/storage`. Drei Gates wie im Modem-Job: die Module existieren,
der vermagic ist exakt der der Kamera, und **jedes undefinierte Symbol
steht in der von der Kamera abgegriffenen Exportliste**
(`tools/kernel/t40nn-exported-symbols.txt`). Vorab gegen diese Liste
geprüft: Block-Layer, Driver-Core, USB-Core, kthread, procfs — alles da;
`CONFIG_BLK_DEV_BSG` ist im Kamera-Kernel aus und muss es bleiben (der Job
bricht sonst ab).

Ergebnis: `scsi_mod.ko`, `sd_mod.ko`, `usb-storage.ko`, dazu
`scsi_common.ko`, falls dieser Baum es getrennt baut. Artefakt
`storage-modules-t40`, im Bundle unter `storage/modules/`, Release-Pflicht
wie jede andere Nutzlast.

**Installer**: `--without-storage-payload`, sonst liegen die Module inert
unter `/etc/machino/modules/` (Vorgabe an, wie beim Modem: ein Schalter,
der erst nach einer Nachinstallation wirkt, ist keiner).

**Boot-Helfer** (`machino-usb-helper`): `usb.storage = true|false` in
`machino.conf`, Vorgabe false. Nach der Portrolle — der Portstrom steht
dann, der Hub hat enumeriert — lädt er `scsi_common` (falls vorhanden),
`scsi_mod`, `sd_mod`, `usb-storage`. Mounten tut **mdev** (OpenIPCs
`automount.sh`, S38, läuft vor S42), nicht das Skript; machinod fasst weder
Modul noch Mount an. Die Marke `/var/run/machino-usb-storage` trägt den
konfigurierten Wert, dieselbe Regel wie `machino-usb-mode`.

**Warum kein vierter Modus.** `usb.mode = off | wifi | cellular` bleibt: der
Port gehört weiter WLAN oder Mobilfunk, ein Hub trägt den Speicher
*daneben*. Speicher ist deshalb ein eigener Schalter — und er braucht eine
Rolle. Mit `usb.mode=off` gibt es keinen Portstrom, nichts enumeriert; die
API lehnt `storage=true` ohne Rolle mit Grund ab, der Helfer lädt dann
nichts und sagt es. Die Rolle später auf off zu stellen bleibt erlaubt.

**API/WebUI**: `GET/PATCH /api/v1/usb` trägt `storage`,
`storageAppliesAt: reboot`, im Status `storage`, `bootStorage`, und
`rebootRequired` deckt jetzt beide Schalter. USB-Seite: Kästchen „USB
storage“ in der Rollen-Karte, ausgegraut bei Disabled, gespeichert mit
Apply zusammen mit der Rolle; die Notiz sagt „Saved … still not loaded — a
reboot is required“ aus den zwei Werten, nicht aus einem Merker.
Storage-Seite: der Extern-Block zeigt gemountete Medien (wie bisher), der
Leer-Text erklärt den Schalter. KI-Seite: „Models on external storage“
listet `.bin` unter `/mnt` (max. 3 Ebenen), „Use“ setzt `ai.model_path`
dorthin — das Modell bleibt auf der Karte.

## Erster Hardware-Befund (2026-09-30)

Der Kartenleser am Hub enumeriert, `sd_mod` legt `/dev/sda` und `/dev/sdb`
an (ein Leser mit zwei Schächten). Die Treiberkette steht also. Gemountet
war nichts, und die Storage-Seite sagte nur „present but not mounted" —
ohne Grund, ohne Werkzeug. Deshalb zeigt die Storage-Seite jetzt je
Blockgerät die Diagnose (Medium im Schacht? Partition? Dateisystem per
`blkid`? gemountet, wo, wie viel frei?) und den Grund, wenn nicht: dieser
Kernel kann nur FAT32 (`vfat` als Modul); exFAT — ab Werk auf SDXC-Karten
über 32 GB — und NTFS nicht; eine leere Karte hat kein Dateisystem. Dazu
die letzten `automount`-Zeilen aus dem Log (OpenIPCs `automount.sh` loggt
den ersten Mount-Fehler per `logger`). Werkzeuge, alles busybox: **Mount**
(`mount -t vfat -o rw,noatime`, Mountpunkt `/mnt/<dev>` wie mdev),
**Unmount**, **Format FAT32** (`mkfs.vfat -n MACHINO`, mit Rückfrage, nie
auf etwas Gemountetes, nie auf `mtd*`). Die CGI läuft als eigener Prozess
unter busybox httpd — machinod forkt weiterhin nicht.

## exFAT als Nutzlast (2026-09-30)

Die frische 128-GB-Karte war exFAT, und „Format FAT32“ war die einzige
Antwort — mit der 4-GB-Dateigrenze von FAT32. Jetzt kommt exFAT als
**Nutzlast per Vorgabe** mit (wie die Speichermodule; `--without-exfat`
lässt sie weg, Cam-Tool: „exFAT-Nutzlast“), rund 200 kB Overlay:

```
exfat.ko      Samsung-Treiber exfat-nofuse (der Android-Treiber fuer 3.x/4.x-
              Kernel), out-of-tree gegen OpenIPC/linux@ingenic-t40 mit der
              Board-Konfiguration gebaut; dieselben drei Gates wie bei den
              Speichermodulen (vermagic, Symbole VON DER KAMERA, existiert).
              Workflow: .github/workflows/build-exfat-t40.yml, Commit gepinnt.
mkfs.exfat    exfatprogs 1.2.9, fuer mipsel/musl wie machino selbst gebaut
              (busybox hat nur mkfs.vfat). Nach /usr/sbin.
```

Das Modul liegt inert unter `/etc/machino/modules/exfat.ko`; der USB-Helfer
lädt es bei `usb.storage=true` **vor** `usb-storage`, denn OpenIPCs
`automount.sh` probiert „vfat exfat …“ genau in dem Moment, in dem `sd_mod`
die Partition meldet — ein später geladenes Modul verpasst das Einstecken.
Die Storage-Seite mountet exFAT von Hand (und lädt das Modul notfalls per
`insmod` nach), sagt bei einer exFAT-Karte ohne Nutzlast, woran es liegt,
und bietet **Format exFAT** neben **Format FAT32** — beide im Hintergrund
mit Statusdatei, beide mit Rückfrage. So lässt sich eine Karte in Machino
jederzeit zwischen FAT32 (4 GB pro Datei, überall lesbar) und exFAT (keine
Dateigrenze, Werksformat) umformatieren. `machino-usb-helper status` zeigt
`exfat:`.

Nebenbefund derselben Karte: OpenIPCs busybox-`blkid` ist ohne
`FEATURE_BLKID_TYPE` gebaut und nennt nie `TYPE=`, nur `LABEL`/`UUID`. Die
frisch formatierte FAT32-Karte stand deshalb nach dem Unmount als „no
filesystem found“ da, ohne Mount-Knopf. Die Storage-Seite liest jetzt die
Signaturen im Bootsektor (exFAT/NTFS ab Byte 3, FAT32 ab Byte 82, FAT12/16
ab Byte 54) und nimmt `TYPE=` nur, wenn `blkid` es doch liefert.

Nicht am Gerät bewiesen (pending-physical B9): dass die Kamera ein
exFAT-Medium mountet und dass ein mit diesem `mkfs.exfat` geschriebenes
Volume von Windows/macOS gelesen wird.

## Dateien auf die Karte: der File Manager unter Machino (2026-09-30)

Karte gemountet, und dann? Der File Manager der WebUI zeigt `mnt/sda1`
und listet — aber Upload, Einzeldatei-Download und der Editor hingen an
majestic: der Upload postet den rohen Dateiinhalt auf `/upload` mit dem Ziel
im Header `File-Location`, der Download holt die Datei über ihren absoluten
Pfad (`GET /mnt/sda1/x.bin`), den majestics Static-Handler streamt. Machino
steht an majestics Stelle und reichte beides an busybox httpd weiter, der
weder das eine noch das andere kennt — der Upload endete still, der
Download mit 404.

Jetzt:

* **Upload** ist nativ in machinod (`http::FileUpload`): der Request wird
  an der Request-Zeile abgefangen, der Kopf allein geparst (`Limits::
  head_only`), dieselbe Login-Schranke wie für jede Route, dann jeder Chunk
  in eine Temp-Datei neben dem Ziel — kein RAM-Puffer auf der 48-MB-Kamera,
  keine Größengrenze aus dem Arbeitspuffer, und ein abgebrochener Upload
  lässt das Ziel unberührt (Temp-Datei weg, Ziel erst durch `rename` am
  Ende). Verweigert: `/proc`, `/sys`, `/dev`, `/rom`, `/overlay`,
  relative Pfade, `..`, Verzeichnisse; Platzprüfung per `statvfs` vorab
  (507 mit Zahlen). Alles andere ist die Kamera des Betreibers, wie bei
  majestic. Damit funktioniert auch der lokale `.tgz`-Upload der
  Update-Seite (`/tmp/firmware.tgz`).
* **Download / Editor**: ein `GET`, dessen Pfad nicht im Webroot liegt,
  aber eine reguläre Datei auf der Kamera ist (`file_get_rewrite`, zwei
  `stat`), wird auf `machino-file-get.cgi` umgeschrieben; das Relay streamt
  die Antwort mit Backpressure, für diese eine Route ohne Byte-Deckel und
  ohne absolute Zeitgrenze (die Länge steht im Kopf; die
  Inaktivitätsgrenze bleibt). Medien inline, alles andere als attachment.
  Kein Range: Spulen im Video lädt neu.

Am Gerät noch zu zeigen (pending-physical B10): ein `.bin` per Drag-and-
drop nach `/mnt/sda1`, Download derselben Datei, Editor auf einer
Textdatei der Karte.

## Bedienung

1. Bundle mit `storage/modules` installieren (jedes Release seit heute).
2. USB-Seite: Rolle Cellular (oder Wi-Fi) und **USB storage** anhaken,
   Apply, Neustart.
3. Kartenleser mit FAT32-Karte in den Hub. `dmesg` zeigt `usb-storage`,
   `sd 0:0:0:0: [sda] …`, mdev mountet `/mnt/sda1`. Storage-Seite zeigt
   das Medium — oder den Grund und den Knopf, wenn nicht (exFAT-Karte ohne
   exFAT-Nutzlast, `--without-exfat`: „Format FAT32"; mit ihr wird sie
   gemountet, und „Format exFAT" steht neben „Format FAT32").
4. Modell: `yolov5s_t40_magik.bin` und `manifest.json` auf die Karte,
   KI-Seite → „Use“. Schreiben geht genauso: das Medium ist rw gemountet.

Terminal: `machino-usb-helper status` zeigt `storage:` und `media:`,
`machino-usb-helper storage` den Schalter.

## Grenzen, ehrlich

* **NNA** bleibt, was sie in AP23 war: ohne `nmem=` läuft keine Erkennung.
  Die Karte löst das Platzproblem für das Modell, nicht die Reservierung.
* **Strom**: der Kameraport führt 3,3 V (AP35.22). Der Kartenleser hängt
  am *aktiven* Hub und wird von dort versorgt — am Port direkt ginge er
  nicht, wie das Modem.
* **Karte beim Boot**: `ai.model_path` auf `/mnt/sda1/…` verlangt, dass das
  Medium bei jedem Start steckt, sonst meldet der Detektor die Datei als
  unlesbar (der bestehende Pfad, keine neue Fehlerklasse).
* **Kartenslot auf der Platine** (AP35 „Offen“, E4): `mmc0` mit SDHCI ist
  fest im Kernel, mdev mountet `mmcblk0p1`. Hängt der Slot am MMC-Host,
  braucht es für die SD-Karte keinen Leser. Eine Karte einstecken und
  `cat /proc/partitions` entscheidet das in einer Minute.
* **RAM**: drei Module und der SCSI-Kern kosten grob 300 KB im 42-MB-
  Budget, nur wenn der Schalter an ist.

## Tests

`tests/test_net_views.cpp` (`test_usb_storage_is_a_separate_switch_that_needs_a_port_role`):
Vorgabe aus, Datei-Schlüssel hin und zurück, JSON-Vertrag, PATCH mit und
ohne Rolle. `tests/test_openipc_install.sh`: Module liegen inert, der
Helfer liest `usb.storage` (aus, true, Unsinn), `--without-storage-payload`
lässt genau sie weg. `tools/check-netui.sh` für die drei Seiten. Der
Workflow beweist Übersetzung, vermagic und Symbole — nicht die Hardware.
