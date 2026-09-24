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
    B="$WORK/bundle"; rm -rf "$B"; mkdir -p "$B/sbin" "$B/init"
    # a stand-in for the binary that answers --version and --migrate-majestic,
    # so machino-manager's live checks and the installer's migration path work
    cat > "$B/machino" <<'FAKE'
#!/bin/sh
case "${1:-}" in
  --version|-V) echo "machino 0.11.0-test" ;;
  --migrate-majestic)
    shift; in=""; out=""
    while [ $# -gt 0 ]; do case "$1" in -o) shift; out="${1:-}" ;; *) [ -z "$in" ] && in="$1" ;; esac; shift; done
    [ -n "$out" ] && printf '# machino.conf migrated from majestic.yaml\nboard = t40nn-imx307-board-a\nvideo.fps = 25\n' > "$out"
    echo "migrated ${in:-?} -> ${out:-stdout}" >&2 ;;
  *) echo "fake machino" ;;
esac
FAKE
    chmod +x "$B/machino"
    printf 'board = t40nn-imx307-board-a\napi.port = 8080\n' > "$B/machino.conf"
    cp "$PKG/sbin/streamerctl" "$PKG/sbin/machino-manager" "$B/sbin/"
    cp "$PKG/init/S95streamer" "$PKG/init/machino" "$PKG/init/S42wifi" "$B/init/"
    mkdir -p "$B/sbin"; cp "$PKG/sbin/machino-wifi-role" "$B/sbin/"
    # Die WLAN-Nutzlast so, wie das Release-Artefakt sie traegt: Treiber,
    # Firmware und hostapd unter wifi/. Sie wird per Default installiert und
    # ist ohne usb.wifi.enabled=true wirkungslos.
    mkdir -p "$B/wifi/modules" "$B/wifi/firmware/aic8800DC"
    printf 'fake-hostapd\n'   > "$B/wifi/hostapd"
    printf 'fake-aic8800\n'   > "$B/wifi/modules/aic8800.ko"
    printf 'fake-loadfw\n'    > "$B/wifi/modules/aic_load_fw.ko"
    # MEHRERE Firmware-Dateien, und das ist kein Zufall: mit genau einer ist
    # eine Schleife, die sich ihr eigenes Zielverzeichnis ueberschreibt, von
    # einer korrekten nicht zu unterscheiden. Genau so ist der Fehler
    # durchgerutscht, bei dem jede Datei in einem Verzeichnis landete, das nach
    # der vorherigen benannt war -- gefunden erst in einer Sandbox auf der
    # Kamera, mit 19 echten Blobs.
    printf 'fake-blob-1\n'    > "$B/wifi/firmware/aic8800DC/fmacfw.bin"
    printf 'fake-blob-2\n'    > "$B/wifi/firmware/aic8800DC/fmacfw_patch.bin"
    printf 'fake-blob-3\n'    > "$B/wifi/firmware/aic8800DC/fw_adid.bin"
    cp "$PKG/udhcpc-wlan.script" "$B/"
    cp "$PKG/install.sh" "$PKG/uninstall.sh" "$B/"
    chmod +x "$B/install.sh" "$B/uninstall.sh" "$B/sbin/streamerctl" "$B/sbin/machino-manager" "$B/init/"*
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

# The stand-in for the daemon is a shell script, so AP21's ELF format check has
# to stand down here. It is exercised with the hook OFF by the two dedicated
# tests further down - one with a real MIPS header, one with an x86-64 one.
run_install()   { ( cd "$WORK/bundle" && MACHINO_ROOT="$WORK/root" MACHINO_INSTALL_SKIP_FORMAT=1 sh ./install.sh "$@" ) >"$WORK/out" 2>&1; }
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
has   "majestic moved aside"    "$R/etc/init.d/majestic"
hasnt "majestic out of the boot slot" "$R/etc/init.d/S95majestic"
has   "backup kept"             "$R/etc/machino/backup/S95majestic"
is    "pre-install state recorded" "$(cat "$R/etc/machino/streamer.preinstall")" "majestic-auto"
is    "selection unchanged"        "$(cat "$R/etc/machino/streamer")"            "majestic"
# The install must NOT touch the stock WebUI (no standalone page, no menu edit).
hasnt "no standalone webui page" "$R/var/www/cgi-bin/machino.cgi"
if diff -q "$WORK/header.orig" "$R/var/www/cgi-bin/p/header.cgi" >/dev/null; then ok; else bad "install modified the stock header.cgi"; fi

# ------------------------------------ 2) installing twice is harmless -------
run_install || bad "second install.sh exited non-zero: $(cat "$WORK/out")"
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
hasnt "state dir removed"              "$R/etc/machino"
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

