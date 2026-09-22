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

say()  { echo "$*"; }
warn() { echo "install: $*" >&2; }
die()  { echo "install: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            cat <<EOF
usage: ./install.sh

The WebUI login is Machino's Majestic drop-in session login against the
camera's root account - there is nothing to configure here.
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
put() {
    _m=$1; _s=$2; _d=$3
    mkdir -p "$(dirname "$_d")" || return 1
    _t="$_d.machino-new.$$"
    if ! cp "$_s" "$_t"; then rm -f "$_t"; return 1; fi
    if ! chmod "$_m" "$_t"; then rm -f "$_t"; return 1; fi
    if mv -f "$_t" "$_d" 2>/dev/null; then return 0; fi
    rm -f "$_t"
    say "atomic replace unavailable for $_d - writing in place"
    cp "$_s" "$_d" && chmod "$_m" "$_d"
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

need_kb=$(( ($(wc -c < "$HERE/machino") / 1024) + 256 ))
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
        *)  say "camera reports '$dt', which this installer has no rule for - platform NOT verified" ;;
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
if [ -f "$ROOT/usr/bin/machino" ]; then
    if cp -p "$ROOT/usr/bin/machino" "$BACKUP/machino.prev.tmp" &&
       mv -f "$BACKUP/machino.prev.tmp" "$BACKUP/machino.prev"; then
        say "kept the previous daemon as $BACKUP/machino.prev"
    else
        rm -f "$BACKUP/machino.prev.tmp"
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
