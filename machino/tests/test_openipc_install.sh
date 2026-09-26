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
    cp "$PKG/init/S95streamer" "$PKG/init/machino" "$PKG/init/S42usb" "$B/init/"
    mkdir -p "$B/sbin"
    cp "$PKG/sbin/machino-wifi-role" "$PKG/sbin/machino-usb-helper" "$B/sbin/"
    cp "$PKG/sbin/machino-dyndns" "$B/sbin/"
    cp "$PKG/init/S49dyndns" "$B/init/"
    # Der Registrierer, sein Boot-Skript und das Manifest. Das Manifest ist die
    # einzige Quelle fuer den Profilnamen -- der Test kopiert es deshalb aus dem
    # Paket und schreibt es nicht selbst, sonst pruefte er seine eigene Kopie.
    cp "$PKG/sbin/machino-device" "$B/sbin/"
    cp "$PKG/init/S39machinodev" "$B/init/"
    mkdir -p "$B/devices"
    cp "$PKG/devices/aic8800.manifest" "$B/devices/"
    # Machinos eigene WebUI-Seiten -- echte OpenIPC-Seiten, siehe install.sh.
    mkdir -p "$B/www"
    cp "$PKG"/www/machino-*.cgi "$B/www/"
    # Die NNA-Nutzlast (Helfer + Modell), wie das Release-Artefakt sie traegt.
    # Sie wird NUR mit --with-nna-payload installiert.
    mkdir -p "$B/nna"
    printf 'fake-machino-nna\n' > "$B/nna/machino-nna"
    printf 'fake-magik-model\n' > "$B/nna/yolov5s_t40_magik.bin"
    printf '{"schemaVersion":1}\n' > "$B/nna/manifest.json"
    printf 'fake-provenance\n'   > "$B/nna/provenance.txt"
    # WeirdIKE (IPsec), NUR mit --with-weirdike.
    mkdir -p "$B/weirdike"
    printf 'fake-weirdiked\n'    > "$B/weirdike/weirdiked"
    printf 'fake-weirdikectl\n'  > "$B/weirdike/weirdikectl"
    printf '#!/bin/sh\nexit 0\n' > "$B/weirdike/S99weirdike"
    printf 'fake-ipsec-page\n'   > "$B/weirdike/ipsec.cgi"
    printf 'gateway = x\n'       > "$B/weirdike/weirdike.conf.example"
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
    cp "$PKG/udhcpc-wlan.script" "$PKG/udhcpc-cellular.script" "$B/"
    cp "$PKG/sbin/machino-cellular-helper" "$B/sbin/"
    mkdir -p "$B/cellular/modules"
    for m in option usb_wwan usbnet cdc_ether; do echo "fake-$m" > "$B/cellular/modules/$m.ko"; done
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

    # Die beiden Vertragsflaechen der OpenIPC-Netzwerkseite. Ohne sie liefe der
    # Installer ins Leere und die Tests darunter waeren wertlos.
    #
    # /lib/modules mit dem 802.11-Unterbau, wie ihn das echte Image mitbringt:
    # cfg80211 und mac80211 ja, Geraetetreiber nein. Genau deshalb zeigte das
    # Dropdown vor dieser Arbeit nur "None".
    mkdir -p "$R/lib/modules/4.4.94/kernel/net/wireless"
    for m in cfg80211 mac80211; do
        printf 'fake-%s\n' "$m" > "$R/lib/modules/4.4.94/kernel/net/wireless/$m.ko"
    done

    # /etc/wireless/usb wie im Feld: ein paar Fremdprofile und am Ende exit 1
    # (so meldet die Datei S40network "diese Id gehoert nicht mir"). Der
    # aic8800-Eintrag von upstream traegt den SoC-Token ssc337de und wird auf
    # einem T40 vom SoC-Filter verworfen -- er darf uns also NICHT retten.
    mkdir -p "$R/etc/wireless"
    cat > "$R/etc/wireless/usb" <<'EOF'
#!/bin/sh
set_gpio() {
	[ "$2" -eq 1 ] && gpio set $1 || gpio clear $1
	sleep 1
}
if [ "$1" = "mt7601u-generic" ]; then
	modprobe mt7601u
	exit 0
fi
if [ "$1" = "aic8800-ssc337de-broadband" ]; then
	modprobe aic8800_fdrv
	exit 0
fi
if [ "$1" = "rtl8188fu-t31-aoni-e97vj62" ]; then
	set_gpio 53 1
	modprobe 8188fu
	exit 0
fi
exit 1
EOF
    chmod 0755 "$R/etc/wireless/usb"
    cp "$R/etc/wireless/usb" "$WORK/wireless-usb.orig"
}

# Die ECHTE adapter_scan()-Logik aus majestic-webui/www/cgi-bin/network.cgi.
#
# Wortgleich uebernommen; geaendert sind nur die zwei absoluten Pfade, die hier
# auf den Testbaum zeigen muessen (/lib/modules und /etc/wireless/$bus). Ein
# Nachbau waere wertlos: geprueft werden soll, ob OpenIPC unser Profil findet,
# nicht ob unsere Vorstellung davon zu sich selbst passt. Aendert sich upstream,
# muss diese Kopie nachgezogen werden -- dann schlaegt hier etwas fehl, und das
# ist der Sinn.
#
# $1 = Wurzel des Testbaums, $2 = soc (wie `ipcinfo --chip-name` ihn liefert).
# Ausgabe: eine Zeile je angebotenem Profil, "bus<TAB>driver<TAB>id<TAB>pad".
real_adapter_scan() {
    _R="$1"; _soc="$2"
    have=" $(find "$_R/lib/modules" -name '*.ko' 2>/dev/null |
        sed 's|.*/||; s|\.ko$||' | tr '\n' ' ') "
    TAB=$(printf '\t')
    for bus in usb sdio modem; do
        f="$_R/etc/wireless/$bus"
        [ -r "$f" ] || continue
        while IFS="$TAB" read -r id pad tok ms; do
            [ -n "$id" ] || continue
            ok=1
            driver=""
            for m in $ms; do
                case "$have" in *" $m "*) ;; *) ok="" ;; esac
                case "$m" in mac80211|cfg80211|rfkill) ;; *) driver="$m" ;; esac
            done
            [ -n "$ok" ] && [ -n "$driver" ] || continue
            if [ "$tok" != "-" ]; then
                case "$_soc" in
                    "$tok"*) ;;
                    *) case "$tok" in "$_soc"*) ;; *) continue ;; esac ;;
                esac
            fi
            printf '%s\t%s\t%s\t%s\n' "$bus" "$driver" "$id" "$pad"
        done <<EOF
$(awk '
	/^if \[ "\$1" = "/ { split($0, a, "\""); id = a[4]; mods = ""; pad = "-"; next }
	id != "" && /set_gpio/ {
		if (pad == "-") { n = split($0, g, /[ \t]+/); for (k = 1; k < n; k++) if (g[k] == "set_gpio") pad = g[k + 1] }
	}
	id != "" && /modprobe/ {
		m = $0; sub(/.*modprobe[ \t]+/, "", m); sub(/[ \t].*/, "", m); mods = mods " " m
	}
	id != "" && /^fi/ {
		tok = "-"
		n = split(id, part, "-")
		for (k = 1; k <= n; k++)
			if (part[k] ~ /^(t[0-9]+|hi[0-9]{4}[a-z0-9]*|gk[0-9]{4}[a-z0-9]*|ssc[0-9]{3}[a-z0-9]*)$/) tok = part[k]
		print id "\t" pad "\t" tok "\t" mods
		id = ""
	}
' "$f")
EOF
    done
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

# 15c-1) der PRODUKT-Deploy fasst die OpenIPC-WebUI NICHT an.
#
# Das ist eine Architekturgrenze, keine Vorsichtsmassnahme: OpenIPC bleibt
# unveraendert, Majestic raus, Machino rein. Eine <li>-Zeile in p/header.cgi
# ist eine Aenderung an fremden Dateien, auch wenn sie markiert ist und der
# Deinstallierer sie byteweise zuruecknimmt.
#
# Der Geraetemanager ist trotzdem erreichbar -- unter /machino/devices, von
# machino selbst ausgeliefert. Diese Trennung ist der ganze Punkt.
HDR="$R/var/www/cgi-bin/p/header.cgi"
if diff -q "$WORK/header.orig" "$HDR" >/dev/null; then ok
else bad "the product deploy edited header.cgi - OpenIPC's WebUI must stay untouched"; fi
for _m in machino-devpage machino-netpage; do
    if grep -q "$_m" "$HDR"; then bad "the deploy left a $_m marker in the stock WebUI"; else ok; fi
done
has "the daemon is installed all the same" "$R/usr/bin/machino"

