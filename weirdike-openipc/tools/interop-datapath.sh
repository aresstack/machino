#!/bin/sh
# AP4 (Feature 2): der ESP-Datenpfad ueber ipsec0, komplett im Namespace-CI.
#
# Zwei Topologien:
#   direkt   wl(10.99.0.2) --veth-- wr(10.99.0.3)
#            strongSwan mit forceencaps=yes: weirdiked ist NAT-T-only
#            (ESP-in-UDP/4500 immer), ohne echtes NAT muss die Gegenseite
#            UDP-Encap erzwingen -- exakt die Produktionsannahme "beide
#            Underlays hinter NAT", nur ehrlich hingeschrieben.
#   nat      wl(10.98.0.2) --veth-- wn(NAT) --veth-- wr(10.99.0.3)
#            MASQUERADE in wn: nat_detected=yes muss im Status stehen und
#            der Traffic weiter laufen.
#
# Der "Remote-Host" hinter dem Gateway ist 10.66.0.1 auf wr (lo-Alias);
# strongSwans eigener XFRM-Stack im wr-Namespace entschluesselt und stellt zu.
#
# Abnahmen (jede aus dem ZUSTAND, nie aus "Socket offen"):
#   - ipsec0 traegt 10.77.0.2, Route 10.66.0.0/24 dev ipsec0 (vom Daemon
#     installiert, nicht vom Test)
#   - Ping UND TCP (HTTP) durch den Tunnel
#   - Selector-Enforcement: Pakete ausserhalb TSr werden GEZAEHLT verworfen
#   - Garbage-ESP (bad SPI/ICV) toetet den Daemon nicht
#   - Rekey unter Traffic: child_generation=2, Ping laeuft weiter
#   - down raeumt restlos: keine Route, ipsec0 ohne UP
#   - PSK nie in Log/Status
set -eu

WLD=${WLD:-weirdike-openipc/build/weirdiked}
CTL=${CTL:-weirdike-openipc/build/weirdikectl}
PSK="datapath-psk-N0chGeheimer"
LOG=/tmp/weirdiked-datapath.log

say()  { echo "== $*"; }
fail() { echo "FAIL: $*" >&2; tail -40 "$LOG" 2>/dev/null >&2; exit 1; }

cleanup() {
    ip netns exec wl pkill -x weirdiked 2>/dev/null || true
    ipsec stop 2>/dev/null || true
    ip netns exec wr ipsec stop 2>/dev/null || true
    ip netns pids wr 2>/dev/null | xargs -r kill 2>/dev/null || true
    ip netns del wl 2>/dev/null || true
    ip netns del wn 2>/dev/null || true
    ip netns del wr 2>/dev/null || true
    rm -f /var/run/weirdike.sock
}
trap cleanup EXIT

swan_stop_everywhere() {
    systemctl stop strongswan-starter 2>/dev/null || true
    ipsec stop 2>/dev/null || true
    ip netns exec wr ipsec stop 2>/dev/null || true
    sleep 1
    rm -f /var/run/charon.pid /var/run/starter.charon.pid /var/run/charon.ctl
}

swan_conf() { # $1 = extra conn lines (z.B. forceencaps=yes)
    cat > /etc/ipsec.conf <<EOF
config setup
	charondebug="ike 1, knl 1"

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
	ike=aes256-sha256-modp2048!
	esp=aes256-sha256!
	$1
EOF
    cat > /etc/ipsec.secrets <<EOF
@vpn.test @cam.test : PSK "$PSK"
EOF
    chmod 600 /etc/ipsec.secrets
}

wl_conf() { # $1 = gateway  $2 = bind_ip (optional)  $3 = bind_dev (optional)
    mkdir -p /etc/weirdike
    cat > /etc/weirdike/weirdike.conf <<EOF
gateway = $1
psk = $PSK
local_id = cam.test
remote_id = vpn.test
local_subnet = 10.77.0.2/32
remote_subnet = 10.66.0.0/24
nat_t = true
dpd_interval_s = 30
EOF
    # AP5: die konkrete Underlay-Bindung, wie machinod sie fuer eine Session
    # schreibt (bind an IP + SO_BINDTODEVICE).
    if [ -n "${2:-}" ]; then echo "bind_ip = $2" >> /etc/weirdike/weirdike.conf; fi
    if [ -n "${3:-}" ]; then echo "bind_dev = $3" >> /etc/weirdike/weirdike.conf; fi
    chmod 600 /etc/weirdike/weirdike.conf
}

