// Der ECM-Datenpfad als Zustandsmaschine.
//
// Portiert aus esp32-modem-host/ec200a_ecm.cpp. Die AT-Reihenfolge, die
// Bedingungen und die Sonderfaelle stammen von dort und sind am echten Modem
// belegt ("live verifiziert: Enumeration, Discovery, QNETDEVCTL, DHCP/Routing
// und NIC-Modus"); ersetzt ist nur, was dort USB-Bulk und lwIP war.
//
// DREI Dinge aus der Referenz, die man beim Nachbauen falsch machen wuerde:
//
//   AT+QNETDEVCTL=3,1 -- nicht 1,1,1. Die Linux-Kurzanleitung im selben Repo
//   nennt 1,1,1; der Code, der am Geraet lief, nimmt Typ 3, und der Kommentar
//   sagt warum: Typ 3 ist auto-connect und PERSISTENT, Typ 1 ist "once" und
//   setzt sich bei jedem Modem-Neustart zurueck.
//
//   AT+QCFG="nat" entscheidet, woher die Adresse kommt. Im Routing-Modus (0)
//   NATet das Modem und beantwortet DHCP mit 192.168.43.x. Im NIC-Modus (1)
//   reicht es die oeffentliche Adresse durch und beantwortet GAR KEIN DHCP --
//   dann kommt alles aus AT+CGCONTRDP. Wer hier nur DHCP kennt, wartet im
//   NIC-Modus bis zum Timeout auf eine Antwort, die nie kommt.
//
//   Jede persistente Umstellung passiert HOECHSTENS EINMAL je Lebenszyklus.
//   usbnet und nat sind im Modem gespeichert und brauchen einen Neustart des
//   Modems; ohne diese Sperre entsteht aus "umstellen, rebooten, pruefen,
//   umstellen" eine Endlosschleife, die ein Modem, das ECM schlicht nicht
//   kann, nie verlaesst. Dieselbe Form wie der PIN-Schutz: der VERSUCH zaehlt.
//
// Was hier NICHT passiert: ein heimlicher Wechsel auf PPP. Wer ECM konfiguriert
// hat, bekommt ECM oder einen Fehler mit Begruendung.
#pragma once
#include "core/cellular/cellular_config.hpp"
#include "core/cellular/cellular_status.hpp"
#include "ports/iat_transport.hpp"
#include "ports/iecm_backend.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace machino { namespace cellular {

// Die Zustaende eines Mobilfunk-Datenlinks -- EINE Skala fuer ECM und PPP.
//
// Zwei Aufzaehlungen waeren die naheliegende Loesung und die falsche: der
// Anfang ist bei beiden derselbe (Geraet, AT-Port, SIM, Registrierung, PDP),
// weil beide dieselbe Control Plane benutzen und sich nur im Datenlink
// unterscheiden. Zwei Enums hiessen zwei Namensfunktionen, zwei Uebersetzungen
// in der Oberflaeche und zwei Stellen, an denen "nicht im Netz" verschieden
// heissen kann.
//
// Ein paar Werte gehoeren nur einer Seite, und das steht dran. Sie sind
// trotzdem hier und nicht in einem Unterenum: ein Zustand, den die andere
// Seite nie annimmt, kostet nichts, und die Alternative ist eine Fallunter-
// scheidung in jedem switch.
enum class DataLinkState {
    Disabled,            // niemand will Mobilfunk
    WaitDevice,          // kein AT-Port
    WaitAt,              // Port da, Modem antwortet nicht
    WaitSim,             // SIM nicht bereit
    WaitRegistration,    // nicht im Netz
    EnsureEcmMode,       // ECM: usbnet/nat pruefen, ggf. umstellen
    WaitReenumeration,   // ECM: Modem startet neu, das ist erwartet
    ConfigurePdp,        // CGDCONT (+ QICSGP bei ECM)
    StartData,           // ECM: QNETDEVCTL
    WaitNetif,           // ECM: Interface erscheint
    Addressing,          // ECM: DHCP oder statisch aus CGCONTRDP
    Dial,                // PPP: ATD laeuft, CONNECT steht aus
    Negotiating,         // PPP: pppd verhandelt LCP/IPCP
    Disconnecting,       // PPP: Abbau laeuft
    Up,
    Failed,              // gibt nicht von selbst auf, aber versucht es langsamer
};

