#include "core/night/night_service.hpp"

#include <chrono>
#include <thread>

namespace machino { namespace night {

namespace {
// Result traegt nur Status+Code; fuer den Menschen am Knopf wird daraus ein
// benannter Grund (busy = ein anderer Treiber haelt den Pin -- nie stehlen).
std::string gpio_err(const char* what, const std::string& pin, const Result& r)
{
    const char* why = r.status == Status::Busy        ? "pin is held by another driver"
                    : r.status == Status::Unsupported ? "not supported on this platform"
                    : r.status == Status::Timeout     ? "timed out"
                                                      : "failed";
    return std::string(what) + " " + pin + ": " + why +
           (r.code ? " (code " + std::to_string(r.code) + ")" : "");
}
} // namespace

NightService::NightService(media::TuningService& tuning, IGpioController* gpio, ConfigStore& store,
                           const hw::IPinResolver* resolver)
    : tuning_(tuning), gpio_(gpio), store_(store), resolver_(resolver)
{
}

// Ein Pin-String ist entweder eine reine GPIO-Nummer (so schreibt die
// WebUI-Karte, so will sysfs) oder ein Name wie "PD22". Beides landet als
// sysfs-Name, den IGpioController::configure_output erwartet.
std::string NightService::pin_name_(const std::string& pin) const
{
    if (pin.empty()) return {};
    bool digits = true;
    for (char ch : pin) if (ch < '0' || ch > '9') { digits = false; break; }
    if (digits) {
        if (!resolver_) return {};
        return resolver_->name_of(atoi(pin.c_str()));   // 118 -> "PD22"
    }
    return pin;                                          // schon ein Name
}

NightPins NightService::pins() const
{
    NightPins p;
    p.ircut               = store_.get("night.ircut") == "true";
    p.ircut_pin1          = store_.get("night.ircut_pin1");
    p.ircut_pin2          = store_.get("night.ircut_pin2");
    p.ircut_single_invert = store_.get("night.ircut_single_invert") == "true";
    p.backlight           = store_.get("night.backlight") == "true";
    p.backlight_pin       = store_.get("night.backlight_pin");
    p.light_sensor_pin    = store_.get("night.light_sensor_pin");
    p.light_sensor_invert = store_.get("night.light_sensor_invert") == "true";
    // W2b: Board-Profil-Defaults, wenn die UI (Store) nichts gesetzt hat --
    // dieselbe Rangfolge wie beim USB-Port: Profil-Vorgabe, Nutzer gewinnt.
    if (p.ircut_pin1.empty() && p.ircut_pin2.empty()) {
        p.ircut_pin1 = def_ircut_pin1_;
        p.ircut_pin2 = def_ircut_pin2_;
    }
    if (p.light_sensor_pin.empty())
        p.light_sensor_pin = def_light_sensor_pin_;
    return p;
}

// Mechanischer Filter: zwei Spulenpins, gegensinnig ~150 ms gepulst, danach
// beide stromlos (Dauerstrom wuerde die Spule kochen). Ein-Pin-Variante:
// Pegel halten, ircut_single_invert dreht die Polaritaet. Der Puls blockiert
// den Aufrufer 150 ms -- das ist der HTTP-Pfad eines manuellen Knopfdrucks,
// nicht der Videopfad (eigene Threads), und majestic tat dasselbe.
std::string NightService::drive_ircut_(bool engaged)
{
    const NightPins p = pins();
    if (!p.ircut)
        return "the IR-cut filter is set to off in Day / Night settings";
    if (!gpio_ || !gpio_->available())
        return "no GPIO on this platform";
    if (p.ircut_pin1.empty())
        return "no IR-cut pin configured in Day / Night settings";

    const std::string n1 = pin_name_(p.ircut_pin1);
    if (n1.empty()) return "IR-cut pin 1 '" + p.ircut_pin1 + "': not a valid pin";

    if (p.ircut_pin2.empty()) {
        const bool level = engaged != p.ircut_single_invert;
        Result r = gpio_->configure_output(n1, level);
        if (!r) return gpio_err("IR-cut pin", n1, r);
        r = gpio_->write(n1, level);
        if (!r) return gpio_err("IR-cut pin", n1, r);
        ircut_ = engaged;
        return {};
    }

    const std::string n2 = pin_name_(p.ircut_pin2);
    if (n2.empty()) return "IR-cut pin 2 '" + p.ircut_pin2 + "': not a valid pin";
    const std::string& hi = engaged ? n1 : n2;
    const std::string& lo = engaged ? n2 : n1;
    Result r = gpio_->configure_output(hi, false);
    if (!r) return gpio_err("IR-cut pin", hi, r);
    r = gpio_->configure_output(lo, false);
    if (!r) return gpio_err("IR-cut pin", lo, r);
    gpio_->write(lo, false);
    gpio_->write(hi, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    gpio_->write(hi, false);
    ircut_ = engaged;
    return {};
}

std::string NightService::drive_light_(bool on)
{
    const NightPins p = pins();
    if (!p.backlight)
        return "the camera light is set to off in Day / Night settings";
    if (!gpio_ || !gpio_->available())
        return "no GPIO on this platform";
    if (p.backlight_pin.empty())
        return "no light pin configured in Day / Night settings";
    const std::string bn = pin_name_(p.backlight_pin);
    if (bn.empty()) return "light pin '" + p.backlight_pin + "': not a valid pin";
    Result r = gpio_->configure_output(bn, on);
    if (!r) return gpio_err("light pin", bn, r);
    r = gpio_->write(bn, on);
    if (!r) return gpio_err("light pin", bn, r);
    light_ = on;
    return {};
}

std::string NightService::set_night(bool on)
{
    // Der ISP-Teil zuerst: RunningMode 0/1. Ein unsupported RunningMode
    // (Plattform ohne den Regler) ist ein ehrlicher Fehler.
    const power::ApplyResult ar = tuning_.set_image_live(ImageControl::RunningMode, on ? 1 : 0);
    if (!ar.ok) return ar.message.empty() ? "running_mode could not be applied" : ar.message;
    night_ = on;

    // Filter und Licht folgen dem Modus NUR, wo die Config sie freigibt --
    // "an actuator told not to follow day/night does not move with it"
    // (Kommentar der Stock-Seite). Deren Fehler ueberschreiben den Erfolg
    // des Modus nicht: die Seite liest alle drei Zustaende ohnehin neu.
    const NightPins p = pins();
    if (p.ircut && !p.ircut_pin1.empty()) drive_ircut_(!on);   // Nacht = Filter raus
    if (p.backlight && !p.backlight_pin.empty()) drive_light_(on);
    return {};
}

std::string NightService::toggle_ircut()
{
    return drive_ircut_(!ircut_);
}

std::string NightService::toggle_light()
{
    return drive_light_(!light_);
}

// Pin-String -> GPIO-Nummer (-1 = leer/ungueltig).
int NightService::pin_number_(const std::string& pin) const
{
    if (pin.empty()) return -1;
    bool digits = true;
    for (char ch : pin) if (ch < '0' || ch > '9') { digits = false; break; }
    if (digits) return atoi(pin.c_str());
    int n = -1;
    if (resolver_ && resolver_->resolve(pin, n)) return n;
    return -1;
}

NightService::GpioMap NightService::gpio_map() const
{
    GpioMap m;
    // Ingenic/xburst: 6 Baenke a 32 (PA..PF). Dieselbe Annahme wie der
    // Pin-Resolver und der USB-Port; die Karte rechnet ihre Geometrie daraus.
    m.bank_size = 32;
    for (int base = 0; base < 32 * 6; base += 32) m.bank_bases.push_back(base);

    const NightPins p = pins();
    auto add_assigned = [&](const std::string& pin, const char* role) {
        int n = pin_number_(pin);
        if (n >= 0) m.assigned.push_back({n, role});
    };
    add_assigned(p.ircut_pin1, "irCutPin1");
    add_assigned(p.ircut_pin2, "irCutPin2");
    add_assigned(p.backlight_pin, "backlightPin");
    add_assigned(p.light_sensor_pin, "lightSensorPin");

    // avoid: komma-separierte Nummern aus dem Store (die Karte darf Pads vom
    // Scan ausschliessen; der Scan selbst kommt spaeter).
    const std::string av = store_.get("night.avoid");
    size_t pos = 0;
    while (pos < av.size()) {
        size_t comma = av.find(',', pos);
        std::string tok = av.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? av.size() : comma + 1;
        size_t a = tok.find_first_not_of(" \t"); if (a == std::string::npos) continue;
        int n = atoi(tok.c_str() + a);
        if (n >= 0) m.avoid.push_back(n);
    }

    // held: machinos EIGENER belegter Port (USB-Strom) -- den kennen wir
    // sicher. Fremdhalter kann machino nicht zuverlaessig ermitteln, deshalb
    // owners_unknown=true (die Karte behauptet dann keine falschen Besitzer).
    const std::string usb_pin = store_.get("usb.power_pin");
    if (!usb_pin.empty()) {
        int n = pin_number_(usb_pin);
        if (n >= 0) m.held.push_back({n, "machino (usb power)"});
    }
    m.owners_unknown = true;
    return m;
}

}} // namespace machino::night
