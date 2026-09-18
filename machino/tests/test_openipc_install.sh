#!/bin/sh
# Host tests for openipc/install.sh and openipc/uninstall.sh against a fake
# camera tree (MACHINO_ROOT). No camera, no root.
#
# What matters here is that the installer is reversible and that it does not
# take the camera off the air: it must not start or stop anything, and the
# uninstaller must hand back exactly the state that was recorded at install
# time - including "Majestic was deliberately off".
set -u

SRC=$(cd "$(dirname "$0")/.." && pwd)
PKG="$SRC/openipc"
PASS=0; FAIL=0; SKIP=0
ok()  { PASS=$((PASS + 1)); }
skip(){ SKIP=$((SKIP + 1)); echo "SKIP: $*"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL: $*" >&2; }
is()  { if [ "$2" = "$3" ]; then ok; else bad "$1: expected '$3', got '$2'"; fi; }
has() { if [ -e "$2" ]; then ok; else bad "$1: $2 is missing"; fi; }
hasnt() { if [ -e "$2" ]; then bad "$1: $2 should be gone"; else ok; fi; }

# Repo-local: $TMPDIR is not writable everywhere the tests run, and a work
# tree next to the tests is easier to inspect when something fails.
WORK="$SRC/tests/tmp-openipc-install"
rm -rf "$WORK"; mkdir -p "$WORK" || { echo "cannot create $WORK" >&2; exit 1; }
trap 'rm -rf "$WORK"' EXIT INT TERM

# Not every filesystem the tests run on represents the executable bit (MSYS2
# does not). The "Majestic was deliberately disabled" case is built on that bit,
# so where it cannot be represented the case is skipped - reporting it as a
# product failure would be wrong.
XBIT=yes
printf '#!/bin/sh
' > "$WORK/.xprobe"; chmod 0644 "$WORK/.xprobe"
[ -x "$WORK/.xprobe" ] && XBIT=no
rm -f "$WORK/.xprobe"

# A bundle as the CI produces it, with a stand-in for the binary.
make_bundle() {
    B="$WORK/bundle"; rm -rf "$B"; mkdir -p "$B/sbin" "$B/init" "$B/webui"
    printf '#!/bin/sh\necho fake machino\n' > "$B/machino"; chmod +x "$B/machino"
    printf 'board = t40nn-imx307-board-a\napi.port = 8080\n' > "$B/machino.conf"
    cp "$PKG/sbin/streamerctl" "$B/sbin/"
    cp "$PKG/init/S95streamer" "$PKG/init/machino" "$B/init/"
    cp "$PKG/webui/machino.cgi" "$B/webui/"
    cp "$PKG/install.sh" "$PKG/uninstall.sh" "$B/"
    chmod +x "$B/install.sh" "$B/uninstall.sh" "$B/sbin/streamerctl" "$B/init/"*
}

# A camera as found in the field. majestic_state: auto | disabled | absent
make_camera() {
    R="$WORK/root"; rm -rf "$R"
    mkdir -p "$R/etc/init.d" "$R/var/www/cgi-bin/p" "$R/usr/bin" "$R/usr/sbin" "$R/var/run"
    case "${1:-auto}" in
        auto)     printf '#!/bin/sh\nexit 0\n' > "$R/etc/init.d/S95majestic"; chmod 0755 "$R/etc/init.d/S95majestic" ;;
        disabled) printf '#!/bin/sh\nexit 0\n' > "$R/etc/init.d/S95majestic"; chmod 0644 "$R/etc/init.d/S95majestic" ;;
        absent)   : ;;
    esac
    # the part of the navigation the installer anchors on
    cat > "$R/var/www/cgi-bin/p/header.cgi" <<'EOF'
					<li class="nav-item dropdown">
						<a id="dropdownSystem" role="button">System</a>
						<ul aria-labelledby="dropdownSystem" class="dropdown-menu">
							<li><a class="dropdown-item" href="network.cgi">Network</a></li>
						</ul>
					</li>
EOF
    cp "$R/var/www/cgi-bin/p/header.cgi" "$WORK/header.orig"
}

