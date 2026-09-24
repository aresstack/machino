// Die Linux-Seite des ECM-Datenpfads.
//
// Was auf dem ESP32 selbst gebaut werden musste -- Interface-Claim, rohe
// Bulk-Transfers, ein eigenes lwIP-netif, eigene Pufferpools -- macht hier der
// Kernel. Uebrig bleibt erstaunlich wenig: ein Netzwerkinterface finden, es
// hochnehmen, eine Adresse besorgen und sagen, ob der Link steht.
//
// Genau diese Reste stehen hinter dieser Schnittstelle, damit die
// Zustandsmaschine darueber ohne Kernel, ohne udhcpc und ohne Modem geprueft
// werden kann.
#pragma once
#include <string>

namespace machino {

struct EcmInterface {
    std::string name;        // "usb0", "eth1", ... NIE angenommen, immer gefunden
    bool        carrier = false;
    bool        up = false;
};

struct EcmAddress {
    std::string ipv4;
    std::string netmask;
    std::string gateway;
    std::string dns1, dns2;
    int         mtu = 0;

    bool has_address() const { return !ipv4.empty(); }
};

class IEcmBackend {
public:
    virtual ~IEcmBackend() = default;

    // Das Netzwerkinterface des Modems, falls es existiert. false heisst: noch
    // nicht da -- nach einem Moduswechsel dauert das, und das ist kein Fehler.
    virtual bool find_interface(EcmInterface& out) = 0;

    // Interface administrativ hochnehmen. Ohne das meldet der Kernel keinen
    // Carrier und DHCP haette nichts zu tun.
    virtual bool set_up(const std::string& ifname, bool up) = 0;

    // DHCP fuer GENAU dieses Interface starten bzw. beenden. Der Aufrufer ist
    // nicht der, der den Prozess startet -- das tut ein kleiner externer
    // Helfer, weil der Prozess mit der IMP-Pipeline nicht forken darf.
    virtual bool dhcp_start(const std::string& ifname) = 0;
    virtual bool dhcp_stop(const std::string& ifname) = 0;

    // Was der Kernel ueber das Interface sagt. Im NIC-Modus wird die Adresse
    // nicht hierher kommen, sondern vom Modem -- dann setzt der Aufrufer sie.
    virtual bool read_address(const std::string& ifname, EcmAddress& out) = 0;

    // Adresse statisch setzen. Der NIC-Modus des EC200A reicht die oeffentliche
    // Adresse direkt durch und beantwortet KEIN DHCP; die Werte kommen dann aus
    // AT+CGCONTRDP.
    virtual bool set_address(const std::string& ifname, const EcmAddress& a) = 0;

    // Alles wieder abraeumen, was zu diesem Interface gehoert -- und nur das.
    virtual void teardown(const std::string& ifname) = 0;
};

} // namespace machino
