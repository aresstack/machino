// INnaProcess auf echtem Linux: fork/exec, zwei Pipes, waitpid.
//
// Nur im Target-Build uebersetzt (wie linux_ecm_backend): die Hosttests
// sprechen gegen eine Attrappe, dieser Adapter gegen den Kernel.
#pragma once
#include "ports/inna_process.hpp"
#include <string>

namespace machino { namespace linuxsys {

class LinuxNnaProcess final : public INnaProcess {
public:
    LinuxNnaProcess() = default;
    ~LinuxNnaProcess() override;

    bool spawn(const std::vector<std::string>& argv) override;
    bool alive() override;
    bool write_line(const std::string& line) override;
    bool read_line(std::string& out, int timeout_ms) override;
    void terminate() override;

private:
    void close_fds();

    pid_t pid_ = -1;
    int   in_fd_ = -1;    // unser Ende von stdin des Kindes
    int   out_fd_ = -1;   // unser Ende von stdout des Kindes
    std::string buf_;     // Zeilenpuffer fuer read_line
};

}} // namespace machino::linuxsys
