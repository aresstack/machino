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

#include "core/devices/device_package.hpp"

#include <string>

#include <cstdio>

extern int g_fail_ext, g_pass_ext;
#define CHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

using namespace machino;
using namespace machino::devices;

namespace {

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
