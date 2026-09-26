#!/bin/sh
# AP7 (Feature 2): Daemon-Lifecycle unter Last -- Rekey und Peer-Verlust.
#
# Direkte veth-Topologie (forceencaps -- NAT-T-only). Was hier gegen einen
# echten charon geprueft wird, ist DAEMON-Verhalten (die machinod-Seite --
# Runtime-State, Reconnect-Backoff, manualStop -- deckt der Host-Test ab):
#   - Child-Rekey unter Traffic OHNE Tunnel-Neuaufbau (Generation waechst,
#     ipsec0/Route bleiben, Ping laeuft weiter)
#   - Peer verschwindet -> DPD gibt auf -> FAILED -> Datenpfad faellt CLOSED
#     (Route weg, ipsec0 down), aber der Daemon lebt und meldet FAILED
#   - kein Crash, keine Secrets im Log
set -eu

WLD=${WLD:-weirdike-openipc/build/weirdiked}
CTL=${CTL:-weirdike-openipc/build/weirdikectl}
PSK="lifecycle-psk-Sup3r"
LOG=/tmp/weirdiked-lifecycle.log

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
ip netns exec wr ip addr add 10.66.0.1/32 dev lo

cat > /etc/ipsec.conf <<EOF
config setup
	charondebug="ike 1"
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
	forceencaps=yes
EOF
cat > /etc/ipsec.secrets <<EOF
@vpn.test @cam.test : PSK "$PSK"
EOF
chmod 600 /etc/ipsec.secrets

mkdir -p /etc/weirdike
cat > /etc/weirdike/weirdike.conf <<EOF
gateway = 10.99.0.3
psk = $PSK
local_id = cam.test
remote_id = vpn.test
local_subnet = 10.77.0.2/32
remote_subnet = 10.66.0.0/24
nat_t = true
dpd_interval_s = 3
EOF
chmod 600 /etc/weirdike/weirdike.conf

swan_stop_everywhere
ip netns exec wr ipsec start
i=0; while [ $i -lt 20 ]; do ip netns exec wr ss -uln 2>/dev/null | grep -q ':500 ' && break; i=$((i+1)); sleep 1; done
ip netns exec wr ss -uln | grep -q ':500 ' || fail "charon lauscht nicht im wr-ns"

rm -f /var/run/weirdike.sock "$LOG"
ip netns exec wl "$WLD" -f > "$LOG" 2>&1 &
WPID=$!
i=0; while [ $i -lt 30 ]; do
    st=$(ip netns exec wl "$CTL" status 2>/dev/null || true)
    case "$st" in *state=CHILD_SA_ESTABLISHED*) break ;; esac
    i=$((i+1)); sleep 1
done
case "$st" in *state=CHILD_SA_ESTABLISHED*) say "CHILD_SA nach ${i}s" ;; *) fail "kein CHILD_SA: $st" ;; esac
ip netns exec wl ip route show | grep -q "10.66.0.0/24 dev ipsec0" || fail "Route fehlt"
ip netns exec wl ping -c2 -W2 10.66.0.1 >/dev/null || fail "Ping vor Rekey"
say "Datenpfad steht"

say "Child-Rekey unter Traffic"
ip netns exec wl ping -i 0.2 10.66.0.1 >/tmp/ping-lifecycle.txt 2>&1 &
PINGPID=$!
ip netns exec wl "$CTL" rekey | grep -q ok || fail "rekey verweigert"
i=0; while [ $i -lt 15 ]; do
    st=$(ip netns exec wl "$CTL" status 2>/dev/null || true)
    case "$st" in *child_generation=2*) break ;; esac
    i=$((i+1)); sleep 1
done
case "$st" in *child_generation=2*) say "Generation 2 nach ${i}s" ;; *) fail "kein Rekey: $st" ;; esac
ip netns exec wl ip route show | grep -q "10.66.0.0/24 dev ipsec0" || fail "Route im Rekey verloren"
ip netns exec wl ping -c2 -W2 10.66.0.1 >/dev/null || fail "Ping nach Rekey"
kill $PINGPID 2>/dev/null || true; wait $PINGPID 2>/dev/null || true
say "Rekey ohne Tunnel-Neuaufbau, Traffic laeuft weiter"

say "Peer-Antworten verschwinden (iptables DROP) -> DPD -> Tunnel weg -> Datenpfad closed"
# Der KANONISCHE DPD-Ausloeser: die Gegenseite bleibt auf IP-Ebene erreichbar
# (weirdikeds Probes gehen RAUS), aber ihre Antworten werden verworfen. So
# laeuft die DPD (dpd_interval_s=3, Default-Retries) sauber in FAILED. Kein
# 'ipsec stop' (das schickte ein graceful DELETE -> CLOSED statt DPD),
# und kein veth-down (dann scheitert schon das Senden, mehrdeutig). Beides,
# FAILED (DPD) ODER CLOSED (falls doch ein DELETE durchkam), muss den
# Datenpfad fail-closed abbauen.
ip netns exec wl iptables -A INPUT -p udp --dport 4500 -j DROP 2>/dev/null || true
ip netns exec wl iptables -A INPUT -p udp --dport 500 -j DROP 2>/dev/null || true
i=0; while [ $i -lt 45 ]; do
    st=$(ip netns exec wl "$CTL" status 2>/dev/null || true)
    case "$st" in *state=FAILED*|*state=CLOSED*) break ;; esac
    # der Daemon koennte den Socket nur bei Leben bedienen -- leerer Status = tot
    [ -z "$st" ] && fail "Daemon weg (ctl leer) -- haette FAILED/CLOSED melden muessen"
    i=$((i+1)); sleep 1
done
case "$st" in
    *state=FAILED*) say "FAILED nach ${i}s (DPD)" ;;
    *state=CLOSED*) say "CLOSED nach ${i}s (Peer-Delete durchgekommen)" ;;
    *) fail "weder FAILED noch CLOSED: $st" ;;
esac

# Fail closed: Route weg, ipsec0 down -- nichts darf auf der toten SA reiten.
if ip netns exec wl ip route show | grep -q "10.66.0.0/24 dev ipsec0"; then fail "Route ueberlebte den Verlust"; fi
if ip netns exec wl ip link show ipsec0 2>/dev/null | grep -q "state UP\|,UP,"; then fail "ipsec0 noch UP"; fi
say "Datenpfad ist CLOSED"

# Der Daemon lebt noch (meldet FAILED), stirbt nicht.
if ! kill -0 "$WPID" 2>/dev/null; then fail "Daemon abgestuerzt statt FAILED zu melden"; fi
say "Daemon lebt und meldet FAILED"

if grep -q "$PSK" "$LOG"; then fail "PSK im Log"; fi
kill "$WPID" 2>/dev/null || true; wait "$WPID" 2>/dev/null || true

say "AP7-Lifecycle: alle Faelle bestanden"
