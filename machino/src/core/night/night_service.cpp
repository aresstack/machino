#include "core/night/night_service.hpp"
#include "core/log.hpp"

#include <chrono>
#include <ctime>
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

NightService::~NightService() { stop(); }

void NightService::start()
{
    std::lock_guard<std::mutex> lk(thread_m_);
    if (thread_.joinable()) return;
    quit_ = false;
    thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lk(thread_m_);
        while (!quit_) {
            lk.unlock();
            struct timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
            tick((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
            lk.lock();
            thread_cv_.wait_for(lk, std::chrono::seconds(2), [this] { return quit_; });
        }
    });
}

void NightService::stop()
{
    {
        std::lock_guard<std::mutex> lk(thread_m_);
        if (!thread_.joinable()) return;
        quit_ = true;
    }
    thread_cv_.notify_all();
    thread_.join();
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
    p.light_monitor       = store_.get("night.light_monitor") == "true";
    p.color_to_gray       = store_.get("night.color_to_gray") != "false";   // abwesend = an
    auto secs = [this](const char* key, int def) {
        const std::string v = store_.get(key);
        if (v.empty()) return def;
        const int n = atoi(v.c_str());
        return n >= 0 && n <= 3600 ? n : def;
    };
    p.auto_night_delay_s  = secs("night.auto_night_delay", p.auto_night_delay_s);
    p.auto_day_delay_s    = secs("night.auto_day_delay", p.auto_day_delay_s);
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
    std::lock_guard<std::mutex> lk(m_);
    return set_night_locked_(on);
}

std::string NightService::toggle_night(bool& out)
{
    std::lock_guard<std::mutex> lk(m_);
    const std::string e = set_night_locked_(!night_);
    out = night_;
    return e;
}

std::string NightService::set_night_locked_(bool on, std::string* actuator_error)
{
    const NightPins p = pins();
    // Der ISP-Teil zuerst: RunningMode 0/1 -- Schwarzweiss nur, wenn
    // colorToGray es will. Ein unsupported RunningMode (Plattform ohne den
    // Regler) ist ein ehrlicher Fehler.
    //
    // set_image_override, nicht set_image und nicht set_image_live: der Wert
    // wird nach jedem Pipeline-Start wieder angewendet (mit _live ging er
    // verloren, wenn das Video gerade kalt war -- eine Kamera, die nachts
    // ohne Zuschauer umschaltet und spaeter geoeffnet wird, zeigte dann
    // Tagmodus), aber er landet NICHT in der gemeldeten Config: set_image
    // schrieb ihn dorthin, und eine nachts gespeicherte Image-Seite machte
    // das Graubild dauerhaft, auch fuer den Tag.
    const power::ApplyResult ar = tuning_.set_image_override(ImageControl::RunningMode, on && p.color_to_gray ? 1 : 0);
    if (!ar.ok) return ar.message.empty() ? "running_mode could not be applied" : ar.message;
    night_ = on;

    // Filter und Licht folgen dem Modus NUR, wo die Config sie freigibt --
    // "an actuator told not to follow day/night does not move with it"
    // (Kommentar der Stock-Seite). Deren Fehler ueberschreiben den Erfolg
    // des Modus nicht (die Seite liest alle drei Zustaende ohnehin neu),
    // aber die Automatik bekommt sie ueber `actuator_error` und schaltet
    // beim naechsten Takt erneut.
    std::string ae;
    if (p.ircut && !p.ircut_pin1.empty()) ae = drive_ircut_(!on);   // Nacht = Filter raus
    if (p.backlight && !p.backlight_pin.empty()) { const std::string le = drive_light_(on); if (ae.empty()) ae = le; }
    if (actuator_error) *actuator_error = ae;
    return {};
}

std::string NightService::reapply_running_mode()
{
    std::lock_guard<std::mutex> lk(m_);
    const NightPins p = pins();
    const power::ApplyResult ar = tuning_.set_image_override(ImageControl::RunningMode, night_ && p.color_to_gray ? 1 : 0);
    return ar.ok ? std::string() : (ar.message.empty() ? "running_mode could not be applied" : ar.message);
}

std::string NightService::toggle_ircut()
{
    std::lock_guard<std::mutex> lk(m_);
    return drive_ircut_(!ircut_);
}

std::string NightService::toggle_light()
{
    std::lock_guard<std::mutex> lk(m_);
    return drive_light_(!light_);
}