start_swan() {
    swan_stop_everywhere
    ip netns exec wr ipsec start
    i=0; while [ $i -lt 20 ]; do
        ip netns exec wr ss -uln 2>/dev/null | grep -q ':500 ' && break
        i=$((i+1)); sleep 1
    done
    ip netns exec wr ss -uln | grep -q ':500 ' || fail "charon lauscht nicht im wr-ns"
}

start_wld() {
    rm -f /var/run/weirdike.sock "$LOG"
    ip netns exec wl "$WLD" -f > "$LOG" 2>&1 &
    WPID=$!
    st=""
    i=0; while [ $i -lt 30 ]; do
        st=$(ip netns exec wl "$CTL" status 2>/dev/null || true)
        case "$st" in *state=CHILD_SA_ESTABLISHED*) break ;; esac
        i=$((i+1)); sleep 1
    done
    case "$st" in *state=CHILD_SA_ESTABLISHED*) say "CHILD_SA_ESTABLISHED nach ${i}s" ;;
        *) fail "kein CHILD_SA: $st" ;; esac
}

status() { ip netns exec wl "$CTL" status 2>/dev/null || true; }

check_datapath() { # $1 = Topologie-Name
    # Der DAEMON muss ipsec0 und die Split-Route gebaut haben -- der Test
    # baut nichts selbst.
    ip netns exec wl ip addr show dev ipsec0 | grep -q "10.77.0.2" \
        || fail "$1: ipsec0 traegt nicht 10.77.0.2"
    ip netns exec wl ip route show | grep -q "10.66.0.0/24 dev ipsec0" \
        || fail "$1: Split-Route 10.66.0.0/24 -> ipsec0 fehlt"
    say "$1: ipsec0 + Route vom Daemon installiert"

    ip netns exec wl ping -c3 -W2 10.66.0.1 >/dev/null \
        || fail "$1: Ping durch den Tunnel"
    say "$1: Ping durch den Tunnel ok"

    # TCP: ein HTTP-GET gegen den Python-Server hinter dem Gateway.
    body=$(ip netns exec wl sh -c "command -v curl >/dev/null && curl -s --max-time 5 http://10.66.0.1:8066/hello.txt || wget -q -T5 -O- http://10.66.0.1:8066/hello.txt") \
        || fail "$1: TCP/HTTP durch den Tunnel"
    [ "$body" = "tunnel-tcp-ok" ] || fail "$1: HTTP-Antwort falsch: $body"
    say "$1: TCP (HTTP) durch den Tunnel ok"

    st=$(status)
    case "$st" in *child=up*) ;; *) fail "$1: child=up fehlt: $st" ;; esac
    case "$st" in *tx_packets=0*) fail "$1: tx_packets=0 trotz Traffic" ;; esac
}

# ---------------------------------------------------------------- direkt
say "Topologie direkt (forceencaps: weirdiked ist NAT-T-only)"
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
# Der Host hinter dem Gateway:
ip netns exec wr ip addr add 10.66.0.1/32 dev lo
mkdir -p /tmp/wwwroot && printf 'tunnel-tcp-ok' > /tmp/wwwroot/hello.txt
ip netns exec wr sh -c "cd /tmp/wwwroot && python3 -m http.server 8066 --bind 10.66.0.1 >/dev/null 2>&1 &"

swan_conf "forceencaps=yes"
wl_conf 10.99.0.3
start_swan
start_wld
check_datapath direkt

say "Selector-Enforcement: Ziel ausserhalb TSr wird gezaehlt verworfen"
ip netns exec wl ip route add 10.55.0.0/24 dev ipsec0
ip netns exec wl ping -c1 -W1 10.55.0.1 >/dev/null 2>&1 || true
st=$(status)
case "$st" in
    *tx_drop_selector=0*) fail "selector: tx_drop_selector blieb 0" ;;
    *tx_drop_selector=*)  say "selector: Drop gezaehlt" ;;
    *) fail "selector: kein tx_drop_selector im Status: $st" ;;
esac
ip netns exec wl ip route del 10.55.0.0/24 dev ipsec0

say "Garbage-ESP: Muell auf 4500 darf den Daemon nicht toeten"
# KEIN -p 4500: den Quellport 4500 haelt in wr charon -- ncs bind schluege
# fehl und der Test waere still leer (Garbage, das nie gesendet wurde).
ip netns exec wr sh -c 'printf "\xde\xad\xbe\xef0123456789012345678901234567890123456789" | timeout 2 nc -u -w1 10.99.0.2 4500' 2>/dev/null || true
sleep 1
st=$(status)
case "$st" in *state=CHILD_SA_ESTABLISHED*) say "garbage: Daemon lebt, SA steht" ;;
    *) fail "garbage: Status danach: $st" ;; esac
ip netns exec wl ping -c1 -W2 10.66.0.1 >/dev/null || fail "garbage: Tunnel danach tot"

