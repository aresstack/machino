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
STATE_DIR="$ROOT/etc/machino"
WWW="$ROOT/var/www"
CGI="$WWW/cgi-bin"
INITD="$ROOT/etc/init.d"
BACKUP="$STATE_DIR/backup"
MARK_BEGIN="<!-- machino:begin -->"
MARK_END="<!-- machino:end -->"

say()  { echo "$*"; }
warn() { echo "install: $*" >&2; }
die()  { echo "install: $*" >&2; exit 1; }

WEBUI_ONLY=0
WEBUI_PASSWORD=""
while [ $# -gt 0 ]; do
    case "$1" in
        --webui-only)     WEBUI_ONLY=1 ;;
        --webui-password) shift; WEBUI_PASSWORD="${1:-}" ;;
        -h|--help)
            cat <<EOF
usage: ./install.sh [--webui-password PASSWORD] [--webui-only]

  --webui-password P  password for the switch page while Machino serves the
                      WebUI (user 'root'). Without it that page is reachable
                      from the camera itself only.
  --webui-only        re-add just the WebUI page and menu entry, e.g. after an
                      OpenIPC WebUI update removed them. Touches nothing else.
EOF
            exit 0 ;;
        *) die "unknown option '$1' (try --help)" ;;
    esac
    shift
done

[ -n "$ROOT" ] || [ "$(id -u)" = "0" ] || die "run as root"

# ------------------------------------------------------------- preflight ---
if [ "$WEBUI_ONLY" = "1" ]; then
    [ -r "$HERE/webui/machino.cgi" ] || die "bundle incomplete: webui/machino.cgi is missing"
else
    for f in machino machino.conf sbin/streamerctl init/S95streamer init/machino webui/machino.cgi; do
        [ -r "$HERE/$f" ] || die "bundle incomplete: $f is missing"
    done
fi
[ -d "$CGI" ] || die "no $CGI - this does not look like an OpenIPC camera with the WebUI installed"

if [ "$WEBUI_ONLY" != "1" ]; then
    need_kb=$(( ($(wc -c < "$HERE/machino") / 1024) + 256 ))
    free_kb=$(df -k / | awk 'NR==2 {print $4}')
    [ "${free_kb:-0}" -ge "$need_kb" ] || die "not enough space on / (need ~${need_kb} kB, have ${free_kb} kB)"
fi

mkdir -p "$STATE_DIR" "$BACKUP" || die "cannot create $STATE_DIR"

# The OpenIPC WebUI has no plugin registry - the ext-*.cgi files are tombstones
# for renamed pages, not extension points. The navigation in p/header.cgi is a
# hand-written list, so the menu entry means editing one stock file. The edit is
# fenced with markers and the original is backed up. A WebUI update overwrites
# that file and the entry is simply gone; the page itself keeps working at
# /cgi-bin/machino.cgi and "install.sh --webui-only" puts the entry back.
webui_menu_add() {
    header="$CGI/p/header.cgi"
    [ -w "$header" ] || { warn "cannot write $header - the page stays reachable at /cgi-bin/machino.cgi"; return 1; }

if [ "$WEBUI_ONLY" = "1" ]; then
    install -m 0755 "$HERE/webui/machino.cgi" "$CGI/machino.cgi" || die "could not install the WebUI page"
    webui_menu_add || true
    say "WebUI page and menu entry refreshed; nothing else was touched."
    exit 0
fi

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
install -m 0755 "$HERE/machino" "$ROOT/usr/bin/machino" || die "cannot install $ROOT/usr/bin/machino"
if [ -f "$STATE_DIR/machino.conf" ]; then
    say "keeping existing $STATE_DIR/machino.conf"
    install -m 0644 "$HERE/machino.conf" "$STATE_DIR/machino.conf.default"
else
    install -m 0644 "$HERE/machino.conf" "$STATE_DIR/machino.conf"
fi
if [ -d "$HERE/profiles" ]; then
    mkdir -p "$STATE_DIR/profiles"
    for p in "$HERE"/profiles/*; do [ -f "$p" ] && install -m 0644 "$p" "$STATE_DIR/profiles/"; done
fi

install -m 0755 "$HERE/sbin/streamerctl" "$ROOT/usr/sbin/streamerctl" || die "cannot install streamerctl"
install -m 0755 "$HERE/init/machino" "$INITD/machino"          || die "cannot install $INITD/machino"

# ------------------------------------------------- take over the boot slot ---
# rcS runs every /etc/init.d/S??* without testing the executable bit, so the
# only reliable way to keep Majestic from auto-starting is to take it out of
# that glob. It stays fully usable as /etc/init.d/majestic.
if [ -f "$INITD/S95majestic" ]; then
    [ -f "$BACKUP/S95majestic" ] || cp -p "$INITD/S95majestic" "$BACKUP/S95majestic"
    mv "$INITD/S95majestic" "$INITD/majestic" || die "cannot move S95majestic aside"
    chmod 0755 "$INITD/majestic"
    say "moved $INITD/S95majestic -> $INITD/majestic (a backup is in $BACKUP)"
fi
install -m 0755 "$HERE/init/S95streamer" "$INITD/S95streamer" || die "cannot install S95streamer"

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

# ------------------------------------------------------------------ WebUI ---
install -m 0755 "$HERE/webui/machino.cgi" "$CGI/machino.cgi" || warn "could not install the WebUI page"

    if grep -q "machino:begin" "$header" 2>/dev/null; then say "menu entry already present"; return 0; fi
    [ -f "$BACKUP/header.cgi" ] || cp -p "$header" "$BACKUP/header.cgi"

    # The label is written out literally instead of going through page_label(),
    # so p/pages.cgi stays untouched: one stock file less to patch and restore.
    anchor='<ul aria-labelledby="dropdownSystem" class="dropdown-menu">'
    grep -qF "$anchor" "$header" || { warn "navigation layout not recognised - page reachable at /cgi-bin/machino.cgi only"; return 1; }

    awk -v anchor="$anchor" -v b="$MARK_BEGIN" -v e="$MARK_END" '
        { print }
        index($0, anchor) {
            print "							" b
            print "							<li><a class=\"dropdown-item\" href=\"machino.cgi\">Media service</a></li>"
            print "							" e
        }' "$header" > "$header.machino.tmp" && mv "$header.machino.tmp" "$header" || {
        warn "could not patch the navigation"; rm -f "$header.machino.tmp"; return 1; }
    say "added the menu entry under System"
}

webui_menu_add || true

# ------------------------------------------------------------------- done ---
say ""
say "Machino is installed. Nothing was switched over."
say ""
STREAMERCTL_ROOT="$ROOT" "$ROOT/usr/sbin/streamerctl" status
say ""
say "Next:"
say "  streamerctl set machino      # switch the camera to Machino"
say "  streamerctl set majestic     # switch it back"
say "  WebUI: System -> Media service"
say ""
say "Uninstall with ./uninstall.sh from this directory."
