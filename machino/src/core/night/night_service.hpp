// W2 (Day/Night): der Halter der drei Schalter, die majestic unter /night/*
// bediente -- Nachtmodus, IR-Cut-Filter, Kameralicht. Die Stock-WebUI (die
// unveraendert bleibt) erwartet exakt diese Flaeche:
//
//   GET /night/on|off|toggle      -> Nachtmodus, Antwort das neue Boolean
//   GET /night/ircut              -> Filter kippen, Antwort das neue Boolean
//   GET /night/light              -> Licht kippen, Antwort das neue Boolean
//   GET /metrics/night?value=night_enabled|ircut_enabled|light_enabled -> "0"/"1"
//
// Der Nachtmodus faehrt ueber ImageControl::RunningMode (0 Tag, 1 Nacht,
// ISP-Sache); Filter und Licht sind GPIO. Die Pins kommen aus der Config
// (night.*-Keys, in der WebUI als "Day / Night"-Sektion nightMode.*): ein
// mechanischer IR-Cut hat ZWEI Spulenpins, die fuer den Wechsel kurz
// gegensinnig gepulst werden; die Ein-Pin-Variante haelt stattdessen einen
// Pegel (ircut_single_invert dreht ihn). Kein Pin konfiguriert = die Aktion
// wird MIT GRUND verweigert, nie stumm geschluckt -- der Tooltip der Seite
// ("set to off in Day / Night settings") speist sich aus genau dieser Config.
#pragma once
#include "core/config_store.hpp"
#include "core/hw/pin_resolver.hpp"
#include "core/media/tuning_service.hpp"
#include "ports/igpio.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace machino { namespace night {

struct NightPins {
    bool        ircut = false;            // nightMode.irCut: Filter-Steuerung erlaubt
    std::string ircut_pin1, ircut_pin2;   // leer = nicht konfiguriert
    bool        ircut_single_invert = false;
    bool        backlight = false;        // nightMode.backlight: Licht-Steuerung erlaubt
    std::string backlight_pin;
    // Tag/Nacht-Fotosensor (Eingang). Nur Anzeige/Vorbelegung bis eine
    // Automatik ihn abfragt -- eine gesetzte Pin schaltet nichts von selbst.
    std::string light_sensor_pin;
    bool        light_sensor_invert = false;
    // Automatik (majestic nightMode.lightMonitor): der Fotosensor schaltet
    // den Nachtmodus -- und mit ihm IR-Cut/Licht, soweit freigegeben. Aus =
    // es bleibt bei den manuellen /night/*-Knoepfen.
    bool        light_monitor = false;
    // majestic nightMode.colorToGray: bringt der Nachtmodus auch das
    // Schwarzweissbild (ISP RunningMode Nacht)? Default an, wie majestic auf
    // dieser Kamera und wie die Stock-Firmware. Aus = nachts Filter raus,
    // Bild bleibt farbig.
    bool        color_to_gray = true;
    // Wie lange der Sensor dunkel (hell) melden muss, bevor umgeschaltet
    // wird (majestic autoNightDelay/autoDayDelay, Sekunden). Stock: Delay 3.
    int         auto_night_delay_s = 3;
    int         auto_day_delay_s = 3;
};

// Was die Automatik gerade weiss -- fuer Telemetrie und die Stock-Seite.
struct AutoState {
    bool        enabled = false;          // lightMonitor an
    bool        sensing = false;          // der Sensor wird gelesen
    int         dark = -1;                // letzter Rohwert: 1 dunkel, 0 hell, -1 unbekannt
    int         pending_s = -1;           // Sekunden bis zum Umschalten, -1 = nichts steht an
    std::string error;                    // warum sie gerade nicht arbeitet
};

class NightService {
public:
    // gpio darf null sein (Plattform ohne GPIO): Filter/Licht sind dann
    // ehrlich unsupported, der Nachtmodus (ISP) geht trotzdem. resolver
    // uebersetzt GPIO-Nummer<->Name (die WebUI-Pin-Karte spricht Nummern,
    // sysfs auch; die Config speichert Nummern).
    NightService(media::TuningService& tuning, IGpioController* gpio, ConfigStore& store,
                 const hw::IPinResolver* resolver = nullptr);
    ~NightService();
    NightService(const NightService&) = delete;
    NightService& operator=(const NightService&) = delete;

    // The automation's own thread: one tick() every two seconds. Its own
    // thread rather than a slot in the daemon's main loop, because a switch
    // pulses the IR-cut coil for 150 ms with the mutex held - on the main
    // loop that stalled the watchdog feeder and every other timer - and
    // because the automation must run whether or not the network poll timer
    // could be created. Tests call tick() directly and never start().
    void start();
    void stop();

    // W4: die GPIO-Landkarte fuer /api/v1/gpio -- Baenke, gehaltene Pins und
    // die aktuelle Rollen-Zuordnung. Rein aus dem, was machino WEISS
    // (Resolver-Baenke, eigene Exporte, Config); nichts erfunden.
    struct HeldPin { int pin; std::string owner; };
    struct AssignedPin { int pin; std::string role; };
    struct GpioMap {
        std::vector<int> bank_bases;      // 0,32,... ; alle Baenke voll (n=bank_size)
        int              bank_size = 32;
        std::vector<HeldPin> held;
        std::vector<AssignedPin> assigned;
        std::vector<int> avoid;
        bool owners_unknown = true;       // machino kann Fremdhalter nicht sicher wissen
    };
    GpioMap gpio_map() const;