run_install()   { ( cd "$WORK/bundle" && MACHINO_ROOT="$WORK/root" sh ./install.sh   "$@" ) >"$WORK/out" 2>&1; }
run_uninstall() { ( cd "$WORK/bundle" && MACHINO_ROOT="$WORK/root" sh ./uninstall.sh "$@" ) >"$WORK/out" 2>&1; }

# ------------------------------------ 1) install on a normal camera ---------
make_bundle; make_camera auto
run_install || bad "install.sh exited non-zero: $(cat "$WORK/out")"
R="$WORK/root"
has   "binary installed"        "$R/usr/bin/machino"
has   "streamerctl installed"   "$R/usr/sbin/streamerctl"
has   "boot script installed"   "$R/etc/init.d/S95streamer"
has   "machino init installed"  "$R/etc/init.d/machino"
has   "config installed"        "$R/etc/machino/machino.conf"
has   "webui page installed"    "$R/var/www/cgi-bin/machino.cgi"
has   "majestic moved aside"    "$R/etc/init.d/majestic"
hasnt "majestic out of the boot slot" "$R/etc/init.d/S95majestic"
has   "backup kept"             "$R/etc/machino/backup/S95majestic"
is    "pre-install state recorded" "$(cat "$R/etc/machino/streamer.preinstall")" "majestic-auto"
is    "selection unchanged"        "$(cat "$R/etc/machino/streamer")"            "majestic"
if grep -q 'machino:begin' "$R/var/www/cgi-bin/p/header.cgi"; then ok; else bad "menu entry not added"; fi
if grep -q 'href="machino.cgi"' "$R/var/www/cgi-bin/p/header.cgi"; then ok; else bad "menu link not added"; fi

# ------------------------------------ 2) installing twice is harmless -------
run_install || bad "second install.sh exited non-zero: $(cat "$WORK/out")"
is "menu entry added once" "$(grep -c 'machino:begin' "$R/var/www/cgi-bin/p/header.cgi")" "1"
is "pre-install state kept" "$(cat "$R/etc/machino/streamer.preinstall")" "majestic-auto"
has "majestic still aside" "$R/etc/init.d/majestic"

# ------------------------------------ 3) a user config survives an upgrade --
printf 'board = my-own-board\n' > "$R/etc/machino/machino.conf"
run_install
is  "user config kept" "$(cat "$R/etc/machino/machino.conf")" "board = my-own-board"
has "new default written next to it" "$R/etc/machino/machino.conf.default"

# ------------------------------------ 4) uninstall restores everything ------
run_uninstall || bad "uninstall.sh exited non-zero: $(cat "$WORK/out")"
has   "majestic back in the boot slot" "$R/etc/init.d/S95majestic"
hasnt "boot script removed"            "$R/etc/init.d/S95streamer"
hasnt "binary removed"                 "$R/usr/bin/machino"
hasnt "streamerctl removed"            "$R/usr/sbin/streamerctl"
hasnt "webui page removed"             "$R/var/www/cgi-bin/machino.cgi"
hasnt "state dir removed"              "$R/etc/machino"
if grep -q 'machino' "$R/var/www/cgi-bin/p/header.cgi"; then bad "menu entry left behind"; else ok; fi
if diff -q "$WORK/header.orig" "$R/var/www/cgi-bin/p/header.cgi" >/dev/null; then ok; else bad "header.cgi not byte-identical after uninstall"; fi
if [ -x "$R/etc/init.d/S95majestic" ]; then ok; else bad "majestic auto-start not restored"; fi

# --------------- 5) a deliberately disabled majestic stays disabled ---------
if [ "$XBIT" = yes ]; then
    make_bundle; make_camera disabled
    run_install
    is "disabled state recorded" "$(cat "$R/etc/machino/streamer.preinstall")" "majestic-disabled"
    is "nothing selected"        "$(cat "$R/etc/machino/streamer")"            "none"
    run_uninstall
    has "majestic file restored" "$R/etc/init.d/S95majestic"
    if [ -x "$R/etc/init.d/S95majestic" ]; then bad "majestic was switched on although it had been disabled"; else ok; fi
