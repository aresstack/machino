// Der AIC8800DC-WLAN-Adapter als Geraetepaket -- die erste Klasse des
// Device Managers.
//
// Es kapselt genau die Wirtsregeln, an denen wir uns den Fehler vom
// 2026-09-25 eingefangen haben: OpenIPCs Netzwerkseite haelt einen Adapter
// fuer vorhanden, wenn seine Module als .ko unter /lib/modules liegen UND ein
// Profil in /etc/wireless/usb sie per modprobe nennt. Unsere Module lagen
// unter /etc/machino/modules und wurden per insmod geladen -- funktionierend,
// aber fuer das Wirtssystem unsichtbar.
//
// Lesen tut dieses Paket selbst. Schreiben NICHT: das Einrichten braucht
// depmod, und machinod darf bei lebendem IMP nicht forken (docs/evidence.md,
// OOM vom 2026-09-22). install()/uninstall() hinterlegen deshalb nur eine
// Absicht; ausgefuehrt wird sie vom externen Helfer beim naechsten Boot.
#pragma once

#include <string>

#include "core/devices/device_package.hpp"

namespace machino { namespace devices {

class Aic8800Package : public IDevicePackage {
public:
    // `root` ist der Praefix fuer alle Pfade -- leer in Produktion, ein
    // Wegwerfbaum im Test. Derselbe Haken wie MACHINO_ROOT im Installer.
    explicit Aic8800Package(std::string root = std::string());

    std::string id() const override { return "aic8800"; }
    std::string title() const override { return "AIC8800DC WLAN"; }

    bool is_supported() const override { return true; }
    bool is_available() const override;
    bool is_installed() const override;
    bool is_hardware_present() const override;
    bool is_active() const override;

    Result install() override;
    Result uninstall() override;
    DeviceStatus status() const override;

    // Das Profil, das der Installer in /etc/wireless/usb eintraegt. Oeffentlich,
    // weil Helfer und Tests denselben Namen meinen muessen wie der Installer --
    // ein zweiter Ort fuer diese Zeichenkette waere der naechste Fehler dieser
    // Art.
    static const char* profile_id() { return "aic8800-t40-machino"; }

private:
    std::string root_;

    std::string intent_path() const;
    std::string read_intent() const;
};

}} // namespace machino::devices