# --------- 9) install cleans up legacy WebUI bits from an older build --------
# A camera that ran an earlier bundle has a standalone machino.cgi and a menu
# block injected into header.cgi. Installing the current bundle must remove both
# so the stock WebUI is restored (the proper Machino WebUI adaptation replaces
# that approach, not a single injected page).
make_bundle; make_camera auto
printf '#!/usr/bin/haserl\nlegacy machino page\n' > "$R/var/www/cgi-bin/machino.cgi"
cat > "$R/var/www/cgi-bin/p/header.cgi" <<'EOF'
					<li class="nav-item dropdown">
						<a id="dropdownSystem" role="button">System</a>
						<ul aria-labelledby="dropdownSystem" class="dropdown-menu">
							<li><a class="dropdown-item" href="network.cgi">Network</a></li>
							<!-- machino:begin -->
							<li><a class="dropdown-item" href="machino.cgi">Media service</a></li>
							<!-- machino:end -->
						</ul>
					</li>
EOF
run_install || bad "install failed while cleaning up legacy WebUI bits: $(cat "$WORK/out")"
hasnt "legacy machino.cgi removed"       "$R/var/www/cgi-bin/machino.cgi"
if grep -q 'machino' "$R/var/www/cgi-bin/p/header.cgi"; then bad "legacy menu block left behind"; else ok; fi

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

# ------------- 13a) install removes a legacy webui.passwd -------------------
# The Basic-auth layer it fed is gone (Machino's session login owns auth);
# a leftover file from an older bundle must not survive an upgrade.
make_bundle; make_camera auto
mkdir -p "$R/etc/machino"; printf 'root:x\n' > "$R/etc/machino/webui.passwd"
run_install || bad "install failed with a legacy webui.passwd present: $(cat "$WORK/out")"
hasnt "legacy webui.passwd removed" "$R/etc/machino/webui.passwd"

# ------------- 13b) the boot-slot move survives a failing mv -----------------
# The camera's 4.4 overlayfs refused rename(2) with EINVAL for the lower-layer
# S95majestic; the cp+rm fallback is what actually ran there.
make_bundle; make_camera auto
MVSTUB="$WORK/mvstub"; mkdir -p "$MVSTUB"
printf '#!/bin/sh
exit 1
' > "$MVSTUB/mv"; chmod +x "$MVSTUB/mv"
( cd "$WORK/bundle" && PATH="$MVSTUB:$PATH" MACHINO_ROOT="$WORK/root" MACHINO_INSTALL_SKIP_FORMAT=1 sh ./install.sh ) >"$WORK/out" 2>&1 ||
    bad "install failed although mv failure has a fallback: $(cat "$WORK/out")"
has   "boot slot moved despite broken mv" "$R/etc/init.d/majestic"
hasnt "old slot gone despite broken mv"   "$R/etc/init.d/S95majestic"
( cd "$WORK/bundle" && PATH="$MVSTUB:$PATH" MACHINO_ROOT="$WORK/root" sh ./uninstall.sh ) >"$WORK/out" 2>&1
has   "restore works despite broken mv"   "$R/etc/init.d/S95majestic"
rm -rf "$MVSTUB"

# ---- 14) majestic.yaml is migrated once on a fresh install -----------------
make_bundle; make_camera auto
printf 'video0:\n  fps: 30\n  bitrate: 3000\n' > "$R/etc/majestic.yaml"
run_install || bad "install with a majestic.yaml present failed: $(cat "$WORK/out")"
if grep -q 'migrated from majestic.yaml' "$R/etc/machino/machino.conf"; then ok; else bad "majestic.yaml was not migrated into machino.conf"; fi
has "shipped default kept for reference" "$R/etc/machino/machino.conf.default"

# ---- 15) machino-manager: live status, ownership, reversal -----------------
# ON is now "truly active": manifest + components + selected==machino + running.
# A pgrep stub stands in for the running daemon; NO_ACTIVATE writes the selection
# (host tests have no real streamer). running/selected only affect the ON verdict.
MSTUB="$WORK/mgrstub"; mkdir -p "$MSTUB"
printf '#!/bin/sh\ncase "$*" in *machino*) echo 4321; exit 0 ;; esac\nexit 1\n' > "$MSTUB/pgrep"; chmod +x "$MSTUB/pgrep"
# with the pgrep stub reporting machino "running", streamerctl status probes the
# API over the network; stub wget so that never blocks the host test.
printf '#!/bin/sh\nexit 1\n' > "$MSTUB/wget"; chmod +x "$MSTUB/wget"
mgr_status() { PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" sh "${1:-$R/usr/sbin/machino-manager}" status 2>/dev/null; }

# 15a) OFF on a bare camera
make_bundle; make_camera auto
S=$(mgr_status "$WORK/bundle/sbin/machino-manager")
case "$S" in *'"state":"OFF"'*) ok ;; *) bad "manager status not OFF on a bare camera: $S" ;; esac

