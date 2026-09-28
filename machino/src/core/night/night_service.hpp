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

#include <string>
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
};

class NightService {
public:
    // gpio darf null sein (Plattform ohne GPIO): Filter/Licht sind dann
    // ehrlich unsupported, der Nachtmodus (ISP) geht trotzdem. resolver
    // uebersetzt GPIO-Nummer<->Name (die WebUI-Pin-Karte spricht Nummern,
    // sysfs auch; die Config speichert Nummern).
    NightService(media::TuningService& tuning, IGpioController* gpio, ConfigStore& store,
                 const hw::IPinResolver* resolver = nullptr);

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
    bool night() const { return night_; }
    bool ircut() const { return ircut_; }
    bool light() const { return light_; }

    // Aktionen. Rueckgabe leer = ok (neuer Zustand via Getter); sonst der
    // konkrete Grund (kein Pin, GPIO belegt, ...). set_night zieht Filter
    // und Licht mit, WENN sie in der Config freigegeben sind -- die Seite
    // fragt danach ohnehin alle drei Zustaende neu ab.
    std::string set_night(bool on);
    std::string toggle_night(bool& out) { const std::string e = set_night(!night_); out = night_; return e; }
    std::string toggle_ircut();
    std::string toggle_light();

    NightPins pins() const;               // live aus dem ConfigStore

    // W2b: Board-Profil-Defaults (USB-Muster): das Profil kennt die
    // Stock-Belegung, der Store (UI) ueberschreibt. Nur die PINS werden
    // vorbelegt -- die irCut/backlight-FREIGABE bleibt eine bewusste
    // Nutzerentscheidung (kein ueberraschendes Klicken ab Werk).
    void set_default_pins(const std::string& ircut_pin1, const std::string& ircut_pin2,
                          const std::string& light_sensor_pin = "")
    { def_ircut_pin1_ = ircut_pin1; def_ircut_pin2_ = ircut_pin2;
      def_light_sensor_pin_ = light_sensor_pin; }

private:
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
    bool light_ = false;
    std::string def_ircut_pin1_, def_ircut_pin2_;   // Board-Profil-Vorgaben
    std::string def_light_sensor_pin_;              // dto., Lichtsensor
};

}} // namespace machino::night
