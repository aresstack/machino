// Das Geraetemanifest, von C++ aus gelesen.
//
// Dieselbe Datei, die auch machino-device und der Installer lesen
// (/etc/machino/devices/<id>.manifest, Quelle openipc/devices/<id>.manifest).
// Der Grund steht im Kopf des Manifests: der Profilname stand nach dem ersten
// Wurf sechsmal in vier Dateien, und wer eine Kopie uebersieht, bekommt keinen
// Fehler, sondern zwei stille Schaeden.
//
// Es gibt hier absichtlich KEINE einkompilierten Ersatzwerte. Ein Ersatzwert
// waere genau die zweite Quelle, die wir loswerden wollten: fehlt das Manifest,
// soll der Geraetemanager "kenne ich nicht" sagen und nicht aus dem Gedaechtnis
// etwas behaupten, das im Wirtssystem gar nicht eingetragen werden kann.
#pragma once

#include <string>
#include <vector>

namespace machino { namespace devices {

struct DeviceManifest {
    bool loaded = false;

    std::string id;
    std::string title;
    std::string driver;
    std::string openipc_profile;
    std::string usb_mode;
    std::vector<std::string> modules;          // Ladereihenfolge
    std::vector<std::string> payload_modules;  // die, die wir mitbringen
    std::string firmware_dir;
    std::string usb_vid;
    std::string usb_pid;

    // Liest <root>/etc/machino/devices/<id>.manifest. Schlaegt das fehl, kommt
    // ein Manifest mit loaded == false zurueck -- kein Wurf, keine Ausnahme.
    static DeviceManifest load(const std::string& root, const std::string& id);

    // Direkt eine Datei, fuer Tests und fuer Werkzeuge, die den Baum nicht
    // kennen.
    static DeviceManifest load_file(const std::string& path);
};

}} // namespace machino::devices
