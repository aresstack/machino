// Die Linux-Seite des ECM-Datenpfads.
//
// Duenn, und das ist der Punkt: der Kernel macht das, wofuer das ESP32-Projekt
// eine eigene Bulk-Pumpe und ein eigenes netif brauchte. Hier bleibt uebrig,
// ein Interface zu finden, es hochzunehmen und eine Adresse zu besorgen.
//
// DHCP startet dieser Prozess NICHT selbst. machino darf bei laufender
// IMP-Pipeline nicht fork+exec -- das ist auf dieser Kamera der dokumentierte
// OOM-Ausloeser. Stattdessen schreibt er einen Wunsch in eine Datei, und der
// kleine externe Helfer (openipc/sbin/machino-cellular-helper) fuehrt ihn aus.
// Dasselbe Muster wie beim WLAN-Rollen-Supervisor, aus demselben Grund.
#pragma once
#include "ports/iecm_backend.hpp"
#include <string>

namespace machino { namespace linuxsys {

class LinuxEcmBackend : public IEcmBackend {
public:
    // sys_root und die beiden Pfade sind Testhaken.
    explicit LinuxEcmBackend(std::string sys_root = "/sys",
                             std::string request_path = "/etc/machino/cellular-dhcp",
                             std::string state_path = "/var/run/machino-cellular.state")
        : sys_root_(std::move(sys_root)),
          request_path_(std::move(request_path)),
          state_path_(std::move(state_path)) {}

    void set_vid_pid(std::string vid, std::string pid)
    {
        vid_ = std::move(vid); pid_ = std::move(pid);
    }

    bool find_interface(EcmInterface& out) override;
    bool set_up(const std::string& ifname, bool up) override;
    bool dhcp_start(const std::string& ifname) override;
    bool dhcp_stop(const std::string& ifname) override;
    bool read_address(const std::string& ifname, LinkAddress& out) override;
    bool set_address(const std::string& ifname, const LinkAddress& a) override;
    void teardown(const std::string& ifname) override;

private:
    bool write_request(const std::string& line);

    std::string sys_root_;
    std::string request_path_;
    std::string state_path_;
    std::string vid_ = "2c7c";
    std::string pid_ = "6005";
};

}} // namespace machino::linuxsys
