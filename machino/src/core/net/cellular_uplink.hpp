// Mobilfunk als Uplink.
//
// Diese Klasse ist eine UEBERSETZUNG und sonst nichts. Sie nimmt zwei Dinge,
// die es schon gibt -- den Statusdienst des Modems (AP-M3) und die
// ECM-Zustandsmaschine (AP-M4) -- und beantwortet damit die Fragen, die
// INetworkUplink stellt. Kein AT-Kommando, kein Interface-Name, kein usbnet,
// kein QNETDEVCTL. Wer hier nach einem AT-String sucht, soll ihn nicht finden.
//
// WARUM DIE ID "cellular" IST UND NICHT "usb0"
//
// Der Benutzer waehlt Zugangsarten, keine Kernel-Interfaces. Der Kernel nennt
// das Modem-Interface je nach Treiber und Enumerationsreihenfolge usb0, eth1
// oder wwan0, und beim naechsten Boot kann es ein anderer Name sein. Eine
// Preference-Liste, die "usb0" enthaelt, waere nach einem Treiberwechsel still
// wirkungslos -- der Eintrag passt auf nichts mehr, und niemand bekaeme es
// gesagt. "cellular" ueberlebt das, und es ueberlebt auch den Wechsel des
// Datenlinks von ECM auf PPP, der spaeter kommt.
//
// Die ID ist trotzdem ein Konstruktorparameter und keine Konstante: ein Board
// mit zwei Modems braucht zwei unterscheidbare, und die Policy kennt beides --
// IDs und Typnamen.
//
// DIE SECHS SCHICHTEN
//
// "Mobilfunk verbunden" ist keine einzelne Tatsache. Zwischen "ein EC200A
// haengt am USB" und "Pakete kommen an" liegen sechs Stufen, und jede kann
// allein scheitern:
//
//     hardware present     es gibt einen AT-Port
//     control plane ready  darauf antwortet jemand
//     registered           das Modem ist im Netz
//     datalink up          der ECM-Kanal steht
//     address assigned     eine IPv4 ist konfiguriert
//     internet reachable   es kommt auch etwas zurueck
//
// LinkState hat fuenf Werte, nicht sechs, und das ist Absicht: es ist dieselbe
// Skala wie bei Ethernet und WLAN, damit der ConnectivityManager Mobilfunk
// nicht gesondert behandeln muss. Die Aufloesung dazwischen steht im Detail-
// Text, nicht im Zustand. Was NICHT passiert: Connected zu melden, weil ein
// usb0 existiert. Connected heisst hier ausschliesslich EcmState::Up, und das
// heisst: Adresse konfiguriert.
#pragma once
#include "core/cellular/cellular_service.hpp"
#include "core/cellular/ecm_link.hpp"
#include "ports/inetwork.hpp"
#include <functional>
#include <string>

namespace machino { namespace net {

class CellularUplink : public INetworkUplink {
public:
    // Wie bei den anderen Uplinks: eine bereits gemessene Erreichbarkeit, kein
    // frischer Test. Der ConnectivityManager fragt das im Sekundentakt und darf
    // dabei nicht blockieren.
    using ReachabilityFn = std::function<bool(const std::string& ifname)>;

    // Dienst und Link werden geliehen und ueberleben diesen Uplink.
    CellularUplink(cellular::CellularService& svc, cellular::EcmLink& link,
                   std::string id = "cellular");

    void set_reachability(ReachabilityFn fn) { reach_ = std::move(fn); }

    // Eine Runde Modemarbeit: Absicht durchsetzen, Status abfragen,
    // Zustandsmaschine einen Schritt weiterdrehen. Bewusst NICHT Teil von
    // INetworkUplink.
    //
    // Ethernet und WLAN brauchen so etwas nicht, weil sysfs sich selbst
    // aktualisiert; das Modem muss gefragt werden. Waere das in state() oder
    // info() versteckt, wuerde jeder Statusaufruf aus dem HTTP-Thread AT-
    // Kommandos ausloesen -- und ein Browser, der die Seite offen laesst,
    // haette die Taktung des Datenpfads in der Hand.
    void tick();

    // Administrativ. Trennt "der Benutzer will keinen Mobilfunk" von "es ist
    // keiner da": beides sieht von aussen wie Absent aus, aber nur eines davon
    // ist ein Grund, jemanden zu benachrichtigen.
    bool enabled() const;

    std::string   id() const override { return id_; }
    UplinkType    type() const override { return UplinkType::Cellular; }
    LinkState     state() const override;
    NetworkInfo   info() const override;
    UplinkMetrics metrics() const override;

    Result connect() override;
    Result disconnect() override;

    bool has_internet() const override;

    // Klartextbegruendung fuer die Anzeige, ohne Geheimnisse. Leer, wenn es
    // nichts zu erklaeren gibt.
    std::string detail() const;

    // EIN Weg, die Konfiguration zu setzen.
    //
    // Sie gehoert zwei Komponenten -- dem Statusdienst (wegen der PIN) und der
    // Zustandsmaschine (wegen APN und Betriebsart). Zwei Aufrufer, die sich
    // beide an beide erinnern muessen, sind eine Falle: wer nur den Dienst
    // setzt, aendert die angezeigte Konfiguration, ohne dass sich am Aufbau
    // etwas aendert.
    void set_config(const cellular::CellularConfig& c);

    // Fuer die Statusseite. Roh und ungefiltert; das Weglassen der Geheimnisse
    // passiert in den JSON-Sichten, nicht hier.
    const cellular::CellularConfig&    config() const { return svc_.config(); }
    const cellular::CellularStatus&    modem_status() const { return svc_.status(); }
    const cellular::CellularLinkState& link_state() const { return link_.state(); }

private:
    cellular::CellularService& svc_;
    cellular::EcmLink&         link_;
    std::string                id_;
    ReachabilityFn             reach_;

    // Spiegel der zuletzt gesehenen Absicht, damit eine Aenderung an der
    // Konfiguration auffaellt, ohne dass jemand sie hier melden muss.
    bool last_enabled_ = false;
    bool last_auto_connect_ = false;
    bool auto_connect_latched_ = false;
};

}} // namespace machino::net