else
    skip "disabled-majestic case (this filesystem does not represent the executable bit)"
fi

# ------------------------------------ 6) --keep-config keeps the config -----
make_bundle; make_camera auto
run_install
printf 'board = keepme\n' > "$R/etc/machino/machino.conf"
run_uninstall --keep-config
has "config kept" "$R/etc/machino/machino.conf"
is  "config unchanged" "$(cat "$R/etc/machino/machino.conf")" "board = keepme"

# ------------------------------------ 7) refuse an incomplete bundle --------
make_bundle; make_camera auto
rm -f "$WORK/bundle/sbin/streamerctl"
if run_install; then bad "incomplete bundle was accepted"; else ok; fi
hasnt "nothing installed from a broken bundle" "$R/etc/init.d/S95streamer"

# ------------------------------------ 8) refuse a camera without the WebUI --
make_bundle; make_camera auto
rm -rf "$R/var/www"
if run_install; then bad "camera without WebUI was accepted"; else ok; fi

# ------------------ 9) an unknown navigation layout is not fatal ------------
make_bundle; make_camera auto
printf '<nav>something else entirely</nav>\n' > "$R/var/www/cgi-bin/p/header.cgi"
run_install || bad "install failed on an unknown navigation layout"
has "page still installed" "$R/var/www/cgi-bin/machino.cgi"
if grep -q 'not recognised' "$WORK/out"; then ok; else bad "unknown layout was not reported"; fi

# ------------------ 10) --webui-only really only touches the WebUI ----------
make_bundle; make_camera auto
( cd "$WORK/bundle" && MACHINO_ROOT="$WORK/root" sh ./install.sh --webui-only ) >"$WORK/out" 2>&1 ||
    bad "--webui-only exited non-zero: $(cat "$WORK/out")"
has   "page installed"                   "$R/var/www/cgi-bin/machino.cgi"
hasnt "no binary from --webui-only"      "$R/usr/bin/machino"
hasnt "no boot script from --webui-only" "$R/etc/init.d/S95streamer"
hasnt "majestic not moved by --webui-only" "$R/etc/init.d/majestic"
has   "majestic left in its boot slot"   "$R/etc/init.d/S95majestic"
if grep -q 'machino:begin' "$R/var/www/cgi-bin/p/header.cgi"; then ok; else bad "--webui-only did not add the menu entry"; fi

# ------- 11) uninstall refuses to continue while machino is still running ---
# Continuing would restore majestic's boot slot and could start it next to a
# machino that still owns the media hardware.
make_bundle; make_camera auto
run_install
MSTUB="$WORK/stub"; mkdir -p "$MSTUB"
printf '#!/bin/sh
case "$*" in *machino*) echo 1234; exit 0 ;; esac
exit 1
' > "$MSTUB/pgrep"
chmod +x "$MSTUB/pgrep"
if ( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$WORK/root" sh ./uninstall.sh ) >"$WORK/out" 2>&1; then
    bad "uninstall continued although machino was still running"
else
    ok
fi
has   "majestic still out of the boot slot"               "$R/etc/init.d/majestic"
hasnt "boot slot not restored behind a running machino"   "$R/etc/init.d/S95majestic"
has   "binary not removed"                                "$R/usr/bin/machino"
if grep -q 'still running' "$WORK/out"; then ok; else bad "no explanation why uninstall stopped"; fi
rm -rf "$MSTUB"

# --- 12) a WebUI update that dropped the markers is not rolled back ---------
make_bundle; make_camera auto
run_install
printf '<nav>a newer webui</nav>
' > "$R/var/www/cgi-bin/p/header.cgi"
run_uninstall
is "updated header kept" "$(cat "$R/var/www/cgi-bin/p/header.cgi")" "<nav>a newer webui</nav>"

if [ "$SKIP" -gt 0 ]; then
    echo "openipc install tests: $PASS passed, $FAIL failed, $SKIP skipped"
else
    echo "openipc install tests: $PASS passed, $FAIL failed"
fi
[ "$FAIL" -eq 0 ]