const char* data_link_state_name(DataLinkState s);

// Welcher Datenlink. Der Benutzer waehlt "Mobilfunk"; das hier ist die
// technische Auspraegung darunter, und sie steht im Status, weil eine
// Fehlersuche ohne sie im Dunkeln stochert.
enum class DataLinkKind { Ecm, Ppp };
const char* data_link_kind_name(DataLinkKind k);
bool        data_link_kind_parse(const std::string& s, DataLinkKind& out);

struct CellularLinkState {
    DataLinkState state = DataLinkState::Disabled;
    DataLinkKind  kind = DataLinkKind::Ecm;
    std::string   interface_name;
    LinkAddress   address;
    std::string   modem_pdp_address;   // was das Modem sagt; kann abweichen
    bool          nic_mode = false;    // ECM: oeffentliche IP direkt am Host
    std::string   detail;              // Klartext, ohne Geheimnisse
    int           attempts = 0;        // Fehlversuche seit dem letzten Erfolg

    bool is_up() const { return state == DataLinkState::Up; }
};

// Was ein Datenlink koennen muss, damit CellularUplink ihn benutzen kann.
//
// Es gibt genau zwei Implementierungen -- EcmLink und PppLink -- und beim Boot
// wird EINE davon aufgebaut. Kein Umschalten zur Laufzeit: der ECM-Pfad
// braucht cdc_ether, der PPP-Pfad einen freien Modem-Port, und beides
// gleichzeitig vorzubereiten hiesse, Treiber fuer einen Weg zu laden, den
// niemand geht.
class ICellularDataLink {
public:
    virtual ~ICellularDataLink() = default;

    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual void set_config(const CellularConfig& c) = 0;

    // Ein Schritt. `status` liefert SIM und Registrierung aus CellularService;
    // der Datenlink fragt sie NICHT selbst ab.
    virtual const CellularLinkState& tick(const CellularStatus& status) = 0;
    virtual const CellularLinkState& state() const = 0;
};

class EcmLink : public ICellularDataLink {
public:
    using ClockFn = std::function<uint64_t()>;

    EcmLink(IAtTransport& at, IEcmBackend& backend) : at_(at), be_(backend) {}

    void set_clock(ClockFn now) { now_ = std::move(now); }
    void set_config(const CellularConfig& c) override { cfg_ = c; }

    // Die Absicht. connect() startet nichts sofort -- es sagt nur, wohin.
    // Gearbeitet wird in tick().
    void connect() override;
    void disconnect() override;

    // Ein Schritt. Der Aufrufer ruft das in seinem Takt; die Maschine
    // entscheidet selbst, ob sie etwas tut: nach einem Fehlschlag wartet sie
    // laenger, statt im Takt des Aufrufers Kommandos zu schicken.
    //
    // `status` liefert SIM und Registrierung -- die kommen aus CellularService
    // und werden hier nicht noch einmal abgefragt.
    const CellularLinkState& tick(const CellularStatus& status) override;

    const CellularLinkState& state() const override { return st_; }
    bool is_up() const { return st_.is_up(); }
    const std::string& interface_name() const { return st_.interface_name; }
    const LinkAddress& address() const { return st_.address; }

    // Fuer den Test sichtbar: wie lange nach dem n-ten Fehlversuch gewartet
    // wird. Begrenzt, damit ein dauerhaft fehlendes Modem nicht alle paar
    // Sekunden eine AT-Runde ausloest.
    static uint32_t backoff_ms(int attempts);

private:
    uint64_t now() const { return now_ ? now_() : 0; }
    void enter(DataLinkState s, const std::string& detail);
    void fail(const std::string& detail);
    bool due() const;

    IAtTransport&    at_;
    IEcmBackend&     be_;
    CellularConfig   cfg_;
    CellularLinkState st_;
    ClockFn          now_;

    bool     want_up_ = false;
    uint64_t next_due_ms_ = 0;
    uint64_t reenum_deadline_ms_ = 0;
    uint64_t netif_deadline_ms_ = 0;
    uint64_t dhcp_deadline_ms_ = 0;

    // Je Lebenszyklus hoechstens einmal -- siehe Kopfkommentar.
    bool usbnet_switch_tried_ = false;
    bool nat_switch_tried_ = false;

    bool dhcp_running_ = false;
    std::string dhcp_iface_;
};

}} // namespace machino::cellular
