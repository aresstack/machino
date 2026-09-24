// Welcher /dev/ttyUSB* ist der AT-Port.
//
// Die Nummer ist es NICHT. Sie haengt daran, in welcher Reihenfolge der Kernel
// die Interfaces bindet, und die aendert sich mit der Zahl der Geraete am Bus,
// mit dem usbnet-Modus des Modems und mit jedem Neustart. `/dev/ttyUSB2` als
// Produktannahme ist ein Fehler, der erst beim Kunden auffaellt.
//
// Stabil ist die INTERFACENUMMER des USB-Geraets. Das EC200A-EU fuehrt laut
// Referenzprojekt (Miguel0888/quectel-ec200a-eu, README.md):
//
//     MI_00   Netzwerk (ECM bzw. RNDIS)
//     MI_02   DIAG
//     MI_03   AT
//     MI_04   Modem / PPP
//
// Diese Zuordnung stammt von der Windows-Treiberseite und ist auf Linux noch
// nicht nachgemessen -- siehe docs/ec200a-reuse-map.md, UNKNOWN 1. Deshalb ist
// die Tabelle hier eine ERWARTUNG, kein Gesetz: `mapped_from_table` sagt, ob
// die Rollen aus ihr kamen, und wer es genauer wissen will, probiert die Ports
// durch. Das Durchprobieren gehoert nach AP-M3; hier steht nur die Zuordnung,
// und sie steht in core, weil sie reine Datenverarbeitung ist und keinerlei
// Hardware braucht, um pruefbar zu sein.
#pragma once
#include <string>
#include <vector>

namespace machino { namespace cellular {

// Ein serieller Port, wie ihn der Scanner im Adapter aus sysfs liest.
struct SerialPortInfo {
    std::string device;             // "/dev/ttyUSB0"
    std::string vid;                // "2c7c", kleingeschrieben wie sysfs es liefert
    std::string pid;                // "6005"
    int         interface_number = -1;   // bInterfaceNumber, -1 = unbekannt
};

struct ModemPorts {
    bool present = false;           // ein Geraet mit passender VID:PID hat Ports
    bool mapped_from_table = false; // Rollen kamen aus der Erwartungstabelle
    std::string at;                 // "" wenn nicht zuzuordnen
    std::string diag;
    std::string modem;
    std::vector<SerialPortInfo> all;  // alles, was zum Geraet gehoert, nach
                                      // Interfacenummer sortiert
};

// Filtert auf vid/pid und ordnet die Rollen zu. Gross-/Kleinschreibung der
// Hex-Ziffern spielt keine Rolle.
//
// Ports ohne Interfacenummer landen in `all`, bekommen aber keine Rolle: eine
// Rolle zu raten waere schlimmer, als keine zu liefern.
ModemPorts map_modem_ports(const std::vector<SerialPortInfo>& ports,
                           const std::string& want_vid = "2c7c",
                           const std::string& want_pid = "6005");

// Aus dem Symlinkziel von /sys/class/tty/<name> das USB-Interface und das
// Geraet herausziehen. Das Ziel sieht so aus:
//
//     ../../devices/platform/.../usb1/1-1/1-1:1.3/ttyUSB0/tty/ttyUSB0
//                                      ^^^^ ^^^^^^^
//                                      dev  interface
//
// Beide sind unter /sys/bus/usb/devices/<name> direkt erreichbar, also genuegt
// der NAME und es braucht kein realpath(3) -- das ist nebenbei der Grund, warum
// diese Zerlegung hier in core steht und nicht im Adapter: so ist sie ohne
// sysfs pruefbar.
//
// Ein Interface heisst `<bus>-<port>[.<port>...]:<config>.<interface>`, das
// Geraet ist derselbe Name ohne den Doppelpunkt-Teil. Gesucht wird die LETZTE
// Komponente mit Doppelpunkt: bei einem Geraet hinter einem Hub steht davor
// noch der Hub, und der ist nicht gemeint.
//
// false, wenn im Pfad kein Interface steckt.
bool usb_names_from_tty_link(const std::string& link_target,
                             std::string& interface_name,
                             std::string& device_name);

}} // namespace machino::cellular
