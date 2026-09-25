#include "core/devices/aic8800_package.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace machino { namespace devices {

namespace {

bool exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

// Ganze Datei lesen, klein und ohne Ausnahmen. Fehlt sie, kommt "" zurueck --
// fuer jede Frage hier ist "nicht da" die richtige Antwort auf "nicht lesbar".
std::string slurp(const std::string& p, size_t cap = 64 * 1024) {
    std::string out;
    FILE* f = ::fopen(p.c_str(), "rb");
    if (!f) return out;
    char buf[1024];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof buf, f)) > 0) {
        out.append(buf, n);
        if (out.size() >= cap) break;
    }
    ::fclose(f);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Liegt irgendwo unter <root>/lib/modules ein .ko dieses Namens? Genau die
// Frage, die OpenIPCs adapter_scan stellt ("find /lib/modules -name '*.ko'"),
// nur andersherum formuliert.
bool module_under_lib_modules(const std::string& root, const std::string& name) {
    const std::string base = root + "/lib/modules";
    DIR* d = ::opendir(base.c_str());
    if (!d) return false;
    bool found = false;
    struct dirent* e;
    while (!found && (e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        // <ver>/machino/<name>.ko -- dorthin legt der Installer sie. Bewusst
        // nicht rekursiv ueber den ganzen Baum: ein fremdes aic8800.ko
        // irgendwo im Kernelbaum ist nicht unseres, und es als unseres zu
        // zaehlen waere eine Falschaussage im Device Manager.
        const std::string cand = base + "/" + e->d_name + "/machino/" + name + ".ko";
        if (exists(cand)) found = true;
    }
    ::closedir(d);
    return found;
}

// Steckt die Hardware? Nur die USB-Id, nichts ueber Treiberbindung.
// a69c:88dc ist der AIC8800DC.
bool usb_id_present(const std::string& root, const char* vid, const char* pid) {
    const std::string base = root + "/sys/bus/usb/devices";
    DIR* d = ::opendir(base.c_str());
    if (!d) return false;
    bool found = false;
    struct dirent* e;
    while (!found && (e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        const std::string dev = base + "/" + e->d_name;
        if (trim(slurp(dev + "/idVendor")) == vid && trim(slurp(dev + "/idProduct")) == pid)
            found = true;
    }
    ::closedir(d);
    return found;
}

// usb.mode aus der Konfiguration -- dieselbe Datei und dasselbe Format, das
// machino-usb-helper liest ("usb.mode = wifi").
std::string usb_mode(const std::string& root) {
    const std::string conf = slurp(root + "/etc/machino/machino.conf");
    size_t pos = 0;
    while (pos < conf.size()) {
        size_t nl = conf.find('\n', pos);
        if (nl == std::string::npos) nl = conf.size();
        const std::string line = trim(conf.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.rfind("usb.mode", 0) != 0) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        return trim(line.substr(eq + 1));
    }
    return std::string();
}

} // namespace

Aic8800Package::Aic8800Package(std::string root) : root_(std::move(root)) {}

std::string Aic8800Package::intent_path() const {
    return root_ + "/etc/machino/device-intent-aic8800";
}

std::string Aic8800Package::read_intent() const {
    return trim(slurp(intent_path(), 64));
}

// Traegt dieses Release die Nutzlast? Ohne sie waere "Installieren" ein Knopf,
// der nur scheitern kann -- der Device Manager bietet ihn dann nicht an.
bool Aic8800Package::is_available() const {
    return exists(root_ + "/etc/machino/payload/wifi/aic8800.ko") ||
           is_installed();   // schon eingerichtet heisst: die Nutzlast war da
}

// Aus Sicht des WIRTSSYSTEMS, nicht aus unserer: beide Module dort, wo
// adapter_scan sucht, UND das Profil in /etc/wireless/usb. Fehlt eines von
// beiden, bietet die OpenIPC-Seite den Adapter nicht an -- dann ist er auch
// nicht installiert, egal was in unseren eigenen Verzeichnissen liegt.
bool Aic8800Package::is_installed() const {
    if (!module_under_lib_modules(root_, "aic8800")) return false;
    if (!module_under_lib_modules(root_, "aic_load_fw")) return false;
    const std::string wl = slurp(root_ + "/etc/wireless/usb");
    return wl.find(profile_id()) != std::string::npos;
}

bool Aic8800Package::is_hardware_present() const {
    return usb_id_present(root_, "a69c", "88dc");
}

bool Aic8800Package::is_active() const {
    return usb_mode(root_) == "wifi";
}

// Nur vormerken. Das eigentliche Einrichten braucht depmod, und machinod darf
// bei lebendem IMP nicht forken -- siehe die Begruendung an InstallPending.
Result Aic8800Package::install() {
    if (is_installed()) return Result::ok();      // idempotent
    if (!is_available()) return Result::error();
    FILE* f = ::fopen(intent_path().c_str(), "wb");
    if (!f) return Result::error();
    ::fputs("install\n", f);
    ::fclose(f);
    return Result::ok();
}

Result Aic8800Package::uninstall() {
    if (!is_installed() && read_intent() != "install") return Result::ok();
    FILE* f = ::fopen(intent_path().c_str(), "wb");
    if (!f) return Result::error();
    ::fputs("remove\n", f);
    ::fclose(f);
    return Result::ok();
}

DeviceStatus Aic8800Package::status() const {
    DeviceStatus s;
    s.id = id();
    s.title = title();
    s.driver = "aic8800";
    s.openipc_registered = slurp(root_ + "/etc/wireless/usb").find(profile_id()) != std::string::npos;
    s.hardware_present = is_hardware_present();
    s.driver_loaded = slurp(root_ + "/proc/modules").find("aic8800 ") != std::string::npos;
    s.active = is_active();

    const std::string intent = read_intent();
    const bool installed = is_installed();
    if (!is_supported())            s.state = InstallState::Unsupported;
    else if (installed && intent == "remove")  s.state = InstallState::RemovePending;
    else if (installed)             s.state = InstallState::Installed;
    else if (intent == "install")   s.state = InstallState::InstallPending;
    else if (!is_available())       s.state = InstallState::Unavailable;
    else                            s.state = InstallState::NotInstalled;

    // Ein Satz Klartext genau dann, wenn der Zustand allein in die Irre fuehrt.
    if (s.state == InstallState::InstallPending || s.state == InstallState::RemovePending)
        s.detail = "wirksam nach dem naechsten Neustart";
    else if (s.state == InstallState::Installed && !s.hardware_present)
        s.detail = "eingerichtet, aber kein Adapter am Port erkannt";
    else if (s.state == InstallState::NotInstalled && s.hardware_present)
        s.detail = "Adapter erkannt, Treiber noch nicht eingerichtet";
    return s;
}

}} // namespace machino::devices
