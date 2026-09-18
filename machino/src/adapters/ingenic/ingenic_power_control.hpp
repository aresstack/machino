// Ingenic adapter: power / performance control for T40 on the OpenIPC kernel.
//
// Honest inventory (2026-09-18, OpenIPC 4.4.94, SDK 1.3.1, T40NN):
//   ISP clock      readable  (/proc/jz/clock/clocks: div_isp)        writable safely: NO  -> unsupported
//   encoder clock  readable  (/proc/jz/clock/clocks: div_el150)      writable safely: NO  -> unsupported
//   CPU frequency  cpufreq sysfs probed at runtime; absent -> unsupported; present -> only kernel-offered
//                  governors/frequencies are accepted and the effective value is read back
// No devmem, no register pokes, no /proc writes other than the cpufreq sysfs
// interface the kernel itself exposes.
#pragma once
#include "ports/ipower_control.hpp"
#include <string>
#include <vector>

namespace machino { namespace ingenic {

class IngenicPowerControl final : public IPowerControl {
public:
    IngenicPowerControl();
    void fill_capabilities(CapabilitySet& caps) const override;
    PowerState current_state() override;
    power::ApplyResult set_isp_performance(power::PerfLevel level) override;
    power::ApplyResult set_encoder_performance(power::PerfLevel level) override;
    power::ApplyResult set_cpu_performance(power::PerfLevel level) override;

    // exposed for the startup log
    bool cpufreq_present() const { return cpufreq_; }
    const std::vector<std::string>& governors() const { return governors_; }
    const std::vector<uint64_t>& frequencies_khz() const { return freqs_khz_; }

private:
    bool read_clock(const char* name, uint64_t& hz);
    bool cpufreq_ = false;
    std::vector<std::string> governors_;
    std::vector<uint64_t>    freqs_khz_;
};

}} // namespace machino::ingenic
