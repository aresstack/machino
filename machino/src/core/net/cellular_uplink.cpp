#include "core/net/cellular_uplink.hpp"

namespace machino { namespace net {

using cellular::EcmState;

CellularUplink::CellularUplink(cellular::CellularService& svc, cellular::EcmLink& link,
                               std::string id)
    : svc_(svc), link_(link), id_(std::move(id))
{
    if (id_.empty()) id_ = "cellular";
}

void CellularUplink::tick()
{
    const cellular::CellularConfig& c = svc_.config();

    // Hat sich die ABSICHT geaendert? Dann darf ein frueher aufgegebener
    // Selbstverbindungsversuch noch einmal stattfinden. Ohne diese Pruefung
    // waere "Mobilfunk aus, speichern, wieder an" wirkungslos, bis jemand den
    // Daemon neu startet -- und ein Neustart ist auf dieser Kamera der
    // dokumentierte Hardlock-Ausloeser, also keine Antwort.
    if (c.enabled != last_enabled_ || c.auto_connect != last_auto_connect_) {
        last_enabled_ = c.enabled;
        last_auto_connect_ = c.auto_connect;
        auto_connect_latched_ = false;
    }

    if (!c.enabled) {
        if (link_.state().state != EcmState::Disabled) link_.disconnect();
        // Aus heisst aus. Kein poll(), also kein einziges AT-Kommando an ein
        // Modem, das der Benutzer abgeschaltet hat -- auch kein lesendes.
        return;
    }

    // Die Selbstverbindung EINMAL je Absicht, nicht in jedem Takt. Sonst
    // haette ein ausdrueckliches disconnect() eine Lebensdauer von einer
    // Sekunde, und der Aus-Knopf waere eine Luege.
    if (c.auto_connect && !auto_connect_latched_) {
        auto_connect_latched_ = true;
        link_.connect();
    }

    // Erst fragen, wie es dem Modem geht, dann die Zustandsmaschine mit
    // DIESER Antwort einen Schritt weiterdrehen. Sie wuerde sonst auf einem
    // Status von vorhin entscheiden.
    link_.tick(svc_.poll());
}

void CellularUplink::set_config(const cellular::CellularConfig& c)
{
    svc_.set_config(c);
    link_.set_config(c);
}

bool CellularUplink::enabled() const
{
    return svc_.config().enabled;
}

LinkState CellularUplink::state() const
{
    // Kein Modemzugriff hier -- nur gelesen, was tick() hinterlassen hat.
    // state() wird aus dem HTTP-Thread aufgerufen.
    const cellular::CellularStatus& st = svc_.status();
    const cellular::CellularLinkState& ls = link_.state();

    // Administrativ aus. Nicht Down, nicht Failed: es ist nichts kaputt, es
    // will nur niemand. Ein Failed hier wuerde die Statusseite Alarm schlagen
    // lassen, weil jemand den Haken nicht gesetzt hat.
    if (!enabled()) return LinkState::Absent;

    // Kein AT-Port: keine Hardware, so weit wir sehen koennen.
    if (!st.present) return LinkState::Absent;

    switch (ls.state) {
        case EcmState::Up:
            return LinkState::Connected;

        case EcmState::Failed:
            return LinkState::Failed;

        case EcmState::Disabled:
            // Das Modem ist da, aber niemand hat connect() gerufen. Vorhanden
            // und traegt nicht -- genau das heisst Down.
            return LinkState::Down;

        case EcmState::WaitDevice:
        case EcmState::WaitAt:
            // Der Port ist da (st.present), die Maschine wartet trotzdem noch
            // auf eine Antwort. Das ist das Modem, das enumeriert aber
            // schweigt: Hardware vorhanden, Control Plane nicht bereit.
            return st.responsive ? LinkState::Connecting : LinkState::Down;

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

NetworkInfo CellularUplink::info() const
{
    const cellular::CellularLinkState& ls = link_.state();
    NetworkInfo n;
    n.ifname  = ls.interface_name;
    n.ipv4    = ls.address.ipv4;
    n.netmask = ls.address.netmask;
    n.gateway = ls.address.gateway;
    // Ein Feld, zwei moegliche Server -- dieselbe Form wie bei den anderen
    // Uplinks, damit die Statusseite nicht zwei Sonderfaelle braucht.
    n.dns     = ls.address.dns1;
    if (!ls.address.dns2.empty())
        n.dns += n.dns.empty() ? ls.address.dns2 : (" " + ls.address.dns2);
    // Im NIC-Modus beantwortet das Modem KEIN DHCP; die Adresse kommt aus
    // AT+CGCONTRDP. Das als dhcp=true zu melden waere schlicht falsch und
    // wuerde jede Fehlersuche in die falsche Richtung schicken.
    n.dhcp    = !ls.nic_mode;
    // Diese Server kommen aus CGCONTRDP bzw. aus dem eigenen DHCP-Lease des
    // Modems. Der Uplink WEISS sie, er liest sie nicht aus resolv.conf zurueck
    // -- und nur deshalb darf er die Datei besitzen.
    n.dns_is_own = !ls.address.dns1.empty() || !ls.address.dns2.empty();
    return n;
}

UplinkMetrics CellularUplink::metrics() const
{
    const cellular::CellularStatus& st = svc_.status();
    UplinkMetrics m;
    m.carrier = (link_.state().state == EcmState::Up);

    // RSRP zuerst, CSQ als Rueckfall. RSRP ist die Messung, die bei LTE
    // etwas bedeutet; CSQ ist ein aus GSM-Zeiten geerbter Index, den das
    // Modem selbst aus RSSI errechnet. Wo beides da ist, ist RSRP das
    // genauere -- und wo keines da ist, bleibt 0 stehen, was UplinkMetrics
    // ausdruecklich als "nicht anwendbar" fuehrt.
    if (st.cell.rsrp.has)            m.rssi_dbm = st.cell.rsrp.value;
    else if (st.signal.rssi_dbm.has) m.rssi_dbm = st.signal.rssi_dbm.value;

    // link_mbit bleibt 0. Eine Kategorie (Cat-4 = 150 MBit/s brutto) ist
    // keine Messung der Strecke, und in einer Anzeige neben dem gemessenen
    // Ethernet-Wert saehe sie aus wie eine.
    return m;
}

Result CellularUplink::connect()
{
    if (!enabled()) return Result::unsupported();
    link_.connect();
    return Result::ok();
}

Result CellularUplink::disconnect()
{
    link_.disconnect();
    return Result::ok();
}

bool CellularUplink::has_internet() const
{
    if (state() != LinkState::Connected) return false;
    const cellular::CellularLinkState& ls = link_.state();
    if (reach_ && !ls.interface_name.empty()) return reach_(ls.interface_name);
    // Ohne Sonde: ein Gateway heisst, dass Verkehr hinaus KANN. Dieselbe
    // Annahme, die der Ethernet-Uplink ohne Sonde trifft -- nicht grosszuegiger
    // und nicht strenger, sonst waere der Vergleich beim Failover schief.
    return !ls.address.gateway.empty();
}

std::string CellularUplink::detail() const
{
    if (!enabled()) return "Mobilfunk ist ausgeschaltet";
    const cellular::CellularLinkState& ls = link_.state();
    if (!ls.detail.empty()) return ls.detail;
    return svc_.status().last_error;
}

}} // namespace machino::net
