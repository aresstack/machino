#!/bin/sh
# Host tests for openipc/sbin/streamerctl. No camera, no root: the script runs
# against a throwaway tree via STREAMERCTL_ROOT, with pgrep/wget/httpd stubbed.
#
# The invariant worth testing is the one that resets a T40NN when it is broken:
# the previous media owner must be fully stopped before the next one starts.
# The fake init scripts append to an event log, so the ordering is checked, not
# just the end state.
set -u

SRC=$(cd "$(dirname "$0")/.." && pwd)
CTL="$SRC/openipc/sbin/streamerctl"
[ -r "$CTL" ] || { echo "cannot find $CTL" >&2; exit 1; }

PASS=0; FAIL=0
ok()   { PASS=$((PASS + 1)); }
bad()  { FAIL=$((FAIL + 1)); echo "FAIL: $*" >&2; }
check(){ if [ "$2" = "$3" ]; then ok; else bad "$1: expected '$3', got '$2'"; fi; }

# Repo-local, see test_openipc_install.sh
ROOT="$SRC/tests/tmp-streamerctl"
rm -rf "$ROOT"; mkdir -p "$ROOT" || { echo "cannot create $ROOT" >&2; exit 1; }
STUB="$ROOT/stub"
EVENTS="$ROOT/events"
RUNDIR="$ROOT/run"
trap 'rm -rf "$ROOT"' EXIT INT TERM

setup() {
    rm -rf "$ROOT"/etc "$ROOT"/var "$STUB" "$EVENTS" "$RUNDIR"
    mkdir -p "$ROOT/etc/init.d" "$ROOT/etc/machino" "$ROOT/var/run" "$ROOT/var/www" "$STUB" "$RUNDIR"
    : > "$EVENTS"

    # fake daemons: a marker file stands for "the process exists"
    for d in majestic machino; do
        cat > "$ROOT/etc/init.d/$d" <<EOF
#!/bin/sh
case "\$1" in
  start) echo "start $d" >> "$EVENTS"; [ -f "$RUNDIR/$d.fail" ] || : > "$RUNDIR/$d" ;;
  stop)  echo "stop $d"  >> "$EVENTS"; rm -f "$RUNDIR/$d" ;;
esac
exit 0
EOF
        chmod +x "$ROOT/etc/init.d/$d"
    done

    # pgrep -x <name>  ->  does the marker exist?
    cat > "$STUB/pgrep" <<EOF
#!/bin/sh
name=""
for a in "\$@"; do case "\$a" in -*) ;; *) name="\$a" ;; esac; done
case "\$name" in
  majestic|machino) [ -f "$RUNDIR/\$name" ] && { echo 4242; exit 0; }; exit 1 ;;
esac
# pgrep -f "httpd ..." -> the pid our httpd stub parked
[ -f "$RUNDIR/httpd.pid" ] && { cat "$RUNDIR/httpd.pid"; exit 0; }
exit 1
EOF
    chmod +x "$STUB/pgrep"

    # httpd stub: a real background process, so kill -0 / kill work for real
    cat > "$STUB/httpd" <<EOF
#!/bin/sh
sleep 120 &
echo \$! > "$RUNDIR/httpd.pid"
exit 0
EOF
    chmod +x "$STUB/httpd"

    # wget stub: machino's healthcheck succeeds unless told otherwise
    cat > "$STUB/wget" <<EOF
#!/bin/sh
[ -f "$RUNDIR/machino" ] || exit 1
[ -f "$RUNDIR/api.fail" ] && exit 1
echo '{"lifecycle":"cold_idle"}'
exit 0
EOF
    chmod +x "$STUB/wget"
}

ctl() {
    STREAMERCTL_ROOT="$ROOT" STREAMERCTL_HTTPD="$STUB/httpd" \
    PATH="$STUB:$PATH" HEALTH_TIMEOUT=3 sh "$CTL" "$@" 2>&1
}

selected() { cat "$ROOT/etc/machino/streamer" 2>/dev/null || echo "(none)"; }
running()  { if [ -f "$RUNDIR/machino" ]; then echo machino
             elif [ -f "$RUNDIR/majestic" ]; then echo majestic
             else echo none; fi; }
events()   { tr '\n' ',' < "$EVENTS"; }

# ---------------------------------------------------------------- 1) status ---
setup
out=$(ctl status)
check "status defaults to majestic" "$(echo "$out" | sed -n 's/^selected:[[:space:]]*//p')" "majestic"

# ------------------------------------------------- 2) switch over to machino ---
setup
: > "$RUNDIR/majestic"                       # majestic is the current owner
ctl set machino >/dev/null
check "machino is selected"     "$(selected)" "machino"
check "machino is running"      "$(running)"  "machino"
check "majestic stopped first"  "$(events)"   "stop majestic,start machino,"

# --------------------------------------------- 3) never two owners at a time ---
# Between "stop majestic" and "start machino" no start may appear, and the
# marker files must never both exist. The event order above is the proof for
# the first half; this checks the script does not start before stopping.
setup
: > "$RUNDIR/majestic"
ctl set machino >/dev/null
first=$(head -1 "$EVENTS")
check "first event is a stop"   "$first" "stop majestic"

# ----------------------------------------- 4) failing machino rolls back ---
setup
: > "$RUNDIR/majestic"
: > "$RUNDIR/machino.fail"                   # machino refuses to start
ctl set machino >/dev/null 2>&1
check "selection stays majestic" "$(selected)" "majestic"
check "majestic runs again"      "$(running)"  "majestic"

# ------------------------------- 5) machino starts but its API stays silent ---
setup
: > "$RUNDIR/majestic"
: > "$RUNDIR/api.fail"
ctl set machino >/dev/null 2>&1
check "unhealthy machino rolls back" "$(selected)" "majestic"
check "majestic is back"             "$(running)"  "majestic"

# ------------------------------------------------- 6) switch back to majestic ---
setup
: > "$RUNDIR/majestic"
ctl set machino >/dev/null
: > "$EVENTS"
ctl set majestic >/dev/null
check "majestic is selected"   "$(selected)" "majestic"
check "majestic is running"    "$(running)"  "majestic"
check "machino stopped first"  "$(events)"   "stop machino,start majestic,"

# ------------------------------------------- 7) the WebUI host follows along ---
setup
: > "$RUNDIR/majestic"
ctl set machino >/dev/null
[ -r "$ROOT/var/run/machino-httpd.pid" ] && ok || bad "webui host not started for machino"
pid=$(cat "$ROOT/var/run/machino-httpd.pid" 2>/dev/null)
ctl set majestic >/dev/null
if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
    bad "webui host still running after switching back to majestic"
else
    ok
fi

# ------------------------------------------------- 8) the boot path applies it ---
setup
printf 'machino\n' > "$ROOT/etc/machino/streamer"
ctl boot >/dev/null
check "boot starts the selection" "$(running)" "machino"

# ------------------------------------------------------- 9) refuse nonsense ---
setup
out=$(ctl set banana 2>&1); rc=$?
[ "$rc" -ne 0 ] && ok || bad "unknown streamer was accepted"

# ---------------------------------------------- 10) api.port is read from conf ---
setup
printf 'api.port = 9099\n' > "$ROOT/etc/machino/machino.conf"
: > "$RUNDIR/machino"
out=$(ctl status)
case "$out" in *9099*) ok ;; *) bad "api.port from machino.conf not used: $out" ;; esac

echo "streamerctl tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
