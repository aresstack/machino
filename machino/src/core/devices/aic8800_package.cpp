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
// Frage, die OpenIPCs adapter_scan stellt, nur andersherum formuliert.
bool module_under_lib_modules(const std::string& root, const std::string& name) {
    const std::string base = root + "/lib/modules";
    DIR* d = ::opendir(base.c_str());
    if (!d) return false;
    bool found = false;
    struct dirent* e;
    while (!found && (e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        // <ver>/machino/<name>.ko -- dorthin legt machino-device sie. Bewusst
        // nicht rekursiv ueber den ganzen Baum: ein fremdes aic8800.ko
        // irgendwo im Kernelbaum ist nicht unseres, und es als unseres zu
        // zaehlen waere eine Falschaussage im Device Manager.
        const std::string cand = base + "/" + e->d_name + "/machino/" + name + ".ko";
        if (exists(cand)) found = true;
    }
    ::closedir(d);
    return found;
}

// Steckt die Hardware? Nur die USB-Id, nichts ueber Treiberbindung. Welche Id
// gemeint ist, sagt das Manifest -- ohne Id lautet die Antwort "weiss nicht",
// und die ist hier dasselbe wie "nein".
bool usb_id_present(const std::string& root, const std::string& vid, const std::string& pid) {
    if (vid.empty() || pid.empty()) return false;
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

Aic8800Package::Aic8800Package(std::string root)
    : root_(std::move(root)), mf_(DeviceManifest::load(root_, "aic8800")) {}

std::string Aic8800Package::title() const {
    return mf_.title.empty() ? std::string("AIC8800DC WLAN") : mf_.title;
}

std::string Aic8800Package::intent_path() const {
    return root_ + "/etc/machino/device-intent-aic8800";
}

std::string Aic8800Package::read_intent() const {
    return trim(slurp(intent_path(), 64));
}

// Was der Boot-Helfer beim letzten Versuch gemeldet hat. Leer heisst: kein
// Fehlschlag bekannt. machino-device legt diese Datei an, wenn eine Absicht
// NICHT durchlief, und loescht sie, sobald sie durchlaeuft -- die Absicht
// selbst bleibt dabei liegen, damit der naechste Boot es wieder versucht.
std::string Aic8800Package::read_intent_failure() const {
    return trim(slurp(intent_path() + ".failed", 256));
}

// Traegt dieses Release die Nutzlast? Ohne sie waere "Installieren" ein Knopf,
// der nur scheitern kann -- der Device Manager bietet ihn dann nicht an.
//
// Die Nutzlast liegt unter /etc/machino/payload/<id>/ und ueberlebt ein
// Deinstallieren im Geraetemanager bewusst; nur das vollstaendige uninstall.sh
// raeumt sie ab. Geprueft wird jedes einzelne Modul, nicht nur das
// Verzeichnis: ein halb gefuellter Ordner wuerde "Installieren" anbieten, das
// dann in machino-device an der fehlenden Datei scheitert.
bool Aic8800Package::is_available() const {
    if (!mf_.loaded || mf_.payload_modules.empty()) return false;
    const std::string dir = root_ + "/etc/machino/payload/" + mf_.id;
    for (size_t i = 0; i < mf_.payload_modules.size(); ++i) {
        if (!exists(dir + "/" + mf_.payload_modules[i] + ".ko"))
            return is_installed();   // schon eingerichtet heisst: sie war da
    }
    return true;
}

// Aus Sicht des WIRTSSYSTEMS, nicht aus unserer: alle mitgebrachten Module
// dort, wo adapter_scan sucht, UND das Profil in /etc/wireless/usb. Fehlt
// eines von beiden, bietet die OpenIPC-Seite den Adapter nicht an -- dann ist
// er auch nicht installiert, egal was in unseren eigenen Verzeichnissen liegt.
bool Aic8800Package::is_installed() const {
    if (!mf_.loaded || mf_.payload_modules.empty()) return false;
    for (size_t i = 0; i < mf_.payload_modules.size(); ++i)
        if (!module_under_lib_modules(root_, mf_.payload_modules[i])) return false;
    const std::string wl = slurp(root_ + "/etc/wireless/usb");
    return wl.find(mf_.openipc_profile) != std::string::npos;
}

bool Aic8800Package::is_hardware_present() const {
    return usb_id_present(root_, mf_.usb_vid, mf_.usb_pid);
}

bool Aic8800Package::is_active() const {
    if (mf_.usb_mode.empty()) return false;
    return usb_mode(root_) == mf_.usb_mode;
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

// Ein AKTIVES Geraet wird nicht deinstalliert.
//
// "Aktiv" heisst: ihm gehoert der eine USB-Port (usb.mode). Wuerden wir hier
// Module und Profil entfernen, bliebe eine Kamera zurueck, die beim naechsten
// Boot einen Adapter aufbauen will, den es nicht mehr gibt -- und deren
// wlandev auf ein Profil zeigt, das aus /etc/wireless/usb verschwunden ist.
// S40network faende dann keinen Zweig.
//
// Die Alternative waere, hier gleich mit zu deaktivieren. Dagegen spricht die
// Trennung, die dieses Modell ueberhaupt erst traegt: installieren aktiviert
// nicht, also darf deinstallieren auch nicht deaktivieren. Ein Knopf, der
// nebenbei den Uplink der Kamera abschaltet, ist eine Ueberraschung, keine
// Bedienung. Also: sagen, was zuerst zu tun ist.
//
// Busy, nicht Error: es ist der falsche Zustand, kein Fehlschlag.
Result Aic8800Package::uninstall() {
    if (is_active()) return Result::busy();
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
    s.driver = mf_.driver;
    s.openipc_registered = !mf_.openipc_profile.empty() &&
        slurp(root_ + "/etc/wireless/usb").find(mf_.openipc_profile) != std::string::npos;
    s.hardware_present = is_hardware_present();
    s.driver_loaded = !mf_.driver.empty() &&
        slurp(root_ + "/proc/modules").find(mf_.driver + " ") != std::string::npos;
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
    const std::string failed = read_intent_failure();
    if (!mf_.loaded)
        s.detail = "kein Manifest unter /etc/machino/devices - dieses Geraet ist nicht beschrieben";
    else if (!failed.empty() &&
             (s.state == InstallState::InstallPending || s.state == InstallState::RemovePending))
        // Wichtiger als der freundliche Satz: dass hier NICHT "wirksam nach dem
        // naechsten Neustart" steht, wenn genau das beim letzten Neustart
        // schon nicht geklappt hat.
        s.detail = "beim letzten Neustart fehlgeschlagen: " + failed;
    else if (s.state == InstallState::InstallPending || s.state == InstallState::RemovePending)
        s.detail = "wirksam nach dem naechsten Neustart";
    else if (s.state == InstallState::Installed && s.active)
        s.detail = "eingerichtet und aktiv - zum Deinstallieren zuerst den USB-Port freigeben";
    else if (s.state == InstallState::Installed && !s.hardware_present)
        s.detail = "eingerichtet, aber kein Adapter am Port erkannt";
    else if (s.state == InstallState::NotInstalled && s.hardware_present)
        s.detail = "Adapter erkannt, Treiber noch nicht eingerichtet";
    return s;
}

}} // namespace machino::devices
