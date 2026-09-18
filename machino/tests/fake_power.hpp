// Fakes for the performance tests: a power-control adapter with configurable
// capabilities and a process-stats source that can be "unavailable".
#pragma once
#include "core/power/system_stats.hpp"
#include "ports/ipower_control.hpp"
#include <string>
#include <vector>

namespace machino { namespace test {

class FakePowerControl final : public IPowerControl {
public:
    Cap  cpu_support = Cap::Unsupported;     // what the "kernel" offers
    bool clocks_readable = true;
    std::vector<std::string> calls;
    power::PerfLevel cpu_level = power::PerfLevel::Auto;

    void fill_capabilities(CapabilitySet& c) const override {
        c.isp.performance     = PerfCap{Cap::Unsupported, ApplyMode::Unsupported, clocks_readable};
        c.encoder.performance = PerfCap{Cap::Unsupported, ApplyMode::Unsupported, clocks_readable};
        c.power.cpu_frequency = PerfCap{cpu_support, cpu_support == Cap::Supported ? ApplyMode::Live : ApplyMode::Unsupported, cpu_support == Cap::Supported};
        c.power.isp_clock_control = Cap::Unsupported; c.power.encoder_clock_control = Cap::Unsupported;
        c.power.cpu_frequency_control = cpu_support;
    }
    PowerState current_state() override {
        PowerState p;
        if (clocks_readable) { p.isp_clock = {true, 366666000ULL}; p.encoder_clock = {true, 550000000ULL}; }
        if (cpu_support == Cap::Supported) p.cpu_clock = {true, 912000000ULL};
        return p;
    }
    power::ApplyResult set_isp_performance(power::PerfLevel l) override {
        calls.push_back("isp");
        if (l == power::PerfLevel::Auto) return power::ApplyResult::applied(ApplyMode::Live, (int)l, (int)l, "auto");
        return power::ApplyResult::rejected(ApplyMode::Unsupported, (int)l, "no safe interface");
    }
    power::ApplyResult set_encoder_performance(power::PerfLevel l) override {
        calls.push_back("encoder");
        if (l == power::PerfLevel::Auto) return power::ApplyResult::applied(ApplyMode::Live, (int)l, (int)l, "auto");
        return power::ApplyResult::rejected(ApplyMode::Unsupported, (int)l, "no safe interface");
    }
    power::ApplyResult set_cpu_performance(power::PerfLevel l) override {
        calls.push_back("cpu");
        if (cpu_support != Cap::Supported) return power::ApplyResult::rejected(ApplyMode::Unsupported, (int)l, "cpufreq absent");
        cpu_level = l; return power::ApplyResult::applied(ApplyMode::Live, (int)l, (int)l, "governor read back");
    }
};

class FakeStats final : public power::ISystemStats {
public:
    bool available = true;
    power::ProcessStats sample() override {
        power::ProcessStats s; s.available = available; s.rss_kb = 2048; s.threads = 3; s.cpu_percent = 12.5; return s;
    }
};

}} // namespace machino::test
