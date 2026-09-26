#!/bin/sh
# AP2 (Feature 2): IKEv2-Control-Plane-Interop gegen einen echten strongSwan.
#
# Laeuft in CI auf einem Linux-Runner mit root (sudo): zwei Netzwerk-Namespaces
# an einem veth-Paar, strongSwan (charon, klassischer starter) als Responder in
# "wr", weirdiked als Initiator in "wl". Abnahme ist der ZUSTAND AUS WEIRDIKE
# (weirdikectl status: state=CHILD_SA_ESTABLISHED), nie "Socket offen".
#
# Vier Faelle, getrennt gemeldet wie WeirdIKE-Diag sie trennt:
#   ok        PSK stimmt          -> CHILD_SA_ESTABLISHED, natt/child-Diag
#   badpsk    PSK falsch          -> FAILED + last_notify=24 (AUTHENTICATION_FAILED)
#   badprop   Responder nur AES128-> kein CHILD, last_notify=14 (NO_PROPOSAL_CHOSEN)
#   timeout   Gateway unerreichbar-> kein CHILD, last_notify=0 (Transport, nicht Auth)
#
# Secrets: der PSK darf in KEINER weirdiked-Ausgabe auftauchen (geprueft).
# AP2-Grenze: ipsec0 entsteht zwar (der Daemon ist integriert gebaut), aber es
# faehrt KEIN Nutzverkehr und es werden keine Routen gesetzt/geprueft -- das
# ist AP4.
set -eu

WLD=${WLD:-weirdike-openipc/build/weirdiked}
CTL=${CTL:-weirdike-openipc/build/weirdikectl}
PSK="interop-psk-Sup3rGeheim"
LOG=/tmp/weirdiked-interop.log

say() { echo "== $*"; }
fail() { echo "FAIL: $*" >&2; cat "$LOG" 2>/dev/null | tail -30 >&2; exit 1; }

cleanup() {
    ip netns exec wl pkill -x weirdiked 2>/dev/null || true
    ipsec stop 2>/dev/null || true
    ip netns exec wr ipsec stop 2>/dev/null || true
    ip netns del wl 2>/dev/null || true
    ip netns del wr 2>/dev/null || true
    rm -f /var/run/weirdike.sock
}
trap cleanup EXIT

say "Namespaces + veth"
cleanup 2>/dev/null || true
ip netns add wl
ip netns add wr
ip link add veth-wl type veth peer name veth-wr
ip link set veth-wl netns wl
ip link set veth-wr netns wr
ip netns exec wl ip addr add 10.99.0.2/24 dev veth-wl
ip netns exec wr ip addr add 10.99.0.3/24 dev veth-wr
ip netns exec wl ip link set lo up; ip netns exec wl ip link set veth-wl up
ip netns exec wr ip link set lo up; ip netns exec wr ip link set veth-wr up
ip netns exec wl ping -c1 -W2 10.99.0.3 >/dev/null || fail "veth traegt nicht"

swan_conf() { # $1 = ike-Proposal
    cat > /etc/ipsec.conf <<EOF
config setup
	charondebug="ike 1, cfg 1"

conn cam
	keyexchange=ikev2
	auto=add
	left=10.99.0.3
	leftid=@vpn.test
	leftsubnet=10.66.0.0/24
	right=%any
	rightid=@cam.test
	rightsubnet=10.77.0.2/32
	authby=secret
	ike=$1!
	esp=aes256-sha256!
EOF
    cat > /etc/ipsec.secrets <<EOF
@vpn.test @cam.test : PSK "$PSK"
EOF
    chmod 600 /etc/ipsec.secrets
}

wl_conf() { # $1 = psk  $2 = gateway
    mkdir -p /etc/weirdike
    cat > /etc/weirdike/weirdike.conf <<EOF
gateway = $2
psk = $1
local_id = cam.test
remote_id = vpn.test
local_subnet = 10.77.0.2/32
remote_subnet = 10.66.0.0/24
nat_t = true
dpd_interval_s = 30
EOF
    chmod 600 /etc/weirdike/weirdike.conf
}

swan_stop_everywhere() {
    # apt startet einen HOST-charon, und /var/run ist zwischen den netns
    # geteilt (kein Mount-Namespace): dessen PID-Files liessen "ipsec start"
    # im wr-ns still NICHT starten -- weirdiked retransmittierte ins Leere
    # (roter Lauf 1, exakt so im starter-Log). Es darf immer nur EINEN
    # charon geben, und zwar unseren im wr-ns.
    systemctl stop strongswan-starter 2>/dev/null || true
    ipsec stop 2>/dev/null || true
    ip netns exec wr ipsec stop 2>/dev/null || true
    sleep 1
    rm -f /var/run/charon.pid /var/run/starter.charon.pid /var/run/charon.ctl
}