# 15b) install.sh alone (no manifest) => EXTERNAL, and uninstall refuses it
run_install
S=$(mgr_status)
case "$S" in *'"state":"EXTERNAL"'*) ok ;; *) bad "unmanaged install not EXTERNAL: $S" ;; esac
# uninstall runs WITHOUT the running-stub: on the host machino is genuinely not
# running, so uninstall.sh's safety check passes (the stub would make it refuse).
if MACHINO_ROOT="$R" sh "$R/usr/sbin/machino-manager" uninstall --owner cam-tool >"$WORK/out" 2>&1; then
    bad "manager removed an EXTERNAL (unowned) install"
else ok; fi
has "external install untouched" "$R/usr/bin/machino"

# 15b-2) but switching ON *takes over* an EXTERNAL install: our bundle goes on
# top, the ownership manifest is written, the user config is untouched —
# ownership gates only the destructive direction (uninstall).
printf 'board = keep-my-board\n' > "$R/etc/machino/machino.conf"
( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" MACHINO_MANAGER_NO_ACTIVATE=1 MACHINO_INSTALL_SKIP_FORMAT=1 sh ./sbin/machino-manager install --owner cam-tool --platform t40nn ) >"$WORK/out" 2>&1 ||
    bad "manager install did not take over an EXTERNAL install: $(cat "$WORK/out")"
has "ownership manifest written on takeover" "$R/etc/machino/install-state.json"
if grep -q '"managedBy": "cam-tool"' "$R/etc/machino/install-state.json"; then ok; else bad "takeover manifest missing owner"; fi
is "user config survives the takeover" "$(cat "$R/etc/machino/machino.conf")" "board = keep-my-board"
S=$(mgr_status)
case "$S" in *'"state":"ON"'*) ok ;; *) bad "state not ON after takeover: $S" ;; esac

# 15c) a full manager install writes the ownership manifest and reports ON (active)
make_bundle; make_camera auto
( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" MACHINO_MANAGER_NO_ACTIVATE=1 MACHINO_INSTALL_SKIP_FORMAT=1 sh ./sbin/machino-manager install --owner cam-tool --platform t40nn ) >"$WORK/out" 2>&1 ||
    bad "manager install exited non-zero: $(cat "$WORK/out")"
has "ownership manifest written" "$R/etc/machino/install-state.json"
if grep -q '"managedBy": "cam-tool"' "$R/etc/machino/install-state.json"; then ok; else bad "manifest missing owner"; fi
if grep -q '"platform": "t40nn"' "$R/etc/machino/install-state.json"; then ok; else bad "manifest missing platform"; fi
is "selection switched to machino" "$(cat "$R/etc/machino/streamer")" "machino"
S=$(mgr_status)
case "$S" in *'"state":"ON"'*) ok ;; *) bad "manager status not ON after install: $S" ;; esac

# 15c-2) not selected => BROKEN, not a false ON (the hardened semantics)
printf 'majestic\n' > "$R/etc/machino/streamer"
S=$(mgr_status)
case "$S" in *'"state":"BROKEN"'*) ok ;; *) bad "selected!=machino not reported BROKEN: $S" ;; esac
printf 'machino\n' > "$R/etc/machino/streamer"

# 15d) BROKEN when a component goes missing under a valid manifest
mv "$R/usr/bin/machino" "$R/usr/bin/machino.bak"
S=$(mgr_status)
case "$S" in *'"state":"BROKEN"'*) ok ;; *) bad "missing binary not reported BROKEN: $S" ;; esac
mv "$R/usr/bin/machino.bak" "$R/usr/bin/machino"

# 15e) the wrong owner cannot uninstall; the right owner can, and it reverses
# (uninstall without the running-stub, as in 15b)
if MACHINO_ROOT="$R" sh "$R/usr/sbin/machino-manager" uninstall --owner someone-else >"$WORK/out" 2>&1; then
    bad "manager uninstalled under the wrong owner"
else ok; fi
has "still installed after refused uninstall" "$R/usr/bin/machino"
MACHINO_ROOT="$R" sh "$R/usr/sbin/machino-manager" uninstall --owner cam-tool >"$WORK/out" 2>&1 ||
    bad "manager uninstall (correct owner) failed: $(cat "$WORK/out")"
