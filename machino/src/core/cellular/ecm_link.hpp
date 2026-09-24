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

enum class EcmState {
    Disabled,            // niemand will Mobilfunk
    WaitDevice,          // kein AT-Port
    WaitAt,              // Port da, Modem antwortet nicht
    WaitSim,             // SIM nicht bereit
    WaitRegistration,    // nicht im Netz
    EnsureEcmMode,       // usbnet/nat pruefen, ggf. umstellen
    WaitReenumeration,   // Modem startet neu, das ist erwartet
    ConfigurePdp,        // CGDCONT + QICSGP
    StartData,           // QNETDEVCTL
    WaitNetif,           // Interface erscheint
    Addressing,          // DHCP oder statisch aus CGCONTRDP
    Up,
    Failed,              // gibt nicht von selbst auf, aber versucht es langsamer
};

const char* ecm_state_name(EcmState s);

struct CellularLinkState {
    EcmState     state = EcmState::Disabled;
    std::string  interface_name;
    EcmAddress   address;
    std::string  modem_pdp_address;   // was das Modem sagt; kann abweichen
    bool         nic_mode = false;    // true = oeffentliche IP direkt am Host
    std::string  detail;              // Klartext, ohne Geheimnisse
    int          attempts = 0;        // Fehlversuche seit dem letzten Erfolg

    bool is_up() const { return state == EcmState::Up; }
};

class EcmLink {
public:
    using ClockFn = std::function<uint64_t()>;

    EcmLink(IAtTransport& at, IEcmBackend& backend) : at_(at), be_(backend) {}

    void set_clock(ClockFn now) { now_ = std::move(now); }
    void set_config(const CellularConfig& c) { cfg_ = c; }

    // Die Absicht. connect() startet nichts sofort -- es sagt nur, wohin.
    // Gearbeitet wird in tick().
    void connect();
    void disconnect();

    // Ein Schritt. Der Aufrufer ruft das in seinem Takt; die Maschine
    // entscheidet selbst, ob sie etwas tut: nach einem Fehlschlag wartet sie
    // laenger, statt im Takt des Aufrufers Kommandos zu schicken.
    //
    // `status` liefert SIM und Registrierung -- die kommen aus CellularService
    // und werden hier nicht noch einmal abgefragt.
    const CellularLinkState& tick(const CellularStatus& status);

    const CellularLinkState& state() const { return st_; }
    bool is_up() const { return st_.is_up(); }
    const std::string& interface_name() const { return st_.interface_name; }
    const EcmAddress& address() const { return st_.address; }

    // Fuer den Test sichtbar: wie lange nach dem n-ten Fehlversuch gewartet
    // wird. Begrenzt, damit ein dauerhaft fehlendes Modem nicht alle paar
    // Sekunden eine AT-Runde ausloest.
    static uint32_t backoff_ms(int attempts);

private:
    uint64_t now() const { return now_ ? now_() : 0; }
    void enter(EcmState s, const std::string& detail);
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
