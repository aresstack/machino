// AP3 (Feature 2): IpsecConfig/IpsecService am Fake-Backend. Was hier
// zaehlt: die Ablehnung MIT NAMEN (kein stilles Downgrade), der write-only-
// PSK (nie zurueck, wird beim Speichern ohne neuen Wert weitergetragen),
// atomare Persistenz beider Dateien, und das Status-Mapping 1:1 aus den
// weirdikectl-Zeilen inklusive der getrennten Fehlerklassen 24/14/38/0.
#include "core/net/ipsec_config.hpp"
#include "core/net/ipsec_service.hpp"

#include <cstdio>
#include <string>

using namespace machino::ipsec;

extern int g_fail_ext, g_pass_ext;
#define ICHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

const char* MCONF = "test_ipsec_machino.conf";
const char* DCONF = "test_ipsec_daemon.conf";

struct FakeBackend : IIpsecBackend {
    bool running = false;
    bool start_ok = true, stop_ok = true;
    int  starts = 0, stops = 0;
    std::string status_text;
    // AP5:
    bool resolve_ok = true;
    std::string resolved = "203.0.113.5";
    struct Route { std::string ip, dev, gw; };
    std::vector<Route> routes;                 // die aktuell installierten
    int route_adds = 0, route_dels = 0;

    bool daemon_running() override { return running; }
    bool start_daemon(std::string& err) override {
        ++starts;
        if (!start_ok) { err = "start kaputt"; return false; }
        running = true; return true;
    }
    bool stop_daemon(std::string& err) override {
        ++stops;
        if (!stop_ok) { err = "stop kaputt"; return false; }
        running = false; return true;
    }
    bool ctl_status(std::string& out) override { out = status_text; return running; }
    bool resolve4(const std::string&, std::string& out) override {
        if (!resolve_ok) return false;
        out = resolved; return true;
    }
    bool add_peer_route(const std::string& ip, const std::string& dev,
                        const std::string& gw, std::string&) override {
        ++route_adds; routes.push_back({ip, dev, gw}); return true;
    }
    bool del_peer_route(const std::string& ip, const std::string& dev,
                        const std::string& gw, std::string&) override {
        ++route_dels;
        for (size_t i = 0; i < routes.size(); ++i)
            if (routes[i].ip == ip && routes[i].dev == dev && routes[i].gw == gw) {
                routes.erase(routes.begin() + i);
                return true;
            }
        return true;                           // idempotent, wie das echte Backend
    }
};

// AP5 §11: zwei Uplinks mit verschiedenen Adressen -- der Test sieht, WOHIN
// gebunden wurde.
struct FakeUplinks : IIpsecUplinks {
    UnderlayView eth, cell;
    FakeUplinks() {
        eth.usable = true;  eth.kind = "ethernet"; eth.ifname = "eth0";
        eth.ipv4 = "10.0.0.5"; eth.gateway = "10.0.0.1";
        cell.usable = true; cell.kind = "cellular"; cell.ifname = "usb0";
        cell.ipv4 = "100.71.3.9"; cell.gateway = "100.71.3.1";
    }
    bool select(Underlay w, UnderlayView& out, std::string& err) override {
        if (w == Underlay::Cellular) { out = cell; if (!cell.usable) err = "cellular nicht verbunden"; return cell.usable; }
        if (w == Underlay::Ethernet) { out = eth;  if (!eth.usable)  err = "ethernet nicht verbunden"; return eth.usable; }
        if (w == Underlay::Auto)     { out = eth.usable ? eth : cell; return out.usable; }
        err = "kein solcher Uplink"; return false;
    }
};

IpsecConfig sample()
{
    IpsecConfig c;
    c.enabled = true;
    c.gateway = "vpn.example.org";
    c.local_id = "cam.test";
    c.remote_id = "vpn.test";
    c.local_subnet = "10.77.0.2/32";
    c.remote_subnet = "10.66.0.0/24";
    return c;
}