hasnt "binary removed by manager"    "$R/usr/bin/machino"
hasnt "manifest removed by manager"  "$R/etc/machino/install-state.json"
has   "majestic restored by manager" "$R/etc/init.d/S95majestic"
S=$(mgr_status "$WORK/bundle/sbin/machino-manager")
case "$S" in *'"state":"OFF"'*) ok ;; *) bad "manager status not OFF after uninstall: $S" ;; esac
rm -rf "$MSTUB"

# ---- 15f) AP21: the rollback, and the manifest it must not leave lying ------
#
# A build that installs cleanly and then will not run. The manager has to put
# the previous daemon back AND rewrite the manifest, because the manifest was
# already written with the version of the binary that just failed - leaving it
# would have the camera claim a build that is no longer on disk.
make_bundle; make_camera auto
( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" MACHINO_MANAGER_NO_ACTIVATE=1 MACHINO_INSTALL_SKIP_FORMAT=1 sh ./sbin/machino-manager install --owner cam-tool --platform t40nn ) >"$WORK/out" 2>&1
# now a "new" bundle whose daemon reports a different version and cannot be
# selected, so post-install verification fails
make_bundle
cat > "$WORK/bundle/machino" <<'BROKEN'
#!/bin/sh
case "${1:-}" in --version|-V) echo "machino 9.9.9-broken" ;; *) echo "broken" ;; esac
BROKEN
chmod +x "$WORK/bundle/machino"
before=$(cat "$R/usr/bin/machino")
( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" MACHINO_INSTALL_SKIP_FORMAT=1 sh ./sbin/machino-manager install --owner cam-tool --platform t40nn ) >"$WORK/out" 2>&1
# Without NO_ACTIVATE and without a real streamerctl target the state cannot be
# ON, so the rollback path is the one under test.
if grep -q "rolling back" "$WORK/out"; then ok; else bad "no rollback attempted: $(cat "$WORK/out")"; fi
if [ "$(cat "$R/usr/bin/machino")" = "$before" ]; then ok; else bad "the previous daemon was not restored"; fi
if grep -q '"version": "9.9.9-broken"' "$R/etc/machino/install-state.json"; then
    bad "the manifest still claims the failed build"
else ok; fi

# ---- 16) AP21: the checks that run BEFORE anything is written ---------------
#
# These are the only cases that leave MACHINO_INSTALL_SKIP_FORMAT off, so they
# are what actually exercises the format check. Each one asserts the refusal
# AND that /usr/bin/machino was never created - "Nothing was written" has to be
# true, not just printed.

# An ELF built for the build host. e_machine 3e = x86-64, which is exactly what
# a cross-build that silently fell back to the host compiler produces.
elf_header() {   # $1 = output file, $2 = e_machine byte pair (little-endian)
    printf '\177ELF\1\1\1\0\0\0\0\0\0\0\0\0\2\0' > "$1"
    printf "$2" >> "$1"
    # pad out past the 20 bytes od reads
    dd if=/dev/zero bs=1 count=64 >> "$1" 2>/dev/null
}

make_bundle; make_camera auto
elf_header "$WORK/bundle/machino" '\076\0'          # 0x003e = x86-64
( cd "$WORK/bundle" && MACHINO_ROOT="$WORK/root" sh ./install.sh ) >"$WORK/out" 2>&1
if grep -q "not a MIPS binary" "$WORK/out"; then ok; else bad "x86-64 bundle was not refused: $(cat "$WORK/out")"; fi
hasnt "nothing written for a wrong-arch bundle" "$WORK/root/usr/bin/machino"

# The same shape with the right machine number passes the format gate. It fails
# later (the stub is not a real daemon), so the assertion is only that the
# format check did NOT reject it.
make_bundle; make_camera auto
elf_header "$WORK/bundle/machino" '\010\0'          # 0x0008 = MIPS
( cd "$WORK/bundle" && MACHINO_ROOT="$WORK/root" sh ./install.sh ) >"$WORK/out" 2>&1
if grep -q "not a MIPS binary\|not a 32-bit little-endian ELF" "$WORK/out"; then
    bad "a MIPS ELF header was rejected by the format check: $(cat "$WORK/out")"
else ok; fi

# A SHA256SUMS that disagrees with the binary stops the install dead.
if command -v sha256sum >/dev/null 2>&1; then
    make_bundle; make_camera auto
    printf '%s  ./machino\n' "0000000000000000000000000000000000000000000000000000000000000000" > "$WORK/bundle/SHA256SUMS"
    run_install
    if grep -q "bundle is corrupt" "$WORK/out"; then ok; else bad "a bad hash was not refused: $(cat "$WORK/out")"; fi
    hasnt "nothing written for a corrupt bundle" "$WORK/root/usr/bin/machino"

    # And the matching hash installs. The entry must be the TOP-LEVEL ./machino:
    # the real bundle also ships ./init/machino, and a first cut of this check
    # matched any path ending in /machino and picked the wrong one.
    make_bundle; make_camera auto
    {
        printf '%s  ./init/machino\n' "1111111111111111111111111111111111111111111111111111111111111111"
        printf '%s  ./machino\n' "$(sha256sum "$WORK/bundle/machino" | awk '{print $1}')"
    } > "$WORK/bundle/SHA256SUMS"
    run_install || bad "a correct hash was refused: $(cat "$WORK/out")"
    if grep -q "sha256 verified" "$WORK/out"; then ok; else bad "hash verification did not run: $(cat "$WORK/out")"; fi
    has "installed with a verified hash" "$WORK/root/usr/bin/machino"
else
    skip "hash cases (no sha256sum on this host)"
fi

# ---- 17) AP21: the replace is atomic and leaves no litter ------------------
make_bundle; make_camera auto
run_install
found=$(find "$WORK/root" -name '*.machino-new.*' 2>/dev/null | wc -l)
is "no temporary files left behind" "$found" "0"

# ---- 18) AP21: the previous daemon is kept for a rollback ------------------
make_bundle; make_camera auto
run_install
printf 'the-old-one\n' > "$WORK/root/usr/bin/machino"
run_install
has "previous daemon kept" "$WORK/root/etc/machino/backup/machino.prev"
is  "and it is the one that was replaced" "$(cat "$WORK/root/etc/machino/backup/machino.prev")" "the-old-one"

# ---- 19) AP26: the platform check discriminates, in both directions --------
mk_dt() {   # $1 = compatible string (nul-separated, as the kernel exposes it)
    mkdir -p "$WORK/root/proc/device-tree"
    printf "$1" > "$WORK/root/proc/device-tree/compatible"
}
# BUILDINFO is what the check reads on the bundle side; the fake bundle has
# none, so give it one that names T40 exactly as CI does.
add_buildinfo() { printf "Target/platform:  T40 / T40NN (xburst2)
" > "$WORK/bundle/BUILDINFO"; }

# A different vendor must be refused. A first cut only shrugged here, which is
# the two-cameras-on-a-desk mistake this check exists for.
make_bundle; make_camera auto; add_buildinfo; mk_dt sigmastar,ssc3350
run_install
if grep -q "but the camera reports" "$WORK/out"; then ok; else bad "a SigmaStar device tree was not refused: $(cat "$WORK/out")"; fi
hasnt "nothing written for the wrong vendor" "$WORK/root/usr/bin/machino"

# Another Ingenic part, or a board whose compatible names only the reference
# design, must still install - there is no table to judge it by, and refusing
# would be a guess.
make_bundle; make_camera auto; add_buildinfo; mk_dt ingenic,shark0
run_install || bad "an Ingenic board was refused: $(cat "$WORK/out")"
has "installed on an Ingenic board without a t40 tag" "$WORK/root/usr/bin/machino"
if grep -q "platform NOT verified" "$WORK/out"; then ok; else bad "the unverified platform was not reported"; fi

# And the real thing passes.
make_bundle; make_camera auto; add_buildinfo; mk_dt ingenic,shark0ingenic,t400
run_install || bad "a t40 device tree was refused: $(cat "$WORK/out")"
has "installed on a t40" "$WORK/root/usr/bin/machino"

# ---------- 12b) the WiFi payload ships by default and stays switched off ---
#
# Beides zusammen ist der Punkt. Die Dateien muessen da sein, sonst waere der
# Schalter in der UI eine Attrappe: niemand kann per Web-Klick ein Kernelmodul
# nachliefern. Und das Radio muss trotzdem aus sein, weil es genau einen
# USB-Port gibt und der spaeter ein Modem tragen soll.
make_bundle; make_camera auto
run_install || bad "install.sh exited non-zero: $(cat "$WORK/out")"
has "wifi boot script installed by default" "$WORK/root/etc/init.d/S42wifi"
has "role supervisor installed by default"  "$WORK/root/usr/sbin/machino-wifi-role"
has "hostapd installed by default"          "$WORK/root/usr/sbin/hostapd"
has "driver installed by default"           "$WORK/root/etc/machino/modules/aic8800.ko"
has "firmware loader installed by default"  "$WORK/root/etc/machino/modules/aic_load_fw.ko"
has "firmware blob installed by default"    "$WORK/root/lib/firmware/aic8800DC/fmacfw.bin"
has "second firmware blob, same directory"  "$WORK/root/lib/firmware/aic8800DC/fmacfw_patch.bin"
has "third firmware blob, same directory"   "$WORK/root/lib/firmware/aic8800DC/fw_adid.bin"
# Und NICHTS darf ausserhalb dieses einen Verzeichnisses liegen. Der Treiber
# sucht genau dort; eine Datei daneben ist eine Datei, die er nicht findet.
n=$(find "$WORK/root/lib/firmware" -type f | grep -cv "/aic8800DC/")
if [ "$n" = "0" ]; then ok; else bad "$n firmware file(s) landed outside lib/firmware/aic8800DC"; fi
has "udhcpc hook installed by default"      "$WORK/root/etc/machino/udhcpc-wlan.script"
hasnt "no separate AP boot script"          "$WORK/root/etc/init.d/S41hostapd"
hasnt "no hostapd_cli"                      "$WORK/root/usr/sbin/hostapd_cli"

# Nothing switched it on, so nothing may claim it is on.
if grep -q "^usb.wifi.enabled = true" "$WORK/root/etc/machino/machino.conf" 2>/dev/null; then
    bad "a default install switched WiFi on"
else ok; fi

# The boot script itself has to agree: with the key absent it must do nothing
# at all. This is the guard that keeps the USB port free for the modem.
out=$(MACHINO_CONF="$WORK/root/etc/machino/machino.conf" sh -c '
    CONF="$MACHINO_CONF"
    v=$(tr -d " \t\r" < "$CONF" | sed -n "s/^usb\.wifi\.enabled=//p" | tail -1)
    if [ "$v" = "true" ] || [ "$v" = "1" ]; then echo ON; else echo OFF; fi')
if [ "$out" = "OFF" ]; then ok; else bad "the boot script would have started WiFi on a default install"; fi

# --with-wifi only pre-sets the switch; the files were already there.
make_bundle; make_camera auto
run_install --with-wifi || bad "--with-wifi was refused: $(cat "$WORK/out")"
if grep -q "^usb.wifi.enabled = true" "$WORK/root/etc/machino/machino.conf"; then ok
else bad "--with-wifi did not set usb.wifi.enabled"; fi
if grep -q "next boot" "$WORK/out"; then ok; else bad "the reboot requirement was not stated"; fi

# Setting it twice must not produce two lines: the boot script takes the last
# one, but a file that accumulates duplicates is a file nobody can read.
run_install --with-wifi || bad "a second --with-wifi run failed: $(cat "$WORK/out")"
n=$(grep -c "usb.wifi.enabled" "$WORK/root/etc/machino/machino.conf")
if [ "$n" = "1" ]; then ok; else bad "usb.wifi.enabled appears $n times after two installs"; fi

# The escape hatch, for a camera whose overlay is needed elsewhere.
make_bundle; make_camera auto
run_install --without-wifi-payload || bad "--without-wifi-payload was refused: $(cat "$WORK/out")"
hasnt "no driver when the payload is declined"  "$WORK/root/etc/machino/modules/aic8800.ko"
hasnt "no hostapd when the payload is declined" "$WORK/root/usr/sbin/hostapd"
hasnt "no boot script when declined"            "$WORK/root/etc/init.d/S42wifi"

# Switching the radio on while refusing its driver is not a configuration,
# it is a boot that fails. It has to be refused up front.
make_bundle; make_camera auto
if run_install --with-wifi --without-wifi-payload; then
    bad "--with-wifi with no payload was accepted"
else ok; fi

# A bundle with no modules must say so rather than leaving a switch that
# cannot work.
make_bundle; rm -f "$WORK/bundle/wifi/modules/"*.ko; make_camera auto
run_install || bad "install.sh exited non-zero: $(cat "$WORK/out")"
if grep -q "nothing to load" "$WORK/out"; then ok; else bad "the missing modules were not reported"; fi

# --with-access-point is still accepted so nobody's install command breaks.
make_bundle; make_camera auto
run_install --with-access-point || bad "--with-access-point was refused: $(cat "$WORK/out")"
has "hostapd there either way" "$WORK/root/usr/sbin/hostapd"

# An unknown flag must still be an error -- adding options is not a licence
# to accept anything.
make_bundle; make_camera auto
if run_install --with-acces-point; then bad "a misspelled flag was accepted"; else ok; fi

# ----------- 12d) uninstall takes the payload with it -----------------------
# It is our megabyte now, so we give it back.
make_bundle; make_camera auto
run_install || bad "install.sh exited non-zero: $(cat "$WORK/out")"
run_uninstall || bad "uninstall.sh exited non-zero: $(cat "$WORK/out")"
hasnt "wifi script removed again"   "$WORK/root/etc/init.d/S42wifi"
hasnt "supervisor removed again"    "$WORK/root/usr/sbin/machino-wifi-role"
hasnt "hostapd removed again"       "$WORK/root/usr/sbin/hostapd"
hasnt "driver removed again"        "$WORK/root/etc/machino/modules/aic8800.ko"
hasnt "loader removed again"        "$WORK/root/etc/machino/modules/aic_load_fw.ko"
hasnt "firmware removed again"      "$WORK/root/lib/firmware/aic8800DC/fmacfw.bin"

# ---------- 12e) zwei Shell-Funktionen, bei denen ein Fehler stumm ist -------
#
# Der Supervisor und S42wifi wurden bisher nur darauf geprueft, ob sie
# INSTALLIERT werden. Was sie tun, stand nirgends. Das sind aber die beiden
# Stellen, an denen ein Fehler nicht auffaellt und teuer ist: ein Geheimnis im
# Log, und ein Treiber, der laedt obwohl er nicht soll.
#
# Die Funktionen werden aus der echten Datei herausgeschnitten und ausgefuehrt,
# nicht nachgebaut -- eine Kopie im Test wuerde mitaltern, ohne es zu merken.

# redact(): wpa_supplicant druckt die beanstandete Passphrase im Klartext,
# nachgemessen auf der Kamera:
#   Line 4: Invalid passphrase length 4 (expected: 8..63) 'kurz"'.
eval "$(sed -n '/^redact() {/,/^}/p' "$PKG/sbin/machino-wifi-role")"
cat > "$WORK/leak.in" <<'LEAK'
Successfully initialized wpa_supplicant
Line 4: Invalid passphrase length 4 (expected: 8..63) 'Sparkasse2000"'.
Line 4: failed to parse psk '"Sparkasse2000"'.
Line 7: invalid WPA passphrase 'nochEinGeheimnis'
wlan0: SME: Trying to authenticate with 06:61:1d:a3:5c:19
LEAK
red=$(redact "$WORK/leak.in")
case "$red" in
    *Sparkasse2000*|*nochEinGeheimnis*) bad "the supervisor log filter let a passphrase through" ;;
    *) ok ;;
esac
# Und der Filter darf nicht so grob sein, dass die Diagnose verschwindet -- der
# Grund, warum ueberhaupt geloggt wird.
case "$red" in
    *"Invalid passphrase length 4 (expected: 8..63)"*) ok ;;
    *) bad "the filter destroyed the diagnosis it exists to preserve" ;;
