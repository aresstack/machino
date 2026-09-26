// LinuxIpsecBackend: die reale Naht zu weirdiked. Prozess-Lifecycle geht
// ueber das Init-Skript (S99weirdike -- dieselbe Wahrheit wie beim Boot),
// Status ueber den Control-Socket, mit exakt dem weirdikectl-Protokoll
// (Kommando schreiben, SHUT_WR, Antwort lesen). Kein Shell-Aufruf, kein
// popen: fork/exec mit argv.
#pragma once
#include "core/net/ipsec_service.hpp"

#include <string>

namespace machino { namespace ipsec {

class LinuxIpsecBackend : public IIpsecBackend {
public:
    LinuxIpsecBackend(std::string init_script = "/etc/init.d/S99weirdike",
                      std::string ctl_socket = "/var/run/weirdike.sock");

    bool daemon_running() override;
    bool start_daemon(std::string& err) override;
    bool stop_daemon(std::string& err) override;
    bool ctl_status(std::string& out) override;

    // AP5: getaddrinfo (einmal, VOR dem Tunnel) und die Peer-Hostroute
    // (SIOCADDRT/SIOCDELRT, /32 via Gateway oder Device -- kein Shell).
    bool resolve4(const std::string& host, std::string& ip_out) override;
    bool add_peer_route(const std::string& peer_ip, const std::string& ifname,
                        const std::string& gateway_ip, std::string& err) override;
    bool del_peer_route(const std::string& peer_ip, const std::string& ifname,
                        const std::string& gateway_ip, std::string& err) override;

private:
    bool peer_route(const std::string& peer_ip, const std::string& ifname,
                    const std::string& gateway_ip, bool add, std::string& err);
    bool run_init(const char* verb, std::string& err);
    bool ctl_command(const char* cmd, std::string& out);

    std::string init_script_, ctl_socket_;
};

}} // namespace machino::ipsec