# ... und --with-pages ist die ausdrueckliche Ausnahme dessen, der installiert.
# Sie bleibt erhalten, damit die Mechanik nicht verrottet, ist aber nicht der
# Normalweg.
make_bundle; make_camera auto
( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" MACHINO_MANAGER_NO_ACTIVATE=1 MACHINO_INSTALL_SKIP_FORMAT=1 sh ./sbin/machino-manager install --owner cam-tool --platform t40nn --with-pages ) >"$WORK/out" 2>&1 ||
    bad "manager install --with-pages exited non-zero: $(cat "$WORK/out")"
HDR="$R/var/www/cgi-bin/p/header.cgi"
if grep -q 'machino-devpage:begin' "$HDR"; then ok
else bad "--with-pages did not add the device manager entry"; fi
if grep -q 'machino-devices.cgi' "$HDR"; then ok
else bad "the device menu entry does not point at the page"; fi
if grep -q 'machino-netpage:begin' "$HDR"; then ok
else bad "--with-pages did not add the network page entry"; fi
if grep -q 'href="network.cgi"' "$HDR"; then ok
else bad "an entry replaced the stock Network item"; fi

# --minimal: die USB-Nutzlast bleibt weg.
#
# Rund 2,4 MB, die sonst DAUERHAFT unter /etc/machino/payload liegen und ein
# uninstall absichtlich ueberleben. Auf einer Kamera mit kleinem Overlay ist
# das der Unterschied zwischen "passt" und "passt nicht" -- und install.sh
# konnte das immer schon, nur kam man durch den Manager nicht daran, und die
# Installation aus dem Cam-Tool laeuft ausschliesslich ueber den Manager.
make_bundle; make_camera auto
( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" MACHINO_MANAGER_NO_ACTIVATE=1 MACHINO_INSTALL_SKIP_FORMAT=1 sh ./sbin/machino-manager install --owner cam-tool --platform t40nn --minimal ) >"$WORK/out" 2>&1 ||
    bad "manager install --minimal exited non-zero: $(cat "$WORK/out")"
has "the daemon is installed all the same" "$R/usr/bin/machino"
if [ -d "$R/etc/machino/payload/aic8800" ]; then
    bad "--minimal installed the WiFi payload anyway"
else ok; fi
if ls "$R/usr/sbin/hostapd" >/dev/null 2>&1; then
    bad "--minimal installed hostapd anyway"
else ok; fi

# Einzeln abwaehlbar, nicht nur beides zusammen.
make_bundle; make_camera auto
( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" MACHINO_MANAGER_NO_ACTIVATE=1 MACHINO_INSTALL_SKIP_FORMAT=1 sh ./sbin/machino-manager install --owner cam-tool --platform t40nn --without-wifi-payload ) >"$WORK/out" 2>&1 ||
    bad "manager install --without-wifi-payload exited non-zero: $(cat "$WORK/out")"
if [ -d "$R/etc/machino/payload/aic8800" ]; then
    bad "--without-wifi-payload installed the WiFi payload anyway"
else ok; fi

# Ein unbekannter Schalter muss weiterhin scheitern -- sonst verschluckt der
# Manager einen Tippfehler und installiert etwas anderes als gemeint.
make_bundle; make_camera auto
if ( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" MACHINO_MANAGER_NO_ACTIVATE=1 MACHINO_INSTALL_SKIP_FORMAT=1 sh ./sbin/machino-manager install --without-wifi-payloads ) >/dev/null 2>&1; then
    bad "a misspelled option was accepted"
else ok; fi

# Und zurueck auf den Normalfall fuer die folgenden Faelle.
make_bundle; make_camera auto
( cd "$WORK/bundle" && PATH="$MSTUB:$PATH" MACHINO_ROOT="$R" MACHINO_MANAGER_NO_ACTIVATE=1 MACHINO_INSTALL_SKIP_FORMAT=1 sh ./sbin/machino-manager install --owner cam-tool --platform t40nn ) >"$WORK/out" 2>&1 ||
    bad "manager re-install exited non-zero: $(cat "$WORK/out")"

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
has "usb boot script installed by default" "$WORK/root/etc/init.d/S42usb"
has "usb boot helper installed by default" "$WORK/root/usr/sbin/machino-usb-helper"
has "role supervisor installed by default"  "$WORK/root/usr/sbin/machino-wifi-role"
has "cgi shim installed by default"         "$WORK/root/var/www/cgi-bin/machino-cgi-run.cgi"
has "dyndns updater installed by default"   "$WORK/root/usr/sbin/machino-dyndns"
has "dyndns boot script installed"          "$WORK/root/etc/init.d/S49dyndns"
has "dyndns page installed"                 "$WORK/root/var/www/cgi-bin/machino-dyndns.cgi"
has "ai page installed"                     "$WORK/root/var/www/cgi-bin/machino-ai.cgi"
has "hostapd installed by default"          "$WORK/root/usr/sbin/hostapd"
# Unter /lib/modules, nicht mehr unter /etc/machino/modules: nur dort sucht
# network.cgi (adapter_scan), und nur von dort loest modprobe die Namen auf, die
# im /etc/wireless/usb-Profil stehen.
has "driver installed where OpenIPC looks"          "$WORK/root/lib/modules/4.4.94/machino/aic8800.ko"
has "firmware loader installed where OpenIPC looks" "$WORK/root/lib/modules/4.4.94/machino/aic_load_fw.ko"
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

# Die Mobilfunk-Nutzlast liegt aus demselben Grund bereit wie die des WLAN:
# ein Schalter, der erst nach einer Nachinstallation wirkt, ist keiner.
has "cellular helper installed"             "$WORK/root/usr/sbin/machino-cellular-helper"
has "cellular dhcp hook installed"          "$WORK/root/etc/machino/udhcpc-cellular.script"
has "option.ko installed"                   "$WORK/root/etc/machino/modules/option.ko"
has "cdc_ether.ko installed"                "$WORK/root/etc/machino/modules/cdc_ether.ko"
# ... und tut nichts: es gibt keinen Wunsch, den der Helfer ausfuehren koennte.
hasnt "no cellular request on install"      "$WORK/root/etc/machino/cellular-dhcp"

# Nothing switched it on, so nothing may claim it is on.
if grep -q "^usb.mode = wifi" "$WORK/root/etc/machino/machino.conf" 2>/dev/null ||
   grep -q "^usb.mode = cellular" "$WORK/root/etc/machino/machino.conf" 2>/dev/null; then
    bad "a default install selected a USB function"
else ok; fi

# Und der Boot-Helfer muss zustimmen: ohne Schluessel laedt er NICHTS. Das ist
# der Waechter, der den Port fuer das freihaelt, was daran haengt.
eval "$(sed -n '/^read_mode() {/,/^}/p' "$PKG/sbin/machino-usb-helper")"
out=$(CONF="$WORK/root/etc/machino/machino.conf"; read_mode)
if [ "$out" = "off" ]; then ok; else bad "the boot helper would have started '$out' on a default install"; fi

# --usb-mode pre-sets the selector; the files were already there.
make_bundle; make_camera auto
run_install --usb-mode=cellular || bad "--usb-mode=cellular was refused: $(cat "$WORK/out")"
if grep -q "^usb.mode = cellular" "$WORK/root/etc/machino/machino.conf"; then ok
else bad "--usb-mode=cellular did not set usb.mode"; fi
# Der Spiegel fuer ein altes Boot-Skript muss dabei auf false stehen -- sonst
# laedt es nach einem halben Upgrade den WLAN-Treiber auf einer Kamera, die
# auf Mobilfunk gestellt wurde.
if grep -q "^usb.wifi.enabled = false" "$WORK/root/etc/machino/machino.conf"; then ok
else bad "the legacy mirror was not cleared when cellular was selected"; fi
if grep -q "next boot" "$WORK/out"; then ok; else bad "the reboot requirement was not stated"; fi

# Der alte Name muss weiter funktionieren: bestehende Installationsbefehle
# duerfen nicht brechen.
make_bundle; make_camera auto
run_install --with-wifi || bad "--with-wifi was refused: $(cat "$WORK/out")"
if grep -q "^usb.mode = wifi" "$WORK/root/etc/machino/machino.conf"; then ok
else bad "--with-wifi did not set usb.mode=wifi"; fi
if grep -q "^usb.wifi.enabled = true" "$WORK/root/etc/machino/machino.conf"; then ok
else bad "--with-wifi did not set the legacy mirror"; fi

# Setting it twice must not produce two lines: the boot helper takes the last
# one, but a file that accumulates duplicates is a file nobody can read.
run_install --with-wifi || bad "a second --with-wifi run failed: $(cat "$WORK/out")"
n=$(grep -c "usb.mode" "$WORK/root/etc/machino/machino.conf")
if [ "$n" = "1" ]; then ok; else bad "usb.mode appears $n times after two installs"; fi

# Ohne --usb-mode bleibt die Wahl des Betreibers stehen. Eine Neuinstallation
# ueber eine bestehende hinweg darf sie nicht auf off zuruecksetzen.
run_install || bad "a plain reinstall failed: $(cat "$WORK/out")"
if grep -q "^usb.mode = wifi" "$WORK/root/etc/machino/machino.conf"; then ok
else bad "a reinstall without --usb-mode reset the selection"; fi

# Unsinn wird abgelehnt, nicht stillschweigend zu off.
make_bundle; make_camera auto
if run_install --usb-mode=lte; then bad "--usb-mode=lte was accepted"; else ok; fi

# Ein Bundle ohne den Boot-Helfer ist kein Bundle.
#
# Er gehoert zu keiner der beiden Nutzlasten und laesst sich nicht abwaehlen:
# ohne ihn waere usb.mode ein Wert, den niemand liest, und die Auswahl in der
# Oberflaeche eine Attrappe. Also abbrechen, statt eine halbe Installation
# hinzulegen, die erst beim naechsten Neustart auffaellt.
make_bundle; make_camera auto
rm -f "$WORK/bundle/sbin/machino-usb-helper"
if run_install; then bad "a bundle without machino-usb-helper was accepted"; else ok; fi
case "$(cat "$WORK/out")" in
    *machino-usb-helper*) ok ;;
    *) bad "the missing boot helper was not named" ;;
esac

make_bundle; make_camera auto
rm -f "$WORK/bundle/init/S42usb"
if run_install; then bad "a bundle without init/S42usb was accepted"; else ok; fi

# Ein bereits installiertes S42wifi MUSS verschwinden.
#
# Sonst laufen nach einem Upgrade zwei Boot-Skripte: eines liest
# usb.wifi.enabled, das andere usb.mode. Steht dort "cellular", laedt das alte
# trotzdem den WLAN-Treiber -- genau die Doppelwahrheit, gegen die usb.mode
# eingefuehrt wurde.
make_bundle; make_camera auto
mkdir -p "$WORK/root/etc/init.d"
printf '#!/bin/sh\nexit 0\n' > "$WORK/root/etc/init.d/S42wifi"
printf '#!/bin/sh\nexit 0\n' > "$WORK/root/etc/init.d/S41hostapd"
run_install || bad "install.sh exited non-zero: $(cat "$WORK/out")"
hasnt "superseded S42wifi removed"   "$WORK/root/etc/init.d/S42wifi"
hasnt "superseded S41hostapd removed" "$WORK/root/etc/init.d/S41hostapd"

# The escape hatch, for a camera whose overlay is needed elsewhere.
make_bundle; make_camera auto
run_install --without-wifi-payload || bad "--without-wifi-payload was refused: $(cat "$WORK/out")"
hasnt "no driver when the payload is declined"  "$WORK/root/etc/machino/modules/aic8800.ko"
hasnt "no hostapd when the payload is declined" "$WORK/root/usr/sbin/hostapd"
# Der Boot-Helfer bleibt: er gehoert zu KEINER der beiden Nutzlasten, sondern
# entscheidet zwischen ihnen. Ohne ihn waere usb.mode ein Wert, den niemand
# liest -- auch nicht der, der auf Mobilfunk stellt.
has "boot script stays when only the wifi payload is declined" "$WORK/root/etc/init.d/S42usb"

# NNA (KI): andersherum gepolt als WLAN/Modem -- AUS, bis jemand
# --with-nna-payload sagt. Mehrere MB auf einem fast vollen Overlay sind
# eine Entscheidung, kein Default. Die Modelle sind Nutzdaten des
# Betreibers und ueberleben ein Deinstallieren; der Helfer geht mit.
make_bundle; make_camera auto
run_install || bad "install.sh exited non-zero: $(cat "$WORK/out")"
hasnt "no NNA helper by default"        "$WORK/root/usr/sbin/machino-nna"
hasnt "no model dir by default"         "$WORK/root/etc/machino/models/yolov5s_t40_magik.bin"
make_bundle; make_camera auto
run_install --with-nna-payload || bad "--with-nna-payload was refused: $(cat "$WORK/out")"
has "NNA helper installed on request"   "$WORK/root/usr/sbin/machino-nna"
has "model installed on request"        "$WORK/root/etc/machino/models/yolov5s_t40_magik.bin"
has "manifest travels with the model"   "$WORK/root/etc/machino/models/manifest.json"
has "provenance travels with the model" "$WORK/root/etc/machino/models/provenance.txt"
run_uninstall || bad "uninstall.sh exited non-zero: $(cat "$WORK/out")"
hasnt "NNA helper removed again"        "$WORK/root/usr/sbin/machino-nna"
has "models survive the uninstall"      "$WORK/root/etc/machino/models/yolov5s_t40_magik.bin"

# IPsec (WeirdIKE): eigener Daemon, nur auf Wunsch. Die Betreiber-Config
# (PSK!) wird nie angefasst und ueberlebt ein Deinstallieren; die tun-Zeile
# in /etc/modules kommt mit dem Paket und geht mit ihm.
make_bundle; make_camera auto
run_install || bad "install.sh exited non-zero: $(cat "$WORK/out")"
hasnt "no weirdiked by default"        "$WORK/root/usr/sbin/weirdiked"
make_bundle; make_camera auto
run_install --with-weirdike || bad "--with-weirdike was refused: $(cat "$WORK/out")"
has "weirdiked installed on request"   "$WORK/root/usr/sbin/weirdiked"
has "weirdikectl installed"            "$WORK/root/usr/sbin/weirdikectl"
has "S99weirdike installed"            "$WORK/root/etc/init.d/S99weirdike"
has "ipsec page installed"             "$WORK/root/var/www/cgi-bin/ipsec.cgi"
has "conf example installed"           "$WORK/root/etc/weirdike/weirdike.conf.example"
if grep -qx tun "$WORK/root/etc/modules" 2>/dev/null; then ok
else bad "tun line missing from /etc/modules"; fi
# Eine echte Betreiber-Config anlegen: sie muss den Uninstall ueberleben.
printf 'gateway = real\npsk = secret\n' > "$WORK/root/etc/weirdike/weirdike.conf"
run_uninstall || bad "uninstall.sh exited non-zero: $(cat "$WORK/out")"
hasnt "weirdiked removed again"        "$WORK/root/usr/sbin/weirdiked"
hasnt "S99weirdike removed again"      "$WORK/root/etc/init.d/S99weirdike"
hasnt "ipsec page removed again"       "$WORK/root/var/www/cgi-bin/ipsec.cgi"
has "operator config survives"         "$WORK/root/etc/weirdike/weirdike.conf"
if grep -qx tun "$WORK/root/etc/modules" 2>/dev/null; then
    bad "tun line not removed from /etc/modules"
else ok; fi

# Switching the radio on while refusing its driver is not a configuration,
# it is a boot that fails. It has to be refused up front.
make_bundle; make_camera auto
if run_install --with-wifi --without-wifi-payload; then
    bad "--with-wifi with no payload was accepted"
else ok; fi
if run_install --usb-mode=cellular --without-cellular-payload; then
    bad "--usb-mode=cellular with no modem payload was accepted"
else ok; fi

# Ein Bundle ohne Module muss HART scheitern, nicht warnen.
#
# Vorher lief der Installer weiter und meldete "nothing to load". Das Ergebnis
# war eine Kamera mit eingetragenem Profil und ohne Module -- adapter_scan
# verwirft das an der have-Pruefung, und im Dropdown steht weiter "None", ohne
# dass irgendwo ein Fehler sichtbar waere. Wer bewusst ohne WLAN ausliefern
# will, hat --without-wifi-payload; einen unabsichtlich leeren Beutel
# stillschweigend zu akzeptieren ist kein Dienst am Benutzer.
make_bundle; rm -f "$WORK/bundle/wifi/modules/"*.ko; make_camera auto
if run_install; then
    bad "install.sh accepted a bundle with no WiFi kernel modules"
else ok; fi
if grep -q "no WiFi kernel modules" "$WORK/out"; then ok
else bad "the missing modules were not reported: $(cat "$WORK/out")"; fi
if grep -q -- "--without-wifi-payload" "$WORK/out"; then ok
else bad "the refusal does not name the way out"; fi

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
hasnt "usb script removed again"    "$WORK/root/etc/init.d/S42usb"
hasnt "usb helper removed again"    "$WORK/root/usr/sbin/machino-usb-helper"
hasnt "supervisor removed again"    "$WORK/root/usr/sbin/machino-wifi-role"
hasnt "cgi shim removed again"      "$WORK/root/var/www/cgi-bin/machino-cgi-run.cgi"
hasnt "dyndns updater removed again" "$WORK/root/usr/sbin/machino-dyndns"
hasnt "dyndns boot script removed"  "$WORK/root/etc/init.d/S49dyndns"
hasnt "dyndns page removed again"   "$WORK/root/var/www/cgi-bin/machino-dyndns.cgi"
hasnt "ai page removed again"       "$WORK/root/var/www/cgi-bin/machino-ai.cgi"
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

# Der Bootpfad selbst: laedt jeder Modus GENAU seinen Stack?
#
# Das ist die eine Zusage von AP-M6, und bis hierher hat sie nichts bewiesen.
# read_mode() unten prueft die ENTSCHEIDUNG; dieser Block prueft, was daraus
# folgt. Ohne ihn waere "im WLAN-Modus wird kein Modem-Modul geladen" ein Satz
# in einem Kommentar.
#
# Die Kommandos werden ueber den PATH abgefangen und schreiben mit, was von
# ihnen verlangt wurde. Nichts davon beruehrt das echte System: MACHINO_ROOT
# zeigt auf einen Wegwerfbaum, und ein "insmod" hier ist ein Zweizeiler.
usb_tree() {
    U="$WORK/usbroot"; rm -rf "$U"
    mkdir -p "$U/etc/machino/modules" "$U/usr/sbin" "$U/var/run" \
             "$U/sys/class/gpio/gpio50" "$U/sys/class/net/wlan0" "$U/dev"
    for m in aic8800 aic_load_fw option usb_wwan usbnet cdc_ether usbserial; do
        echo fake > "$U/etc/machino/modules/$m.ko"
    done
    # Die beiden Daemons: sie duerfen laufen, sollen aber sofort wieder gehen.
    for s in machino-wifi-role machino-cellular-helper; do
        printf '#!/bin/sh\necho "started %s" >> "$USB_ACTIONS"\nexit 0\n' "$s" > "$U/usr/sbin/$s"
        chmod +x "$U/usr/sbin/$s"
    done
    # Das Modem ist da: sonst liefe der 20-s-Notnagel bei jedem Testlauf.
    : > "$U/dev/ttyUSB0"

    BIN="$WORK/usbbin"; rm -rf "$BIN"; mkdir -p "$BIN"
    for c in insmod ip usleep; do
        printf '#!/bin/sh\necho "%s $*" >> "$USB_ACTIONS"\nexit 0\n' "$c" > "$BIN/$c"
        chmod +x "$BIN/$c"
    done
    # modprobe wie auf dem Geraet: es findet NUR, was unter /lib/modules liegt.
    # Ein Stub, der immer 0 liefert, machte die insmod-Erwartungen unten
    # wertlos -- load_module kaeme nie bis dorthin und der Test waere gruen,
    # ohne irgendetwas zu beweisen.
    cat > "$BIN/modprobe" <<'EOS'
#!/bin/sh
echo "modprobe $*" >> "$USB_ACTIONS"
for a in "$@"; do
    case "$a" in -*) continue ;; esac
    find "${MACHINO_ROOT:-}/lib/modules" -name "$a.ko" 2>/dev/null | grep -q . && exit 0
    exit 1
done
exit 1
EOS
    chmod +x "$BIN/modprobe"
    # lsmod meldet konsequent "nichts geladen", damit load_module wirklich
    # jedes Mal bis zum insmod kommt.
    printf '#!/bin/sh\nexit 0\n' > "$BIN/lsmod"; chmod +x "$BIN/lsmod"
}

run_usb_helper() {
    printf 'usb.mode = %s\n' "$1" > "$U/etc/machino/machino.conf"
    USB_ACTIONS="$WORK/actions.log"; : > "$USB_ACTIONS"
    export USB_ACTIONS
    PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" start \
        > "$WORK/usbout" 2>&1
}
did()   { if grep -q -- "$2" "$WORK/actions.log"; then ok; else bad "$1: '$2' was not done"; fi; }
didnt() { if grep -q -- "$2" "$WORK/actions.log"; then bad "$1: '$2' happened anyway"; else ok; fi; }

usb_tree
# --- off: NICHTS. Kein Modul, kein Portstrom, kein Daemon.
run_usb_helper off
didnt "off loads no wifi driver"      "aic8800"
didnt "off loads no firmware loader"  "aic_load_fw"
didnt "off loads no cfg80211"         "cfg80211"
didnt "off loads no modem serial"     "option"
didnt "off loads no usb_wwan"         "usb_wwan"
didnt "off loads no cdc_ether"        "cdc_ether"
didnt "off starts no daemon"          "started machino"
didnt "off runs no insmod at all"     "insmod"
# Und der Portstrom bleibt unten: bei "off" wird gpio50/value nicht angefasst.
if [ ! -s "$U/sys/class/gpio/gpio50/value" ]; then ok
else bad "off raised the port power (PB18)"; fi
is "off records what it started" "$(cat "$U/var/run/machino-usb-mode")" "off"

# --- wifi: der WLAN-Stack und NUR der. Und KEIN Supplicant von uns: Station
# gehoert seit der Architekturkorrektur vom 2026-09-25 OpenIPCs Netzwerkseite
# (S40network/ifup startet den wpa_supplicant). Zwei Supplicants auf wlan0
# war der Doppelbesitzer-Befund. Der Rollen-Supervisor startet nur noch fuer
# die AP-Rolle -- das, was OpenIPC nicht kann.
usb_tree
run_usb_helper wifi
did   "wifi loads cfg80211"        "modprobe cfg80211"
did   "wifi loads aic_load_fw"     "aic_load_fw.ko"
did   "wifi loads aic8800"         "aic8800.ko"
didnt "wifi (station) starts no supervisor" "started machino-wifi-role"
didnt "wifi loads no option"       "option.ko"
didnt "wifi loads no usb_wwan"     "usb_wwan.ko"
didnt "wifi loads no usbnet"       "usbnet.ko"
didnt "wifi loads no cdc_ether"    "cdc_ether.ko"
didnt "wifi starts no modem helper" "started machino-cellular-helper"
is "wifi raised the port power" "$(cat "$U/sys/class/gpio/gpio50/value")" "1"
is "wifi records what it started" "$(cat "$U/var/run/machino-usb-mode")" "wifi"

# --- wifi mit AP-Rolle: NUR dann startet der Rollen-Supervisor.
usb_tree
echo ap > "$U/etc/machino/wifi-role"
run_usb_helper wifi
did   "wifi (ap) starts the supervisor" "started machino-wifi-role"

# --- wifi-attach (der OpenIPC-Profilpfad): Hardware und sonst NICHTS.
# Nach der Rueckkehr faehrt S40network mit `ifup wlan0` fort; ein Supervisor
# von hier waere der zweite Besitzer. Auch mit gesetzter AP-Rolle nicht --
# wlandev gehoert der Station. usb.mode=wifi ist Voraussetzung (Guard unten).
usb_tree
printf 'usb.mode = wifi\n' > "$U/etc/machino/machino.conf"
echo ap > "$U/etc/machino/wifi-role"
USB_ACTIONS="$WORK/actions.log"; : > "$USB_ACTIONS"; export USB_ACTIONS
PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" wifi-attach \
    > "$WORK/usbout" 2>&1
did   "wifi-attach loads the driver"  "aic8800.ko"
did   "wifi-attach brings wlan0 up"   "ip link set wlan0 up"
didnt "wifi-attach starts no supervisor" "started machino-wifi-role"
is "wifi-attach raised the port power" "$(cat "$U/sys/class/gpio/gpio50/value")" "1"

# --- wifi-attach bei usb.mode=cellular: der Port gehoert dem Modem, also NICHTS.
# Das ist der Kern des Ein-Port-Konflikts: S40network ruft dieses Profil ueber
# ein stehengebliebenes wlandev auf, bevor S42usb Cellular startet. Der Guard
# muss verhindern, dass hier Treiber geladen oder Portstrom geschaltet wird.
usb_tree
printf 'usb.mode = cellular\n' > "$U/etc/machino/machino.conf"
USB_ACTIONS="$WORK/actions.log"; : > "$USB_ACTIONS"; export USB_ACTIONS
PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" wifi-attach \
    > "$WORK/usbout" 2>&1
didnt "wifi-attach (cellular) loads no driver"    "aic8800.ko"
didnt "wifi-attach (cellular) loads no cfg80211"  "cfg80211"
didnt "wifi-attach (cellular) brings up no wlan0" "ip link set wlan0 up"
if [ ! -s "$U/sys/class/gpio/gpio50/value" ]; then ok
else bad "wifi-attach raised the port power while usb.mode=cellular"; fi

# --- cellular: der Mobilfunkstack und NUR der.
usb_tree
run_usb_helper cellular
did   "cellular loads usbnet"      "usbnet.ko"
did   "cellular loads cdc_ether"   "cdc_ether.ko"
did   "cellular loads usb_wwan"    "usb_wwan.ko"
did   "cellular loads option"      "option.ko"
did   "cellular starts the helper" "started machino-cellular-helper"
didnt "cellular loads no aic8800"  "aic8800.ko"
didnt "cellular loads no aic_load_fw" "aic_load_fw.ko"
didnt "cellular loads no cfg80211" "cfg80211"
didnt "cellular starts no wifi supervisor" "started machino-wifi-role"
is "cellular raised the port power" "$(cat "$U/sys/class/gpio/gpio50/value")" "1"
is "cellular records what it started" "$(cat "$U/var/run/machino-usb-mode")" "cellular"

# Die Reihenfolge, und zwar genau diese: `option` bindet ueber new_id ALLE
# Interfaces eines Geraets. Kaeme es vor cdc_ether, verschluckte der Notnagel
# das ECM-Interface -- ein Modem mit tadellosem AT-Port und ohne Datenpfad.
n_cdc=$(grep -n "cdc_ether.ko" "$WORK/actions.log" | head -1 | cut -d: -f1)
n_opt=$(grep -n "option.ko"    "$WORK/actions.log" | head -1 | cut -d: -f1)
if [ -n "$n_cdc" ] && [ -n "$n_opt" ] && [ "$n_cdc" -lt "$n_opt" ]; then ok
else bad "option was loaded before cdc_ether (cdc=$n_cdc option=$n_opt)"; fi

# --- cellular + PPP: nur der gewaehlte Datenlink wird vorbereitet.
#
# Bei PPP traegt die Strecke pppd ueber den seriellen Modem-Port. Ein
# cdc_ether daneben legte ein usb0 an, das niemand benutzt -- und haette
# ausserdem das Interface belegt, das `option` im Notfall braucht.
usb_tree
printf 'usb.mode = cellular\ncellular.data_link = ppp\n' > "$U/etc/machino/machino.conf"
USB_ACTIONS="$WORK/actions.log"; : > "$USB_ACTIONS"; export USB_ACTIONS
PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" start > "$WORK/usbout" 2>&1
did   "ppp still loads the serial drivers" "option.ko"
did   "ppp still loads usb_wwan"           "usb_wwan.ko"
did   "ppp starts the cellular helper"     "started machino-cellular-helper"
didnt "ppp does not load usbnet"           "usbnet.ko"
didnt "ppp does not load cdc_ether"        "cdc_ether.ko"

# stop MUSS ein laufendes pppd beenden -- und auf sein Ende warten.
#
# Der Mobilfunk-Helfer startet pppd in einer Hintergrund-Subshell. Wer nur den
# Helfer beendet, laesst die Subshell und damit pppd weiterleben; danach
# besitzt ein verwaister pppd den Modem-Port, und der naechste Start findet
# einen Port, der auf nichts antwortet. Das ist im Review aufgefallen und war
# ein echter Defekt.
#
# Zwei Schlafprozesse als Statthalter: einer fuer pppd, einer fuer den Helfer.
# Beide muessen weg sein, wenn stop zurueckkehrt.
sleep 300 & _fake_pppd=$!
sleep 300 & _fake_helper=$!
echo "$_fake_pppd"   > "$U/var/run/pppd.pid"
echo "$_fake_helper" > "$U/var/run/machino-cellular-helper.pid"
: > "$U/var/run/machino-ppp.status"
PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" stop > "$WORK/usbout" 2>&1

if kill -0 "$_fake_pppd" 2>/dev/null; then
    bad "stop left pppd running"; kill -9 "$_fake_pppd" 2>/dev/null
else ok; fi
if kill -0 "$_fake_helper" 2>/dev/null; then
    bad "stop left the cellular helper running"; kill -9 "$_fake_helper" 2>/dev/null
else ok; fi
hasnt "stop removed the pppd pid file"    "$U/var/run/pppd.pid"
hasnt "stop removed the ppp status file"  "$U/var/run/machino-ppp.status"

# Und ohne laufendes PPP darf stop trotzdem sauber durchlaufen.
PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" stop > "$WORK/usbout" 2>&1
if [ $? = 0 ]; then ok; else bad "a second stop failed: $(cat "$WORK/usbout")"; fi

# Eine STALE PID-Datei darf keinen fremden Prozess treffen.
#
# Beendet sich pppd selbst -- abgelehnte Authentifizierung, NO CARRIER,
# Linkverlust --, bleibt ohne Aufraeumen ein Zettel mit einer toten PID liegen.
# Vergibt der Kernel die Nummer neu, schiesst ein spaeteres stop auf einen
# fremden Prozess. Auf dieser Kamera laeuft alles als root: das kann der
# Mediendaemon sein.
#
# Der Zettel wird nach einem natuerlichen Ende entfernt; diese Pruefung ist
# fuer den Fall, dass genau das einmal nicht passiert.
usb_tree
sleep 300 & _foreign=$!
echo "$_foreign" > "$U/var/run/pppd.pid"
# Das gefaelschte procfs sagt, was dieser Prozess WIRKLICH ist: kein pppd.
mkdir -p "$U/proc/$_foreign"
printf 'sleep\000300\000' > "$U/proc/$_foreign/cmdline"
PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" stop > "$WORK/usbout" 2>&1
if kill -0 "$_foreign" 2>/dev/null; then ok
else bad "stop killed a foreign process that had inherited pppd's old pid"; fi
kill -9 "$_foreign" 2>/dev/null
hasnt "the stale pid file was discarded" "$U/var/run/pppd.pid"
case "$(cat "$WORK/usbout")" in
    *stale*) ok ;;
    *) bad "the stale pid file was discarded without saying so" ;;