    // Zustand (Boot: Tag, Filter drin, Licht aus -- und der erste Wechsel
    // stellt den Filter AKTIV auf den gewuenschten Zustand, statt einem
    // unbekannten Boot-Zustand zu vertrauen).
    bool night() const { std::lock_guard<std::mutex> lk(m_); return night_; }
    bool ircut() const { std::lock_guard<std::mutex> lk(m_); return ircut_; }
    bool light() const { std::lock_guard<std::mutex> lk(m_); return light_; }

    // Aktionen. Rueckgabe leer = ok (neuer Zustand via Getter); sonst der
    // konkrete Grund (kein Pin, GPIO belegt, ...). set_night zieht Filter
    // und Licht mit, WENN sie in der Config freigegeben sind -- die Seite
    // fragt danach ohnehin alle drei Zustaende neu ab.
    std::string set_night(bool on);
    std::string toggle_night(bool& out);
    std::string toggle_ircut();
    std::string toggle_light();

    NightPins pins() const;               // live aus dem ConfigStore
    // colorToGray geaendert: nur den ISP-RunningMode fuer den aktuellen Modus
    // neu anwenden -- Filter und Licht stehen schon richtig und werden nicht
    // erneut gepulst.
    std::string reapply_running_mode();

    // Automatik-Takt (eigener Thread, alle zwei Sekunden). Liest den
    // Fotosensor und schaltet NUR bei einem Wechsel, der auto_*_delay_s
    // lang stabil war -- ein manueller Knopfdruck bleibt also stehen, bis
    // sich das Licht wirklich aendert. Beim Einschalten der Automatik wird
    // der aktuelle Zustand einmal angewendet. Ein Wechsel gilt erst als
    // vollzogen, wenn der Schaltvorgang GELUNGEN ist: schlaegt er fehl
    // (ISP noch nicht da, GPIO belegt), wird beim naechsten Takt erneut
    // geschaltet -- nicht erst beim naechsten Lichtwechsel.
    void tick(int64_t now_ms);
    AutoState auto_state() const;

    // W2b: Board-Profil-Defaults (USB-Muster): das Profil kennt die
    // Stock-Belegung, der Store (UI) ueberschreibt. Nur die PINS werden
    // vorbelegt -- die irCut/backlight-FREIGABE bleibt eine bewusste
    // Nutzerentscheidung (kein ueberraschendes Klicken ab Werk).
    void set_default_pins(const std::string& ircut_pin1, const std::string& ircut_pin2,
                          const std::string& light_sensor_pin = "")
    { def_ircut_pin1_ = ircut_pin1; def_ircut_pin2_ = ircut_pin2;
      def_light_sensor_pin_ = light_sensor_pin; }

private:
    // `actuator_error`: the first IR-cut/light failure, separately - it
    // does not undo the mode (the return value), but the automation retries.
    std::string set_night_locked_(bool on, std::string* actuator_error = nullptr);
    std::string drive_ircut_(bool engaged);
    std::string drive_light_(bool on);
    // Pin-String (Nummer ODER Name) -> sysfs-Name fuer den GPIO-Aufruf.
    // Leer, wenn nicht aufloesbar.
    std::string pin_name_(const std::string& pin) const;
    int         pin_number_(const std::string& pin) const;   // -1 = leer/ungueltig

    media::TuningService& tuning_;
    IGpioController*      gpio_;
    ConfigStore&          store_;
    const hw::IPinResolver* resolver_ = nullptr;
    bool night_ = false;
    bool ircut_ = true;                   // Tag = Filter drin
    // Zuletzt tatsaechlich GEPULSTE Richtung der Zweipin-Spule (-1 = noch
    // nie). Ein Retry der Automatik darf eine bereits geschaltete Spule
    // nicht erneut pulsen: hoerbares Klicken, Verschleiss, 150 ms unter m_.
    int  ircut_driven_ = -1;
    bool light_ = false;
    std::string def_ircut_pin1_, def_ircut_pin2_;   // Board-Profil-Vorgaben
    std::string def_light_sensor_pin_;              // dto., Lichtsensor
    // Aktionen kommen aus dem HTTP-Thread (/night/*) UND aus der
    // Hauptschleife (tick): ein Mutex serialisiert beide.
    mutable std::mutex m_;
    struct Auto {
        std::string input_pin;            // als Eingang konfiguriert (sysfs-Name), leer = noch nicht
        int64_t     retry_at_ms = 0;      // nach einem Fehler nicht jede Runde neu exportieren
        int         committed = -1;       // ANGEWENDETER Zustand: 1 dunkel, 0 hell
        int         candidate = -1;       // Wechsel, der gerade reift (oder dessen Schalten fehlschlug)
        int64_t     candidate_since_ms = 0;
        int         raw = -1;
        int         pending_s = -1;
        bool        announced = false;    // "automatic on" einmal geloggt
        std::string error;                // Sensor: warum nicht gelesen wird
        std::string switch_error;         // Schalten: warum der letzte Versuch scheiterte (einmal geloggt)
        // Der MODUS steht, ein Aktor (Licht/Filter) fehlt noch: begrenzt
        // nachfassen statt die ganze Nacht alle 2 s -- ein dauerhaft
        // scheiternder Pin ist keine Endlosschleife wert.
        bool        actuator_pending = false;
        int         actuator_tries = 0;
    } auto_;
    std::thread             thread_;
    std::mutex              thread_m_;
    std::condition_variable thread_cv_;
    bool                    quit_ = false;
};

}} // namespace machino::night