std::string slurp(const char* p)
{
    FILE* f = fopen(p, "rb");
    if (!f) return {};
    std::string s; char b[512]; size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

void test_config_roundtrip()
{
    const IpsecConfig c = sample();
    ICHECK(validate(c).empty());
    IpsecConfig back;
    std::string err;
    ICHECK(from_machino_conf(to_machino_conf(c), back, err));
    ICHECK(back.enabled == c.enabled);
    ICHECK(back.gateway == c.gateway);
    ICHECK(back.port == c.port);
    ICHECK(back.local_subnet == c.local_subnet);
    ICHECK(back.remote_subnet == c.remote_subnet);
    ICHECK(back.nat_t == c.nat_t);
}

void test_validate_names_the_problem()
{
    IpsecConfig c = sample();
    c.ike_enc = {"aes128cbc"};
    std::string e = validate(c);
    // Die Ablehnung nennt den fremden Algorithmus BEIM NAMEN.
    ICHECK(e.find("aes128cbc") != std::string::npos);
    ICHECK(e.find("ikeEnc") != std::string::npos);

    c = sample(); c.esp_hash = {"sha256", "sha1"};   // Menge != genau eins
    ICHECK(!validate(c).empty());

    c = sample(); c.remote_subnet = "10.66.0.0/33";
    e = validate(c);
    ICHECK(e.find("remoteSubnet") != std::string::npos);

    c = sample(); c.gateway = "bad host!";
    ICHECK(validate(c).find("gateway") != std::string::npos);

    c = sample(); c.remote_subnet.clear();           // enabled braucht das Split-Netz
    ICHECK(!validate(c).empty());

    Underlay u;
    ICHECK(!underlay_from_name("modem", u));
    ICHECK(underlay_from_name("cellular", u) && u == Underlay::Cellular);
}

void test_psk_write_only_carry()
{
    // Erster Schreibvorgang traegt den PSK, der zweite (ohne PSK) MUSS ihn
    // unveraendert weitertragen -- und er darf nur in der Daemon-Datei stehen.
    bool present = false;
    const std::string d1 = to_weirdike_conf(sample(), "s3cret-psk", "", &present);
    ICHECK(present);
    ICHECK(d1.find("psk = s3cret-psk\n") != std::string::npos);

    const std::string d2 = to_weirdike_conf(sample(), "", d1, &present);
    ICHECK(present);
    ICHECK(d2.find("psk = s3cret-psk\n") != std::string::npos);

    const std::string d3 = to_weirdike_conf(sample(), "", "", &present);
    ICHECK(!present);
    ICHECK(d3.find("psk") == std::string::npos);

    // Die machino-Datei kennt das Secret NICHT.
    ICHECK(to_machino_conf(sample()).find("psk") == std::string::npos);
}

void test_service_persists_and_guards()
{
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    IpsecService svc(be, MCONF, DCONF);

    // enabled=true ohne PSK: das Speichern wird abgelehnt, BEVOR etwas auf
    // der Platte landet.
    ICHECK(!svc.set_config(sample(), "").empty());
    ICHECK(slurp(MCONF).empty());

    ICHECK(svc.set_config(sample(), "s3cret-psk").empty());
    ICHECK(svc.psk_set());
    const std::string m = slurp(MCONF);
    ICHECK(!m.empty());
    ICHECK(m.find("s3cret-psk") == std::string::npos);      // Secret NIE in der machino-Datei
    ICHECK(slurp(DCONF).find("psk = s3cret-psk") != std::string::npos);

    // Reload liest denselben Stand.
    const IpsecConfig c2 = svc.config();
    ICHECK(c2.enabled && c2.gateway == "vpn.example.org");

    // Speichern ohne neuen PSK laesst den alten stehen.
    IpsecConfig c3 = c2; c3.port = 4500;
    ICHECK(svc.set_config(c3, "").empty());
    ICHECK(svc.psk_set());
    ICHECK(slurp(DCONF).find("psk = s3cret-psk") != std::string::npos);

    remove(MCONF); remove(DCONF);
}

void test_connect_disconnect()
{
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    IpsecService svc(be, MCONF, DCONF);

    // disabled: connect verweigert, disconnect ist idempotent-ok.
    ICHECK(!svc.connect().empty());
    ICHECK(svc.disconnect().empty());
    ICHECK(be.starts == 0 && be.stops == 0);

    ICHECK(svc.set_config(sample(), "s3cret-psk").empty());
    ICHECK(svc.connect().empty());
    ICHECK(be.starts == 1 && be.running);
    ICHECK(svc.connect().empty());          // schon verbunden: kein 2. Start
    ICHECK(be.starts == 1);

    ICHECK(svc.disconnect().empty());
    ICHECK(be.stops == 1 && !be.running);

    // Backend-Fehler kommen als Text an, nicht als stilles Okay.
    be.start_ok = false;
    ICHECK(svc.connect() == "start kaputt");

    remove(MCONF); remove(DCONF);
}

void test_status_mapping()
{
    // Daemon aus: enabled entscheidet Disabled vs Disconnected.
    VpnStatus st = parse_status("", false, false);
    ICHECK(st.state == VpnState::Disabled && !st.daemon_running);
    st = parse_status("", false, true);
    ICHECK(st.state == VpnState::Disconnected);

    // Der Gutfall, Zeilen wie weirdikectl sie liefert.
    st = parse_status(
        "state=CHILD_SA_ESTABLISHED\ngateway=vpn.example.org:500\ninterface=ipsec0\n"
        "local_ts=10.77.0.2/32\nremote_ts=10.66.0.0/24\nnatt=yes\nnat_detected=yes\n"
        "child=aes256cbc-sha256\nchild_generation=2\nike_generation=1\nlast_notify=0\n"
        "uptime_s=42\ntx_packets=10\ntx_bytes=1200\nrx_packets=9\nrx_bytes=1100\n",
        true, true);
    ICHECK(st.state == VpnState::ChildEstablished);
    ICHECK(st.failure == VpnFailure::None);
    ICHECK(st.gateway == "vpn.example.org:500");
    ICHECK(st.interface_name == "ipsec0");
    ICHECK(st.nat_t && st.nat_detected);
    ICHECK(st.child_generation == 2 && st.ike_generation == 1);
    ICHECK(st.uptime_s == 42 && st.tx_bytes == 1200 && st.rx_packets == 9);

    // Zwischenzustaende.
    ICHECK(parse_status("state=SA_INIT_SENT\n", true, true).state == VpnState::Connecting);
    ICHECK(parse_status("state=AUTH_SENT\n", true, true).state == VpnState::Connecting);
    ICHECK(parse_status("state=IKE_SA_ESTABLISHED\n", true, true).state == VpnState::IkeEstablished);
    ICHECK(parse_status("state=CLOSED\n", true, true).state == VpnState::Disconnected);

    // Fehlerklassifikation: WeirdIKEs Diag-Trennung bleibt getrennt.
    st = parse_status("state=FAILED\nlast_notify=24\n", true, true);
    ICHECK(st.state == VpnState::Failed && st.failure == VpnFailure::AuthenticationFailed);
    st = parse_status("state=FAILED\nlast_notify=14\n", true, true);
    ICHECK(st.failure == VpnFailure::NoProposalChosen);
    st = parse_status("state=FAILED\nlast_notify=38\n", true, true);
    ICHECK(st.failure == VpnFailure::TsUnacceptable);
    st = parse_status("state=FAILED\nlast_notify=0\n", true, true);
    ICHECK(st.failure == VpnFailure::TransportTimeout);
    st = parse_status("state=FAILED\nlast_notify=17\n", true, true);
    ICHECK(st.failure == VpnFailure::Other);

    // Ein UNBEKANNTER Zustand faellt auf Failed, nie auf etwas Gruenes.
    ICHECK(parse_status("state=SOMETHING_NEW\n", true, true).state == VpnState::Failed);

    // Namen sind stabil (API-Vertrag).
    ICHECK(std::string(vpn_state_name(VpnState::ChildEstablished)) == "childEstablished");
    ICHECK(std::string(vpn_failure_name(VpnFailure::AuthenticationFailed)) == "authenticationFailed");
}

void test_review_findings()
{
    // Review-Fund 1: PSK-Regeln. Ein \n waere eine Config-Zeilen-Injection
    // in die Daemon-Datei, Randleerzeichen wuerden vom Daemon-Parser still
    // wegtrimmt, >128 lehnte erst der Daemon-START ab.
    ICHECK(psk_check("").empty());
    ICHECK(psk_check("ganz-normal-42").empty());
    ICHECK(psk_check("mit innen raum").empty());
    ICHECK(!psk_check("boese\ngateway = evil.host").empty());
    ICHECK(!psk_check("rand ").empty());
    ICHECK(!psk_check(" rand").empty());
    ICHECK(!psk_check(std::string(65, 'x')).empty());
    ICHECK(psk_check(std::string(64, 'x')).empty());
    // ... und die Meldung traegt NIE den Wert.
    ICHECK(psk_check("boese\nzeile").find("boese") == std::string::npos);

    // set_config setzt das durch, BEVOR etwas geschrieben wird.
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    IpsecService svc(be, MCONF, DCONF);
    ICHECK(!svc.set_config(sample(), "x\npsk-injection").empty());
    ICHECK(slurp(DCONF).empty());

    // Review-Fund 3: stirbt der Daemon zwischen zwei Aufrufen (ctl liefert
    // running=true, aber leeren Text), wird daraus KEIN failed fantasiert.
    ICHECK(svc.set_config(sample(), "s3cret-psk").empty());
    be.running = true; be.status_text = "";
    ICHECK(svc.status().state == VpnState::Disconnected);

    remove(MCONF); remove(DCONF);
}

void test_ap5_underlay_binding()
{
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    FakeUplinks up;
    IpsecService svc(be, MCONF, DCONF, &up);

    IpsecConfig c = sample();
    c.underlay = Underlay::Cellular;
    ICHECK(svc.set_config(c, "s3cret-psk").empty());

    // cellular unbrauchbar: Connect VERWEIGERT, nichts gestartet, keine Route.
    up.cell.usable = false;
    ICHECK(!svc.connect().empty());
    ICHECK(be.starts == 0 && be.routes.empty());

    // cellular da: Daemon-Datei traegt die KONKRETE Cellular-Bindung und das
    // VORAB aufgeloeste Gateway-Literal; die Peer-Route liegt auf usb0.
    up.cell.usable = true;
    ICHECK(svc.connect().empty());
    std::string d = slurp(DCONF);
    ICHECK(d.find("bind_ip = 100.71.3.9\n") != std::string::npos);
    ICHECK(d.find("bind_dev = usb0\n") != std::string::npos);
    ICHECK(d.find("gateway = 203.0.113.5\n") != std::string::npos);
    ICHECK(d.find("psk = s3cret-psk\n") != std::string::npos);   // write-only weitergetragen
    ICHECK(be.routes.size() == 1 && be.routes[0].ip == "203.0.113.5"
           && be.routes[0].dev == "usb0" && be.routes[0].gw == "100.71.3.1");

    VpnStatus st = svc.status();
    ICHECK(st.requested_underlay == "cellular");
    ICHECK(st.actual_underlay == "cellular");
    ICHECK(st.underlay_interface == "usb0" && st.underlay_ipv4 == "100.71.3.9");
    ICHECK(st.peer_ipv4 == "203.0.113.5");

    // Disconnect raeumt die Peer-Route restlos.
    ICHECK(svc.disconnect().empty());
    ICHECK(be.routes.empty());
    ICHECK(svc.status().actual_underlay.empty());

    // ethernet gewuenscht: Socket an A, nicht B.
    c.underlay = Underlay::Ethernet;
    ICHECK(svc.set_config(c, "").empty());
    ICHECK(svc.connect().empty());
    d = slurp(DCONF);
    ICHECK(d.find("bind_ip = 10.0.0.5\n") != std::string::npos);
    ICHECK(d.find("bind_dev = eth0\n") != std::string::npos);
    ICHECK(be.routes.size() == 1 && be.routes[0].dev == "eth0");
    ICHECK(svc.disconnect().empty() && be.routes.empty());

    // DNS-Fehlschlag: verweigert, BEVOR eine Route existiert.
    be.resolve_ok = false;
    ICHECK(!svc.connect().empty());
    ICHECK(be.routes.empty());
    be.resolve_ok = true;

    remove(MCONF); remove(DCONF);
}

void test_ap5_underlay_loss()
{
    remove(MCONF); remove(DCONF);
    FakeBackend be;
    FakeUplinks up;
    IpsecService svc(be, MCONF, DCONF, &up);

    IpsecConfig c = sample();
    c.underlay = Underlay::Cellular;
    ICHECK(svc.set_config(c, "s3cret-psk").empty());
    ICHECK(svc.connect().empty());
    be.status_text = "state=CHILD_SA_ESTABLISHED\n";

    // Solange das Underlay unveraendert ist, tut tick() nichts.
    svc.tick();
    ICHECK(be.running && be.routes.size() == 1);

    // Cellular faellt weg: Abbau (Daemon stop, Route weg) und Failed/
    // underlayLost -- KEIN stiller Wechsel auf ethernet.
    up.cell.usable = false;
    svc.tick();
    ICHECK(!be.running);
    ICHECK(be.routes.empty());
    be.status_text.clear();
    VpnStatus st = svc.status();
    ICHECK(st.state == VpnState::Failed);
    ICHECK(st.failure == VpnFailure::UnderlayLost);
    ICHECK(std::string(vpn_failure_name(st.failure)) == "underlayLost");

    // Neue Adresse nach Reconnect des Modems = neues Underlay: erst ein
    // NEUER connect nimmt es (kein MOBIKE-Vortaeuschen).
    up.cell.usable = true;
    up.cell.ipv4 = "100.71.9.1";
    ICHECK(svc.connect().empty());
    ICHECK(slurp(DCONF).find("bind_ip = 100.71.9.1\n") != std::string::npos);
    ICHECK(svc.status().state != VpnState::Failed);   // lost ist geloescht
    ICHECK(svc.disconnect().empty());

    remove(MCONF); remove(DCONF);
}

void test_ap6_routes_and_full_tunnel()
{
    // AP6: mehrere Routen aus dem Daemon-Status, source tsr|cp getrennt.
    VpnStatus st = parse_status(
        "state=CHILD_SA_ESTABLISHED\ninterface=ipsec0\n"
        "route=192.168.178.0/24 tsr ipsec0\n"
        "route=10.20.0.0/16 tsr ipsec0\n"
        "route=10.99.0.0/24 cp ipsec0\n",
        true, true);
    ICHECK(st.routes.size() == 3);
    ICHECK(st.routes[0].prefix == "192.168.178.0/24" && st.routes[0].source == "tsr");
    ICHECK(st.routes[0].device == "ipsec0");
    ICHECK(st.routes[2].source == "cp");
    ICHECK(!st.full_tunnel_refused);

    // full_tunnel_refused wird durchgereicht.
    st = parse_status("state=CHILD_SA_ESTABLISHED\nfull_tunnel_refused=yes\n", true, true);
    ICHECK(st.full_tunnel_refused);
    ICHECK(st.routes.empty());

    // AP6: die Config darf mehrere remote-Netze anfordern (komma-getrennt);
    // jedes muss ein gueltiges CIDR sein, hoechstens vier.
    IpsecConfig c = sample();
    c.remote_subnet = "192.168.178.0/24, 10.20.0.0/16";
    ICHECK(validate(c).empty());
    c.remote_subnet = "192.168.178.0/24, garbage";
    ICHECK(validate(c).find("garbage") != std::string::npos);
    c.remote_subnet = "1.0.0.0/8,2.0.0.0/8,3.0.0.0/8,4.0.0.0/8,5.0.0.0/8";
    ICHECK(!validate(c).empty());
    // ... und die Liste faehrt unveraendert in die Daemon-Datei (der Daemon
    // parst die Kommas).
    c.remote_subnet = "192.168.178.0/24,10.20.0.0/16";
    bool present = false;
    const std::string d = to_weirdike_conf(c, "s3cret-psk", "", &present);
    ICHECK(d.find("remote_subnet = 192.168.178.0/24,10.20.0.0/16\n") != std::string::npos);
}

} // namespace

void run_ipsec_tests()
{
    test_review_findings();
    test_ap5_underlay_binding();
    test_ap5_underlay_loss();
    test_ap6_routes_and_full_tunnel();
    test_config_roundtrip();
    test_validate_names_the_problem();
    test_psk_write_only_carry();
    test_service_persists_and_guards();
    test_connect_disconnect();
    test_status_mapping();
}
