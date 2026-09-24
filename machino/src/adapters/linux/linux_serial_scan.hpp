// Die seriellen Ports eines USB-Geraets aus sysfs lesen.
//
// Der duenne Teil: nur Verzeichnisse lesen und Symlinks aufloesen. Die
// Zuordnung Interfacenummer -> Rolle liegt in core/cellular/modem_ports und
// braucht dafuer keine Hardware.
//
// Der Weg durch sysfs, an der Struktur der Kamera abgelesen:
//
//     /sys/class/tty/ttyUSB0/device        -> .../1-1:1.3   (das INTERFACE)
//     .../1-1:1.3/bInterfaceNumber         -> "03"
//     .../1-1/idVendor  .../1-1/idProduct  -> das GERAET, ein Verzeichnis hoeher
//
// `1-1:1.3` heisst Bus 1, Port 1, Konfiguration 1, Interface 3. Das Geraet ist
// das Elternverzeichnis. Genau so haengen auf dieser Kamera heute die
// Interfaces des WLAN-Adapters (1-1:1.0, 1-1:1.1, 1-1:1.2).
#pragma once
#include "core/cellular/modem_ports.hpp"
#include <string>
#include <vector>

namespace machino { namespace linuxsys {

// Alle ttyUSB*-Ports mit ihrer VID/PID und Interfacenummer. sys_root und
// dev_root sind Testhaken -- in Produktion "/sys" und "/dev".
//
// Ein Port, dessen Geraet sich nicht aufloesen laesst, wird mit leerer
// vid/pid zurueckgegeben statt weggelassen: dass er DA ist, ist eine
// Information, auch wenn wir ihn nicht zuordnen koennen.
std::vector<cellular::SerialPortInfo> scan_usb_serial_ports(
        const std::string& sys_root = "/sys",
        const std::string& dev_root = "/dev");

}} // namespace machino::linuxsys
