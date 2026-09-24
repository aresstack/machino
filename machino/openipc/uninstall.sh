#!/bin/sh
# Remove Machino from an OpenIPC camera and put the streamer setup back the way
# it was found.
#
# "The way it was found" is taken literally: if Majestic was deliberately
# disabled before Machino was installed, it is not switched on here. The state
# recorded at install time decides, not a guess.
set -u

ROOT="${MACHINO_ROOT:-}"            # test hook, see install.sh
STATE_DIR="$ROOT/etc/machino"
WWW="$ROOT/var/www"
CGI="$WWW/cgi-bin"
INITD="$ROOT/etc/init.d"
BACKUP="$STATE_DIR/backup"
KEEP_CONFIG=0

say()  { echo "$*"; }
warn() { echo "uninstall: $*" >&2; }
die()  { echo "uninstall: $*" >&2; exit 1; }

[ -n "$ROOT" ] || [ "$(id -u)" = "0" ] || die "run as root"

# see install.sh: overlayfs on kernel 4.4 can refuse rename(2) with EINVAL
move_file() {
    _s=$1; _d=$2
    mv "$_s" "$_d" 2>/dev/null && return 0
    cp -p "$_s" "$_d" && rm -f "$_s"
}

# Same compatibility rule as streamerctl/init: on this BusyBox pgrep, machino
# must be matched by its executable path, not "pgrep -x machino" (its argv0 is
# the full path). This is the safety/rollback path, so it must use the verified
# matcher too.
machino_running() { pgrep -f /usr/bin/machino >/dev/null 2>&1; }

# MACHINO_ROOT heisst: Dateien in einen Wegwerfbaum, und sonst NICHTS anfassen.
#
# Das stand so schon in install.sh ("everything the installer touches goes
# through it") und war fuer Prozesse schlicht falsch. Ein Sandbox-Uninstall hat
# auf der Kamera den echten machino gestoppt und wlan0 heruntergefahren: die
# Init-Skripte liegen zwar unter $ROOT, aber ihr stop() arbeitet mit absoluten
# Pfaden -- /var/run/..., pgrep, ip link. Und der Zweig, der Majestics
# Boot-Slot wiederherstellt, haette Majestic auf der echten Kamera GESTARTET,
# neben einem laufenden machino. Zwei Besitzer der Medienhardware ist genau
# das, was dieses Paket verhindern soll.
#
# Deshalb: kein Start, kein Stop, kein kill, solange MACHINO_ROOT gesetzt ist.
# Dateien werden weiterhin vollstaendig behandelt, damit der Test etwas wert
# bleibt.
sandboxed() { [ -n "$ROOT" ]; }
run_live() {
    if sandboxed; then
        say "sandbox ($ROOT): uebersprungen -- $*"
        return 0
    fi
    # Die Ausgabe wird HIER unterdrueckt, nicht an der Aufrufstelle. Stuende
    # dort ein >/dev/null, verschluckte es auch die Sandbox-Meldung, und der
    # Test koennte nicht mehr pruefen, dass wirklich nichts passiert ist.
    "$@" >/dev/null 2>&1
}
case "${1:-}" in --keep-config) KEEP_CONFIG=1 ;; esac

preinstall=none
[ -r "$STATE_DIR/streamer.preinstall" ] && preinstall=$(cat "$STATE_DIR/streamer.preinstall")
say "pre-install state was: $preinstall"

# ------------------------------------------------- stop Machino completely ---
# Do this first: the media hardware must be free before anything else may claim
# it, and before the init script that knows how to stop it is removed.
if [ -x "$INITD/machino" ]; then
    run_live "$INITD/machino" stop
fi
i=0
while machino_running && [ $i -lt 15 ]; do i=$((i + 1)); sleep 1; done
if machino_running; then
    # Hard stop. Continuing would restore Majestic's boot slot and possibly
    # start it next to a Machino that still owns the media hardware - two media
    # owners, which is the one thing this whole package exists to prevent.
    # Nothing has been changed at this point, so aborting is safe.
    warn "machino is still running and would not stop"
    warn "nothing was changed. Stop it and run uninstall.sh again:"
    warn "    /etc/init.d/machino stop     # or: kill \$(pgrep -f /usr/bin/machino)"
    exit 1
fi

# ---------------------------------------------------- restore the boot slot ---
rm -f "$INITD/S95streamer"

# The USB boot script. Stopped first so no supplicant, no hostapd and no DHCP
# client is left running against a machino that is going away. Its stop tears
# down BOTH stacks regardless of the selected mode, which is what we want here.
#
# S42wifi is the name this had before AP-M6. An installation that was never
# upgraded still has it, and leaving it behind would leave a boot script that
# loads a WiFi driver for a machino that is no longer installed.
for _s in S42usb S42wifi; do
    if [ -f "$INITD/$_s" ]; then
        run_live "$INITD/$_s" stop
        rm -f "$INITD/$_s"
    fi
done
rm -f "$ROOT/usr/sbin/machino-usb-helper" "$ROOT/usr/sbin/machino-wifi-role"
rm -f "$STATE_DIR/udhcpc-wlan.script" "$STATE_DIR/wifi-role"

# Die Treiber und die Firmware. Die installiert machino jetzt selbst, also
# raeumt machino sie auch wieder weg -- rund 1 MB auf einem Overlay, das
# knapp ist, liegen zu lassen waere unhoeflich.
#
# Entfernt werden NUR die Dateien, die dieses Paket kennt. Ein anderes
# aic8800.ko, das jemand von Hand dorthin gelegt hat, traegt denselben Namen;
# das ist hinnehmbar, weil das Verzeichnis /etc/machino/modules uns gehoert.
# /lib/firmware gehoert uns nicht, deshalb dort nur das eine Unterverzeichnis.
if [ -d "$STATE_DIR/modules" ]; then
    rm -f "$STATE_DIR/modules/aic8800.ko" "$STATE_DIR/modules/aic_load_fw.ko"
    rmdir "$STATE_DIR/modules" 2>/dev/null || true
