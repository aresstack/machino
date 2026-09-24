#include "adapters/linux/linux_serial_scan.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

namespace machino { namespace linuxsys {

namespace {

std::string slurp_trimmed(const std::string& path)
{
    FILE* f = ::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    char buf[128];
    const size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
    ::fclose(f);
    buf[n] = 0;
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
    return s;
}

// Nur das Ziel EINES Symlinks, nicht die ganze Kette. realpath(3) waere
// bequemer, ist aber nicht ueberall deklariert, und gebraucht wird es auch
// nicht: der Name des Interface steht im Zieltext, und unter
// /sys/bus/usb/devices/<name> ist es direkt erreichbar.
bool read_link(const std::string& path, std::string& out)
{
    char buf[1024];
    const ssize_t n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    out = buf;
    return true;
}

} // namespace

std::vector<cellular::SerialPortInfo> scan_usb_serial_ports(const std::string& sys_root,
                                                            const std::string& dev_root)
{
    std::vector<cellular::SerialPortInfo> out;

    const std::string tty_dir = sys_root + "/class/tty";
    DIR* d = ::opendir(tty_dir.c_str());
    if (!d) return out;

    while (struct dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.compare(0, 6, "ttyUSB") != 0) continue;

        cellular::SerialPortInfo p;
        p.device = dev_root + "/" + name;

        std::string target, iface_name, dev_name;
        if (read_link(tty_dir + "/" + name, target) &&
            cellular::usb_names_from_tty_link(target, iface_name, dev_name)) {
            const std::string usbdev = sys_root + "/bus/usb/devices/";
            const std::string num = slurp_trimmed(usbdev + iface_name + "/bInterfaceNumber");
            // bInterfaceNumber steht in sysfs HEXADEZIMAL ("03"). Als Dezimal
            // gelesen stimmt es bis 9 zufaellig und dann nicht mehr.
            if (!num.empty()) p.interface_number = (int)::strtol(num.c_str(), nullptr, 16);
            p.vid = slurp_trimmed(usbdev + dev_name + "/idVendor");
            p.pid = slurp_trimmed(usbdev + dev_name + "/idProduct");
        }
        out.push_back(p);
    }
    ::closedir(d);

    std::sort(out.begin(), out.end(),
              [](const cellular::SerialPortInfo& a, const cellular::SerialPortInfo& b) {
                  return a.device < b.device;
              });
    return out;
}

}} // namespace machino::linuxsys
