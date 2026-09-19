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

# BusyBox has no install(1) - found out on the camera, not in review. Everything
# this script needs must exist there, so it is checked up front instead of
# failing halfway through with part of the files in place.
for c in cp chmod mkdir mv rm grep sed awk df wc cat dirname; do
    command -v "$c" >/dev/null 2>&1 || die "required command '$c' not found on this system"
done

# cp+chmod instead of install(1), for the same reason.
put() { _m=$1; _s=$2; _d=$3; mkdir -p "$(dirname "$_d")" && cp "$_s" "$_d" && chmod "$_m" "$_d"; }

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
if [ "$WEBUI_ONLY" = "1" ]; then
    [ -r "$HERE/webui/machino.cgi" ] || die "bundle incomplete: webui/machino.cgi is missing"
else
    for f in machino machino.conf sbin/streamerctl sbin/machino-manager init/S95streamer init/machino webui/machino.cgi; do
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

    if grep -q "machino:begin" "$header" 2>/dev/null; then say "menu entry already present"; return 0; fi
    [ -f "$BACKUP/header.cgi" ] || cp -p "$header" "$BACKUP/header.cgi"

    # The label is written out literally instead of going through page_label(),
    # so p/pages.cgi stays untouched: one stock file less to patch and restore.
    anchor='<ul aria-labelledby="dropdownSystem" class="dropdown-menu">'
    grep -qF "$anchor" "$header" || { warn "navigation layout not recognised - page reachable at /cgi-bin/machino.cgi only"; return 1; }

    # The injected block is haserl (header.cgi is rendered by haserl per request):
    # when Machino is the active streamer it labels the entry "Camera (Machino)"
    # and hides the stock Majestic menu items (which write /etc/majestic.yaml and
    # are wrong under Machino) via a CSS :has() rule; otherwise it is the plain
    # "Media service" switch link and Majestic stays untouched. Removed verbatim
    # by uninstall (between the markers), so the stock header restores byte-exact.
    blk="$STATE_DIR/.menu-block"
    cat > "$blk" <<'BLK'
							<!-- machino:begin -->
							<% if [ "$(cat /etc/machino/streamer 2>/dev/null)" = "machino" ]; then %>
							<li><a class="dropdown-item" href="machino.cgi">Camera (Machino)</a></li>
							<style>.nav-item.dropdown:has(a[href*="majestic"]),li:has(> a[href*="majestic"]){display:none!important}</style>
							<% else %>
							<li><a class="dropdown-item" href="machino.cgi">Media service</a></li>
							<% fi %>
							<!-- machino:end -->
BLK
    awk -v anchor="$anchor" -v blk="$blk" '
        { print }
        index($0, anchor) {
            while ((getline line < blk) > 0) print line
            close(blk)
        }' "$header" > "$header.machino.tmp" && mv "$header.machino.tmp" "$header" || {
        warn "could not patch the navigation"; rm -f "$header.machino.tmp" "$blk"; return 1; }
    rm -f "$blk"
    say "added the Machino menu entry under System (hides the Majestic menu when Machino is active)"
}

if [ "$WEBUI_ONLY" = "1" ]; then
    put 0755 "$HERE/webui/machino.cgi" "$CGI/machino.cgi" || die "could not install the WebUI page"
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
if [ -d "$HERE/profiles" ]; then
    mkdir -p "$STATE_DIR/profiles"
    for p in "$HERE"/profiles/*; do [ -f "$p" ] && put 0644 "$p" "$STATE_DIR/profiles/${p##*/}"; done
fi

put 0755 "$HERE/sbin/streamerctl" "$ROOT/usr/sbin/streamerctl" || die "cannot install streamerctl"
# The Cam-Tool's control surface + a stored uninstaller, so status/uninstall
# work on the camera later without redeploying the bundle.
put 0755 "$HERE/sbin/machino-manager" "$ROOT/usr/sbin/machino-manager" || die "cannot install machino-manager"
[ -r "$HERE/uninstall.sh" ] && put 0755 "$HERE/uninstall.sh" "$STATE_DIR/uninstall.sh"
if [ -n "$WEBUI_PASSWORD" ]; then
    STREAMERCTL_ROOT="$ROOT" "$ROOT/usr/sbin/streamerctl" webui-password "$WEBUI_PASSWORD" ||
        warn "could not set the WebUI password - set it later with: streamerctl webui-password <password>"
fi
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
put 0755 "$HERE/webui/machino.cgi" "$CGI/machino.cgi" || warn "could not install the WebUI page"

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