esac

# Und umgekehrt: sagt procfs, es IST ein pppd, wird es beendet.
usb_tree
sleep 300 & _ourppp=$!
echo "$_ourppp" > "$U/var/run/pppd.pid"
mkdir -p "$U/proc/$_ourppp"
printf '/usr/sbin/pppd\000file\000/etc/machino/ppp/options\000' > "$U/proc/$_ourppp/cmdline"
PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" stop > "$WORK/usbout" 2>&1
if kill -0 "$_ourppp" 2>/dev/null; then
    bad "stop left our own pppd running"; kill -9 "$_ourppp" 2>/dev/null
else ok; fi

# release_pidfile() aus dem Mobilfunk-Helfer, ECHT aufgerufen.
#
# Nicht nachgebaut: ein Test, der die Logik nachbaut statt sie auszufuehren,
# bleibt gruen, wenn jemand sie aus dem Skript entfernt. Deshalb wird die
# Funktion aus der Datei geholt -- dieselbe Technik wie bei read_mode.
eval "$(sed -n '/^release_pidfile() {/,/^}/p' "$PKG/sbin/machino-cellular-helper")"

PF="$WORK/waiter.pid"
echo 4242 > "$PF"
release_pidfile "$PF" 4242
hasnt "release_pidfile removes its own pid file" "$PF"