start_swan() {
    swan_stop_everywhere
    ip netns exec wr ipsec start
    # charon braucht einen Moment, bis 500/4500 lauschen
    i=0; while [ $i -lt 20 ]; do
        ip netns exec wr ipsec status >/dev/null 2>&1 && break
        i=$((i+1)); sleep 1
    done
}

run_case() { # $1 name  $2 erwartetes state-Muster  $3 erwartetes last_notify ('' = egal)
    rm -f /var/run/weirdike.sock "$LOG"
    ip netns exec wl "$WLD" -f > "$LOG" 2>&1 &
    WPID=$!
    st=""
    i=0; while [ $i -lt 30 ]; do
        st=$(ip netns exec wl "$CTL" status 2>/dev/null || true)
        case "$st" in *"state=$2"*) break ;; esac
        # FAILED frueher akzeptieren, wenn genau das erwartet wird
        case "$2" in FAILED) case "$st" in *state=FAILED*) break ;; esac ;; esac
        i=$((i+1)); sleep 1
    done
    echo "$st" > "/tmp/status-$1.txt"
    case "$st" in
        *"state=$2"*) say "$1: state=$2 nach ${i}s" ;;
        *) fail "$1: erwartet state=$2, status: $st" ;;
    esac
    if [ -n "$3" ]; then
        case "$st" in
            *"last_notify=$3"*) say "$1: last_notify=$3 wie erwartet" ;;
            *) fail "$1: erwartet last_notify=$3, status: $st" ;;
        esac
    fi
    # Secrets: der PSK darf weder im Log noch im Status stehen.
    if grep -q "$PSK" "$LOG" || echo "$st" | grep -q "$PSK"; then
        fail "$1: PSK ist in Log/Status sichtbar"
    fi
    kill "$WPID" 2>/dev/null || true
    wait "$WPID" 2>/dev/null || true
}

say "Fall ok: PSK korrekt, AES-CBC-256/SHA-256/DH14"
swan_conf "aes256-sha256-modp2048"
wl_conf "$PSK" "10.99.0.3"
start_swan
run_case ok CHILD_SA_ESTABLISHED ""
st=$(cat /tmp/status-ok.txt)
case "$st" in *child=*) say "ok: Child-Diag vorhanden" ;; *) fail "ok: kein child= im Status" ;; esac
case "$st" in *natt=*) say "ok: NAT-T-Diag vorhanden" ;; *) fail "ok: kein natt= im Status" ;; esac
# strongSwan-Seite als Gegenprobe:
ip netns exec wr ipsec status | grep -q "ESTABLISHED" || fail "strongSwan sieht keine SA"
say "strongSwan bestaetigt die SA"

say "Fall badpsk: AUTHENTICATION_FAILED (24), sauber getrennt vom Timeout"
wl_conf "voellig-falscher-psk" "10.99.0.3"
run_case badpsk FAILED 24

say "Fall badprop: Responder erlaubt nur AES128/SHA1 -> NO_PROPOSAL_CHOSEN (14)"
swan_conf "aes128-sha1-modp1024"
wl_conf "$PSK" "10.99.0.3"
start_swan
run_case badprop FAILED 14

say "Fall timeout: Gateway unerreichbar -> Transportfehler, KEIN Auth-Notify"
wl_conf "$PSK" "10.99.0.9"
rm -f /var/run/weirdike.sock "$LOG"
ip netns exec wl "$WLD" -f > "$LOG" 2>&1 &
WPID=$!
sleep 12
st=$(ip netns exec wl "$CTL" status 2>/dev/null || true)
echo "$st" > /tmp/status-timeout.txt
case "$st" in
    *state=CHILD_SA_ESTABLISHED*|*state=IKE_SA_ESTABLISHED*) fail "timeout: SA gegen ein Nichts?" ;;
esac
case "$st" in
    *last_notify=0*) say "timeout: last_notify=0 (Transport, nicht Auth) wie erwartet" ;;
    *) fail "timeout: unerwartete Diag: $st" ;;
esac
kill "$WPID" 2>/dev/null || true; wait "$WPID" 2>/dev/null || true

say "AP2-Interop: alle vier Faelle bestanden"
