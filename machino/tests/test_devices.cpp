// Der Geraetemanager: verfuegbar -> installiert -> aktiv.
//
// Geprueft wird vor allem, was NICHT passieren darf. Der Fehler vom
// 2026-09-25 war keine kaputte Funktion, sondern ein verwaschener Begriff:
// "installiert" hiess bei uns "liegt in unserem Verzeichnis", bei OpenIPC
// aber "unter /lib/modules auffindbar und in /etc/wireless registriert". Der
// AIC8800 lief, und die Oberflaeche bot trotzdem nur "None" an.
//
// Deshalb hier: die drei Zustaende sind unabhaengig, installieren aktiviert
// nicht, und es gibt hoechstens ein aktives Geraet -- es gibt genau einen Port.

#include "core/devices/aic8800_package.hpp"
#include "core/devices/device_package.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>

extern int g_fail_ext, g_pass_ext;
#define CHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

using namespace machino;
using namespace machino::devices;

namespace {

// Wegwerfbaum-Helfer. Absichtlich winzig: der Test soll den Vertrag pruefen,
// nicht ein Dateisystem nachbauen.
// Ohne Shell. Ein erster Versuch rief `mkdir -p` ueber system() auf -- auf dem
// Entwicklerrechner landet das bei cmd.exe, das weder -p noch die Quotes
// kennt, und der Test scheiterte an seinem eigenen Helfer statt am Code.
// mkdir(2) hat auf mingw nur ein Argument, daher die Weiche. Das Produktivpaket
// fasst nichts davon an -- es liest nur.
#ifdef _WIN32
#  include <direct.h>
#  define MACHINO_MKDIR(p) ::_mkdir(p)
#else
#  define MACHINO_MKDIR(p) ::mkdir((p), 0755)
#endif

void mkpath(const std::string& p) {
    size_t pos = 0;
    while (pos <= p.size()) {
        size_t sl = p.find('/', pos);
        if (sl == std::string::npos) sl = p.size();
        const std::string cur = p.substr(0, sl);
        if (!cur.empty()) MACHINO_MKDIR(cur.c_str());
        if (sl == p.size()) break;
        pos = sl + 1;
    }
}

void write_file(const std::string& p, const std::string& body) {
    FILE* f = ::fopen(p.c_str(), "wb");
    if (!f) return;
    ::fwrite(body.data(), 1, body.size(), f);
    ::fclose(f);
}

// Ebenfalls ohne Shell, aus demselben Grund. Reste zwischen zwei Laeufen sind
// hier nicht harmlos: die Zusicherungen bauen aufeinander auf ("noch kein
// Profil" -> "jetzt Profil"), ein liegengebliebener Baum machte sie gruen,
// ohne etwas zu zeigen.
void sysrm(const std::string& p) {
    DIR* d = ::opendir(p.c_str());
    if (d) {
        struct dirent* e;
        while ((e = ::readdir(d)) != nullptr) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            sysrm(p + "/" + n);
        }
        ::closedir(d);
    }
    ::remove(p.c_str());   // Datei oder (jetzt leeres) Verzeichnis
}

// Ein Paket, dessen Zustaende der Test frei setzt. Es bildet kein Dateisystem
// nach: geprueft wird der Vertrag des Managers, nicht die Wirtsregeln -- die
// gehoeren in die Hosttests des Installers, wo ein echter Baum liegt.
class FakePackage : public IDevicePackage {
public:
    FakePackage(std::string id, std::string title) : id_(std::move(id)), title_(std::move(title)) {}

    std::string id() const override { return id_; }
    std::string title() const override { return title_; }
    bool is_supported() const override { return supported; }
    bool is_available() const override { return available; }
    bool is_installed() const override { return installed; }
    bool is_hardware_present() const override { return hardware; }
    bool is_active() const override { return active; }

    Result install() override {
        if (!supported) return Result::unsupported();
        if (!available) return Result::error();
        ++installs;
        installed = true;
        return Result::ok();   // aktiviert ausdruecklich NICHT
    }
    Result uninstall() override {
        ++uninstalls;
        installed = false;
        return Result::ok();
    }

    DeviceStatus status() const override {
        DeviceStatus s;
        s.id = id_; s.title = title_; s.driver = "fake";
        s.state = !supported ? InstallState::Unsupported
                : !available ? InstallState::Unavailable
                : installed  ? InstallState::Installed
                             : InstallState::NotInstalled;
        s.openipc_registered = installed;
        s.hardware_present = hardware;
        s.active = active;
        return s;
    }