# Hat inzwischen ein NEUER Anruf die Datei uebernommen, bleibt sie stehen --
# sonst waere der Nachfolger nicht mehr beendbar.
echo 999999 > "$PF"
release_pidfile "$PF" 4242
if [ "$(cat "$PF" 2>/dev/null)" = "999999" ]; then ok
else bad "release_pidfile deleted the successor's pid file"; fi

# Und es darf nie fehlschlagen: im Waiter steht es vor write_ppp_status, und
# ein Rueckgabewert ungleich 0 waere dort mit `set -e` das Ende des Aufraeumens.
rm -f "$PF"
if release_pidfile "$PF" 4242; then ok
else bad "release_pidfile failed on a missing pid file"; fi
didnt "ppp loads no wifi driver"           "aic8800"

# Und andersherum: bei ECM (auch ohne den Schluessel) kommt das Netzwerkpaar.
usb_tree
printf 'usb.mode = cellular\n' > "$U/etc/machino/machino.conf"
USB_ACTIONS="$WORK/actions.log"; : > "$USB_ACTIONS"; export USB_ACTIONS
PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" start > "$WORK/usbout" 2>&1
did "a missing data_link means ecm" "cdc_ether.ko"

# Unsinn im Datenlink faellt auf ECM zurueck -- der Normalfall, nicht die
# Ausweichmoeglichkeit. Ein Tippfehler darf nicht in den selteneren Pfad
# schicken.
usb_tree
printf 'usb.mode = cellular\ncellular.data_link = pppoe\n' > "$U/etc/machino/machino.conf"
USB_ACTIONS="$WORK/actions.log"; : > "$USB_ACTIONS"; export USB_ACTIONS
PATH="$BIN:$PATH" MACHINO_ROOT="$U" sh "$PKG/sbin/machino-usb-helper" start > "$WORK/usbout" 2>&1
did "an unknown data link falls back to ecm" "cdc_ether.ko"

