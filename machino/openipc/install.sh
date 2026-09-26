#!/bin/sh
# Install Machino next to Majestic on an OpenIPC camera.
#
# Installing does NOT switch the camera over: whatever streams now keeps
# streaming. It only makes Machino available and adds the selector (CLI and
# WebUI) you use to switch. That separation is deliberate - an install should
# never be the thing that takes your camera off the air.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
# MACHINO_ROOT is a test hook: empty in production, a throwaway tree in the
# host tests. Everything the installer touches goes through it.
ROOT="${MACHINO_ROOT:-}"
WITH_AP=0
WITH_NETPAGE=0
WITH_DEVPAGE=0
# BEIDE USB-Nutzlasten werden per DEFAULT mitinstalliert, und beide bleiben AUS.
#
# Das klingt widerspruechlich und ist es nicht. Der Schalter sitzt in der
# Machino-UI, und ein Schalter, der erst wirkt, nachdem sich jemand per SSH
# Dateien nachkopiert hat, ist kein Schalter. Also liegen die Dateien bereit
# und tun nichts: machino-usb-helper liest usb.mode und laedt bei "off" kein
# einziges Modul.
#
# Der Preis sind rund 2 MB Overlay fuer WLAN (aic8800.ko 550 K, aic_load_fw.ko
# 87 K, Firmware 362 K, hostapd 996 K) und einige hundert KB fuer die
# Modem-Module, von 8,7 MB. Wer den Platz braucht, nimmt
# --without-wifi-payload bzw. --without-cellular-payload; dann fehlt der
# Schalter nicht, er meldet nur ehrlich, dass nichts zu schalten da ist.
WITH_WIFI_PAYLOAD=1
WITH_CELL_PAYLOAD=1
# NNA (KI) ist andersherum gepolt: AUS, bis jemand --with-nna-payload sagt.
# Mehrere MB Helfer+Modell auf einem fast vollen Overlay sind eine
# Entscheidung, kein Default.
WITH_NNA_PAYLOAD=0
# IPsec (WeirdIKE) genauso: ein eigener Daemon mit eigenem Initskript und
# einem Preshared-Key in der Config -- ein VPN ist eine Entscheidung.
WITH_WEIRDIKE=0

# Der eine Schalter fuer den einen Port. Leer heisst "nicht angefasst": eine
# Neuinstallation ueber eine bestehende hinweg darf die Wahl des Betreibers
# nicht auf off zuruecksetzen, nur weil niemand die Option mitgegeben hat.
USB_MODE=""
STATE_DIR="$ROOT/etc/machino"
WWW="$ROOT/var/www"
CGI="$WWW/cgi-bin"
INITD="$ROOT/etc/init.d"
BACKUP="$STATE_DIR/backup"

# Der Vertrag der OpenIPC-Netzwerkseite. Nicht erfunden, sondern aus
# www/cgi-bin/network.cgi (adapter_scan) und /etc/init.d/S40network gelesen:
#
#   network.cgi   parst /etc/wireless/{usb,sdio,modem} auf Bloecke der Form
#                 if [ "$1" = "<id>" ] ... fi, zieht die modprobe-NAMEN heraus
#                 und bietet ein Profil nur an, wenn JEDES dieser Module als
#                 .ko unter /lib/modules liegt und mindestens eines davon kein
#                 reines Stack-Modul (mac80211/cfg80211/rfkill) ist.
#   SoC-Filter    der Token kommt aus der Id (Teil, der auf ^t[0-9]+$ o.ae.
#                 passt) und muss praefix-kompatibel zu `ipcinfo --chip-name`
#                 sein. "t40" passt damit auch auf t40nn.
#   S40network    liest wlandev aus dem U-Boot-Env und ruft
#                 /etc/wireless/usb "$wlandev" auf, dann ifup wlan0.
#
# Deshalb genuegt es NICHT, die Module nach /etc/machino/modules zu legen und
# selbst per insmod zu laden: fuer die Seite existieren sie dann nicht, das
# Dropdown zeigt nur "None", und der bereits auf Hardware bewiesene AIC8800
# bleibt unerreichbar. Genau dieser Befund am 2026-09-25.
# Die Registrierung selbst macht machino-device; der Profilname steht
# ausschliesslich in openipc/devices/aic8800.manifest.

say()  { echo "$*"; }
warn() { echo "install: $*" >&2; }
die()  { echo "install: $*" >&2; exit 1; }

# Die Registrierung im Wirtssystem (/lib/modules + /etc/wireless/usb) macht
# openipc/sbin/machino-device. Sie stand frueher hier -- und danach ein zweites
# Mal im Deinstallierer und ein drittes Mal im Boot-Helfer, mitsamt dem
# Profilnamen als Zeichenkette. Eine Quelle, sonst driftet es.

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            cat <<EOF
usage: ./install.sh [--usb-mode=off|wifi|cellular] [--with-network-page]
            [--with-device-page]

The WebUI login is Machino's Majestic drop-in session login against the
camera's root account - there is nothing to configure here.

BOTH USB payloads are installed BY DEFAULT and both are inert.

  WiFi      AIC8800 modules, firmware, hostapd, the role supervisor
  cellular  option/usb_wwan/usbnet/cdc_ether modules and the data-path helper

Installed is not the same as running. There is ONE USB port, so there is one
setting -- usb.mode -- and it defaults to off: no module is loaded, PB18 stays
down, no daemon runs, and the port is free for whatever is plugged into it.
The selector lives on the Machino network page and takes effect at the next
boot, because swapping kernel modules under a running IMP pipeline is how this
camera hardlocks.

Files are installed regardless so that the selector is a real switch. One that
only works after someone has copied files over by SSH is not a switch.

  --usb-mode=MODE       pre-set the selector to off, wifi or cellular, so the
                        stack comes up at the next boot without anyone
                        visiting the web page. Default: off. This only writes
                        the setting; the files are installed either way.

  --with-wifi           the old name for --usb-mode=wifi. Kept so existing
                        install commands and scripts do not break.

  --without-wifi-payload
                        do not install the WiFi driver, firmware or hostapd.
                        Saves about 2 MB of the 8.7 MB overlay and makes the
                        WiFi selection inoperable -- the page then says so
                        rather than offering something that cannot work.

  --without-cellular-payload
                        the same for the modem modules and the helper.

  --with-nna-payload    install the NNA inference helper (machino-nna) and any
                        bundled detection model into /etc/machino/models. OFF
                        by default: several MB on an overlay that is usually
                        nearly full -- AI is an explicit choice, and the AI
                        page can free the space (majestic backup) first.

  --with-weirdike       install the IKEv2/IPsec client (weirdiked, weirdikectl,
                        S99weirdike, the ipsec.cgi status page). OFF by
                        default. The daemon does not start until you create
                        /etc/weirdike/weirdike.conf (mode 0600, see the
                        installed .example); a missing or broken VPN never
                        touches the video path. Loads the tun module at boot.

  --with-access-point   accepted and ignored; hostapd is part of the default
                        payload now. Kept so existing install commands and
                        scripts do not break.

  --with-device-page    add a "Geraete (machino)" entry to the stock WebUI menu.
                        Same rule as --with-network-page: off by default, and
                        the page works by URL without it.
  --with-network-page   add a "Netzwerk & USB (machino)" entry to the stock
                        WebUI menu, pointing at the machino network page. Off by default:
                        the installer does not edit p/header.cgi behind your
                        back. The page is reachable at that URL either way;
                        this only adds the link. The edit is marked and the
                        uninstaller removes exactly it.