void NightService::tick(int64_t now_ms)
{
    std::lock_guard<std::mutex> lk(m_);
    const NightPins p = pins();
    if (!p.light_monitor) {
        // Aus: vergessen, was gereift ist. Beim naechsten Einschalten wird der
        // dann aktuelle Zustand wieder einmal angewendet.
        auto_.committed = auto_.candidate = auto_.raw = auto_.pending_s = -1;
        auto_.announced = false;
        auto_.error.clear(); auto_.switch_error.clear();
        return;
    }
    auto fail = [&](const std::string& why) {
        if (why != auto_.error) LOGW("NIGHT", "automatic day/night idle: %s", why.c_str());
        auto_.error = why;
        auto_.raw = auto_.pending_s = -1;
    };
    if (!gpio_ || !gpio_->available()) return fail("no GPIO on this platform");
    if (p.light_sensor_pin.empty()) return fail("no daylight sensor pin configured in Day / Night settings");
    const std::string name = pin_name_(p.light_sensor_pin);
    if (name.empty()) return fail("daylight sensor pin '" + p.light_sensor_pin + "': not a valid pin");

    if (auto_.input_pin != name) {
        if (now_ms < auto_.retry_at_ms) return;
        const Result r = gpio_->configure_input(name);
        if (!r) { auto_.retry_at_ms = now_ms + 60000; return fail(gpio_err("daylight sensor", name, r)); }
        auto_.input_pin = name;
    }
    bool level = false;
    const Result r = gpio_->read(name, level);
    if (!r) { auto_.input_pin.clear(); auto_.retry_at_ms = now_ms + 60000; return fail(gpio_err("daylight sensor", name, r)); }
    auto_.error.clear();

    // Konvention: HIGH = dunkel (so melden die gaengigen Fotozellen-Module
    // mit Komparator); ein Sensor, der andersherum liegt, wird mit
    // lightSensorInvert gedreht.
    const int dark = (level != p.light_sensor_invert) ? 1 : 0;
    auto_.raw = dark;

    // Ein Wechsel ist erst vollzogen, wenn das Schalten gelungen ist. Vorher
    // stand `committed = dark` VOR dem Aufruf: ein einmal fehlgeschlagener
    // Schaltvorgang in der Daemmerung (ISP beim Boot noch nicht da, GPIO
    // gerade belegt) wurde nie wiederholt -- eine ganze Nacht im Tagmodus.
    auto apply = [&](const char* how) {
        if (auto_.switch_error.empty()) LOGI("NIGHT", "automatic day/night%s: switching to %s", how, dark ? "night" : "day");
        std::string ae;
        std::string e = set_night_locked_(dark == 1, &ae);
        if (e.empty()) e = ae;                          // the mode switched, an actuator did not: try again too
        if (!e.empty()) {
            if (e != auto_.switch_error) LOGW("NIGHT", "automatic switch to %s failed: %s - retrying", dark ? "night" : "day", e.c_str());
            auto_.switch_error = e;
            auto_.pending_s = 0;                    // faellig, noch nicht geschafft
            return false;
        }
        if (!auto_.switch_error.empty()) LOGI("NIGHT", "automatic switch to %s succeeded on retry", dark ? "night" : "day");
        auto_.switch_error.clear();
        auto_.committed = dark;
        auto_.candidate = auto_.pending_s = -1;
        return true;
    };
    if (auto_.committed < 0) {                  // gerade eingeschaltet: einmal anwenden
        if (!auto_.announced) { LOGI("NIGHT", "automatic day/night on: sensor says %s", dark ? "dark" : "light"); auto_.announced = true; }
        apply(" on");
        return;
    }
    if (dark == auto_.committed) { auto_.candidate = auto_.pending_s = -1; auto_.switch_error.clear(); return; }
    if (auto_.candidate != dark) { auto_.candidate = dark; auto_.candidate_since_ms = now_ms; auto_.switch_error.clear(); }
    const int64_t need_ms = (int64_t)(dark ? p.auto_night_delay_s : p.auto_day_delay_s) * 1000;
    const int64_t left_ms = need_ms - (now_ms - auto_.candidate_since_ms);
    if (left_ms > 0) { auto_.pending_s = (int)((left_ms + 999) / 1000); return; }
    apply("");
}

AutoState NightService::auto_state() const
{
    std::lock_guard<std::mutex> lk(m_);
    AutoState a;
    a.enabled = pins().light_monitor;
    a.sensing = a.enabled && !auto_.input_pin.empty() && auto_.error.empty();
    a.dark = auto_.raw;
    a.pending_s = auto_.pending_s;
    a.error = !auto_.error.empty() ? auto_.error : auto_.switch_error;
    return a;
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