# Ein unbekannter Modus faellt geschlossen aus -- nicht auf WLAN, nicht auf
# Mobilfunk, sondern auf gar nichts.
usb_tree
run_usb_helper lte
didnt "an unknown mode loads nothing" "insmod"
is "an unknown mode records off" "$(cat "$U/var/run/machino-usb-mode")" "off"

# read_mode(): entscheidet, WELCHER Stack beim Boot geladen wird und ob der
# USB-Port ueberhaupt bestromt wird. Fail-closed in jedem Zweifelsfall.
#
# Die andere Haelfte dieses Vertrags steht in test_net_views.cpp
# (test_the_usb_mode_is_written_the_way_the_boot_helper_reads_it): dort wird
# festgenagelt, dass machino genau "usb.mode" mit genau "wifi"/"cellular"/"off"
# schreibt. Hier wird geprueft, dass der Parser das liest. Die beiden Seiten
# laufen nie zusammen -- C++ schreibt die Datei, Shell liest sie vor dem Start
# von machino -- also muessen sie sich an einer woertlichen Zeile treffen.
eval "$(sed -n '/^read_mode() {/,/^}/p' "$PKG/sbin/machino-usb-helper")"
gate() { CONF="$WORK/gate.conf"; printf '%s' "$1" > "$CONF"; read_mode; }
for case_ in \
    'usb.mode = wifi|wifi' \
    'usb.mode=cellular|cellular' \
    'usb.mode = off|off' \
    'board = t40nn
usb.mode = cellular|cellular' \
    'board = t40nn|off' \
    '|off' \
    '#usb.mode = wifi|off' \
    'xusb.mode = wifi|off' \
    'usb.modex = wifi|off' \
    'usb.mode = WIFI|off' \
    'usb.mode = lte|off' \
    'usb.mode = cellular
usb.mode = off|off' \
    'usb.mode = off
