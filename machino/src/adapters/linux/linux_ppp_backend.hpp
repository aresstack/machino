// PPP unter Linux: Dateien schreiben, einen Wunsch absetzen, nachsehen.
//
// pppd STARTET DIESER PROZESS NICHT SELBST, und das ist dieselbe Regel wie
// ueberall sonst in machino: fork+exec bei laufender IMP-Pipeline ist auf
// dieser Kamera der dokumentierte Ausloeser eines Out-of-Memory-Vorfalls, und
// der OOM-Killer trifft den Mediendaemon, der /dev/watchdog haelt. Also
// schreibt machino eine Zeile nach /etc/machino/cellular-dhcp, und
// machino-cellular-helper -- ein paar hundert Kilobyte RSS -- fuehrt sie aus.
// Kein zweiter Prozessstarter, kein eigener Supervisor.
//
// WAS HIER ENTSTEHT
//
//   /etc/machino/ppp/options      pppd-Optionen, 0600 (enthaelt das Passwort)
//   /etc/machino/ppp/chat         die Wahl-Unterhaltung (chat(8))
//
// Das APN-Passwort steht in der Optionsdatei -- die ist 0600, und `password`
// ist von pppd ausdruecklich NUR dort erlaubt, nicht auf der Kommandozeile.
// Auf dieser Kamera laeuft alles als root und /proc ist lesbar; ein Passwort
// in argv steht in `ps` und im Log jedes Werkzeugs, das Prozesslisten
// aufnimmt. /etc/ppp/pap-secrets wird NICHT angefasst: den Pfad kann man bei
// pppd nicht verschieben, und die Datei gehoert dem System, nicht machino.
//
// WAS HIER NICHT ENTSTEHT
//
// Keine Default-Route (`nodefaultroute`) und kein Ueberschreiben von
// /etc/resolv.conf. Beides entscheidet core/net/route_plan fuer alle Uplinks
// zusammen -- ein pppd, der sich selbst zur Default-Route macht, nimmt dem
// Betreiber die Wahl und dem Failover die Grundlage.
#pragma once
#include "ports/ippp_backend.hpp"
#include <string>

namespace machino { namespace linuxsys {

class LinuxPppBackend : public cellular::IPppBackend {
public:
    // Alle Pfade sind Testhaken; in Produktion die Vorgaben.
    explicit LinuxPppBackend(std::string conf_dir = "/etc/machino/ppp",
                             std::string request_path = "/etc/machino/cellular-dhcp",
                             std::string status_path = "/var/run/machino-ppp.status",
                             std::string state_path = "/var/run/machino-cellular.state");

    Result start(const cellular::PppRequest& req) override;
    Result stop() override;
    bool   status(cellular::PppStatus& out) const override;

    // Der Exit-Code von pppd in Klartext. Oeffentlich, weil er die einzige
    // Stelle ist, an der "es geht nicht" zu "das Passwort stimmt nicht" wird --
    // und weil eine Tabelle, die nur im Feld gelesen wird, nie geprueft waere.
    static cellular::PppExit exit_from_code(int code);

private:
    bool write_file(const std::string& path, const std::string& text, int mode) const;

    std::string conf_dir_;
    std::string request_path_;
    std::string status_path_;
    std::string state_path_;
};

}} // namespace machino::linuxsys
