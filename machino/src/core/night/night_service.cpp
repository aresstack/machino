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

NightService::NightService(media::TuningService& tuning, IGpioController* gpio, ConfigStore& store)
    : tuning_(tuning), gpio_(gpio), store_(store)
{
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
    // W2b: Board-Profil-Defaults, wenn die UI (Store) nichts gesetzt hat --
    // dieselbe Rangfolge wie beim USB-Port: Profil-Vorgabe, Nutzer gewinnt.
    if (p.ircut_pin1.empty() && p.ircut_pin2.empty()) {
        p.ircut_pin1 = def_ircut_pin1_;
        p.ircut_pin2 = def_ircut_pin2_;
    }
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

    if (p.ircut_pin2.empty()) {
        const bool level = engaged != p.ircut_single_invert;
        Result r = gpio_->configure_output(p.ircut_pin1, level);
        if (!r) return gpio_err("IR-cut pin", p.ircut_pin1, r);
        r = gpio_->write(p.ircut_pin1, level);
        if (!r) return gpio_err("IR-cut pin", p.ircut_pin1, r);
        ircut_ = engaged;
        return {};
    }

    const std::string& hi = engaged ? p.ircut_pin1 : p.ircut_pin2;
    const std::string& lo = engaged ? p.ircut_pin2 : p.ircut_pin1;
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
    Result r = gpio_->configure_output(p.backlight_pin, on);
    if (!r) return gpio_err("light pin", p.backlight_pin, r);
    r = gpio_->write(p.backlight_pin, on);
    if (!r) return gpio_err("light pin", p.backlight_pin, r);
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

}} // namespace machino::night
