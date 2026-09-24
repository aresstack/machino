#include "core/cellular/ecm_link.hpp"

#include "core/cellular/at_parse.hpp"

namespace machino { namespace cellular {

namespace {

// Wie lange das Modem nach AT+CFUN=1,1 zum Wiederkommen braucht. Im
// Referenzprojekt: "~15-20s, bis wieder erkannt"; die USB-Host-Recovery dort
// wartet bis zu 90 s. Hier grosszuegig, weil ein zu kurzes Fenster die
// Re-Enumeration als Fehlschlag zaehlt und einen zweiten Neustart ausloesen
// wuerde -- genau die Schleife, die vermieden werden soll.
const uint32_t kReenumWaitMs = 90000;

// Das Interface erscheint nicht sofort, nachdem der Datenkanal steht.
const uint32_t kNetifWaitMs = 20000;

// DHCP im Routing-Modus. Das Modem antwortet normalerweise sofort; laenger als
// das heisst, dass etwas anderes nicht stimmt.
const uint32_t kDhcpWaitMs = 30000;

const int kQnetdevType = 3;   // auto-connect + persistent, siehe Kopfkommentar

} // namespace

const char* ecm_state_name(EcmState s)
{
    switch (s) {
        case EcmState::Disabled:          return "disabled";
        case EcmState::WaitDevice:        return "wait_device";
        case EcmState::WaitAt:            return "wait_at";
        case EcmState::WaitSim:           return "wait_sim";
        case EcmState::WaitRegistration:  return "wait_registration";
        case EcmState::EnsureEcmMode:     return "ensure_ecm_mode";
        case EcmState::WaitReenumeration: return "wait_reenumeration";
        case EcmState::ConfigurePdp:      return "configure_pdp";
        case EcmState::StartData:         return "start_data";
        case EcmState::WaitNetif:         return "wait_netif";
        case EcmState::Addressing:        return "addressing";
        case EcmState::Up:                return "up";
        case EcmState::Failed:            return "failed";
    }
    return "?";
}

uint32_t EcmLink::backoff_ms(int attempts)
{
    // 2, 4, 8, 16, 30, 30, ... Sekunden. Kein Retry-Sturm, aber auch kein
    // Aufgeben: ein Netz kommt wieder, und dann soll die Kamera es merken,
    // ohne dass jemand etwas anfassen muss.
    if (attempts <= 0) return 0;
    uint32_t s = 2;
    for (int i = 1; i < attempts && s < 30; ++i) s *= 2;
    return (s > 30 ? 30u : s) * 1000u;
}

void EcmLink::enter(EcmState s, const std::string& detail)
{
    st_.state = s;
    st_.detail = detail;
}

void EcmLink::fail(const std::string& detail)
{
    ++st_.attempts;
    next_due_ms_ = now() + backoff_ms(st_.attempts);
    enter(EcmState::Failed, detail);
}

bool EcmLink::due() const
{
    return next_due_ms_ == 0 || now() >= next_due_ms_;
}

void EcmLink::connect()
{
    if (want_up_) return;              // wiederholter Aufruf ist ein No-op
    want_up_ = true;
    st_.attempts = 0;
    next_due_ms_ = 0;
    // Ein neuer Verbindungswunsch darf die Einmalsperren loesen: der Benutzer
    // hat etwas getan, und vielleicht war es genau die Aenderung, die fehlte.
    usbnet_switch_tried_ = false;
    nat_switch_tried_ = false;
    enter(EcmState::WaitDevice, "Verbindung angefordert");
}

void EcmLink::disconnect()
{
    want_up_ = false;
    if (dhcp_running_) {
        be_.dhcp_stop(dhcp_iface_);
        dhcp_running_ = false;
    }
    if (!st_.interface_name.empty()) {
        // NUR teardown, kein zusaetzliches set_up(false).
        //
        // Das Protokoll zum Helfer ist EINE Zeile, und der Helfer liest sie im
        // Sekundentakt. Zwei Wuensche direkt hintereinander heissen, dass er
        // den ersten nie sieht -- hier haette das "down" das "stop"
        // ueberschrieben und der DHCP-Client waere weitergelaufen. teardown
        // nimmt das Interface selbst herunter.
        be_.teardown(st_.interface_name);
        // Den Namen VERGESSEN, sonst raeumt ein zweites disconnect() dasselbe
        // Interface noch einmal ab. "Zweimal trennen ist kein Fehler" heisst
        // nicht nur, dass es nicht abstuerzt, sondern auch, dass beim zweiten
        // Mal nichts mehr passiert.
        st_.interface_name.clear();
    }
    st_.address = EcmAddress{};
    st_.modem_pdp_address.clear();
    st_.attempts = 0;
    next_due_ms_ = 0;
    enter(EcmState::Disabled, "getrennt");
}

const CellularLinkState& EcmLink::tick(const CellularStatus& status)
{
    if (!want_up_) {
        if (st_.state != EcmState::Disabled) disconnect();
        return st_;
    }

    // Das Modem ist weg. Alles, was daran hing, ist damit auch weg -- das ist
    // kein Fehler, sondern eine Tatsache, und waehrend einer erwarteten
    // Re-Enumeration sogar der Normalfall.
    if (!status.present || !status.responsive) {
        if (dhcp_running_) { be_.dhcp_stop(dhcp_iface_); dhcp_running_ = false; }
        st_.address = EcmAddress{};
        if (st_.state == EcmState::WaitReenumeration) {
            if (now() > reenum_deadline_ms_)
                fail("Modem kam nach dem Moduswechsel nicht zurueck");
            return st_;
        }
        enter(status.present ? EcmState::WaitAt : EcmState::WaitDevice,
              status.present ? "Modem antwortet nicht" : "kein Modem");
        return st_;
    }

    // Es ist wieder da. Eine laufende Re-Enumeration ist damit beendet.
    if (st_.state == EcmState::WaitReenumeration)
        enter(EcmState::EnsureEcmMode, "Modem ist zurueck");

    if (st_.state == EcmState::Failed && !due()) return st_;

    if (status.sim != SimState::Ready) {
        enter(EcmState::WaitSim, status.sim_detail.empty()
              ? std::string("SIM nicht bereit") : status.sim_detail);
        return st_;
    }
    if (!reg_is_registered(status.registration)) {
        // Kein Datenkanal ohne Registrierung. Es hier trotzdem zu versuchen
        // kostet nur AT-Runden und liefert einen Fehler, der nichts erklaert.
        enter(EcmState::WaitRegistration,
              std::string("nicht im Netz: ") + reg_state_name(status.registration));
        return st_;
    }

    // ---- ECM-Modus sicherstellen -----------------------------------------
    if (st_.state != EcmState::ConfigurePdp && st_.state != EcmState::StartData &&
        st_.state != EcmState::WaitNetif && st_.state != EcmState::Addressing &&
        st_.state != EcmState::Up) {
        enter(EcmState::EnsureEcmMode, "pruefe Betriebsart");

        const MaybeInt usbnet = parse_qcfg_int(at_.command("AT+QCFG=\"usbnet\"").raw, "usbnet");
        if (!usbnet.has) {
            fail("AT+QCFG=\"usbnet\" nicht lesbar - Modus unbekannt, nichts umgestellt");
            return st_;
        }
        if (usbnet.value != 1) {
            if (usbnet_switch_tried_) {
                // Einmal umgestellt und immer noch nicht ECM: dieses Modem
                // kann es offenbar nicht. Weiter zu rebooten macht daraus eine
                // Schleife.
                fail("Modem bleibt nach der Umstellung auf usbnet=" +
                     std::to_string(usbnet.value) + " - ECM wird hier nicht unterstuetzt");
                return st_;
            }
            usbnet_switch_tried_ = true;
            at_.command("AT+QCFG=\"usbnet\",1");
            at_.command("AT+CFUN=1,1", 10000);
            reenum_deadline_ms_ = now() + kReenumWaitMs;
            enter(EcmState::WaitReenumeration, "auf ECM umgestellt, Modem startet neu");
            return st_;
        }

        // NAT-Betriebsart. Sie entscheidet, woher gleich die Adresse kommt.
        const MaybeInt nat = parse_qcfg_int(at_.command("AT+QCFG=\"nat\"").raw, "nat");
        const int want_nat = cfg_.nic_mode ? 1 : 0;
        if (nat.has && nat.value != want_nat) {
            if (!nat_switch_tried_) {
                nat_switch_tried_ = true;
                at_.command("AT+QCFG=\"nat\"," + std::to_string(want_nat));
                at_.command("AT+CFUN=1,1", 10000);
                reenum_deadline_ms_ = now() + kReenumWaitMs;
                enter(EcmState::WaitReenumeration, "Betriebsart umgestellt, Modem startet neu");
                return st_;
            }
            // Nicht umstellbar: weitermachen mit dem, was das Modem tut, und
            // es sagen. Das ist besser als gar keine Verbindung.
            st_.detail = "Betriebsart liess sich nicht umstellen";
        }
        st_.nic_mode = nat.has ? (nat.value == 1) : cfg_.nic_mode;
        enter(EcmState::ConfigurePdp, "Betriebsart in Ordnung");
    }

    // ---- PDP-Kontext ------------------------------------------------------
    if (st_.state == EcmState::ConfigurePdp) {
        if (cfg_.apn.empty()) {
            fail("kein APN konfiguriert");
            return st_;
        }
        const char* pdp = pdp_type_name(cfg_.pdp);
        const AtExchange cg = at_.command(
            std::string("AT+CGDCONT=1,\"") + pdp + "\",\"" + cfg_.apn + "\"");
        if (!cg.ok()) {
            fail("AT+CGDCONT abgelehnt - APN oder PDP-Typ passt nicht");
            return st_;
        }
        // QICSGP traegt die Zugangsdaten. Der Datenkanal nimmt seine Parameter
        // von dort, nicht aus CGDCONT -- deshalb beides.
        const int ctx_type = (cfg_.pdp == PdpType::Ipv4v6) ? 3 : 1;
        const int auth = (cfg_.auth == AuthMode::Chap) ? 2 : (cfg_.auth == AuthMode::Pap) ? 1 : 0;
        at_.command("AT+QICSGP=1," + std::to_string(ctx_type) + ",\"" + cfg_.apn + "\",\"" +
                    cfg_.username + "\",\"" + cfg_.password + "\"," + std::to_string(auth));
        enter(EcmState::StartData, "PDP-Kontext gesetzt");
    }

    // ---- Datenkanal -------------------------------------------------------
    if (st_.state == EcmState::StartData) {
        const AtExchange qn = at_.command(
            "AT+QNETDEVCTL=" + std::to_string(kQnetdevType) + ",1", 10000);
        if (!qn.ok()) {
            fail("AT+QNETDEVCTL abgelehnt - Datenkanal kam nicht hoch");
            return st_;
        }
        netif_deadline_ms_ = now() + kNetifWaitMs;
        enter(EcmState::WaitNetif, "Datenkanal an, warte auf das Interface");
    }

    // ---- Netzwerkinterface ------------------------------------------------
    if (st_.state == EcmState::WaitNetif) {
        EcmInterface iface;
        if (!be_.find_interface(iface)) {
            if (now() > netif_deadline_ms_) {
                fail("kein ECM-Netzwerkinterface erschienen - cdc_ether geladen?");
                return st_;
            }
            return st_;
        }
        st_.interface_name = iface.name;
        be_.set_up(iface.name, true);
        dhcp_deadline_ms_ = now() + kDhcpWaitMs;
        enter(EcmState::Addressing, "Interface " + iface.name + " da");
    }

    // ---- Adresse ----------------------------------------------------------
    if (st_.state == EcmState::Addressing) {
        // Verschwindet das Interface WAEHREND der Adressvergabe, hat das
        // Warten keinen Sinn mehr: DHCP laeuft dann auf etwas, das es nicht
        // mehr gibt, und CGCONTRDP beschreibt einen Kontext ohne Traeger. Ohne
        // diese Pruefung liefe die volle DHCP-Frist ins Leere.
        EcmInterface still_there;
        if (!be_.find_interface(still_there) || still_there.name != st_.interface_name) {
            if (dhcp_running_) { be_.dhcp_stop(dhcp_iface_); dhcp_running_ = false; }
            st_.interface_name.clear();
            fail("Interface verschwunden");
            return st_;
        }

        // Die Adresse des Modems merken wir uns immer: im NIC-Modus IST sie die
        // Adresse, im Routing-Modus ist sie die oeffentliche hinter dem NAT und
        // damit das, was jemand von aussen ansprechen wuerde.
        const PdpContextParams rdp = parse_cgcontrdp(at_.command("AT+CGCONTRDP=1").raw);
        st_.modem_pdp_address = rdp.ipv4;

        if (st_.nic_mode) {
            // KEIN DHCP. Das Modem beantwortet es im NIC-Modus nicht -- hier zu
            // warten hiesse, auf eine Antwort zu warten, die nie kommt.
            if (rdp.ipv4.empty()) {
                if (now() > dhcp_deadline_ms_) {
                    fail("Modem meldet keine Adresse (CGCONTRDP leer) im NIC-Modus");
                    return st_;
                }
                return st_;
            }
            EcmAddress a;
            a.ipv4 = rdp.ipv4;
            a.netmask = rdp.netmask;
            a.gateway = rdp.gateway;
            a.dns1 = rdp.dns1;
            a.dns2 = rdp.dns2;
            if (!be_.set_address(st_.interface_name, a)) {
                fail("Adresse liess sich nicht setzen");
                return st_;
            }
            st_.address = a;
            st_.attempts = 0;
            enter(EcmState::Up, "verbunden (NIC-Modus, Adresse vom Modem)");
            return st_;
        }

        if (!dhcp_running_) {
            if (!be_.dhcp_start(st_.interface_name)) {
                fail("DHCP liess sich nicht starten");
                return st_;
            }
            dhcp_running_ = true;
            dhcp_iface_ = st_.interface_name;
        }
        EcmAddress a;
        if (be_.read_address(st_.interface_name, a) && a.has_address()) {
            st_.address = a;
            st_.attempts = 0;
            enter(EcmState::Up, "verbunden (DHCP)");
            return st_;
        }
        if (now() > dhcp_deadline_ms_) {
            be_.dhcp_stop(st_.interface_name);
            dhcp_running_ = false;
            fail("DHCP lieferte keine Adresse");
        }
        return st_;
    }

    // ---- oben halten ------------------------------------------------------
    if (st_.state == EcmState::Up) {
        EcmInterface iface;
        if (!be_.find_interface(iface) || iface.name != st_.interface_name) {
            if (dhcp_running_) { be_.dhcp_stop(dhcp_iface_); dhcp_running_ = false; }
            st_.address = EcmAddress{};
            st_.interface_name.clear();
            fail("Interface verschwunden");
            return st_;
        }
        EcmAddress a;
        if (!be_.read_address(st_.interface_name, a) || !a.has_address()) {
            // Adresse weg, Interface noch da: Lease abgelaufen oder Link
            // gefallen. Zurueck in die Adressvergabe, nicht gleich alles
            // abreissen -- udhcpc erneuert von selbst.
            dhcp_deadline_ms_ = now() + kDhcpWaitMs;
            enter(EcmState::Addressing, "Adresse verloren, hole neue");
            return st_;
        }
        st_.address = a;
    }

    // Kein "aus Failed heraus neu anfangen" mehr an dieser Stelle: der Block
    // war unerreichbar. Wer Failed und faellig ist, kommt oben gar nicht bis
    // hierher -- die Pruefung am Anfang laesst ihn durch, und der grosse
    // if-Block faengt dann direkt wieder bei EnsureEcmMode an.
    return st_;
}

}} // namespace machino::cellular