EOF
            exit 0 ;;
        --with-access-point) WITH_AP=1 ;;
        --usb-mode=*) USB_MODE="${1#--usb-mode=}" ;;
        --with-network-page) WITH_NETPAGE=1 ;;
        --with-device-page) WITH_DEVPAGE=1 ;;
        --with-wifi) USB_MODE=wifi ;;          # Altname, siehe --help
        --without-wifi-payload) WITH_WIFI_PAYLOAD=0 ;;
        --without-cellular-payload) WITH_CELL_PAYLOAD=0 ;;
        --with-nna-payload) WITH_NNA_PAYLOAD=1 ;;
        --with-weirdike) WITH_WEIRDIKE=1 ;;
        *) die "unknown option '$1' (try --help)" ;;
    esac
    shift
done

[ -n "$ROOT" ] || [ "$(id -u)" = "0" ] || die "run as root"

# BusyBox has no install(1) - found out on the camera, not in review. Everything
# this script needs must exist there, so it is checked up front instead of
# failing halfway through with part of the files in place.
for c in cp chmod mkdir mv rm grep sed awk df wc cat dirname; do
    command -v "$c" >/dev/null 2>&1 || die "required command '$c' not found on this system"
done

# cp+chmod instead of install(1), for the same reason.
#
# AP21: the copy goes to a temporary in the DESTINATION directory and is then
# renamed over the target. A plain `cp` onto a live path writes in place, so a
# full disk, a power cut or a killed script leaves a TRUNCATED file where a
# working one used to be - and for /usr/bin/machino that is a camera that no
# longer streams. rename(2) within one filesystem is atomic: the target is
# either the old file or the whole new one, never half of either.
#
# The temporary is removed on any failure, so a failed install cannot leave
# litter behind that the next one has to reason about.
#
# The rename can itself fail on this camera: move_file() below documents an
# EINVAL from this kernel's overlayfs, seen in the field. The temporary here is
# created in the DESTINATION directory, so it is already in the upper layer and
# the rename should be an ordinary upper-to-upper move - but "should" is not a
# guarantee on a box that has already surprised us once. So a failed rename
# falls back to the old in-place copy, and SAYS that it did: a non-atomic write
# is better than an install that cannot proceed, and an unnoticed one is not.
# Die Variablen heissen _put_*, damit sie nicht mit denen der Aufrufer
# kollidieren. Sie hiessen einmal _m/_s/_d/_t, und eine Schleife, die selbst
# ein _d als Verzeichnis benutzte, hat daraufhin JEDE Firmware-Datei in ein
# Verzeichnis gelegt, das nach der vorigen Datei benannt war -- basename "$_d"
# las ab dem zweiten Durchlauf das Ziel des letzten put. Der Treiber haette
# keine einzige Firmware gefunden. Auf der Kamera in einer Sandbox
# aufgefallen, nicht in den Hosttests: deren Fixture hatte genau EINE
# Firmware-Datei, und bei einem Element faellt ein Iterationsfehler nie auf.
put() {
    _put_m=$1; _put_s=$2; _put_d=$3
    mkdir -p "$(dirname "$_put_d")" || return 1
    _put_t="$_put_d.machino-new.$$"
    if ! cp "$_put_s" "$_put_t"; then rm -f "$_put_t"; return 1; fi
    if ! chmod "$_put_m" "$_put_t"; then rm -f "$_put_t"; return 1; fi
    if mv -f "$_put_t" "$_put_d" 2>/dev/null; then return 0; fi
    rm -f "$_put_t"
    say "atomic replace unavailable for $_put_d - writing in place"
    cp "$_put_s" "$_put_d" && chmod "$_put_m" "$_put_d"
}

# Einen Schluessel in machino.conf setzen, ohne den Rest anzufassen.
#
# Idempotent: ist der Schluessel schon da, wird seine Zeile ersetzt, sonst
# angehaengt. Geschrieben wird ueber eine temporaere Datei und mv, damit ein
# Stromausfall mitten im Schreiben keine halbe Konfiguration hinterlaesst --
# dieselbe Regel wie ueberall sonst hier.
#
# Bewusst KEIN sed -i: BusyBox sed -i schreibt die Datei in place, und genau
# dann ist sie waehrend des Schreibens kaputt.
set_conf() {
    _k=$1; _v=$2
    _f="$STATE_DIR/machino.conf"
    [ -f "$_f" ] || : > "$_f" || return 1
    _t="$_f.machino-new.$$"
    {
        grep -v "^[[:space:]]*$(echo "$_k" | sed 's/\./\\./g')[[:space:]]*=" "$_f" 2>/dev/null
        echo "$_k = $_v"
    } > "$_t" || { rm -f "$_t"; return 1; }
    chmod 0644 "$_t" || { rm -f "$_t"; return 1; }
    mv -f "$_t" "$_f" 2>/dev/null && return 0
    rm -f "$_t"
    return 1
}

# mv can fail with EINVAL on this kernel's overlayfs when the source still
# lives in the squashfs lower layer (seen on the camera: "mv: can't rename
# '/etc/init.d/S95majestic': Invalid argument"). Copy+delete works there: the
# copy goes to the upper layer and the delete becomes a whiteout.
move_file() {
    _s=$1; _d=$2
    mv "$_s" "$_d" 2>/dev/null && return 0
    cp -p "$_s" "$_d" && rm -f "$_s"
}

# ------------------------------------------------------------- preflight ---
for f in machino machino.conf sbin/streamerctl sbin/machino-manager init/S95streamer init/machino; do
    [ -r "$HERE/$f" ] || die "bundle incomplete: $f is missing"
done
[ -d "$CGI" ] || die "no $CGI - this does not look like an OpenIPC camera with the WebUI installed"

# Ein Rollback-Binary aus einer FRUEHEREN Installation ist ab hier wertlos --
# das nuetzliche Rollback-Ziel ist immer der Stand von vor DIESER Installation,
# und den legt der Backup-Schritt unten neu an. Es zuerst zu loeschen ist auf
# dieser Kamera der Unterschied zwischen "passt" und "not enough space":
# am 2026-09-26 scheiterte ein Install-ueber-Install an ~200 kB, waehrend
# 2,9 MB alte Backup-Kopie auf dem Overlay lagen.
if [ -f "$BACKUP/machino.prev" ]; then
    rm -f "$BACKUP/machino.prev" "$BACKUP/machino.prev.tmp"
    say "removed the rollback binary of the previous install (this install writes its own)"
