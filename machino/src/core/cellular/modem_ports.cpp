#include "core/cellular/modem_ports.hpp"

#include <algorithm>

namespace machino { namespace cellular {

namespace {

bool iequal(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

} // namespace

bool usb_names_from_tty_link(const std::string& link_target,
                             std::string& interface_name,
                             std::string& device_name)
{
    interface_name.clear();
    device_name.clear();

    // Komponenten von hinten durchgehen und die letzte mit ':' nehmen.
    size_t end = link_target.size();
    while (end > 0) {
        while (end > 0 && link_target[end - 1] == '/') --end;
        if (end == 0) break;
        size_t begin = link_target.find_last_of('/', end - 1);
        begin = (begin == std::string::npos) ? 0 : begin + 1;
        const std::string comp = link_target.substr(begin, end - begin);

        const size_t colon = comp.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < comp.size()) {
            // Muss wie ein USB-Interface aussehen: vor dem Doppelpunkt ein
            // Bindestrich (bus-port), dahinter ein Punkt (config.interface).
            // Sonst waere jede beliebige Komponente mit Doppelpunkt ein Treffer.
            if (comp.find('-') < colon && comp.find('.', colon) != std::string::npos) {
                interface_name = comp;
                device_name = comp.substr(0, colon);
                return true;
            }
        }
        end = (begin == 0) ? 0 : begin - 1;
    }
    return false;
}

ModemPorts map_modem_ports(const std::vector<SerialPortInfo>& ports,
                           const std::string& want_vid,
                           const std::string& want_pid)
{
    ModemPorts out;

    for (const SerialPortInfo& p : ports) {
        if (!iequal(p.vid, want_vid) || !iequal(p.pid, want_pid)) continue;
        if (p.device.empty()) continue;
        out.all.push_back(p);
    }
    if (out.all.empty()) return out;

    out.present = true;
    std::sort(out.all.begin(), out.all.end(),
              [](const SerialPortInfo& a, const SerialPortInfo& b) {
                  if (a.interface_number != b.interface_number)
                      return a.interface_number < b.interface_number;
                  return a.device < b.device;
              });

    // Die Erwartungstabelle. Nur was hier steht, bekommt eine Rolle.
    //
    // Ein Interface, das zweimal auftaucht, wird NICHT ueberschrieben: das
    // waere ein Zustand, den wir nicht verstehen, und die erste Zuordnung ist
    // so gut wie die zweite. Besser eine Rolle weniger als eine falsche.
    for (const SerialPortInfo& p : out.all) {
        switch (p.interface_number) {
            case 2: if (out.diag.empty())  { out.diag  = p.device; out.mapped_from_table = true; } break;
            case 3: if (out.at.empty())    { out.at    = p.device; out.mapped_from_table = true; } break;
            case 4: if (out.modem.empty()) { out.modem = p.device; out.mapped_from_table = true; } break;
            default: break;    // 0/1 sind das Netzwerk, alles andere kennen wir nicht
        }
    }
    return out;
}

}} // namespace machino::cellular
