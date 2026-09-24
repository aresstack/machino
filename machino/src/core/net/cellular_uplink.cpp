#include "core/net/cellular_uplink.hpp"

namespace machino { namespace net {

using cellular::EcmState;

CellularUplink::CellularUplink(cellular::CellularService& svc, cellular::EcmLink& link,
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
    {
        std::lock_guard<std::mutex> g(m_);
        cfg = cfg_;
        cfg_changed = cfg_dirty_; cfg_dirty_ = false;
        intent_changed = intent_dirty_; intent_dirty_ = false;
        want_up = want_up_;
    }

    if (cfg_changed) {
        svc_.set_config(cfg);
        link_.set_config(cfg);

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
        std::lock_guard<std::mutex> g(m_);
        ls_ = link_.state();
        // Und keine Modemdaten von vorhin stehenlassen: eine Statusseite, die
        // nach dem Abschalten weiter Betreiber und Signal anzeigt, behauptet
        // eine Messung, die niemand mehr macht.
        st_ = cellular::CellularStatus{};
        return;
    }

    // Erst fragen, wie es dem Modem geht, dann die Zustandsmaschine mit DIESER
    // Antwort einen Schritt weiterdrehen. Sie wuerde sonst auf einem Status von
    // vorhin entscheiden.
    //
    // Beides ausserhalb der Sperre: eine AT-Runde dauert bis zu Sekunden, und
    // der HTTP-Thread darf darauf nicht warten.
    const cellular::CellularStatus    s = svc_.poll();
    const cellular::CellularLinkState l = link_.tick(s);

    std::lock_guard<std::mutex> g(m_);
    st_ = s;
    ls_ = l;
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
        case EcmState::Up:
            return LinkState::Connected;

        case EcmState::Failed:
            return LinkState::Failed;

        case EcmState::Disabled:
            // Das Modem ist da, aber der Aufbau laeuft nicht. Vorhanden und
            // traegt nicht -- genau das heisst Down.
            return LinkState::Down;

        case EcmState::WaitDevice:
        case EcmState::WaitAt:
            // Der Port ist da (st_.present), die Maschine wartet trotzdem noch
            // auf eine Antwort. Das ist das Modem, das enumeriert aber
            // schweigt: Hardware vorhanden, Control Plane nicht bereit.
            return st_.responsive ? LinkState::Connecting : LinkState::Down;

        case EcmState::WaitSim:
        case EcmState::WaitRegistration:
        case EcmState::EnsureEcmMode:
        case EcmState::WaitReenumeration:
        case EcmState::ConfigurePdp:
        case EcmState::StartData:
        case EcmState::WaitNetif:
        case EcmState::Addressing:
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
    // Im NIC-Modus beantwortet das Modem KEIN DHCP; die Adresse kommt aus
    // AT+CGCONTRDP. Das als dhcp=true zu melden waere schlicht falsch und
    // wuerde jede Fehlersuche in die falsche Richtung schicken.
    n.dhcp    = !ls_.nic_mode;
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
    m.carrier = (ls_.state == EcmState::Up);

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
    if (!ls_.detail.empty()) return ls_.detail;
    return st_.last_error;
}

}} // namespace machino::net
