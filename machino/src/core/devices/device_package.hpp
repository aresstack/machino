// USB-Zusatzgeraete als Paket: verfuegbar -> installiert -> aktiv.
//
// Warum drei Zustaende und nicht zwei: genau ihre Vermischung hat uns einen
// Tag gekostet. Der AIC8800-Treiber war gebaut, ausgeliefert und auf dieser
// Kamera nachweislich gelaufen -- trotzdem bot OpenIPCs Netzwerkseite nur
// "None" an. "Installiert" hiess bei uns "liegt in unserem eigenen
// Verzeichnis", waehrend OpenIPC unter "installiert" versteht: als .ko unter
// /lib/modules auffindbar und in /etc/wireless registriert. Zwei Bedeutungen
// desselben Wortes, und niemandem faellt es auf, bis die Oberflaeche leer
// bleibt.
//
//   Available    Machino kennt den Geraetetyp UND das Release traegt die
//                Nutzlast (Module/Firmware) mit sich. Ohne Nutzlast waere
//                "installieren" ein Knopf, der nur scheitern kann.
//   Installed    Treiber und OpenIPC-Kompatibilitaetsprofil sind auf der
//                Kamera eingerichtet -- so, dass die UNVERAENDERTE
//                OpenIPC-Seite sie sieht.
//   Active       Dieser Geraetetyp hat den gemeinsamen USB-Port (usb.mode).
//                Es gibt genau einen Port, also hoechstens ein aktives Geraet.
//
// Installieren aktiviert NICHT. Beides gleichzeitig zu tun war die zweite
// Falle: WLAN und Modem duerfen zusammen installiert sein, aber nur eines
// bekommt den Port.
//
// Der Device Manager ersetzt die OpenIPC-Funktionalitaet nicht. Er sorgt
// dafuer, dass die Hardwareunterstuetzung eingerichtet ist; bedient wird der
// Adapter danach weiter ueber OpenIPCs eigene Netzwerkseite.
#pragma once

#include <string>
#include <vector>

#include "core/result.hpp"

namespace machino { namespace devices {

enum class InstallState {
    Unsupported,  // dieses Release kennt den Typ nicht
    Unavailable,  // gekannt, aber ohne Nutzlast im Release -- nichts zu installieren
    NotInstalled, // Nutzlast da, auf der Kamera nicht eingerichtet
    // Vorgemerkt, aber noch nicht eingerichtet -- wirksam beim naechsten Boot.
    //
    // Dieser Zustand ist keine Bequemlichkeit, er folgt aus einer harten Regel
    // dieses Daemons: fork() bei lebendem IMP ist der belegte Ausloeser des
    // OOM vom 2026-09-22 (docs/evidence.md, A/B-Nachweis). machinod ruft
    // deshalb zur Laufzeit kein depmod und kein modprobe auf. Das Einrichten
    // erledigt der externe Helfer beim Boot -- so, wie ohnehin jeder
    // Modulwechsel und jede Portumbelegung einen Neustart braucht. Die
    // Oberflaeche muss das sagen, statt "installiert" zu behaupten und den
    // Adapter danach nicht anzubieten.
    InstallPending,
    Installed,    // Treiber + OpenIPC-Profil eingerichtet
    // Gegenstueck: Rueckbau vorgemerkt, wirksam beim naechsten Boot.
    RemovePending,
};

const char* install_state_name(InstallState s);

// Was die Oberflaeche zeigt. Bewusst getrennte Felder statt eines Textes:
// "Treiber installiert: ja / geladen: nein" beantwortet eine andere Frage als
// "aktiv", und beides in einen Satz zu pressen war schon einmal der Fehler.
struct DeviceStatus {
    std::string  id;             // stabil, z.B. "aic8800"
    std::string  title;          // "AIC8800DC WLAN"
    std::string  driver;         // "aic8800" bzw. "option / usb_wwan / cdc_ether"
    InstallState state = InstallState::Unsupported;
    bool openipc_registered = false; // Profil in /etc/wireless vorhanden
    bool hardware_present   = false; // per USB-ID gesehen (nicht: Treiber gebunden)
    bool driver_loaded      = false; // Modul im Kernel
    bool active             = false; // haelt gerade den USB-Port (usb.mode)
    std::string detail;              // ein Satz Klartext, wenn etwas klemmt
};

// Ein Geraetetyp. Jede Implementierung kapselt GENAU ein Geraet; die Regeln
// des Wirtssystems (wo Module hingehoeren, wie ein Profil aussieht) stehen in
// der Implementierung, nicht im Manager.
class IDevicePackage {
public:
    virtual ~IDevicePackage() = default;

    virtual std::string id() const = 0;
    virtual std::string title() const = 0;

    // Kennt dieses Release den Typ ueberhaupt? Falsch heisst: nicht anbieten.
    virtual bool is_supported() const = 0;
    // Liegt die Nutzlast bereit (Module/Firmware im Release)?
    virtual bool is_available() const = 0;
    // Ist sie auf der Kamera eingerichtet -- aus Sicht des WIRTSSYSTEMS,
    // nicht aus Sicht unserer eigenen Verzeichnisse.
    virtual bool is_installed() const = 0;
    // Steckt die Hardware? Getrennt von allem anderen: ein Treiber kann
    // eingerichtet sein, ohne dass jemand den Stick eingesteckt hat.
    virtual bool is_hardware_present() const = 0;
    // Haelt dieses Geraet den gemeinsamen Port?
    virtual bool is_active() const = 0;

    // Einrichten bzw. entfernen. Beides idempotent: zweimal installieren ist
    // kein Fehler, und deinstallieren, was nicht da ist, auch nicht.
    // install() aktiviert NICHT.
    virtual Result install() = 0;
    virtual Result uninstall() = 0;

    virtual DeviceStatus status() const = 0;
};

// Haelt die Pakete und beantwortet die Portfrage. Mehr nicht -- die Arbeit
// steckt in den Paketen.
class DeviceManager {
public:
    void add(IDevicePackage* p);             // Eigentum bleibt beim Aufrufer
    std::vector<DeviceStatus> list() const;
    IDevicePackage* find(const std::string& id) const;

    // Es gibt einen Port. Liefert das Paket, das ihn haelt, sonst nullptr.
    IDevicePackage* active() const;

private:
    std::vector<IDevicePackage*> packages_;
};

}} // namespace machino::devices