esac
case "$red" in
    *"SME: Trying to authenticate with 06:61:1d:a3:5c:19"*) ok ;;
    *) bad "the filter mangled a line that has nothing to do with secrets" ;;
esac

# wifi_enabled(): entscheidet, ob beim Boot ein Kernelmodul geladen und der
# USB-Port bestromt wird. Fail-closed in jedem Zweifelsfall.
eval "$(sed -n '/^wifi_enabled() {/,/^}/p' "$PKG/init/S42wifi")"
gate() { CONF="$WORK/gate.conf"; printf '%s' "$1" > "$CONF"; if wifi_enabled; then echo AN; else echo AUS; fi; }
for case_ in \
    'usb.wifi.enabled = true|AN' \
    'usb.wifi.enabled=1|AN' \
    'board = t40nn
usb.wifi.enabled = true|AN' \
    'usb.wifi.enabled = false|AUS' \
    'board = t40nn|AUS' \
    '|AUS' \
    '#usb.wifi.enabled = true|AUS' \
    'xusb.wifi.enabled = true|AUS' \
    'usb.wifi.enabledx = true|AUS' \
    'usb.wifi.enabled = TRUE|AUS' \
    'usb.wifi.enabled = true
usb.wifi.enabled = false|AUS' \
; do
    body=${case_%|*}; want=${case_##*|}
    got=$(gate "$body")
    if [ "$got" = "$want" ]; then ok
    else bad "wifi gate: expected $want, got $got for [$(echo "$body" | tr '\n' ';')]"; fi
done
# Eine fehlende Datei ist kein "vielleicht".
CONF="$WORK/does-not-exist.conf"
if wifi_enabled; then bad "the wifi gate opened with no config file at all"; else ok; fi

# MACHINO_ROOT darf NICHTS am echten System anfassen.
#
# Das war es naemlich nicht: ein Sandbox-Uninstall hat auf der Kamera den
# laufenden machino gestoppt und wlan0 heruntergefahren. Die Init-Skripte
# liegen unter $ROOT, aber ihr stop() arbeitet mit absoluten Pfaden. Der
# Uninstaller sagt jetzt fuer jede uebersprungene Aktion, dass er sie
# uebersprungen hat -- und darauf wird hier bestanden.
make_bundle; make_camera auto
run_install >/dev/null 2>&1
run_uninstall || bad "uninstall.sh exited non-zero: $(cat "$WORK/out")"
if grep -q "sandbox.*uebersprungen" "$WORK/out"; then ok
else bad "the sandbox uninstall did not report skipping a live action - it may have performed one"; fi
if grep -q "sandbox.*uebersprungen -- .*init.d/machino stop" "$WORK/out"; then ok
else bad "stopping machino was not skipped in the sandbox"; fi

# ------- 12c) the menu entry is opt-in and header.cgi stays byte-identical --
# The installer must not edit p/header.cgi behind the user's back: a later
# upgrade of the stock WebUI would then either revert the change or conflict
# with it.
make_bundle; make_camera auto
run_install || bad "install.sh exited non-zero: $(cat "$WORK/out")"
if diff -q "$WORK/header.orig" "$WORK/root/var/www/cgi-bin/p/header.cgi" >/dev/null; then ok
else bad "install touched header.cgi without --with-network-page"; fi

make_bundle; make_camera auto
run_install --with-network-page || bad "--with-network-page was refused: $(cat "$WORK/out")"
H="$WORK/root/var/www/cgi-bin/p/header.cgi"
if grep -q 'machino-netpage:begin' "$H"; then ok; else bad "no menu entry was added"; fi
if grep -q '/machino/net' "$H"; then ok; else bad "the menu entry does not point at the page"; fi
# The anchor line must still be there: the entry is added AFTER it, not over it.
if grep -q 'href="network.cgi"' "$H"; then ok; else bad "the entry replaced the stock Network item"; fi

# Installing twice must not add it twice.
run_install --with-network-page
is "entry added exactly once" "$(grep -c 'machino-netpage:begin' "$H")" "1"

run_uninstall || bad "uninstall.sh exited non-zero: $(cat "$WORK/out")"
if diff -q "$WORK/header.orig" "$H" >/dev/null; then ok
else bad "header.cgi is not byte-identical again after uninstall"; fi

# A WebUI whose System menu does not look as expected gets NO entry and says
# so -- a menu item in the wrong place is worse than none, and the page is
# still reachable by URL.
make_bundle; make_camera auto
printf '<html><body>nothing familiar here</body></html>\n' > "$WORK/root/var/www/cgi-bin/p/header.cgi"
run_install --with-network-page || bad "install failed on an unfamiliar header: $(cat "$WORK/out")"
if grep -q "does not look as expected" "$WORK/out"; then ok; else bad "the unfamiliar menu was not reported"; fi
if grep -q 'machino-netpage' "$WORK/root/var/www/cgi-bin/p/header.cgi"; then
    bad "an entry was forced into an unfamiliar header"
else ok; fi

# --------- 13) everything shipped to the camera stays BusyBox-clean ---------
# Both of these were found on the hardware, not in review: BusyBox tar has no
# -z, and there is no install(1). The host runs GNU coreutils, so only a static
# check keeps the next such regression out.
for f in "$PKG/install.sh" "$PKG/uninstall.sh" "$PKG/sbin/streamerctl" "$PKG/sbin/machino-manager" "$PKG/init/S95streamer" "$PKG/init/machino" "$PKG/init/S42wifi" "$PKG/sbin/machino-wifi-role" "$PKG/udhcpc-wlan.script"; do
    if grep -nE '(^|[^-a-z_])install +-[dm]' "$f"; then bad "$(basename "$f") uses install(1), which BusyBox does not have"; else ok; fi
    if grep -nE 'tar +[a-z]*z' "$f"; then bad "$(basename "$f") uses tar -z, which BusyBox tar does not have"; else ok; fi
    if grep -nE '(^|[^a-z_])(mktemp|readlink -f|stat +-)' "$f"; then bad "$(basename "$f") uses a non-BusyBox tool"; else ok; fi
done

if [ "$SKIP" -gt 0 ]; then
    echo "openipc install tests: $PASS passed, $FAIL failed, $SKIP skipped"
else
    echo "openipc install tests: $PASS passed, $FAIL failed"
fi
[ "$FAIL" -eq 0 ]