usb.mode = cellular|cellular' \
; do
    body=${case_%|*}; want=${case_##*|}
    got=$(gate "$body")
    if [ "$got" = "$want" ]; then ok
    else bad "usb gate: expected $want, got $got for [$(echo "$body" | tr '\n' ';')]"; fi
done

# Die einmalige Migration, im SKRIPT und nicht nur in C++.
#
# Eine Kamera, die vor AP-M6 installiert wurde, hat kein usb.mode -- nur
# usb.wifi.enabled. Ohne diese Ableitung waere ihr WLAN nach dem Upgrade
# stumm aus, und "nach dem Update ist das WLAN weg" fuehrt niemanden zu einem
# umbenannten Konfigurationsschluessel.
for case_ in \
    'usb.wifi.enabled = true|wifi' \
    'usb.wifi.enabled=1|wifi' \
    'usb.wifi.enabled = false|off' \
    'usb.wifi.enabled = true
usb.mode = cellular|cellular' \
    'usb.mode = cellular
usb.wifi.enabled = true|cellular' \
; do
    body=${case_%|*}; want=${case_##*|}
    got=$(gate "$body")
    if [ "$got" = "$want" ]; then ok
    else bad "usb migration: expected $want, got $got for [$(echo "$body" | tr '\n' ';')]"; fi
done

# Eine fehlende Datei ist kein "vielleicht".
CONF="$WORK/does-not-exist.conf"
if [ "$(read_mode)" = "off" ]; then ok; else bad "the usb gate opened with no config file at all"; fi

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
if grep -q 'machino-usb.cgi' "$H"; then ok; else bad "the menu entry does not point at the page"; fi
# The anchor line must still be there: the entry is added AFTER it, not over it.
if grep -q 'href="network.cgi"' "$H"; then ok; else bad "the entry replaced the stock Network item"; fi

# Installing twice must not add it twice.
run_install --with-network-page
is "entry added exactly once" "$(grep -c 'machino-netpage:begin' "$H")" "1"

run_uninstall || bad "uninstall.sh exited non-zero: $(cat "$WORK/out")"
if diff -q "$WORK/header.orig" "$H" >/dev/null; then ok
else bad "header.cgi is not byte-identical again after uninstall"; fi

# Dieselbe Regel fuer die Geraeteseite, und beide zusammen. Der Grund fuer das
# "zusammen": die Eintraege werden an derselben Ankerzeile eingefuegt und an
# eigenen Markern wieder entfernt. Nimmt einer beim Entfernen den anderen mit,
# faellt das nur auf, wenn man beide gesetzt hat.
make_bundle; make_camera auto
run_install || bad "install.sh exited non-zero: $(cat "$WORK/out")"
if diff -q "$WORK/header.orig" "$WORK/root/var/www/cgi-bin/p/header.cgi" >/dev/null; then ok
else bad "install touched header.cgi without --with-device-page"; fi

make_bundle; make_camera auto
run_install --with-network-page --with-device-page ||
    bad "--with-device-page was refused: $(cat "$WORK/out")"
H="$WORK/root/var/www/cgi-bin/p/header.cgi"
if grep -q 'machino-devpage:begin' "$H"; then ok; else bad "no device menu entry was added"; fi
if grep -q 'machino-devices.cgi' "$H"; then ok; else bad "the device entry does not point at the page"; fi
if grep -q 'machino-netpage:begin' "$H"; then ok; else bad "the device entry displaced the network entry"; fi
if grep -q 'href="network.cgi"' "$H"; then ok; else bad "an entry replaced the stock Network item"; fi
run_install --with-network-page --with-device-page
is "device entry added exactly once" "$(grep -c 'machino-devpage:begin' "$H")" "1"
is "network entry still exactly once" "$(grep -c 'machino-netpage:begin' "$H")" "1"
run_uninstall || bad "uninstall.sh exited non-zero: $(cat "$WORK/out")"
if diff -q "$WORK/header.orig" "$H" >/dev/null; then ok
else bad "header.cgi is not byte-identical again after uninstalling both entries"; fi

# Die Seite selbst ist einkompiliert, nicht installiert. Ein Asset unter
# /var/www waere genau der Weg, auf dem Seite und API in verschiedenen
# Versionen auseinanderlaufen.
if [ -e "$WORK/root/var/www/machino" ] || [ -e "$WORK/root/var/www/devices.html" ]; then
    bad "the device page was installed as a file - it must be compiled in"
else ok; fi

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
for f in "$PKG/install.sh" "$PKG/uninstall.sh" "$PKG/sbin/streamerctl" "$PKG/sbin/machino-manager" "$PKG/init/S95streamer" "$PKG/init/machino" "$PKG/init/S42usb" "$PKG/sbin/machino-usb-helper" "$PKG/sbin/machino-wifi-role" "$PKG/www/machino-cgi-run.cgi" "$PKG/sbin/machino-dyndns" "$PKG/init/S49dyndns" "$PKG/udhcpc-wlan.script" "$PKG/sbin/machino-device" "$PKG/init/S39machinodev"; do
    if grep -nE '(^|[^-a-z_])install +-[dm]' "$f"; then bad "$(basename "$f") uses install(1), which BusyBox does not have"; else ok; fi
    if grep -nE 'tar +[a-z]*z' "$f"; then bad "$(basename "$f") uses tar -z, which BusyBox tar does not have"; else ok; fi
    if grep -nE '(^|[^a-z_])(mktemp|readlink -f|stat +-)' "$f"; then bad "$(basename "$f") uses a non-BusyBox tool"; else ok; fi
done

# --------- 13b) the CGI shim fills GET_/POST_ for the stock sh-CGIs ----------
#
# Gemessen 2026-09-26: OpenIPCs j/*.cgi lesen GET_<k>/POST_<k>, die majestics
# httpd setzt und busybox nicht -- files.cgi listete stur /, download.cgi sagte
# "not a file". Der Interpreter-Weg fiel aus (dieses busybox kennt die Direktive
# nicht). Das Shim wird von machinos Front-Door per PATH_INFO angesprochen
# (/cgi-bin/machino-cgi-run.cgi/j/<name>.cgi), fuellt die Umgebung und exec't
# das UNVERAENDERTE Zielskript. Vier Vertraege, alle am Geraeteverhalten belegt.
SHIM="$PKG/www/machino-cgi-run.cgi"
cgi_tmp="$WORK/cgishim"; rm -rf "$cgi_tmp"; mkdir -p "$cgi_tmp/j"
# Ein Fake-Zielskript unter j/, das GET_/POST_ und seinen stdin sichtbar macht.
cat > "$cgi_tmp/j/files.cgi" <<'EOS'
#!/bin/sh
printf 'HTTP/1.1 200 OK\nContent-Type: text/plain\n\n'
printf 'GET_cd=[%s] GET_path=[%s] POST_op=[%s] POST_path=[%s]\n' \
    "$GET_cd" "$GET_path" "$POST_op" "$POST_path"
cat
EOS
chmod +x "$cgi_tmp/j/files.cgi"

# a) GET: Query -> GET_<k>, urldecodiert; Ziel aus PATH_INFO.
out=$(env MACHINO_CGI_DIR="$cgi_tmp" PATH_INFO=/j/files.cgi \
    QUERY_STRING='cd=%2Fetc&path=%2Ftmp%2Fx' REQUEST_METHOD=GET \
    sh "$SHIM" </dev/null)
case "$out" in *"GET_cd=[/etc]"*"GET_path=[/tmp/x]"*) ok ;; *) bad "shim GET: $out" ;; esac

# b) POST urlencoded: Body -> POST_<k>, und stdin wird NICHT durchgereicht.
body='op=delete&path=%2Ftmp%2Fy'
out=$(printf '%s' "$body" | env MACHINO_CGI_DIR="$cgi_tmp" PATH_INFO=/j/files.cgi \
    REQUEST_METHOD=POST CONTENT_TYPE=application/x-www-form-urlencoded \
    CONTENT_LENGTH=${#body} sh "$SHIM")
case "$out" in
    *"POST_op=[delete]"*"POST_path=[/tmp/y]"*) ok ;;
    *) bad "shim POST urlencoded: $out" ;;
esac
case "$out" in *"$body"*) bad "shim leaked the urlencoded body to stdin" ;; *) ok ;; esac

# c) POST text/plain (save.cgi-Fall): GET_ aus Query, Rohbody UNANGETASTET an
#    stdin. Genau das braucht `cat > $f`.
out=$(printf 'ROHDATEN' | env MACHINO_CGI_DIR="$cgi_tmp" PATH_INFO=/j/files.cgi \
    QUERY_STRING='path=/tmp/z' REQUEST_METHOD=POST CONTENT_TYPE='text/plain' \
    CONTENT_LENGTH=8 sh "$SHIM")
case "$out" in *"GET_path=[/tmp/z]"*"ROHDATEN"*) ok ;; *) bad "shim raw body: $out" ;; esac

# d) Ein Ziel mit .. wird abgewiesen (400), nicht ausserhalb von j/ ausgefuehrt.
out=$(env MACHINO_CGI_DIR="$cgi_tmp" PATH_INFO=/j/../etc.cgi REQUEST_METHOD=GET \
    sh "$SHIM" </dev/null | head -1)
case "$out" in *"400"*) ok ;; *) bad "shim did not reject a .. target: $out" ;; esac

# e) Ein unbekanntes Ziel -> 404 (kein exec ins Leere).
out=$(env MACHINO_CGI_DIR="$cgi_tmp" PATH_INFO=/j/nope.cgi REQUEST_METHOD=GET \
    sh "$SHIM" </dev/null | head -1)
case "$out" in *"404"*) ok ;; *) bad "shim did not 404 an unknown target: $out" ;; esac

# --------- 13c) the DynDNS updater: config gate, status, no fork when off ---
#
# Standalone updater (no machinod): reads /etc/machino/dyndns.conf line by line
# (never sourced), records a status line, and refuses to run when disabled.
DDNS="$PKG/sbin/machino-dyndns"
dd_tmp="$WORK/ddns"; rm -rf "$dd_tmp"; mkdir -p "$dd_tmp/etc/machino" "$dd_tmp/var/run" "$dd_tmp/bin"
# A fake curl on PATH that echoes a body + the -w http_code, controllable via a file.
cat > "$dd_tmp/bin/curl" <<'EOS'
#!/bin/sh
code=$(cat "$CURL_CODE" 2>/dev/null || echo 200)
printf 'body-here\n%s' "$code"
EOS
chmod +x "$dd_tmp/bin/curl"
export CURL_CODE="$dd_tmp/code"; echo 200 > "$CURL_CODE"

# a) disabled -> start runs nothing, status says so.
printf 'enabled=false\n' > "$dd_tmp/etc/machino/dyndns.conf"
MACHINO_DYNDNS_ROOT="$dd_tmp" PATH="$dd_tmp/bin:$PATH" sh "$DDNS" start >/dev/null 2>&1
r=$(MACHINO_DYNDNS_ROOT="$dd_tmp" sh "$DDNS" status | sed -n 's/^running: //p')
is "dyndns disabled does not run" "$r" "no"

# b) enabled + HTTP 200 -> update-now records ok.
printf 'enabled=true\nurl=https://example.test/upd?token=abc\ninterval=300\n' > "$dd_tmp/etc/machino/dyndns.conf"
MACHINO_DYNDNS_ROOT="$dd_tmp" PATH="$dd_tmp/bin:$PATH" sh "$DDNS" update-now >/dev/null 2>&1
case "$(cat "$dd_tmp/var/run/machino-dyndns.status" 2>/dev/null)" in
    *" ok 200 "*) ok ;;
    *) bad "dyndns update-now did not record ok/200: $(cat "$dd_tmp/var/run/machino-dyndns.status" 2>/dev/null)" ;;
esac

# c) HTTP 401 -> recorded as fail (a bad token must not read as success).
echo 401 > "$CURL_CODE"
MACHINO_DYNDNS_ROOT="$dd_tmp" PATH="$dd_tmp/bin:$PATH" sh "$DDNS" update-now >/dev/null 2>&1
case "$(cat "$dd_tmp/var/run/machino-dyndns.status" 2>/dev/null)" in
    *" fail 401 "*) ok ;;
    *) bad "dyndns did not record a 401 as failure" ;;
esac

# d) the conf is read line-by-line, never sourced: a URL with shell metachars
#    must not execute. $(touch pwned) in the value stays literal.
printf 'enabled=true\nurl=https://x/?a=$(touch %s/pwned)&b=1\ninterval=300\n' "$dd_tmp" > "$dd_tmp/etc/machino/dyndns.conf"
echo 200 > "$CURL_CODE"
MACHINO_DYNDNS_ROOT="$dd_tmp" PATH="$dd_tmp/bin:$PATH" sh "$DDNS" update-now >/dev/null 2>&1
if [ -e "$dd_tmp/pwned" ]; then bad "dyndns sourced the conf - command substitution in the URL ran"; else ok; fi
unset CURL_CODE

# --------- 14) the OpenIPC network page must be able to offer our WiFi -------
#
# Der Befund vom 2026-09-25: das Dropdown zeigte ausschliesslich "None", obwohl
# der AIC8800 auf genau dieser Kamera schon lief. Die Module lagen unter
# /etc/machino/modules, wo network.cgi nicht sucht, und ein passendes Profil gab
# es nicht. Geprueft wird deshalb gegen die ECHTE adapter_scan()-Logik.
make_bundle; make_camera auto