fi

# 1280 kB Zuschlag: die groesste Einzeldatei der Nutzlast (hostapd ~1 MB) wird
# als tmp+mv geschrieben und braucht ihren Platz TRANSIENT doppelt, dazu etwas
# Luft. Das alte Binary kostet nichts mehr: es wird per move_file zum Backup
# UMBENANNT, nicht kopiert.
need_kb=$(( ($(wc -c < "$HERE/machino") / 1024) + 1280 ))
free_kb=$(df -k / | awk 'NR==2 {print $4}')
[ "${free_kb:-0}" -ge "$need_kb" ] || die "not enough space on / (need ~${need_kb} kB, have ${free_kb} kB)"

# ------------------------------------------------- AP21: check before writing
#
# Nothing below touches MTD - this installer replaces one binary and its
# config, never a partition. But "it cannot brick the flash" is not a reason to
# copy an unchecked file over the daemon: a truncated download or a bundle
# built for another SoC produces a camera that does not stream, and on a box
# reached only over the network that is just as unreachable.
#
# Three questions, cheapest first, all of them BEFORE the first write.

# 1. Hash. The bundle ships SHA256SUMS; CI generates it next to the binary.
#    Verified when the file and a hashing tool are both present - an older
#    bundle without one, or a camera whose busybox lacks sha256sum, still
#    installs, and says which check it could not make rather than implying it
#    passed.
if [ -r "$HERE/SHA256SUMS" ]; then
    if command -v sha256sum >/dev/null 2>&1; then
        # Exactly the top-level entry. A first cut matched any path ENDING in
        # /machino - and the bundle also ships ./init/machino, which sorts
        # first, so every install would have compared the daemon against the
        # init script's hash and refused a perfectly good bundle.
        want=$(awk '$2 == "./machino" || $2 == "machino" {print $1; exit}' "$HERE/SHA256SUMS")
        if [ -n "$want" ]; then
            have=$(sha256sum "$HERE/machino" | awk '{print $1}')
            [ "$want" = "$have" ] || die "bundle is corrupt: machino hashes $have, SHA256SUMS says $want. Nothing was written."
            say "sha256 verified against SHA256SUMS"
        else
            say "SHA256SUMS has no entry for machino - hash NOT verified"
        fi
    else
        say "no sha256sum on this camera - hash NOT verified"
    fi
else
    say "bundle has no SHA256SUMS - hash NOT verified"
fi

# 2. Format. A 32-bit little-endian MIPS ELF, read straight out of the header
#    rather than trusted from a filename: \x7fELF, class 1, data 1 (LSB), and
#    e_machine 8 (EM_MIPS) at offset 18. A bundle built for the build host -
#    which is what a broken cross-build produces - fails here instead of
#    becoming a daemon the camera cannot exec.
#
#    Six bytes, not seven. The shipped binary's header starts
#    7f454c46 01 01 01 00, where byte 6 is EI_VERSION (1) and byte 7 is
#    EI_OSABI (0). A first cut asserted 00 at byte 6 and would have refused
#    every valid build - caught by running it against the real artifact rather
#    than reasoning about the layout.
#    MACHINO_INSTALL_SKIP_FORMAT is the same kind of test hook as
#    MACHINO_MANAGER_NO_ACTIVATE next door: the host tests stand a shell script
#    in for the daemon so it can answer --version and --migrate-majestic, and a
#    shell script is not an ELF. The hook is used ONLY by that harness, and the
#    check itself is exercised by two tests that leave it off - one with a real
#    MIPS header, one with an x86-64 one.
if [ -n "${MACHINO_INSTALL_SKIP_FORMAT:-}" ]; then
    say "format check skipped (test hook)"
else
    elf=$(od -An -tx1 -N 20 "$HERE/machino" 2>/dev/null | tr -d ' \n')
    case "$elf" in
        7f454c460101*) : ;;                     # ELF, 32-bit, little-endian
        "") say "cannot read the ELF header (no od?) - format NOT verified" ;;
        *) die "machino is not a 32-bit little-endian ELF (header ${elf}). Nothing was written." ;;
    esac
    if [ -n "$elf" ]; then
        # e_machine is the 2 bytes at offset 18; little-endian, 0800 = 8 = MIPS.
        mach=$(printf '%s' "$elf" | cut -c37-40)
        [ "$mach" = "0800" ] || die "machino is not a MIPS binary (e_machine ${mach}). Nothing was written."
    fi
fi

# 3. Target platform. BUILDINFO records what the bundle was built for; the
#    camera says what it is. A mismatch is the mistake that costs the most and
#    is the easiest to make with two cameras on a desk.
if [ -r "$HERE/BUILDINFO" ] && [ -r "$ROOT/proc/device-tree/compatible" ]; then
    dt=$(tr -d '\000' < "$ROOT/proc/device-tree/compatible" 2>/dev/null)
    case "$dt" in
        *t40*|*T40*)
            grep -qi "T40" "$HERE/BUILDINFO" ||
                die "this bundle does not name T40 in BUILDINFO, but the camera reports '$dt'. Nothing was written." ;;
        "") say "camera does not report a device-tree compatible - platform NOT verified" ;;
        *ingenic*|*Ingenic*)
            # Another Ingenic part, or a board whose compatible names only the
            # reference design. The binary may still be wrong, but the vendor
            # matches and this installer has no table to judge the rest by.
            say "camera reports '$dt' (Ingenic, not t40) - platform NOT verified" ;;
        *)
            # A different vendor entirely. This bundle is a MIPS o32 binary for
            # an Ingenic T40; on a SigmaStar or HiSilicon board it cannot even
            # be executed. A first cut only shrugged here and let the install
            # proceed - that is the two-cameras-on-a-desk mistake, and it is
            # the one this check exists for.
            grep -qi "T40" "$HERE/BUILDINFO" &&
                die "this bundle is built for T40, but the camera reports '$dt'. Nothing was written."
            say "camera reports '$dt' - platform NOT verified" ;;
    esac
fi

mkdir -p "$STATE_DIR" "$BACKUP" || die "cannot create $STATE_DIR"

# --------------------------------------------- remember the previous state ---
# Only on the very first install, so re-running the installer (or upgrading)
# never overwrites what the camera looked like before Machino existed.
if [ ! -f "$STATE_DIR/streamer.preinstall" ]; then
    if [ -f "$INITD/S95majestic" ] && [ -x "$INITD/S95majestic" ]; then
        printf 'majestic-auto\n' > "$STATE_DIR/streamer.preinstall"
    elif [ -f "$INITD/S95majestic" ]; then
        printf 'majestic-disabled\n' > "$STATE_DIR/streamer.preinstall"
    else
        printf 'none\n' > "$STATE_DIR/streamer.preinstall"
    fi
    say "recorded pre-install state: $(cat "$STATE_DIR/streamer.preinstall")"