say "Rekey unter Traffic"
ip netns exec wl ping -i 0.2 10.66.0.1 >/tmp/ping-rekey.txt 2>&1 &
PINGPID=$!
ip netns exec wl "$CTL" rekey | grep -q "ok" || fail "rekey: ctl verweigert"
i=0; while [ $i -lt 15 ]; do
    st=$(status)
    case "$st" in *child_generation=2*) break ;; esac
    i=$((i+1)); sleep 1
done
case "$st" in *child_generation=2*) say "rekey: Generation 2 nach ${i}s" ;;
    *) fail "rekey: Generation blieb: $st" ;; esac
ip netns exec wl ping -c3 -W2 10.66.0.1 >/dev/null || fail "rekey: Ping nach Rekey"
kill $PINGPID 2>/dev/null || true; wait $PINGPID 2>/dev/null || true
say "rekey: Traffic laeuft auf Generation 2 weiter"

say "down raeumt restlos"
ip netns exec wl "$CTL" down >/dev/null || true
i=0; while [ $i -lt 10 ]; do
    kill -0 "$WPID" 2>/dev/null || break
    i=$((i+1)); sleep 1
done
# if-Form, nicht `cmd && fail`: unter set -e beendet eine falsche &&-Liste
# das Skript GENAU im Gutfall (vierte Instanz dieser Fallenfamilie).
if kill -0 "$WPID" 2>/dev/null; then fail "down: Daemon lebt noch"; fi
if ip netns exec wl ip route show | grep -q "dev ipsec0"; then fail "down: Route ueberlebte"; fi
if ip netns exec wl ip link show ipsec0 2>/dev/null | grep -q "UP"; then fail "down: ipsec0 noch UP"; fi
say "down: Route weg, ipsec0 down"

if grep -q "$PSK" "$LOG"; then fail "PSK steht im Daemon-Log"; fi

# ---------------------------------------------------------------- NAT
say "Topologie nat: wl hinter MASQUERADE (nat_detected=yes + Traffic)"
ip netns exec wr ipsec stop 2>/dev/null || true
ip netns del wl 2>/dev/null || true
ip netns del wr 2>/dev/null || true
ip netns add wl
ip netns add wn
ip netns add wr
ip link add veth-a type veth peer name veth-b     # wl <-> wn
ip link add veth-c type veth peer name veth-d     # wn <-> wr
ip link set veth-a netns wl; ip link set veth-b netns wn
ip link set veth-c netns wn; ip link set veth-d netns wr
ip netns exec wl ip addr add 10.98.0.2/24 dev veth-a
ip netns exec wn ip addr add 10.98.0.1/24 dev veth-b
ip netns exec wn ip addr add 10.99.0.1/24 dev veth-c
ip netns exec wr ip addr add 10.99.0.3/24 dev veth-d
for ns in wl wn wr; do ip netns exec $ns ip link set lo up; done
ip netns exec wl ip link set veth-a up
ip netns exec wn ip link set veth-b up; ip netns exec wn ip link set veth-c up
ip netns exec wr ip link set veth-d up
ip netns exec wl ip route add default via 10.98.0.1
ip netns exec wn sysctl -qw net.ipv4.ip_forward=1
ip netns exec wn iptables -t nat -A POSTROUTING -o veth-c -j MASQUERADE
ip netns exec wr ip addr add 10.66.0.1/32 dev lo
ip netns exec wr sh -c "cd /tmp/wwwroot && python3 -m http.server 8066 --bind 10.66.0.1 >/dev/null 2>&1 &"

swan_conf ""
# AP5: an die konkrete Underlay-IP + Device gebunden, wie machinod es fuer
# eine Session schreibt -- der Handshake muss trotzdem durch das NAT gehen.
wl_conf 10.99.0.3 10.98.0.2 veth-a
start_swan
start_wld
st=$(status)
case "$st" in *nat_detected=yes*) say "nat: nat_detected=yes" ;;
    *) fail "nat: nat_detected fehlt: $st" ;; esac
case "$st" in *ike_transport=udp4500*) say "nat: IKE floated auf UDP/4500" ;;
    *) fail "nat: ike_transport=udp4500 fehlt: $st" ;; esac
case "$st" in *esp_transport=udp4500*) say "nat: ESP-in-UDP bestaetigt" ;;
    *) fail "nat: esp_transport fehlt: $st" ;; esac
check_datapath nat
if grep -q "$PSK" "$LOG"; then fail "PSK steht im Daemon-Log (nat)"; fi

kill "$WPID" 2>/dev/null || true; wait "$WPID" 2>/dev/null || true
say "AP4-Datenpfad: alle Faelle bestanden"
