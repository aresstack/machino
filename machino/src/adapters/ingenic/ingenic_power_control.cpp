#include "adapters/ingenic/ingenic_power_control.hpp"
#include "core/log.hpp"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace machino { namespace ingenic {

static const char* MOD = "ING_PWR";
static const char* CPUFREQ = "/sys/devices/system/cpu/cpu0/cpufreq";

static bool read_file(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "r"); if (!f) return false;
    char buf[512]; out.clear();
    while (fgets(buf, sizeof buf, f)) out += buf;
    fclose(f); return true;
}

static std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> v; std::string cur;
    for (char c : s) { if (c == ' ' || c == '\n' || c == '\t') { if (!cur.empty()) { v.push_back(cur); cur.clear(); } } else cur += c; }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

IngenicPowerControl::IngenicPowerControl() {
    struct stat st;
    cpufreq_ = (stat((std::string(CPUFREQ) + "/scaling_cur_freq").c_str(), &st) == 0);
    if (cpufreq_) {
        std::string s;
        if (read_file(std::string(CPUFREQ) + "/scaling_available_governors", s)) governors_ = split_ws(s);
        if (read_file(std::string(CPUFREQ) + "/scaling_available_frequencies", s))
            for (auto& x : split_ws(s)) freqs_khz_.push_back(strtoull(x.c_str(), nullptr, 10));
        LOGI(MOD, "cpufreq present: %zu governors, %zu frequencies", governors_.size(), freqs_khz_.size());
    } else {
        LOGI(MOD, "cpufreq not exposed by this kernel - cpu frequency control unsupported");
    }
}

// /proc/jz/clock/clocks lines: " ID  NAME   FRE(MHz)   sta  count  parent"
bool IngenicPowerControl::read_clock(const char* name, uint64_t& hz) {
    std::string s; if (!read_file("/proc/jz/clock/clocks", s)) return false;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos); std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? s.size() : nl + 1;
        std::vector<std::string> f = split_ws(line);
        if (f.size() >= 3 && f[1] == name) {
            double mhz = atof(f[2].c_str());          // "366.666MHz"
            if (mhz > 0) { hz = (uint64_t)(mhz * 1e6); return true; }
        }
    }
    return false;
}

void IngenicPowerControl::fill_capabilities(CapabilitySet& caps) const {
    uint64_t hz;
    IngenicPowerControl* self = const_cast<IngenicPowerControl*>(this);
    bool isp_r = self->read_clock("div_isp", hz), enc_r = self->read_clock("div_el150", hz);
    caps.isp.performance     = PerfCap{Cap::Unsupported, ApplyMode::Unsupported, isp_r};   // no safe runtime API
    caps.encoder.performance = PerfCap{Cap::Unsupported, ApplyMode::Unsupported, enc_r};
    caps.power.isp_clock_control     = Cap::Unsupported;
    caps.power.encoder_clock_control = Cap::Unsupported;
    if (cpufreq_) { caps.power.cpu_frequency = PerfCap{Cap::Supported, ApplyMode::Live, true}; caps.power.cpu_frequency_control = Cap::Supported; }
    else          { caps.power.cpu_frequency = PerfCap{Cap::Unsupported, ApplyMode::Unsupported, false}; caps.power.cpu_frequency_control = Cap::Unsupported; }
}

PowerState IngenicPowerControl::current_state() {
    PowerState p; uint64_t hz;
    if (read_clock("div_isp", hz))   { p.isp_clock.available = true;     p.isp_clock.hz = hz; }
    if (read_clock("div_el150", hz)) { p.encoder_clock.available = true; p.encoder_clock.hz = hz; }
    if (cpufreq_) {
        std::string s;
        if (read_file(std::string(CPUFREQ) + "/scaling_cur_freq", s)) { p.cpu_clock.available = true; p.cpu_clock.hz = strtoull(s.c_str(), nullptr, 10) * 1000ULL; }
        if (read_file(std::string(CPUFREQ) + "/scaling_governor", s)) { p.cpu_governor = split_ws(s).empty() ? "" : split_ws(s)[0]; }
    } else if (read_clock("div_cpu", hz)) { p.cpu_clock.available = true; p.cpu_clock.hz = hz; }   // read-only fallback
    return p;
}

power::ApplyResult IngenicPowerControl::set_isp_performance(power::PerfLevel level) {
    if (level == power::PerfLevel::Auto) return power::ApplyResult::applied(ApplyMode::Live, (int)level, (int)level, "auto = kernel default (no change)");
    return power::ApplyResult::rejected(ApplyMode::Unsupported, (int)level, "ISP clock has no safe runtime interface on this kernel (read-only via /proc/jz/clock)");
}

power::ApplyResult IngenicPowerControl::set_encoder_performance(power::PerfLevel level) {
    if (level == power::PerfLevel::Auto) return power::ApplyResult::applied(ApplyMode::Live, (int)level, (int)level, "auto = kernel default (no change)");
    return power::ApplyResult::rejected(ApplyMode::Unsupported, (int)level, "encoder (EL150/AVPU) clock has no safe runtime interface on this kernel");
}

// Only what the kernel offers: governor names from scaling_available_governors.
power::ApplyResult IngenicPowerControl::set_cpu_performance(power::PerfLevel level) {
    if (!cpufreq_) return power::ApplyResult::rejected(ApplyMode::Unsupported, (int)level, "cpufreq not exposed by this kernel");
    if (level == power::PerfLevel::Auto) return power::ApplyResult::applied(ApplyMode::Live, (int)level, (int)level, "auto = kernel default governor");
    const char* want = level == power::PerfLevel::Low ? "powersave" : "performance";
    bool offered = false; for (auto& g : governors_) if (g == want) offered = true;
    if (!offered) return power::ApplyResult::rejected(ApplyMode::Unsupported, (int)level, std::string("governor '") + want + "' not offered by the kernel");
    FILE* f = fopen((std::string(CPUFREQ) + "/scaling_governor").c_str(), "w");
    if (!f) return power::ApplyResult::rejected(ApplyMode::Live, (int)level, "cannot write scaling_governor");
    fputs(want, f); fclose(f);
    std::string s; read_file(std::string(CPUFREQ) + "/scaling_governor", s);
    bool eff = split_ws(s).size() && split_ws(s)[0] == want;
    return eff ? power::ApplyResult::applied(ApplyMode::Live, (int)level, (int)level, "governor read back")
               : power::ApplyResult::rejected(ApplyMode::Live, (int)level, "governor did not take effect");
}

}} // namespace machino::ingenic
