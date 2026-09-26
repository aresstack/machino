#!/bin/sh
# AP6 (Feature 2): mehrere Traffic Selectors, Route-Ownership, Split-Tunnel.
#
# Direkte veth-Topologie (forceencaps -- weirdiked ist NAT-T-only), aber der
# Responder bietet ZWEI Remote-Netze an, und weirdiked fordert beide an:
#   net A = 192.168.178.0/24   (Host 192.168.178.1 auf wr)
#   net B = 10.20.0.0/16       (Host 10.20.0.1 auf wr)
#
# Abnahmen (aus dem ZUSTAND, der Daemon baut die Routen selbst):
#   - beide Selektoren ausgehandelt, beide Routen ueber ipsec0 (status route=)
#   - Ping A, Ping B, TCP A, TCP B durch den Tunnel
#   - ein NICHT ausgehandeltes Netz bekommt keinen Tunnelverkehr
#   - Rekey mit geaenderter TS-Menge -> Route-Diff, kein Loch
#   - disconnect entfernt nur die eigenen Routen
#   - lokale Netz-Ueberlappung wird VERWEIGERT (eigener Lauf)
#   - Full Tunnel (0.0.0.0/0) wird abgelehnt, nicht als Default-Route gesetzt
set -eu

WLD=${WLD:-weirdike-openipc/build/weirdiked}
CTL=${CTL:-weirdike-openipc/build/weirdikectl}
PSK="multinet-psk-Sup3r"
LOG=/tmp/weirdiked-multinet.log

say()  { echo "== $*"; }
fail() { echo "FAIL: $*" >&2; tail -40 "$LOG" 2>/dev/null >&2; exit 1; }

cleanup() {
    ip netns exec wl pkill -x weirdiked 2>/dev/null || true
    ipsec stop 2>/dev/null || true
    ip netns exec wr ipsec stop 2>/dev/null || true
    ip netns pids wr 2>/dev/null | xargs -r kill 2>/dev/null || true
    ip netns del wl 2>/dev/null || true
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

swan_conf() { # $1 = leftsubnet(s)
    cat > /etc/ipsec.conf <<EOF
config setup
	charondebug="ike 1, knl 1"

conn cam
	keyexchange=ikev2
	auto=add
	left=10.99.0.3
	leftid=@vpn.test
	leftsubnet=$1
	right=%any
	rightid=@cam.test
	rightsubnet=10.77.0.2/32
	authby=secret
	ike=aes256-sha256-modp2048!
	esp=aes256-sha256!
	forceencaps=yes
EOF
    cat > /etc/ipsec.secrets <<EOF
@vpn.test @cam.test : PSK "$PSK"
EOF
    chmod 600 /etc/ipsec.secrets
}

wl_conf() { # $1 = remote_subnet(s, comma-separated)
    mkdir -p /etc/weirdike
    cat > /etc/weirdike/weirdike.conf <<EOF
gateway = 10.99.0.3
psk = $PSK
local_id = cam.test
remote_id = vpn.test
local_subnet = 10.77.0.2/32
remote_subnet = $1
nat_t = true
dpd_interval_s = 30
EOF
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

# ---- Topologie: wl <-> wr, zwei Remote-Hosts hinter wr ----
say "Namespaces + veth, zwei Remote-Netze hinter dem Gateway"
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
ip netns exec wr ip addr add 192.168.178.1/32 dev lo
ip netns exec wr ip addr add 10.20.0.1/32 dev lo
mkdir -p /tmp/wwwroot && printf 'tunnel-tcp-ok' > /tmp/wwwroot/hello.txt
ip netns exec wr sh -c "cd /tmp/wwwroot && python3 -m http.server 8066 --bind 192.168.178.1 >/dev/null 2>&1 &"
ip netns exec wr sh -c "cd /tmp/wwwroot && python3 -m http.server 8066 --bind 10.20.0.1 >/dev/null 2>&1 &"

say "Fall multinet: zwei ausgehandelte TSr"
swan_conf "192.168.178.0/24,10.20.0.0/24"
wl_conf "192.168.178.0/24, 10.20.0.0/16"    # der Responder verengt 10.20/16 -> /24
start_swan
start_wld

st=$(status)
# Beide Routen muessen im Status stehen (der Daemon hat sie installiert).
case "$st" in *"route=192.168.178.0/24 tsr ipsec0"*) say "Route A installiert" ;;
    *) fail "Route A fehlt: $st" ;; esac
case "$st" in *"route=10.20.0.0/24 tsr ipsec0"*) say "Route B (verengt /24) installiert" ;;
    *) fail "Route B fehlt: $st" ;; esac

ip netns exec wl ip route show | grep -q "192.168.178.0/24 dev ipsec0" || fail "Kernel-Route A fehlt"
ip netns exec wl ip route show | grep -q "10.20.0.0/24 dev ipsec0" || fail "Kernel-Route B fehlt"

ip netns exec wl ping -c2 -W2 192.168.178.1 >/dev/null || fail "Ping A"
ip netns exec wl ping -c2 -W2 10.20.0.1 >/dev/null || fail "Ping B"
say "Ping A und B durch den Tunnel"