# Vorher: die Seite hat nichts anzubieten. Das ist der reproduzierte Fehler.
if real_adapter_scan "$WORK/root" t40nn | grep -q aic8800; then
    bad "the fake camera already offers an aic8800 profile before installing"
else ok; fi

run_install || bad "install failed: $(cat "$WORK/out")"

# a) Die Module liegen dort, wo OpenIPC sucht.
if find "$WORK/root/lib/modules" -name 'aic8800.ko' | grep -q . &&
   find "$WORK/root/lib/modules" -name 'aic_load_fw.ko' | grep -q .; then ok
else bad "the AIC modules are not under /lib/modules, where network.cgi looks"; fi

# b) Die echte Scanner-Logik bietet unser Profil an.
scan=$(real_adapter_scan "$WORK/root" t40nn)
if printf '%s\n' "$scan" | grep -q 'aic8800-t40-machino'; then ok
else bad "the real adapter_scan does not offer our profile: [$scan]"; fi

# c) Es wird als Treiber aic8800 gefuehrt, nicht als reines Stack-Modul.
if printf '%s\n' "$scan" | grep -q "^usb	aic8800	aic8800-t40-machino"; then ok
else bad "our profile is not reported with driver aic8800: [$scan]"; fi

# c2) Der Ein-Port-Guard steht im Profilblock und VOR den modprobe-Zeilen:
#     S40network ruft das Profil auch bei usb.mode=cellular (stehengebliebenes
#     wlandev); ohne diesen Guard kaeme der WLAN-Treiber vor dem Modem an den
#     einen Port. Die Guard-Zeile muss vor dem ersten modprobe stehen.
blk=$(sed -n '/>>> machino aic8800-t40-machino/,/<<< machino aic8800-t40-machino/p' \
    "$WORK/root/etc/wireless/usb")
if printf '%s\n' "$blk" | grep -q 'machino-usb-helper mode.*= wifi'; then ok
else bad "the profile block has no usb.mode guard: [$blk]"; fi
gline=$(printf '%s\n' "$blk" | grep -n 'machino-usb-helper mode' | head -1 | cut -d: -f1)
mline=$(printf '%s\n' "$blk" | grep -n 'modprobe' | head -1 | cut -d: -f1)
if [ -n "$gline" ] && [ -n "$mline" ] && [ "$gline" -lt "$mline" ]; then ok
else bad "the usb.mode guard is not before the first modprobe (guard=$gline modprobe=$mline)"; fi

# d) Der SoC-Filter bleibt scharf: das ssc337de-Profil von upstream darf auf
#    einem T40 NICHT erscheinen, und unseres nicht auf einer Sigmastar.
if printf '%s\n' "$scan" | grep -q 'ssc337de'; then
    bad "the ssc337de profile leaked onto a T40 - the SoC filter is broken"
else ok; fi
if real_adapter_scan "$WORK/root" ssc337de | grep -q 'aic8800-t40-machino'; then
    bad "our t40 profile leaked onto a sigmastar SoC"
else ok; fi

# e) t40 muss per Praefix auch t40nn treffen -- das ist der ganze Grund fuer
#    den Profilnamen.
if real_adapter_scan "$WORK/root" t40 | grep -q 'aic8800-t40-machino'; then ok
else bad "our profile does not match the plain t40 SoC"; fi

# f) Idempotent: zweimal installieren ergibt genau einen Block.
run_install || bad "second install failed: $(cat "$WORK/out")"
n=$(grep -c 'machino aic8800-t40-machino >>>' "$WORK/root/etc/wireless/usb")
if [ "$n" = "1" ]; then ok; else bad "installing twice left $n profile blocks"; fi

# g) Der Block steht VOR dem abschliessenden exit 1 -- dahinter waere er tot.
if awk '/^exit 1$/ { last = NR } /aic8800-t40-machino/ { mine = NR } END { exit !(mine && last && mine < last) }' \
      "$WORK/root/etc/wireless/usb"; then ok
else bad "the profile block sits after the final exit 1 and can never run"; fi

# h) Fremde Profile bleiben unangetastet.
for keep in mt7601u-generic aic8800-ssc337de-broadband rtl8188fu-t31-aoni-e97vj62; do
    if grep -q "\"$keep\"" "$WORK/root/etc/wireless/usb"; then ok
    else bad "installing removed the foreign profile $keep"; fi
done

# i) Kein Hosttest darf depmod auf dem Entwicklerrechner ausfuehren.
if grep -q 'depmod' "$WORK/out"; then bad "depmod ran against a fake root"; else ok; fi

# j) /var/www bleibt unberuehrt (ohne --with-network-page).
if cmp -s "$WORK/root/var/www/cgi-bin/p/header.cgi" "$WORK/header.orig"; then ok
else bad "the WiFi integration modified /var/www"; fi

# k) Keine doppelte Boot-Aktivierung: der Helfer tritt zurueck, wenn wlandev
#    unser Profil nennt (S40network laeuft vorher und hat es schon gestartet).
if grep -q 'wlandev_owns_wifi' "$PKG/sbin/machino-usb-helper" &&
   grep -q 'wifi-attach' "$PKG/sbin/machino-usb-helper"; then ok
else bad "the helper has no wlandev stand-down / wifi-attach entry point"; fi

# l) Deinstallieren stellt den vorherigen Zustand wieder her.
run_uninstall || bad "uninstall failed: $(cat "$WORK/out")"
if cmp -s "$WORK/root/etc/wireless/usb" "$WORK/wireless-usb.orig"; then ok
else bad "uninstall did not restore /etc/wireless/usb byte for byte"; fi
if find "$WORK/root/lib/modules" -name 'aic8800.ko' | grep -q .; then
    bad "uninstall left the AIC modules under /lib/modules"
else ok; fi
if real_adapter_scan "$WORK/root" t40nn | grep -q 'aic8800-t40-machino'; then
    bad "the profile is still offered after uninstalling"
else ok; fi

# --------- 15) Nutzlast, Registrierung und die EINE Quelle ------------------
#
# Drei Zusagen, die vorher keine Pruefung hatten und die zusammen die
# Einbahnstrasse schliessen: installieren -> deinstallieren -> wieder
# installieren, ohne neues Bundle.
make_bundle; make_camera auto
run_install || bad "install failed: $(cat "$WORK/out")"

# Der Profilname wird hier NICHT wiederholt. Er kommt aus dem Manifest -- sonst
# pruefte dieser Test eine siebte Kopie derselben Zeichenkette.
PROF=$(sed -n 's/^openipc_profile=//p' "$PKG/devices/aic8800.manifest")
if [ -n "$PROF" ]; then ok; else bad "the manifest has no openipc_profile"; fi

# a) Die Nutzlast liegt dauerhaft unter /etc/machino/payload/<id>/.
for m in aic8800 aic_load_fw; do
    if [ -f "$WORK/root/etc/machino/payload/aic8800/$m.ko" ]; then ok
    else bad "the payload $m.ko is not under /etc/machino/payload/aic8800"; fi
done

# b) Das Manifest liegt dort, wo Shell UND C++ es suchen.
if [ -f "$WORK/root/etc/machino/devices/aic8800.manifest" ]; then ok
else bad "the device manifest was not installed"; fi

# c) Der Boot-Absichtsausfuehrer ist eingerichtet, und zwar VOR S40network.
if [ -f "$WORK/root/etc/init.d/S39machinodev" ]; then ok
else bad "S39machinodev was not installed - a pending intent would never run"; fi

# d) machino-device uninstall entfernt NUR die Registrierung. Genau hier war
#    frueher die Einbahnstrasse: die Nutzlast fiel mit, und danach liess sich
#    der Adapter ohne neues Bundle nicht mehr einrichten.
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" uninstall aic8800 \
    > "$WORK/out" 2>&1 || bad "machino-device uninstall failed: $(cat "$WORK/out")"
if find "$WORK/root/lib/modules" -name 'aic8800.ko' | grep -q .; then
    bad "deregistering left the module under /lib/modules"
else ok; fi
if real_adapter_scan "$WORK/root" t40nn | grep -q "$PROF"; then
    bad "deregistering left the profile in /etc/wireless/usb"
else ok; fi
for m in aic8800 aic_load_fw; do
    if [ -f "$WORK/root/etc/machino/payload/aic8800/$m.ko" ]; then ok
    else bad "deregistering deleted the payload $m.ko - that is the one-way door"; fi
done
if [ "$(MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" status aic8800)" = "available" ]; then ok
else bad "status after deregistering is not 'available'"; fi

# e) ... und genau deshalb geht Wiedereinrichten ohne Bundle.
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" install aic8800 \
    > "$WORK/out" 2>&1 || bad "re-install failed: $(cat "$WORK/out")"
if real_adapter_scan "$WORK/root" t40nn | grep -q "$PROF"; then ok
else bad "re-installing from the kept payload did not restore the profile"; fi
if [ "$(MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" status aic8800)" = "registered" ]; then ok
else bad "status after re-installing is not 'registered'"; fi

