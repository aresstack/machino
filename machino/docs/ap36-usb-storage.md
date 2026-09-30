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

## Bedienung

1. Bundle mit `storage/modules` installieren (jedes Release seit heute).
2. USB-Seite: Rolle Cellular (oder Wi-Fi) und **USB storage** anhaken,
   Apply, Neustart.
3. Kartenleser mit FAT32-Karte in den Hub. `dmesg` zeigt `usb-storage`,
   `sd 0:0:0:0: [sda] …`, mdev mountet `/mnt/sda1`. Storage-Seite zeigt
   das Medium.
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
