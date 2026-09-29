#include "core/net/cellular_uplink.hpp"

namespace machino { namespace net {

using cellular::DataLinkState;

CellularUplink::CellularUplink(cellular::CellularService& svc, cellular::ICellularDataLink& link,
                               std::string id)
    : svc_(svc), link_(link), id_(std::move(id))
{
    if (id_.empty()) id_ = "cellular";
}

void CellularUplink::set_config(const cellular::CellularConfig& c)
{
    std::lock_guard<std::mutex> g(m_);
    cfg_ = c;
    cfg_dirty_ = true;
}

void CellularUplink::set_persist(PersistFn fn)
{
    std::lock_guard<std::mutex> g(m_);
    persist_ = std::move(fn);
}

cellular::RadioState CellularUplink::radio_state() const
{
    std::lock_guard<std::mutex> g(m_);
    return radio_;
}

cellular::NeighbourReport CellularUplink::neighbour_cells() const
{
    std::lock_guard<std::mutex> g(m_);
    return nb_;
}

cellular::SimLockReport CellularUplink::sim_lock() const
{
    std::lock_guard<std::mutex> g(m_);
    return sl_;
}

// Was jede Aktion braucht: Mobilfunk an und ein Modem, das antwortet. Beides
// steht in der Abschrift; die Sperre haelt der Aufrufer.
static bool modem_usable(const cellular::CellularConfig& cfg, const cellular::CellularStatus& st,
                         std::string& why_not)
{
    if (!cfg.enabled) { why_not = "cellular is switched off (USB role)"; return false; }
    if (!st.present)  { why_not = "no modem found"; return false; }
    if (!st.responsive) { why_not = "the modem does not answer on the AT port"; return false; }
    return true;
}

bool CellularUplink::request_band_scan(std::string& why_not)
{
    std::lock_guard<std::mutex> g(m_);
    if (!modem_usable(cfg_, st_, why_not)) return false;
    if (scan_requested_ || radio_.scan == cellular::ScanState::Running) {
        why_not = "a scan is already running"; return false;
    }
    if (cfg_.net_mode == cellular::NetMode::GsmOnly) {
        why_not = "the SINR scan needs LTE - the network mode is GSM only"; return false;
    }
    if (cellular::profile_mask(cfg_.band_profile, cfg_.band_mask) == 0) {
        why_not = "no LTE band selected"; return false;
    }
    scan_requested_ = true;
    return true;
}

bool CellularUplink::request_neighbour_cells(std::string& why_not)
{
    std::lock_guard<std::mutex> g(m_);
    if (!modem_usable(cfg_, st_, why_not)) return false;
    if (radio_.scan == cellular::ScanState::Running) {
        why_not = "a band scan is running - the modem is busy"; return false;
    }
    nb_requested_ = true;
    return true;
}

bool CellularUplink::request_sim_lock(const cellular::SimLockRequest& r, std::string& why_not)
{
    std::lock_guard<std::mutex> g(m_);
    if (!modem_usable(cfg_, st_, why_not)) return false;
    if (sim_requested_) { why_not = "a SIM action is already waiting"; return false; }
    if (radio_.scan == cellular::ScanState::Running) {
        why_not = "a band scan is running - the modem is busy"; return false;
    }
    // Die Formpruefung macht ModemActions; hier nur trocken, ohne Modem.
    cellular::ModemActions probe;
    if (!probe.request_sim_lock(r, why_not)) return false;
    sim_req_ = r;
    sim_requested_ = true;
    return true;
}

bool CellularUplink::request_modem_restart(std::string& why_not)
{
    std::lock_guard<std::mutex> g(m_);
    if (!modem_usable(cfg_, st_, why_not)) return false;
    if (radio_.scan == cellular::ScanState::Running) {
        why_not = "a band scan is running - finish it first"; return false;
    }
    restart_requested_ = true;
    return true;
}

cellular::CellularConfig CellularUplink::config() const
{
    std::lock_guard<std::mutex> g(m_);
    return cfg_;
}

cellular::CellularStatus CellularUplink::modem_status() const
{
    std::lock_guard<std::mutex> g(m_);
    return st_;
}

cellular::CellularLinkState CellularUplink::link_state() const
{
    std::lock_guard<std::mutex> g(m_);
    return ls_;
}

bool CellularUplink::enabled() const
{
    std::lock_guard<std::mutex> g(m_);
    return cfg_.enabled;
}

Result CellularUplink::connect()
{
    std::lock_guard<std::mutex> g(m_);
    // Kein Aufbau hinter dem Ruecken des Schalters. Wer Mobilfunk abgeschaltet
    // hat, soll ihn nicht ueber einen zweiten Weg wiederbekommen.
    if (!cfg_.enabled) return Result::unsupported();
    want_up_ = true;
    intent_dirty_ = true;
    return Result::ok();
}

Result CellularUplink::disconnect()
{
    std::lock_guard<std::mutex> g(m_);
    want_up_ = false;
    intent_dirty_ = true;
    return Result::ok();
}

void CellularUplink::tick()
{
    cellular::CellularConfig cfg;
    bool cfg_changed = false, intent_changed = false, want_up = false;
    bool scan_req = false, nb_req = false, restart_req = false, sim_req = false;
    cellular::SimLockRequest sim_request;
    PersistFn persist;
    {
        std::lock_guard<std::mutex> g(m_);
        cfg = cfg_;
        cfg_changed = cfg_dirty_; cfg_dirty_ = false;
        intent_changed = intent_dirty_; intent_dirty_ = false;
        want_up = want_up_;
        scan_req = scan_requested_; scan_requested_ = false;
        nb_req = nb_requested_; nb_requested_ = false;
        restart_req = restart_requested_; restart_requested_ = false;
        sim_req = sim_requested_; sim_requested_ = false;
        if (sim_req) { sim_request = sim_req_; sim_req_ = cellular::SimLockRequest{}; }
        persist = persist_;
    }

    if (cfg_changed) {
        svc_.set_config(cfg);
        link_.set_config(cfg);
        tuner_.set_config(cfg);

        // `enabled` IST der Schalter.
        //
        // Ein zweites Haekchen daneben waere eine Falle: eine erste Fassung
        // liess den Aufbau von `auto_connect` abhaengen, und weil das per
        // Vorgabe aus ist, passierte nach dem Einschalten von Mobilfunk gar
        // nichts -- ohne Fehler, ohne Hinweis, und ohne einen zweiten Knopf,
        // der es haette ausloesen koennen. `auto_connect` beschreibt etwas
        // anderes, naemlich die persistente Selbstverbindung IM MODEM
        // (AT+QNETDEVCTL Typ 3); das gehoert zur Zustandsmaschine und nicht
        // hierher.
        //
        // Eine geaenderte Konfiguration ist ausserdem eine neue Absicht: sie
        // hebt ein vorheriges ausdrueckliches Trennen auf. Vielleicht war ja
        // genau die Aenderung der Grund.
        want_up = cfg.enabled;
        intent_changed = true;
    }

    if (intent_changed) {
        if (want_up) link_.connect();
        else         link_.disconnect();
        std::lock_guard<std::mutex> g(m_);
        want_up_ = want_up;
    }

    if (!cfg.enabled) {
        // Aus heisst aus. Kein poll(), also kein einziges AT-Kommando an ein
        // Modem, das der Benutzer abgeschaltet hat -- auch kein lesendes.
        //
        // Der Tuner und die Aktionen sehen ein "kein Modem": ein Scan bricht
        // ab, Absichten werden beantwortet statt aufgehoben, und beim
        // Wiedereinschalten wird die Bandkonfiguration neu gelesen -- das
        // Modem koennte inzwischen ein anderes sein. Ohne Status geht kein
        // Kommando hinaus; der Transport wird nur durchgereicht.
        tuner_.tick(svc_.transport(), cellular::CellularStatus{});
        actions_.tick(svc_.transport(), cellular::CellularStatus{});
        scan_held_link_ = false;
        std::lock_guard<std::mutex> g(m_);
        ls_ = link_.state();
        // Und keine Modemdaten von vorhin stehenlassen: eine Statusseite, die
        // nach dem Abschalten weiter Betreiber und Signal anzeigt, behauptet
        // eine Messung, die niemand mehr macht.
        st_ = cellular::CellularStatus{};
        radio_ = tuner_.state();
        nb_ = actions_.neighbours();
        sl_ = actions_.sim_lock();
        return;
    }

    // Erst fragen, wie es dem Modem geht, dann die Zustandsmaschine mit DIESER
    // Antwort einen Schritt weiterdrehen. Sie wuerde sonst auf einem Status von
    // vorhin entscheiden.
    //
    // Alles ausserhalb der Sperre: eine AT-Runde dauert bis zu Sekunden, und
    // der HTTP-Thread darf darauf nicht warten.
    const cellular::CellularStatus s = svc_.poll();
    IAtTransport& at = svc_.transport();

    // Die Absichten weiterreichen. Was der Tuner ablehnt, steht danach in
    // seinem Scan-Zustand; die API hat die groben Faelle schon abgefangen.
    if (scan_req)    { std::string why; tuner_.request_scan(why); }
    if (nb_req)      actions_.request_neighbours();
    if (restart_req) actions_.request_restart();
    if (sim_req)     { std::string why; actions_.request_sim_lock(sim_request, why); }

    // ---- Funk: Bandwahl durchsetzen, Scan weiterdrehen ----------------------
    //
    // Der Scan braucht das Modem fuer sich: jedes Band ein RF-Zyklus, dazwischen
    // Messungen. Der Datenlink wird fuer die Dauer heruntergenommen -- nicht
    // weil der AT-Port es verlangte (anders als auf dem ESP32 ist er hier frei),
    // sondern weil ein Datenlink, der zehnmal hintereinander die Registrierung
    // verliert, zehnmal einen Neuaufbau anfinge. Danach kommt er von selbst
    // wieder, falls er gewollt ist.
    const bool radio_busy = tuner_.tick(at, s);
    if (tuner_.scanning() && !scan_held_link_) {
        link_.disconnect();
        scan_held_link_ = true;
    } else if (!tuner_.scanning() && scan_held_link_) {
        scan_held_link_ = false;
        if (want_up) link_.connect();
    }

    // ---- Einmal-Aktionen ----------------------------------------------------
    cellular::ModemActions::Outcome out;
    if (!radio_busy) out = actions_.tick(at, s);

    if (out.pin_changed) {
        // Die Karte hat die Aenderung angenommen; ab jetzt gilt die neue PIN
        // -- fuer die Zustandsmaschinen sofort, fuer die Datei ueber den Haken.
        cfg.sim_pin = out.new_pin;
        svc_.set_config(cfg);
        link_.set_config(cfg);
        {
            std::lock_guard<std::mutex> g(m_);
            cfg_.sim_pin = out.new_pin;
        }
        if (persist) persist(cfg);
    }

    // Kein Antrieb des Datenlinks, solange das Modem mit Funkarbeit beschaeftigt
    // ist oder gerade neu startet: er saehe eine Registrierung verschwinden, die
    // gleich wiederkommt, bzw. ein Modem, das im naechsten Takt ohnehin weg ist.
    const cellular::CellularLinkState l =
        (radio_busy || out.restarted) ? link_.state() : link_.tick(s);

    std::lock_guard<std::mutex> g(m_);
    st_ = s;
    ls_ = l;
    radio_ = tuner_.state();
    nb_ = actions_.neighbours();
    sl_ = actions_.sim_lock();
}

LinkState CellularUplink::state_locked() const
{
    // Administrativ aus. Nicht Down, nicht Failed: es ist nichts kaputt, es
    // will nur niemand. Ein Failed hier wuerde die Statusseite Alarm schlagen
    // lassen, weil jemand den Haken nicht gesetzt hat.
    if (!cfg_.enabled) return LinkState::Absent;

    // Kein AT-Port: keine Hardware, so weit wir sehen koennen.
    if (!st_.present) return LinkState::Absent;

    switch (ls_.state) {
        case DataLinkState::Up:
            return LinkState::Connected;

        case DataLinkState::Failed:
            return LinkState::Failed;

        case DataLinkState::Disabled:
        case DataLinkState::Disconnecting:
            // Das Modem ist da, aber der Aufbau laeuft nicht. Vorhanden und
            // traegt nicht -- genau das heisst Down. Disconnecting gehoert
            // dazu: ein Abbau, der laeuft, traegt keinen Verkehr mehr, und ihn
            // als Connecting zu melden liesse das Failover darauf warten.
            return LinkState::Down;

        case DataLinkState::WaitDevice:
        case DataLinkState::WaitAt:
            // Der Port ist da (st_.present), die Maschine wartet trotzdem noch
            // auf eine Antwort. Das ist das Modem, das enumeriert aber
            // schweigt: Hardware vorhanden, Control Plane nicht bereit.
            return st_.responsive ? LinkState::Connecting : LinkState::Down;

        case DataLinkState::WaitSim:
        case DataLinkState::WaitRegistration:
        case DataLinkState::EnsureEcmMode:
        case DataLinkState::WaitReenumeration:
        case DataLinkState::ConfigurePdp:
        case DataLinkState::StartData:
        case DataLinkState::WaitNetif:
        case DataLinkState::Addressing:
        case DataLinkState::Dial:
        case DataLinkState::Negotiating:
            // Unterwegs. Auch WaitSim: eine fehlende PIN ist kein Defekt des
            // Uplinks, sondern etwas, das der Benutzer nachreichen kann, und
            // Failed waere die falsche Farbe dafuer.
            return LinkState::Connecting;
    }
    return LinkState::Absent;
}

LinkState CellularUplink::state() const
{
    std::lock_guard<std::mutex> g(m_);
    return state_locked();
}

NetworkInfo CellularUplink::info() const
{
    std::lock_guard<std::mutex> g(m_);
    NetworkInfo n;
    n.ifname  = ls_.interface_name;
    n.ipv4    = ls_.address.ipv4;
    n.netmask = ls_.address.netmask;
    n.gateway = ls_.address.gateway;
    // Ein Feld, zwei moegliche Server -- dieselbe Form wie bei den anderen
    // Uplinks, damit die Statusseite nicht zwei Sonderfaelle braucht.
    n.dns     = ls_.address.dns1;
    if (!ls_.address.dns2.empty())
        n.dns += n.dns.empty() ? ls_.address.dns2 : (" " + ls_.address.dns2);
    // Woher die Adresse kommt, und das ist bei jedem der drei Wege anders:
    //
    //   ECM/Routing  DHCP vom Modem (192.168.43.x)
    //   ECM/NIC      statisch aus AT+CGCONTRDP, das Modem beantwortet KEIN DHCP
    //   PPP          aus der IPCP-Aushandlung, also auch kein DHCP
    //
    // Eine erste Fassung fragte nur nach nic_mode -- und weil PppLink das Feld
    // nie setzt, meldete jeder PPP-Anruf dhcp=true. Wer dann sucht, warum
    // "kein Lease" kommt, sucht nach einem DHCP-Server, den es nie gab.
    // ECM wird jetzt IMMER per DHCP adressiert (das Modem serviert es in beiden
    // NAT-Modi, siehe ecm_link.cpp). nic_mode entscheidet nur noch oeffentliche
    // vs. private IP am Host, nicht die Adressierungsmethode.
    n.dhcp    = (ls_.kind == cellular::DataLinkKind::Ecm);
    // Diese Server kommen aus CGCONTRDP bzw. aus dem eigenen DHCP-Lease des
    // Modems. Der Uplink WEISS sie, er liest sie nicht aus resolv.conf zurueck
    // -- und nur deshalb darf er die Datei besitzen.
    n.dns_is_own = !ls_.address.dns1.empty() || !ls_.address.dns2.empty();
    return n;
}

UplinkMetrics CellularUplink::metrics() const
{
    std::lock_guard<std::mutex> g(m_);
    UplinkMetrics m;
    m.carrier = (ls_.state == DataLinkState::Up);

    // RSRP zuerst, CSQ als Rueckfall. RSRP ist die Messung, die bei LTE etwas
    // bedeutet; CSQ ist ein aus GSM-Zeiten geerbter Index, den das Modem selbst
    // aus RSSI errechnet. Wo beides da ist, ist RSRP das genauere -- und wo
    // keines da ist, bleibt 0 stehen, was UplinkMetrics ausdruecklich als
    // "nicht anwendbar" fuehrt.
    if (st_.cell.rsrp.has)            m.rssi_dbm = st_.cell.rsrp.value;
    else if (st_.signal.rssi_dbm.has) m.rssi_dbm = st_.signal.rssi_dbm.value;

    // link_mbit bleibt 0. Eine Kategorie (Cat-4 = 150 MBit/s brutto) ist keine
    // Messung der Strecke, und in einer Anzeige neben dem gemessenen
    // Ethernet-Wert saehe sie aus wie eine.
    return m;
}

bool CellularUplink::has_internet() const
{
    std::string ifname;
    {
        std::lock_guard<std::mutex> g(m_);
        if (state_locked() != LinkState::Connected) return false;
        if (!reach_ || ls_.interface_name.empty()) {
            // Ohne Sonde: ein Gateway heisst, dass Verkehr hinaus KANN.
            // Dieselbe Annahme, die der Ethernet-Uplink ohne Sonde trifft --
            // nicht grosszuegiger und nicht strenger, sonst waere der Vergleich
            // beim Failover schief.
            return !ls_.address.gateway.empty();
        }
        ifname = ls_.interface_name;
    }
    // Die Sonde AUSSERHALB der Sperre: sie ist vom Aufrufer gestellt, und was
    // sie tut, weiss diese Klasse nicht.
    return reach_(ifname);
}

std::string CellularUplink::detail() const
{
    std::lock_guard<std::mutex> g(m_);
    if (!cfg_.enabled) return "Mobilfunk ist ausgeschaltet";
    if (radio_.scan == cellular::ScanState::Running)
        return "band scan running - " + radio_.scan_detail;
    if (!ls_.detail.empty()) return ls_.detail;
    return st_.last_error;
}

}} // namespace machino::net
