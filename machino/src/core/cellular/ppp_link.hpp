// Der PPP-Datenpfad als Zustandsmaschine.
//
// Portiert aus esp32-modem-host/ec200a_modem.cpp (PppMachine::doConnect). Die
// AT-Reihenfolge, die Bedingungen und die Sonderfaelle stammen von dort;
// ersetzt ist, was dort lwIP-PPPoS ueber USB-Bulk war -- unter Linux macht das
// pppd.
//
// VIER Dinge aus der Referenz, die man beim Nachbauen weglassen wuerde:
//
//   NICHT WAEHLEN, SOLANGE DIE SIM NICHT BEREIT IST. Der Dial liefe ins Leere,
//   und die Fehlermeldung waere "kein CONNECT" statt "PIN fehlt". Dieselbe
//   Regel wie bei ECM, und derselbe SimManager -- die PIN wird je Lebenszyklus
//   hoechstens einmal gesendet, und daran aendert ein anderer Datenlink nichts.
//
//   AT+CGACT=0,1 VOR dem Waehlen. Das raeumt einen PDP-Kontext ab, der von
//   einer frueheren Sitzung noch steht. Die Referenz nennt den Fall beim Namen:
//   ein Modem, das nach einem Neustart des Hosts noch im Datenmodus haengt --
//   "vorher half nur Neu-Anstecken".
//
//   DER MODEM-PORT MUSS IM KOMMANDOMODUS SEIN. Antwortet er nicht auf "AT",
//   steckt dort noch eine alte PPP-Sitzung. Die Referenz loest das mit "+++"
//   (eine Sekunde Ruhe davor und danach, sonst gehen die drei Zeichen als
//   Nutzdaten durch) und "ATH". Unter Linux macht das chat(8) im Chat-Skript,
//   das LinuxPppBackend schreibt -- diese Klasse spricht ausschliesslich ueber
//   den AT-PORT und fasst den Modem-Port nie an. Zwei Sprecher auf demselben
//   seriellen Port waeren der sicherste Weg, eine funktionierende Einwahl zu
//   zerlegen.
//
//   EINE HAENGENDE AUSHANDLUNG IST KEIN LANGSAMER ERFOLG. LCP/IPCP sind
//   normalerweise in unter fuenf Sekunden durch. Bleibt es stehen, kommt gar
//   kein Ereignis mehr, und laenger zu warten heilt das nicht. Die Referenz
//   bricht nach zwoelf Sekunden ab und waehlt neu; genau das tut diese Maschine
//   auch, und sie sagt im Detailtext, dass SIE abgebrochen hat und nicht das
//   Netz.
//
// WAS AUSDRUECKLICH NICHT PORTIERT WIRD: die lwIP-TCP-Fenster-Korrekturen, die
// USB-Transfer-Pools, txmaxinflight und die esp_netif-Kniffe. Das waren
// Plattformoptimierungen fuer einen Stack, den es hier nicht gibt.
//
// UND: kein heimlicher Wechsel. Wer PPP konfiguriert hat, bekommt PPP oder
// einen Fehler mit Begruendung -- so wie EcmLink nie nach PPP ausweicht.
#pragma once
#include "core/cellular/ecm_link.hpp"     // DataLinkState, ICellularDataLink
#include "ports/iat_transport.hpp"
#include "ports/ippp_backend.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace machino { namespace cellular {

class PppLink : public ICellularDataLink {
public:
    using ClockFn = std::function<uint64_t()>;

    // Transport und Backend werden geliehen und ueberleben diesen Link.
    PppLink(IAtTransport& at, IPppBackend& backend) : at_(at), be_(backend)
    {
        st_.kind = DataLinkKind::Ppp;
    }

    void set_clock(ClockFn now) { now_ = std::move(now); }
    void set_config(const CellularConfig& c) override { cfg_ = c; }

    // Welcher Port gewaehlt wird. Ermittelt wird er draussen (modem_ports aus
    // AP-M2, MI_04 = Modem); hier steht nur, worauf gewaehlt werden soll.
    //
    // Leer heisst "noch nicht gefunden" und ist ein Wartezustand, kein Fehler:
    // nach einer Re-Enumeration heisst der Port anders, und ein hartkodiertes
    // /dev/ttyUSB4 waere genau so lange richtig, bis es das nicht mehr ist.
    void set_modem_port(std::string tty) { modem_tty_ = std::move(tty); }
    const std::string& modem_port() const { return modem_tty_; }

    void connect() override;
    void disconnect() override;

    const CellularLinkState& tick(const CellularStatus& status) override;
    const CellularLinkState& state() const override { return st_; }

    bool is_up() const { return st_.is_up(); }

    // Dieselbe Kurve wie beim ECM-Pfad. Eine eigene waere ein zweiter Ort, an
    // dem jemand "warum versucht es das so oft" nachsehen muesste.
    static uint32_t backoff_ms(int attempts) { return EcmLink::backoff_ms(attempts); }

private:
    uint64_t now() const { return now_ ? now_() : 0; }
    void enter(DataLinkState s, const std::string& detail);
    void fail(const std::string& detail);
    bool due() const;

    IAtTransport& at_;
    IPppBackend&  be_;
    CellularConfig    cfg_;
    CellularLinkState st_;
    ClockFn           now_;
    std::string       modem_tty_;

    bool     want_up_ = false;
    uint64_t next_due_ms_ = 0;
    uint64_t negotiate_deadline_ms_ = 0;
    bool     started_ = false;       // pppd wurde angefordert
};

}} // namespace machino::cellular