fi

# ------------------------------------------------------------ the daemon ---
mkdir -p "$ROOT/usr/bin" "$ROOT/usr/sbin" "$ROOT/var/run"

# AP21: keep the binary we are about to replace, so an upgrade that installs
# cleanly but will not run has somewhere to go back to. Only the DAEMON is kept
# - it is the one file whose failure costs the camera its stream, and the one
# whose size makes keeping several copies a problem on 4.6 MB of flash.
#
# Overwritten on every install on purpose: the useful rollback target is the
# version that was working five minutes ago, not the one from three upgrades
# back. machino-manager restores it when post-install verification fails.
# UMBENENNEN statt kopieren. Eine Kopie braucht Binary-Groesse EXTRA freien
# Platz -- auf dieser Kamera hiess das: Install-ueber-Install unmoeglich, weil
# 2x 2,9 MB nie gleichzeitig frei sind. move_file ist auf demselben Overlay ein
# rename (Fallback cp+rm nur fuer den squashfs-Lower-Layer-Fall). Das kleine
# Fenster, in dem /usr/bin/machino fehlt, schliesst der put-Schritt direkt
# danach; der Daemon ist zu diesem Zeitpunkt ohnehin gestoppt.
if [ -f "$ROOT/usr/bin/machino" ]; then
    if move_file "$ROOT/usr/bin/machino" "$BACKUP/machino.prev"; then
        say "kept the previous daemon as $BACKUP/machino.prev"
    else
        # Not fatal: a camera with no room for a spare copy still deserves the
        # upgrade. But it must be SAID, because the rollback will not be there.
        say "could not keep a copy of the previous daemon - no rollback target"
    fi
fi

put 0755 "$HERE/machino" "$ROOT/usr/bin/machino" || die "cannot install $ROOT/usr/bin/machino"
if [ -f "$STATE_DIR/machino.conf" ]; then
    say "keeping existing $STATE_DIR/machino.conf"          # upgrade-safe: never clobber a user config
    put 0644 "$HERE/machino.conf" "$STATE_DIR/machino.conf.default"
else
    # Fresh install: always keep the shipped default for reference. If the camera
    # already runs Majestic, import its majestic.yaml one-way; fall back to the
    # default if that produces nothing (e.g. staging install, no native binary).
    put 0644 "$HERE/machino.conf" "$STATE_DIR/machino.conf.default"
    if [ -f "$ROOT/etc/majestic.yaml" ] &&
       "$ROOT/usr/bin/machino" --migrate-majestic "$ROOT/etc/majestic.yaml" -o "$STATE_DIR/machino.conf" 2>"$STATE_DIR/migrate.log" &&
       [ -s "$STATE_DIR/machino.conf" ]; then
        chmod 0644 "$STATE_DIR/machino.conf"
        say "migrated $ROOT/etc/majestic.yaml -> $STATE_DIR/machino.conf (report in $STATE_DIR/migrate.log)"
    else
        [ -f "$ROOT/etc/majestic.yaml" ] && warn "majestic.yaml present but migration produced nothing; using defaults"
        put 0644 "$HERE/machino.conf" "$STATE_DIR/machino.conf"
    fi
fi
# The config MUST select hardware, or the daemon refuses to start:
#
#   [ERR] MAIN hardware resolution failed: no platform
#   [ERR] MAIN refusing to start: no guessing of buses or pins
#
# That refusal is right - guessing i2c buses and reset pins is how you get a
# camera that half-works. But two paths above produce exactly such a config:
#
#   * the majestic.yaml migration, because majestic.yaml has no notion of a
#     board profile and therefore cannot supply one;
#   * "keeping existing machino.conf" on a camera whose config came from that
#     migration in an earlier version.
#
# The consequence was measured on the T40NN and is worse than a failed start:
# install.sh reports success, machino-manager then switches the streamer over,
# machino does not come up, the rollback to majestic runs - and the camera
# RESETS. From the outside that looks like "the install hangs", which is what it
# was diagnosed as for a long time.
#
# So: if nothing selects hardware, take the line out of the shipped default.
# This ADDS a mandatory key that is missing; it clobbers no user setting, which
# is what "upgrade-safe" was protecting.
_conf="$STATE_DIR/machino.conf"
if [ -f "$_conf" ] &&
   ! grep -qE '^[[:space:]]*(board|board_profile_file|platform)[[:space:]]*=' "$_conf"; then
    _board=$(sed -n 's/^[[:space:]]*board[[:space:]]*=[[:space:]]*\([^[:space:]#]*\).*/\1/p' \
             "$HERE/machino.conf" | head -n1)
    if [ -n "$_board" ]; then
        _t="$_conf.machino-new.$$"
        { echo "# added by install.sh: the daemon refuses to start without one"
          echo "board = $_board"
          cat "$_conf"; } > "$_t" && chmod 0644 "$_t" && mv -f "$_t" "$_conf" &&
            say "config selected no hardware - added 'board = $_board' from the shipped default" ||
            { rm -f "$_t"; die "cannot add the missing board selection to $_conf"; }
    else
        # Refuse rather than install something that cannot start: the operator
        # gets a config to fix instead of a camera that resets on switch-over.
        die "no hardware selection in $_conf and none in the bundle's default - set 'board = <profile>' and run again"
    fi
fi

# --------------------------------------------------- machino's own pages ---
#
# Zwei haserl-Seiten im Webroot der Kamera, exakt nach dem Muster von
# wireguard.cgi (common/header/footer-Includes): damit sind Head, Navbar,
# Theme und main.js im ERSTEN HTML, relative Links haben denselben
# Basiskontext wie jede Stock-Seite, und es gibt keine zweite WebUI.
#
# Die Grenze, praezisiert am 2026-09-25: OpenIPC-eigene Dateien bleiben
# byte-identisch -- Machino darf EIGENE Dateien hinzufuegen, und sein
# Uninstall entfernt sie restlos. Vorher gab es zwei Anlaeufe ohne eigene
# Dateien (nachgebaute Leiste, clientseitig uebernommene Leiste); beide sind
# im Browser gescheitert und dokumentiert in docs/openipc-webui-assets.md.
for _pg in machino-uplinks.cgi machino-cellular.cgi machino-usb.cgi machino-devices.cgi machino-dyndns.cgi machino-ai.cgi; do
    if [ -r "$HERE/www/$_pg" ]; then
        put 0755 "$HERE/www/$_pg" "$CGI/$_pg" || die "cannot install $CGI/$_pg"
        say "installed $CGI/$_pg"
    else
        # Ein Bundle ohne die Seiten installiert trotzdem -- die API und die
        # Weiterleitungen funktionieren, nur die Seiten fehlen. Gesagt wird es.
        say "bundle has no www/$_pg - page not installed"
    fi