# e2) Der Modulindex OHNE depmod. Diese Kamera hat keines (gemessen am
#     2026-09-26: busybox ohne depmod-Applet, die Registrierung scheiterte auf
#     echter Hardware genau hier). machino-device schreibt die Zeilen selbst:
#     busybox-modprobe liest nichts als modules.dep, Pfade relativ zu
#     /lib/modules/<ver>, hinter dem Doppelpunkt die Abhaengigkeiten in
#     Ladereihenfolge (aic8800 braucht aic_load_fw).
MDEP=$(find "$WORK/root/lib/modules" -name modules.dep | head -1)
if [ -n "$MDEP" ]; then ok; else bad "no modules.dep was written without depmod"; fi
if grep -q '^machino/aic_load_fw\.ko:$' "$MDEP"; then ok
else bad "modules.dep has no entry for aic_load_fw"; fi
if grep -q '^machino/aic8800\.ko: machino/aic_load_fw\.ko$' "$MDEP"; then ok
else bad "modules.dep does not give aic8800 its aic_load_fw dependency"; fi
# Doppelt registrieren stapelt keine Zeilen.
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" install aic8800 >/dev/null 2>&1
if [ "$(grep -c '^machino/aic8800\.ko:' "$MDEP")" = "1" ]; then ok
else bad "re-registering duplicated the modules.dep entry"; fi
# Deregistrieren raeumt die Zeilen wieder ab -- und NUR sie.
printf 'kernel/net/foo.ko:\n' >> "$MDEP"
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" uninstall aic8800 >/dev/null 2>&1
if grep -q '^machino/' "$MDEP"; then bad "deregistering left machino lines in modules.dep"
else ok; fi
if grep -q '^kernel/net/foo\.ko:$' "$MDEP"; then ok
else bad "deregistering ate a foreign modules.dep line"; fi
# ... und fuer die restlichen Pruefungen wieder registrieren.
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" install aic8800 >/dev/null 2>&1

# f) run-intent fuehrt aus, was machinod hinterlegt hat, und raeumt die Marke
#    weg. Das ist der ganze Grund, warum machinod nicht selbst forkt.
echo remove > "$WORK/root/etc/machino/device-intent-aic8800"
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" run-intent \
    > "$WORK/out" 2>&1 || bad "run-intent failed: $(cat "$WORK/out")"
if [ -f "$WORK/root/etc/machino/device-intent-aic8800" ]; then
    bad "run-intent did not consume the intent file - it would run again every boot"
else ok; fi
if real_adapter_scan "$WORK/root" t40nn | grep -q "$PROF"; then
    bad "the remove intent did not deregister the adapter"
else ok; fi
echo install > "$WORK/root/etc/machino/device-intent-aic8800"
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" run-intent \
    > "$WORK/out" 2>&1 || bad "run-intent (install) failed: $(cat "$WORK/out")"
if real_adapter_scan "$WORK/root" t40nn | grep -q "$PROF"; then ok
else bad "the install intent did not register the adapter"; fi

# f2) Eine Absicht, die NICHT durchlaeuft, bleibt liegen -- und sagt warum.
#
# Zwischen dem Kopieren der Module und dem Schreiben des Profils liegen mehrere
# Schritte auf einem jffs2-Overlay. Faellt dazwischen der Strom, waere die
# Haelfte getan; loeschte run-intent die Marke anhand des Rueckgabewerts,
# versuchte es der naechste Boot nie wieder und die Oberflaeche verspraeche
# weiter "wirksam nach dem naechsten Neustart".
#
# Nachgestellt wird das ueber die einzige Bedingung, die der Hosttest wirklich
# herbeifuehren kann: eine Nutzlast, die nicht (mehr) da ist.
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" uninstall aic8800 >/dev/null 2>&1
mv "$WORK/root/etc/machino/payload/aic8800/aic8800.ko" "$WORK/aic8800.ko.parked"
echo install > "$WORK/root/etc/machino/device-intent-aic8800"
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" run-intent > "$WORK/out" 2>&1
if [ -f "$WORK/root/etc/machino/device-intent-aic8800" ]; then ok
else bad "a failed intent was consumed - the next boot would never retry it"; fi
if [ -s "$WORK/root/etc/machino/device-intent-aic8800.failed" ]; then ok
else bad "a failed intent left no reason behind"; fi
if real_adapter_scan "$WORK/root" t40nn | grep -q "$PROF"; then
    bad "a half-done install still registered the profile - OpenIPC would offer an adapter that cannot load"
else ok; fi

# ... und derselbe Intent laeuft durch, sobald die Ursache weg ist. Ohne das
# waere "bleibt liegen" nur eine andere Art, stecken zu bleiben.
mv "$WORK/aic8800.ko.parked" "$WORK/root/etc/machino/payload/aic8800/aic8800.ko"
MACHINO_ROOT="$WORK/root" sh "$WORK/root/usr/sbin/machino-device" run-intent > "$WORK/out" 2>&1
if [ -f "$WORK/root/etc/machino/device-intent-aic8800" ]; then
    bad "the retry did not consume the intent"
else ok; fi
if [ -f "$WORK/root/etc/machino/device-intent-aic8800.failed" ]; then
    bad "the failure marker survived a successful retry - the UI would keep showing it"
else ok; fi
if real_adapter_scan "$WORK/root" t40nn | grep -q "$PROF"; then ok
else bad "the retry did not register the adapter"; fi

# g) Der Profilname steht NUR im Manifest. Jede weitere Kopie im ausgelieferten
#    Paket ist die naechste stille Drift -- genau die, die uns sechs Kopien in
#    vier Dateien eingebracht hat.
hits=$(grep -rl "$PROF" "$PKG" 2>/dev/null | grep -v '/devices/aic8800.manifest$' || true)
if [ -z "$hits" ]; then ok
else bad "the profile name is hardcoded outside the manifest: $hits"; fi

# Machinos WebUI-Seiten: nach dem Install im Webroot und ausfuehrbar, nach dem
# vollstaendigen Uninstall restlos weg. Die OpenIPC-Dateien daneben prueft der
# header.cgi-Vergleich weiter oben byte-genau.
if [ -x "$WORK/root/var/www/cgi-bin/machino-usb.cgi" ] &&
   [ -x "$WORK/root/var/www/cgi-bin/machino-cellular.cgi" ] &&
   [ -x "$WORK/root/var/www/cgi-bin/machino-uplinks.cgi" ] &&
   [ -x "$WORK/root/var/www/cgi-bin/machino-devices.cgi" ]; then ok
else bad "the machino pages were not installed into the webroot"; fi
if head -1 "$WORK/root/var/www/cgi-bin/machino-usb.cgi" | grep -q haserl; then ok
else bad "the installed network page is not a haserl page"; fi

# h) Das vollstaendige uninstall.sh -- und NUR das -- raeumt auch die Nutzlast.
run_uninstall || bad "uninstall failed: $(cat "$WORK/out")"
if [ -d "$WORK/root/etc/machino/payload" ]; then
    bad "the full uninstall left the payload behind"
else ok; fi
if [ -f "$WORK/root/etc/init.d/S39machinodev" ]; then
    bad "the full uninstall left S39machinodev behind"
else ok; fi
if cmp -s "$WORK/root/etc/wireless/usb" "$WORK/wireless-usb.orig"; then ok
else bad "the full uninstall did not restore /etc/wireless/usb byte for byte"; fi
if ls "$WORK"/root/var/www/cgi-bin/machino-*.cgi >/dev/null 2>&1; then
    bad "the full uninstall left machino's pages in the webroot"
else ok; fi

# ---------------------------------------------------------------------------
# Die Konfiguration MUSS Hardware auswaehlen.
#
# Auf der T40NN gemessen: ohne board/platform verweigert der Daemon den Start
# ("refusing to start: no guessing of buses or pins"). install.sh meldete
# trotzdem Erfolg, der Manager schaltete um, machino kam nicht hoch, der
# Rueckfall auf majestic lief -- und die Kamera startete neu. Von aussen sah
# das wie "die Installation haengt" aus.
#
# Erzeugt wird so eine Konfiguration von der majestic.yaml-Migration, die gar
# keinen Boardbegriff kennt, und danach von jedem Upgrade weitergetragen, weil
# eine vorhandene machino.conf bewusst nicht ueberschrieben wird.
#
# Geprueft wird install.sh DIREKT: es geht um das Schreiben der Konfiguration,
# nicht um die Umschaltung des Streamers, die der Manager danach macht.
# ---------------------------------------------------------------------------
make_bundle; make_camera auto
R="$WORK/root"
mkdir -p "$R/etc/machino"
printf '# migriert aus majestic.yaml\nvideo.0.fps = 20\nrtsp.port = 554\n' > "$R/etc/machino/machino.conf"
run_install || bad "install over a board-less config exited non-zero: $(cat "$WORK/out")"
if grep -qE '^[[:space:]]*board[[:space:]]*=' "$R/etc/machino/machino.conf"; then ok
else bad "install left a config with no hardware selection - the daemon would refuse to start"; fi
if grep -q 'rtsp.port = 554' "$R/etc/machino/machino.conf"; then ok
else bad "adding the board line destroyed the existing settings"; fi

# Eine Konfiguration, die schon eine Auswahl hat, wird nicht angefasst -- auch
# nicht um eine zweite board-Zeile ergaenzt.
make_bundle; make_camera auto
R="$WORK/root"
mkdir -p "$R/etc/machino"
printf 'board = eigenes-profil\nrtsp.port = 555\n' > "$R/etc/machino/machino.conf"
run_install || bad "install over an explicit config exited non-zero: $(cat "$WORK/out")"
if [ "$(grep -c '^board' "$R/etc/machino/machino.conf")" = 1 ] &&
   grep -q '^board = eigenes-profil' "$R/etc/machino/machino.conf"; then ok
else bad "an existing board selection was overwritten or duplicated"; fi

# Auch platform= allein genuegt -- dann fasst install.sh nichts an.
make_bundle; make_camera auto
R="$WORK/root"
mkdir -p "$R/etc/machino"
printf 'platform = ingenic-t40nn\nsensor.model = imx307\n' > "$R/etc/machino/machino.conf"
run_install || bad "install over a platform-only config exited non-zero: $(cat "$WORK/out")"
if grep -qE '^[[:space:]]*board[[:space:]]*=' "$R/etc/machino/machino.conf"; then
    bad "install added a board line although platform was already set"
else ok; fi

if [ "$SKIP" -gt 0 ]; then
    echo "openipc install tests: $PASS passed, $FAIL failed, $SKIP skipped"
else
    echo "openipc install tests: $PASS passed, $FAIL failed"
fi
[ "$FAIL" -eq 0 ]