fi
rm -rf "$ROOT/lib/firmware/aic8800DC"

# Mobilfunk: Helfer, DHCP-Hook und die Modem-Kernelmodule. Auch das haben wir
# installiert, also raeumen wir es ab.
run_live pkill -f machino-cellular-helper
rm -f "$ROOT/usr/sbin/machino-cellular-helper"
rm -f "$STATE_DIR/udhcpc-cellular.script" "$STATE_DIR/cellular-dhcp"
if [ -d "$STATE_DIR/modules" ]; then
    for _m in option usb_wwan usbnet cdc_ether usbserial; do
        rm -f "$STATE_DIR/modules/$_m.ko"
    done
    rmdir "$STATE_DIR/modules" 2>/dev/null || true
fi

# hostapd itself, if --with-access-point put one there. The role supervisor
# has already been stopped above, so nothing is serving from it any more.
rm -f "$ROOT/usr/sbin/hostapd" "$ROOT/usr/sbin/hostapd_cli"
rm -f "$STATE_DIR/hostapd.conf" "$STATE_DIR/udhcpd.conf"
rm -f "$INITD/S41hostapd"      # superseded by S42usb + the role supervisor

if [ -f "$INITD/majestic" ]; then
    move_file "$INITD/majestic" "$INITD/S95majestic" || warn "could not move majestic back into the boot slot"
elif [ -f "$BACKUP/S95majestic" ]; then
    cp -p "$BACKUP/S95majestic" "$INITD/S95majestic" || warn "could not restore S95majestic from the backup"
fi

case "$preinstall" in
    majestic-auto)
        [ -f "$INITD/S95majestic" ] && chmod 0755 "$INITD/S95majestic"
        say "Majestic auto-start restored (it was enabled before the install)"
        # Bring it back up now, so the camera is not left without a streamer.
        if ! pgrep -x majestic >/dev/null 2>&1 && [ -x "$INITD/S95majestic" ]; then
            run_live "$INITD/S95majestic" start || warn "majestic did not start - try: /etc/init.d/S95majestic start"
        fi
        ;;
    majestic-disabled)
        # It was off on purpose. Leaving the file non-executable reproduces what
        # was found, even though rcS ignores the bit: the camera is handed back
        # exactly as it was, not "helpfully" switched on.
        [ -f "$INITD/S95majestic" ] && chmod 0644 "$INITD/S95majestic"
        say "Majestic stays disabled (it was disabled before the install)"
        ;;
    *)
        say "no Majestic auto-start was present before the install - none restored"
        ;;
esac

# ------------------------------------------------------------------ WebUI ---
rm -f "$CGI/machino.cgi"
header="$CGI/p/header.cgi"
# The network page entry, if --with-network-page added one. Removed by its own
# markers so it cannot take a neighbouring edit with it, and before the legacy
# cleanup below because the two use different markers on purpose.
if [ -f "$header" ] && grep -q 'machino-netpage:begin' "$header" 2>/dev/null; then
    sed '/machino-netpage:begin/,/machino-netpage:end/d' "$header" > "$header.machino.tmp" &&
        mv "$header.machino.tmp" "$header" && say "removed the network page menu entry" ||
        { rm -f "$header.machino.tmp"; warn "could not remove the network page entry from $header"; }
fi
if [ -f "$header" ] && grep -q 'machino:begin' "$header" 2>/dev/null; then
    sed '/machino:begin/,/machino:end/d' "$header" > "$header.machino.tmp" &&
        mv "$header.machino.tmp" "$header" && say "removed the menu entry" ||
        { rm -f "$header.machino.tmp"; warn "could not remove the menu entry from $header"; }
elif [ -f "$header" ]; then
    # No markers: either the entry was never added, or a WebUI update has since
    # replaced the file. Copying the old backup over it would roll that update
    # back, so the current file is left alone. The backup stays in
    # $BACKUP/header.cgi as a recovery artefact, not as an automatic fallback.
    [ -f "$BACKUP/header.cgi" ] && say "no machino markers in $header - left untouched (backup kept in $BACKUP)"
fi

# Stop the WebUI host we may have started; Majestic serves port 80 itself.
if [ -r "$ROOT/var/run/machino-httpd.pid" ]; then
    pid=$(cat "$ROOT/var/run/machino-httpd.pid" 2>/dev/null)
    # Auch das nicht in der Sandbox: die Zahl in der Datei ist eine PID auf dem
    # ECHTEN System, egal unter welchem Wurzelverzeichnis die Datei liegt.
    [ -n "${pid:-}" ] && ! sandboxed && kill "$pid" 2>/dev/null
    rm -f "$ROOT/var/run/machino-httpd.pid"
fi

# ----------------------------------------------------------------- files ---
rm -f "$ROOT/usr/bin/machino" "$ROOT/usr/sbin/streamerctl" "$ROOT/usr/sbin/machino-manager" "$INITD/machino"
if [ "$KEEP_CONFIG" = "1" ]; then
    say "keeping $STATE_DIR (configuration and board profiles)"
else
    rm -rf "$STATE_DIR"
fi
rm -f "$ROOT/var/log/machino.log" "$ROOT/var/run/machino.pid"

say ""
say "Machino removed."
if pgrep -x majestic >/dev/null 2>&1; then
    say "Majestic is running and serves the WebUI on port 80."
else
    say "No media service is running right now."
    [ "$preinstall" = "majestic-auto" ] && say "Start it with: /etc/init.d/S95majestic start"
fi