done

# Das CGI-Shim, das OpenIPCs sh-Backends (cgi-bin/j/*.cgi) die von ihnen
# erwartete GET_/POST_-Umgebung gibt, die busybox httpd nicht setzt. machinos
# Front-Door schreibt /cgi-bin/j/<x>.cgi hierauf um (PATH_INFO). Ohne das ist
# der File Manager unbenutzbar (files.cgi listet stur /, download sagt "not a
# file"). Eine machino-eigene cgi-bin-Datei wie die Seiten.
[ -r "$HERE/www/machino-cgi-run.cgi" ] || die "the bundle has no www/machino-cgi-run.cgi"
put 0755 "$HERE/www/machino-cgi-run.cgi" "$CGI/machino-cgi-run.cgi" ||
    die "cannot install $CGI/machino-cgi-run.cgi"
say "installed $CGI/machino-cgi-run.cgi"

if [ -d "$HERE/profiles" ]; then
    mkdir -p "$STATE_DIR/profiles"
    for p in "$HERE"/profiles/*; do [ -f "$p" ] && put 0644 "$p" "$STATE_DIR/profiles/${p##*/}"; done
fi

put 0755 "$HERE/sbin/streamerctl" "$ROOT/usr/sbin/streamerctl" || die "cannot install streamerctl"
# The Cam-Tool's control surface + a stored uninstaller, so status/uninstall
# work on the camera later without redeploying the bundle.
put 0755 "$HERE/sbin/machino-manager" "$ROOT/usr/sbin/machino-manager" || die "cannot install machino-manager"
[ -r "$HERE/uninstall.sh" ] && put 0755 "$HERE/uninstall.sh" "$STATE_DIR/uninstall.sh"
# A webui.passwd from an older bundle would only confuse a reader - the Basic
# auth layer it fed is gone (Machino's session login owns authentication now).
rm -f "$STATE_DIR/webui.passwd"
put 0755 "$HERE/init/machino" "$INITD/machino"          || die "cannot install $INITD/machino"

# ------------------------------------------------- take over the boot slot ---
# rcS runs every /etc/init.d/S??* without testing the executable bit, so the
# only reliable way to keep Majestic from auto-starting is to take it out of
# that glob. It stays fully usable as /etc/init.d/majestic.
if [ -f "$INITD/S95majestic" ]; then
    [ -f "$BACKUP/S95majestic" ] || cp -p "$INITD/S95majestic" "$BACKUP/S95majestic"
    move_file "$INITD/S95majestic" "$INITD/majestic" || die "cannot move S95majestic aside"
    chmod 0755 "$INITD/majestic"
    say "moved $INITD/S95majestic -> $INITD/majestic (a backup is in $BACKUP)"
fi
put 0755 "$HERE/init/S95streamer" "$INITD/S95streamer" || die "cannot install S95streamer"

# ------------------------------------------------------------- USB WiFi ---
#
# Alles wird installiert, nichts wird eingeschaltet. machino-usb-helper liest
# usb.mode aus machino.conf und kehrt bei "off" sofort zurueck -- kein Modul,
# kein Portstrom, kein Daemon. Der Schalter sitzt in der UI.
#
# Die Module muessen gegen genau diesen Kernel gebaut sein (vermagic, und
# CONFIG_MODVERSIONS=n macht vermagic zum ganzen ABI-Vertrag). Das Bundle
# bringt die Fassung mit, die CI fuer den OpenIPC-T40-Kernel gebaut hat.
# Der Boot-Helfer zuerst, und zwar IMMER -- er gehoert zu keiner der beiden
# Nutzlasten, sondern entscheidet zwischen ihnen. Ohne ihn waere usb.mode ein
# Wert, den niemand liest.
[ -r "$HERE/init/S42usb" ] || die "the bundle has no init/S42usb"
[ -r "$HERE/sbin/machino-device" ] || die "the bundle has no sbin/machino-device"
put 0755 "$HERE/sbin/machino-device" "$ROOT/usr/sbin/machino-device" ||
    die "cannot install machino-device"
# Das Manifest ist die einzige Quelle fuer Profilname, Modulreihenfolge und
# USB-Id -- Shell und C++ lesen dieselbe Datei. Es gehoert NICHT in den
# Nutzlast-Zweig: ohne Manifest kann der Geraetemanager nicht einmal sagen,
# WELCHES Geraet ihm fehlt, und meldete "nicht unterstuetzt" statt "keine
# Nutzlast in diesem Release".
[ -r "$HERE/devices/aic8800.manifest" ] || die "the bundle has no devices/aic8800.manifest"
mkdir -p "$STATE_DIR/devices" 2>/dev/null
put 0644 "$HERE/devices/aic8800.manifest" "$STATE_DIR/devices/aic8800.manifest" ||
    die "cannot install the aic8800 manifest"

# Der Absichtsausfuehrer beim Boot. Er MUSS vor S40network laufen, sonst
# griffe ein frisch gesetztes wlandev erst einen Neustart spaeter -- die
# Begruendung steht im Skript selbst.
[ -r "$HERE/init/S39machinodev" ] || die "the bundle has no init/S39machinodev"
put 0755 "$HERE/init/S39machinodev" "$INITD/S39machinodev" ||
    die "cannot install S39machinodev"
[ -r "$HERE/sbin/machino-usb-helper" ] || die "the bundle has no sbin/machino-usb-helper"
put 0755 "$HERE/sbin/machino-usb-helper" "$ROOT/usr/sbin/machino-usb-helper" ||
    die "cannot install machino-usb-helper"
put 0755 "$HERE/init/S42usb" "$INITD/S42usb" || die "cannot install S42usb"

# DynDNS: eigenstaendiger Updater + Boot-Huelle. Gehoert zum Kern (kleiner
# Shell-Dienst, keine Nutzlast). Die Config schreibt die WebUI-Seite; ohne
# dyndns.conf (enabled) tut der Updater beim Boot nichts.
[ -r "$HERE/sbin/machino-dyndns" ] || die "the bundle has no sbin/machino-dyndns"
put 0755 "$HERE/sbin/machino-dyndns" "$ROOT/usr/sbin/machino-dyndns" ||
    die "cannot install machino-dyndns"
[ -r "$HERE/init/S49dyndns" ] || die "the bundle has no init/S49dyndns"
put 0755 "$HERE/init/S49dyndns" "$INITD/S49dyndns" || die "cannot install S49dyndns"