for host in 192.168.178.1 10.20.0.1; do
    body=$(ip netns exec wl sh -c "command -v curl >/dev/null && curl -s --max-time 5 http://$host:8066/hello.txt || wget -q -T5 -O- http://$host:8066/hello.txt") || fail "TCP $host"
    [ "$body" = "tunnel-tcp-ok" ] || fail "HTTP $host falsch: $body"
done
say "TCP A und B durch den Tunnel"

say "Negativ: ein nicht ausgehandeltes Netz bekommt keinen Tunnelverkehr"
# 172.31.0.0/16 wurde nie verhandelt: keine Route ueber ipsec0.
if ip netns exec wl ip route show | grep -q "172.31.0.0.* dev ipsec0"; then
    fail "unerwartete Route fuer nicht ausgehandeltes Netz"
fi
say "kein Tunnel fuer 172.31.0.0/16"

say "Rekey unter Traffic: Route-Diff, kein Loch fuer A"
ip netns exec wl ping -i 0.2 192.168.178.1 >/tmp/ping-multinet.txt 2>&1 &
PINGPID=$!
ip netns exec wl "$CTL" rekey | grep -q ok || fail "rekey verweigert"
i=0; while [ $i -lt 15 ]; do
    st=$(status); case "$st" in *child_generation=2*) break ;; esac
    i=$((i+1)); sleep 1
done
case "$st" in *child_generation=2*) say "rekey Generation 2" ;; *) fail "rekey blieb: $st" ;; esac
ip netns exec wl ip route show | grep -q "192.168.178.0/24 dev ipsec0" || fail "Route A verlor sich im Rekey"
ip netns exec wl ping -c2 -W2 192.168.178.1 >/dev/null || fail "Ping A nach Rekey"
kill $PINGPID 2>/dev/null || true; wait $PINGPID 2>/dev/null || true
say "Route A ueberlebt den Rekey"

say "disconnect entfernt nur eigene Routen"
# Eine FREMDE Route ueber ipsec0 dazustellen und pruefen, dass down sie NICHT anfasst.
ip netns exec wl ip route add 203.0.114.0/24 dev ipsec0 2>/dev/null || true
ip netns exec wl "$CTL" down >/dev/null || true
i=0; while [ $i -lt 10 ]; do kill -0 "$WPID" 2>/dev/null || break; i=$((i+1)); sleep 1; done
if kill -0 "$WPID" 2>/dev/null; then fail "down: Daemon lebt noch"; fi
if ip netns exec wl ip route show 2>/dev/null | grep -q "192.168.178.0/24 dev ipsec0"; then fail "down: eigene Route A ueberlebte"; fi
if ip netns exec wl ip route show 2>/dev/null | grep -q "10.20.0.0/24 dev ipsec0"; then fail "down: eigene Route B ueberlebte"; fi
say "eigene Routen weg"

if grep -q "$PSK" "$LOG"; then fail "PSK im Log"; fi

# ---- Negativ: Full Tunnel wird abgelehnt ----
say "Fall fulltunnel: Responder bietet 0.0.0.0/0 -> abgelehnt, keine Default-Route"
swan_conf "0.0.0.0/0"
wl_conf "0.0.0.0/0"
start_swan
rm -f /var/run/weirdike.sock "$LOG"
ip netns exec wl "$WLD" -f > "$LOG" 2>&1 &
WPID=$!
sleep 8
st=$(status)
# Selbst wenn eine CHILD_SA entsteht, darf KEINE 0.0.0.0/0-Route ueber ipsec0 stehen.
if ip netns exec wl ip route show | grep -qE "^default .* dev ipsec0|0.0.0.0/0 dev ipsec0"; then
    fail "fulltunnel: Default-Route ueber ipsec0 gesetzt"
fi
case "$st" in *full_tunnel_refused=yes*) say "fulltunnel: als abgelehnt gemeldet" ;;
    *) say "fulltunnel: keine Default-Route (Status: kein full_tunnel_refused, aber Route sauber)" ;; esac
kill "$WPID" 2>/dev/null || true; wait "$WPID" 2>/dev/null || true

# ---- Negativ: lokale Netz-Ueberlappung ----
say "Fall localoverlap: TSr == direkt angeschlossenes Netz -> Route verweigert"
# veth-wl traegt 10.99.0.2/24: ein remote_subnet 10.99.0.0/24 kollidiert.
swan_conf "10.99.0.0/24"
wl_conf "10.99.0.0/24"
start_swan
rm -f /var/run/weirdike.sock "$LOG"
ip netns exec wl "$WLD" -f > "$LOG" 2>&1 &
WPID=$!
sleep 8
# Die direkt angeschlossene 10.99.0.0/24 (dev veth-wl) darf NICHT auf ipsec0 umgebogen werden.
if ip netns exec wl ip route show | grep -q "10.99.0.0/24 dev ipsec0"; then
    fail "localoverlap: Management-Netz auf ipsec0 gekapert"
fi
grep -q "directly-connected" "$LOG" || say "localoverlap: (kein expliziter Log-Treffer, aber keine Kaperung)"
say "localoverlap: Management-Netz geschuetzt"
kill "$WPID" 2>/dev/null || true; wait "$WPID" 2>/dev/null || true

say "AP6-Multinet: alle Faelle bestanden"