    bool supported = true, available = true, installed = false;
    bool hardware = false, active = false;
    int installs = 0, uninstalls = 0;

private:
    std::string id_, title_;
};

} // namespace

void run_devices_tests() {
    // --- der Manager haelt und findet ------------------------------------
    {
        FakePackage wifi("aic8800", "AIC8800DC WLAN");
        FakePackage modem("ec200a", "Quectel EC200A-EU 4G");
        DeviceManager m;
        m.add(&wifi);
        m.add(&modem);

        CHECK(m.list().size() == 2);
        CHECK(m.find("aic8800") == &wifi);
        CHECK(m.find("ec200a") == &modem);
        CHECK(m.find("gibtsnicht") == nullptr);
        CHECK(m.active() == nullptr);          // nichts aktiv, nichts behauptet
    }

    // --- installieren aktiviert NICHT ------------------------------------
    //
    // Der gemeinsame Port ist die Begruendung: waeren die beiden dasselbe,
    // wuerde das Installieren des Modems stillschweigend das WLAN verdraengen.
    {
        FakePackage wifi("aic8800", "AIC8800DC WLAN");
        DeviceManager m;
        m.add(&wifi);

        CHECK(wifi.install().is_ok());
        CHECK(wifi.is_installed());
        CHECK(!wifi.is_active());
        CHECK(m.active() == nullptr);
    }

    // --- beide duerfen installiert sein, nur eines aktiv ------------------
    {
        FakePackage wifi("aic8800", "AIC8800DC WLAN");
        FakePackage modem("ec200a", "Quectel EC200A-EU 4G");
        DeviceManager m;
        m.add(&wifi);
        m.add(&modem);

        CHECK(wifi.install().is_ok());
        CHECK(modem.install().is_ok());
        CHECK(wifi.is_installed() && modem.is_installed());

        modem.active = true;                    // der Port gehoert dem Modem
        CHECK(m.active() == &modem);
        CHECK(wifi.is_installed());             // WLAN bleibt installiert
        CHECK(!wifi.is_active());
    }

    // --- die Zustaende sind unabhaengig ----------------------------------
    //
    // Genau hier lag der Fehler: "Treiber da" wurde mit "vom Wirtssystem
    // gesehen" verwechselt. Ein Paket darf installiert sein ohne Hardware,
    // und Hardware darf stecken, ohne dass etwas installiert ist.
    {
        FakePackage wifi("aic8800", "AIC8800DC WLAN");
        wifi.hardware = false;
        CHECK(wifi.install().is_ok());
        CHECK(wifi.is_installed() && !wifi.is_hardware_present());

        FakePackage modem("ec200a", "Quectel EC200A-EU 4G");
        modem.hardware = true;
        CHECK(modem.is_hardware_present() && !modem.is_installed());
        CHECK(modem.status().state == InstallState::NotInstalled);
    }

    // --- ohne Nutzlast kein Knopf, der nur scheitern kann -----------------
    {
        FakePackage wifi("aic8800", "AIC8800DC WLAN");
        wifi.available = false;
        CHECK(wifi.status().state == InstallState::Unavailable);
        CHECK(!wifi.install().is_ok());
        CHECK(!wifi.is_installed());

        wifi.supported = false;
        CHECK(wifi.status().state == InstallState::Unsupported);
        CHECK(wifi.install().status == Status::Unsupported);
    }

    // --- idempotent: zweimal schadet nicht --------------------------------
    {
        FakePackage wifi("aic8800", "AIC8800DC WLAN");
        CHECK(wifi.install().is_ok());
        CHECK(wifi.install().is_ok());
        CHECK(wifi.is_installed());
        CHECK(wifi.uninstall().is_ok());
        CHECK(wifi.uninstall().is_ok());
        CHECK(!wifi.is_installed());
    }

    // --- das AIC8800-Paket gegen einen echten Baum ------------------------
    //
    // Hier wird der Fehler vom 2026-09-25 reproduziert: Module an unserer
    // Stelle, Profil fehlt -> OpenIPC sieht nichts -> NICHT installiert.
    {
        const std::string root = "tests/tmp-devices";
        sysrm(root);
        mkpath(root + "/etc/machino");
        mkpath(root + "/etc/wireless");
        mkpath(root + "/etc/machino/modules");
        write_file(root + "/etc/wireless/usb", "#!/bin/sh\nexit 1\n");

        Aic8800Package p(root);
        CHECK(p.is_supported());
        CHECK(!p.is_installed());
        CHECK(!p.is_available());              // keine Nutzlast, kein Knopf
        CHECK(p.status().state == InstallState::Unavailable);

        // Der alte Zustand: Module NUR in unserem Verzeichnis. Genau so lief
        // der Treiber -- und genau so bot die OpenIPC-Seite nur "None" an.
        write_file(root + "/etc/machino/modules/aic8800.ko", "x");
        write_file(root + "/etc/machino/modules/aic_load_fw.ko", "x");
        CHECK(!p.is_installed());              // unsere Stelle zaehlt nicht

        // Module dort, wo adapter_scan sucht -- aber noch ohne Profil.
        mkpath(root + "/lib/modules/4.4.94/machino");
        write_file(root + "/lib/modules/4.4.94/machino/aic8800.ko", "x");
        write_file(root + "/lib/modules/4.4.94/machino/aic_load_fw.ko", "x");
        CHECK(!p.is_installed());              // ohne Profil bietet OpenIPC nichts an
        CHECK(!p.status().openipc_registered);

        // Erst beides zusammen ist "installiert".
        write_file(root + "/etc/wireless/usb",
                   std::string("#!/bin/sh\nif [ \"$1\" = \"") + Aic8800Package::profile_id() +
                       "\" ]; then\n\tmodprobe aic8800\n\texit 0\nfi\nexit 1\n");
        CHECK(p.is_installed());
        CHECK(p.status().openipc_registered);
        CHECK(p.status().state == InstallState::Installed);
        CHECK(p.is_available());               // eingerichtet => war verfuegbar

        // Installiert heisst nicht aktiv: der Port gehoert noch niemandem.
        CHECK(!p.is_active());
        write_file(root + "/etc/machino/machino.conf", "usb.mode = wifi\n");
        CHECK(p.is_active());
        write_file(root + "/etc/machino/machino.conf", "usb.mode = cellular\n");
        CHECK(!p.is_active());                 // Port beim Modem, WLAN bleibt installiert
        CHECK(p.is_installed());

        // Hardware ist unabhaengig von alledem.
        CHECK(!p.is_hardware_present());
        mkpath(root + "/sys/bus/usb/devices/1-1");
        write_file(root + "/sys/bus/usb/devices/1-1/idVendor", "a69c\n");
        write_file(root + "/sys/bus/usb/devices/1-1/idProduct", "88dc\n");
        CHECK(p.is_hardware_present());

        // Rueckbau wird vorgemerkt, nicht sofort getan -- machinod darf nicht
        // forken, also kann es weder depmod noch die Datei wirklich aendern.
        CHECK(p.uninstall().is_ok());
        CHECK(p.status().state == InstallState::RemovePending);
        CHECK(p.is_installed());               // bis zum Neustart unveraendert
        CHECK(p.status().detail.find("Neustart") != std::string::npos);

        sysrm(root);
    }

    // --- vormerken, wenn die Nutzlast da ist ------------------------------
    {
        const std::string root = "tests/tmp-devices2";
        sysrm(root);
        mkpath(root + "/etc/machino/payload/wifi");
        mkpath(root + "/etc/wireless");
        write_file(root + "/etc/wireless/usb", "#!/bin/sh\nexit 1\n");
        write_file(root + "/etc/machino/payload/wifi/aic8800.ko", "x");

        Aic8800Package p(root);
        CHECK(p.is_available());
        CHECK(!p.is_installed());
        CHECK(p.status().state == InstallState::NotInstalled);
        CHECK(p.install().is_ok());
        CHECK(p.status().state == InstallState::InstallPending);
        CHECK(!p.is_installed());              // erst der Helfer richtet ein
        CHECK(p.install().is_ok());            // idempotent
        sysrm(root);
    }

    // --- die Zustandsnamen sind stabil (die API gibt sie heraus) ----------
    {
        CHECK(std::string(install_state_name(InstallState::Unsupported)) == "unsupported");
        CHECK(std::string(install_state_name(InstallState::Unavailable)) == "unavailable");
        CHECK(std::string(install_state_name(InstallState::NotInstalled)) == "not-installed");
        CHECK(std::string(install_state_name(InstallState::InstallPending)) == "install-pending");
        CHECK(std::string(install_state_name(InstallState::Installed)) == "installed");
        CHECK(std::string(install_state_name(InstallState::RemovePending)) == "remove-pending");
    }
}