# Das Vorgaengerskript MUSS weg, und das ist kein Aufraeumen.
#
# S42wifi und S42usb liegen beide in /etc/init.d und werden beide gestartet.
# Nach einem Upgrade liefen also zwei Skripte: eines liest usb.wifi.enabled,
# das andere usb.mode. Steht dort "cellular", laedt das alte trotzdem den
# AIC8800 -- und das Modem bekaeme einen Port, an dem schon ein WLAN-Treiber
# haengt. Genau die widerspruechliche Doppelwahrheit, gegen die usb.mode
# eingefuehrt wurde.
#
# Dasselbe fuer S41hostapd, einen Entwurf aus der AP-Vorarbeit: hostapd wird
# heute vom Rollen-Supervisor gestartet, und ein zweites hostapd auf demselben
# Interface ist ein AP, der hochkommt und sofort wieder wegbricht.
for _stale in S42wifi S41hostapd; do
    if [ -f "$INITD/$_stale" ]; then
        rm -f "$INITD/$_stale" && say "removed the superseded $INITD/$_stale"
    fi
done

if [ "$WITH_WIFI_PAYLOAD" = "1" ]; then
    [ -r "$HERE/sbin/machino-wifi-role" ] || die "the bundle has no sbin/machino-wifi-role"
    put 0755 "$HERE/sbin/machino-wifi-role" "$ROOT/usr/sbin/machino-wifi-role" ||
        die "cannot install machino-wifi-role"
    [ -r "$HERE/udhcpc-wlan.script" ] &&
        { put 0755 "$HERE/udhcpc-wlan.script" "$STATE_DIR/udhcpc-wlan.script" ||
          warn "could not install the udhcpc hook - the WiFi default route will have no metric"; }

    # Die Nutzlast liegt DAUERHAFT unter /etc/machino/payload/<id>/ und
    # ueberlebt jedes Deinstallieren. Die Registrierung (Kopie nach
    # /lib/modules + Profil) kommt und geht.
    #
    # Eine fruehere Fassung legte die Module nur nach /lib/modules und entfernte
    # sie beim Deinstallieren -- danach war "Installieren" unmoeglich, weil
    # nichts mehr da war, was man haette eintragen koennen. Install -> Uninstall
    # -> Install muss ohne neues Bundle gehen.
    _mods=0
    for _ko in "$HERE"/wifi/modules/*.ko; do
        [ -r "$_ko" ] || continue
        put 0644 "$_ko" "$STATE_DIR/payload/aic8800/$(basename "$_ko")" ||
            die "cannot install $(basename "$_ko")"
        _mods=$((_mods + 1))
    done

    # Harte Bedingung, keine Warnung.
    #
    # Vorher meldete der Installer "0 module(s)" und lief weiter; das Ergebnis
    # war eine Kamera, auf der das Profil eingetragen war, die Module aber
    # fehlten -- adapter_scan verwirft es dann an der have-Pruefung und im
    # Dropdown steht weiter "None", ohne dass irgendwo ein Fehler steht. Wer
    # bewusst ohne WLAN ausliefern will, nimmt --without-wifi-payload; dann
    # kommt dieser Block gar nicht erst dran.
    [ "$_mods" -gt 0 ] ||
        die "the bundle carries no WiFi kernel modules. Build them against this exact
     kernel (see the build-aic8800-t40 workflow) or install with
     --without-wifi-payload if this artefact is meant to ship without WiFi."

    # Ohne Firmware bindet der Treiber und scheitert danach: der Chip laedt
    # sein Image beim Probe. Ein Modul ohne Blobs ist schlimmer als keines,
    # weil es wie ein Hardwarefehler aussieht.
    # Die Namen der Schleifenvariablen sind hier nicht beliebig: der Zielname
    # wird VOR dem put berechnet und in einer eigenen Variablen gehalten. Die
    # erste Fassung las ihn per $(basename "$_d") als Argument von put, und put
    # selbst benutzte damals ebenfalls ein _d -- ab dem zweiten Durchlauf zeigte
    # es auf das Ziel des letzten Kopiervorgangs.
    _fw=0
    if [ -d "$HERE/wifi/firmware" ]; then
        for _fwdir in "$HERE"/wifi/firmware/*; do
            [ -d "$_fwdir" ] || continue
            _fwname=$(basename "$_fwdir")
            for _fwfile in "$_fwdir"/*; do
                [ -f "$_fwfile" ] || continue
                put 0644 "$_fwfile" "$ROOT/lib/firmware/$_fwname/$(basename "$_fwfile")" ||
                    die "cannot install firmware $(basename "$_fwfile")"
                _fw=$((_fw + 1))
            done
        done
    fi

    _ap=0
    if [ -r "$HERE/wifi/hostapd" ]; then
        put 0755 "$HERE/wifi/hostapd" "$ROOT/usr/sbin/hostapd" || die "cannot install hostapd"
        _ap=1
    elif [ -r "$HERE/hostapd" ]; then
        # Aeltere Bundles legten es flach ab.
        put 0755 "$HERE/hostapd" "$ROOT/usr/sbin/hostapd" || die "cannot install hostapd"
        _ap=1
    fi
    # Kein hostapd_cli. machino spricht den ctrl-Socket ueber wpa_ctrl.cpp
    # selbst an, und der Rollen-Supervisor startet hostapd frisch, statt es
    # fernzusteuern. Auf einem 8,7-MB-Overlay sind 150 KB fuer ein Werkzeug,
    # das niemand aufruft, keine gute Entscheidung.

    say "installed the WiFi payload: $_mods module(s), $_fw firmware file(s), hostapd $([ "$_ap" = 1 ] && echo yes || echo no)"

    # Registrieren, damit die UNVERAENDERTE OpenIPC-Netzwerkseite den Adapter
    # nach dieser Installation anbietet. Bewusst ueber dasselbe Skript, das der
    # Device Manager und der Boot-Helfer benutzen: eine zweite Fassung dieser
    # Logik hier waere genau die Drift, die uns den Profilnamen sechsmal
    # eingebracht hat.
    MACHINO_ROOT="$ROOT" sh "$ROOT/usr/sbin/machino-device" install aic8800 ||
        warn "could not register the WiFi adapter with OpenIPC - the network page will not offer it"
else
    say "skipped the WiFi payload (--without-wifi-payload)"
fi

# ------------------------------------------------------------- Mobilfunk ---
#
# Dieselbe Regel wie beim WLAN: die Dateien liegen bereit und tun nichts. Wer
# Mobilfunk benutzt, waehlt es in AP-M6 ueber usb.function aus; bis dahin
# startet diesen Helfer niemand. Ein Helfer, der nicht laeuft, kostet nichts
# ausser dem Platz -- und ein Helfer, der erst nachinstalliert werden muss,
# macht den spaeteren Schalter zur Attrappe.
if [ "$WITH_CELL_PAYLOAD" = "1" ] &&
   { [ -d "$HERE/cellular" ] || [ -r "$HERE/sbin/machino-cellular-helper" ]; }; then
    [ -r "$HERE/sbin/machino-cellular-helper" ] &&
        { put 0755 "$HERE/sbin/machino-cellular-helper" "$ROOT/usr/sbin/machino-cellular-helper" ||
          die "cannot install machino-cellular-helper"; }
    [ -r "$HERE/udhcpc-cellular.script" ] &&
        { put 0755 "$HERE/udhcpc-cellular.script" "$STATE_DIR/udhcpc-cellular.script" ||
          warn "could not install the cellular udhcpc hook - the modem route would have no metric"; }

    # PPP. Die Hooks muessen nach /etc/ppp -- pppd sucht dort und nirgends
    # sonst, der Pfad ist fest einkompiliert. Auf diesem Image benutzt sonst
    # nichts PPP; der Uninstaller raeumt sie wieder weg.
    #
    # ip-up sagt machino, wie das Interface heisst und welche Adresse es
    # bekommen hat -- ppp0 ist der haeufige Fall und nicht der einzige, und ein
    # geratener Name zeigte auf eine fremde Verbindung.
    if [ -r "$HERE/ppp-ip-up.script" ]; then
        put 0755 "$HERE/ppp-ip-up.script"   "$ROOT/etc/ppp/ip-up" ||
            warn "could not install /etc/ppp/ip-up - a PPP call would come up without machino noticing"
        put 0755 "$HERE/ppp-ip-down.script" "$ROOT/etc/ppp/ip-down" ||
            warn "could not install /etc/ppp/ip-down - a dropped PPP call would keep reporting an address"
    fi
    # pppd selbst, falls das Bundle es mitbringt. Viele OpenIPC-Images haben
    # keines, und dann ist die PPP-Auswahl eine Attrappe -- also wird es
    # mitgeliefert und hier abgelegt.
    _ppp=0
    if [ -r "$HERE/cellular/pppd" ]; then
        put 0755 "$HERE/cellular/pppd" "$ROOT/usr/sbin/pppd" || die "cannot install pppd"
        _ppp=1
    fi
    # chat(8) fuehrt die Wahl-Unterhaltung. Ohne es kommt pppd nie bis zum
    # CONNECT, und der Fehler saehe aus wie ein totes Modem.
    if [ -r "$HERE/cellular/chat" ]; then
        put 0755 "$HERE/cellular/chat" "$ROOT/usr/sbin/chat" || die "cannot install chat"
    fi
    _cmods=0
    for _cko in "$HERE"/cellular/modules/*.ko; do
        [ -r "$_cko" ] || continue
        put 0644 "$_cko" "$STATE_DIR/modules/$(basename "$_cko")" ||
            die "cannot install $(basename "$_cko")"
        _cmods=$((_cmods + 1))
    done
    say "installed the cellular payload: $_cmods module(s), pppd $([ "$_ppp" = 1 ] && echo yes || echo "no - PPP will say so")"
    if [ "$_cmods" = "0" ]; then
        warn "no modem kernel modules in the bundle - selecting cellular will find nothing to load."
        warn "they must be built against this exact kernel; see the build-modem-modules-t40 workflow"
    fi
elif [ "$WITH_CELL_PAYLOAD" != "1" ]; then
    say "skipped the cellular payload (--without-cellular-payload)"
fi

# ------------------------------------------------------------ NNA (KI) ---
#
# Der Inferenzhelfer und ein mitgeliefertes Modell. Nur auf ausdruecklichen
# Wunsch (--with-nna-payload): mehrere MB auf einem fast vollen Overlay.
# Modelle landen unter /etc/machino/models und UEBERLEBEN ein Deinstallieren
# -- dieselbe Regel wie bei der USB-Nutzlast: Nutzdaten des Betreibers
# verschwinden nicht, weil eine Software geht. Der Helfer selbst wird beim
# Deinstallieren entfernt.
if [ "$WITH_NNA_PAYLOAD" = "1" ]; then
    if [ -r "$HERE/nna/machino-nna" ]; then
        put 0755 "$HERE/nna/machino-nna" "$ROOT/usr/sbin/machino-nna" ||
            die "cannot install machino-nna"
        say "installed the NNA inference helper"
    else
        warn "--with-nna-payload, but the bundle carries no nna/machino-nna."
        warn "the AI page will say the helper is missing; see build-nna-t40"
    fi
    _nmods=0
    for _nbin in "$HERE"/nna/*.bin; do
        [ -r "$_nbin" ] || continue
        mkdir -p "$ROOT/etc/machino/models"
        put 0644 "$_nbin" "$ROOT/etc/machino/models/$(basename "$_nbin")" ||
            die "cannot install model $(basename "$_nbin")"
        _nmods=$((_nmods + 1))
    done
    [ "$_nmods" -gt 0 ] && say "installed $_nmods detection model(s) into /etc/machino/models"
    # Der Kerneltreiber (soc-nna.ko) ist bewusst NICHT im Bundle: seine Quelle
    # ist oeffentlich noch nicht gefunden (OpenIPC #2031), und Binaermodule
    # gehoeren erst nach der Hardware-Abnahme hinein. Die KI-Seite zeigt den
    # Zustand ehrlich an.
fi

# ------------------------------------------------------ IPsec (WeirdIKE) ---
#
# Eigener Daemon, eigenes Initskript, eigenes Paket -- Machino enthaelt
# keinerlei VPN-Code (weirdike-openipc/README). Nur auf Wunsch. Die Config
# mit dem PSK legt der Betreiber selbst an; installiert wird nur das
# .example daneben, eine vorhandene Config wird NIE angefasst.
if [ "$WITH_WEIRDIKE" = "1" ]; then
    [ -r "$HERE/weirdike/weirdiked" ] ||
        die "--with-weirdike, but the bundle carries no weirdike/weirdiked (see build-weirdike-t40)"
    put 0755 "$HERE/weirdike/weirdiked"   "$ROOT/usr/sbin/weirdiked"   || die "cannot install weirdiked"
    put 0755 "$HERE/weirdike/weirdikectl" "$ROOT/usr/sbin/weirdikectl" || die "cannot install weirdikectl"
    put 0755 "$HERE/weirdike/S99weirdike" "$ROOT/etc/init.d/S99weirdike" || die "cannot install S99weirdike"
    if [ -r "$HERE/weirdike/ipsec.cgi" ]; then
        put 0755 "$HERE/weirdike/ipsec.cgi" "$CGI/ipsec.cgi" || die "cannot install ipsec.cgi"
    fi
    mkdir -p "$ROOT/etc/weirdike"
    put 0600 "$HERE/weirdike/weirdike.conf.example" "$ROOT/etc/weirdike/weirdike.conf.example" ||
        die "cannot install weirdike.conf.example"
    # Userspace-ESP laeuft ueber TUN; das Modul liegt im Image, wird aber
    # nicht geladen. Eine Zeile in /etc/modules laedt es bei S35modules --
    # idempotent, und der Uninstaller entfernt genau diese Zeile wieder.
    if ! grep -qx tun "$ROOT/etc/modules" 2>/dev/null; then
        echo tun >> "$ROOT/etc/modules" || die "cannot add tun to /etc/modules"
    fi
    say "installed weirdike (daemon idle until /etc/weirdike/weirdike.conf exists, mode 0600)"
fi

# ---------------------------------------------------------- USB-Auswahl ---
#
# Nur wenn ausdruecklich gewuenscht. Ohne --usb-mode bleibt stehen, was in der
# Datei steht -- eine Neuinstallation ueber eine bestehende hinweg darf die
# Wahl des Betreibers nicht zuruecksetzen, und eine Erstinstallation hat mit
# "kein Schluessel" ohnehin off.
if [ -n "$USB_MODE" ]; then
    case "$USB_MODE" in
        off) ;;
        wifi)
            [ "$WITH_WIFI_PAYLOAD" = "1" ] ||
                die "--usb-mode=wifi together with --without-wifi-payload: that would select a radio whose driver is not installed"
            ;;
        cellular)
            [ "$WITH_CELL_PAYLOAD" = "1" ] ||
                die "--usb-mode=cellular together with --without-cellular-payload: that would select a modem whose driver is not installed"
            ;;
        *) die "--usb-mode must be off, wifi or cellular, not '$USB_MODE'" ;;
    esac
    set_conf usb.mode "$USB_MODE" || die "cannot set usb.mode"
    # Der Spiegel fuer den Fall, dass jemand ein Bundle von vor AP-M6 darueber
    # installiert: dessen S42wifi liest nur diesen Schluessel. Geschrieben,
    # nie gelesen -- usb.mode entscheidet.
    if [ "$USB_MODE" = "wifi" ]; then
        set_conf usb.wifi.enabled true || die "cannot set usb.wifi.enabled"
    else
        set_conf usb.wifi.enabled false || die "cannot set usb.wifi.enabled"
    fi
    say "usb.mode = $USB_MODE (takes effect at the next boot)"
fi

# ------------------------------------------------------- initial selection ---
# Keep streaming whatever streams right now. Installing must not switch.
if [ ! -f "$STATE_DIR/streamer" ]; then
    if pgrep -x majestic >/dev/null 2>&1; then sel=majestic
    elif [ "$(cat "$STATE_DIR/streamer.preinstall")" = "majestic-auto" ]; then sel=majestic
    else sel=none
    fi
    printf '%s\n' "$sel" > "$STATE_DIR/streamer"
    say "initial selection: $sel (unchanged - nothing was started or stopped)"
fi

# ----------------------------------------------- clean up legacy WebUI bits ---
# Earlier bundles shipped a standalone /cgi-bin/machino.cgi page and injected a
# "Media service" entry (with CSS :has() tricks) into p/header.cgi. That approach
# is gone; remove any leftovers so upgrading from such a build restores the stock
# WebUI. The proper Machino WebUI adaptation replaces this, not a single page.
rm -f "$CGI/machino.cgi"
legacy_header="$CGI/p/header.cgi"
if [ -f "$legacy_header" ] && grep -q 'machino:begin' "$legacy_header" 2>/dev/null; then
    sed '/machino:begin/,/machino:end/d' "$legacy_header" > "$legacy_header.machino.tmp" &&
        mv "$legacy_header.machino.tmp" "$legacy_header" && say "removed the legacy WebUI menu entry" ||
        { rm -f "$legacy_header.machino.tmp"; warn "could not remove the legacy menu entry from $legacy_header"; }
fi

# ------------------------------------------ the network page's menu entry ---
# Opt-in, marked, and removed by exactly the same markers on uninstall.
#
# The default is to leave p/header.cgi byte-identical. An installer that edits
# the stock navigation makes a later upgrade of the stock WebUI either revert
# the change or conflict with it, and a user who did not ask for it should not
# find their files modified. The page works either way; this only adds the
# link.
netpage_header="$CGI/p/header.cgi"

# Eine Funktion, zwei Aufrufer. Die zweite Seite hat die Wahl gelassen zwischen
# "denselben awk-Block noch einmal" und dem hier; der erste Weg ist genau die
# Art Kopie, die uns den Profilnamen sechsmal eingebracht hat. Der Marker
# traegt den Seitennamen, damit der Deinstallierer beide Bloecke einzeln findet.
#
#   $1 Marke ("netpage" / "devpage")   $2 Schalter, der ihn angefordert hat
#   $3 Ziel-URL                        $4 Beschriftung
menu_entry() {
    _mark=$1; _flag=$2; _href=$3; _label=$4
    if [ ! -f "$netpage_header" ]; then
        warn "$_flag given but $netpage_header does not exist - no menu entry added"
        return 0
    fi
    if grep -q "machino-$_mark:begin" "$netpage_header" 2>/dev/null; then
        say "the $_mark menu entry is already there"
        return 0
    fi
    # Anchored on the System dropdown, which is where these pages belong and
    # the one anchor this WebUI has had in every version we have seen. If it
    # is not there we do NOT guess another spot: a menu entry in the wrong
    # place is worse than none, and the page is still reachable by URL.
    _anchor='<li><a class="dropdown-item" href="network.cgi">'
    if ! grep -qF "$_anchor" "$netpage_header"; then
        warn "the System menu in $netpage_header does not look as expected - no menu entry added"
        warn "the page is still reachable at http://<camera>$_href"
        return 0
    fi
    awk -v anchor="$_anchor" -v mark="$_mark" -v flag="$_flag" \
        -v href="$_href" -v label="$_label" '
        { print }
        index($0, anchor) && !done {
            print "<!-- machino-" mark ":begin (added by machino install.sh " flag ") -->"
            print "<li><a class=\"dropdown-item\" href=\"" href "\">" label "</a></li>"
            print "<!-- machino-" mark ":end -->"
            done = 1
        }
    ' "$netpage_header" > "$netpage_header.machino.tmp" &&
        mv "$netpage_header.machino.tmp" "$netpage_header" &&
        say "added $_label to the System menu (marked machino-$_mark)" ||
        { rm -f "$netpage_header.machino.tmp"; warn "could not add the menu entry to $netpage_header"; }
}

if [ "$WITH_NETPAGE" = "1" ]; then
    menu_entry netpage --with-network-page machino-usb.cgi "USB (machino)"
fi
if [ "$WITH_DEVPAGE" = "1" ]; then
    menu_entry devpage --with-device-page machino-devices.cgi "Ger&auml;te (machino)"
fi

# ------------------------------------------------------------------- done ---
say ""
say "Machino is installed. Nothing was switched over."
say ""
STREAMERCTL_ROOT="$ROOT" "$ROOT/usr/sbin/streamerctl" status
say ""
say "Next:"
say "  streamerctl set machino      # switch the camera to Machino"
say "  streamerctl set majestic     # switch it back"
say ""
say "Uninstall with ./uninstall.sh from this directory."
