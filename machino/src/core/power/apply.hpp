// Machino core: apply semantics for runtime settings.
// A setter never silently ignores a request: it either applies it (and
// reports the EFFECTIVE value, which may differ from the requested one) or
// rejects it with a reason.
#pragma once
#include "core/capabilities.hpp"
#include <string>

namespace machino { namespace power {

enum class PerfLevel : int { Auto = 0, Low, High };
inline const char* perf_level_name(PerfLevel l) {
    switch (l) { case PerfLevel::Auto: return "auto"; case PerfLevel::Low: return "low"; case PerfLevel::High: return "high"; }
    return "?";
}
inline bool parse_perf_level(const std::string& s, PerfLevel& out) {
    if (s == "auto") { out = PerfLevel::Auto; return true; }
    if (s == "low")  { out = PerfLevel::Low;  return true; }
    if (s == "high") { out = PerfLevel::High; return true; }
    return false;
}

enum class Profile : int { Performance = 0, Balanced, Battery, Custom };
inline const char* profile_name(Profile p) {
    switch (p) {
        case Profile::Performance: return "performance";
        case Profile::Balanced:    return "balanced";
        case Profile::Battery:     return "battery";
        case Profile::Custom:      return "custom";
    }
    return "?";
}
inline bool parse_profile(const std::string& s, Profile& out) {
    if (s == "performance") { out = Profile::Performance; return true; }
    if (s == "balanced")    { out = Profile::Balanced;    return true; }
    if (s == "battery")     { out = Profile::Battery;     return true; }
    if (s == "custom")      { out = Profile::Custom;      return true; }
    return false;
}

struct ApplyResult {
    bool        ok = false;
    ApplyMode   mode = ApplyMode::Unsupported;   // how it was (or would be) applied
    int         requested = -1;
    int         effective = -1;                  // value actually in effect after the call
    bool        deferred = false;                // pipeline cold: stored, takes effect at next start
    std::string message;

    static ApplyResult applied(ApplyMode m, int req, int eff, const char* msg = "") { ApplyResult r; r.ok = true; r.mode = m; r.requested = req; r.effective = eff; r.message = msg; return r; }
    static ApplyResult stored(ApplyMode m, int req, const char* msg = "stored; takes effect at next pipeline start") { ApplyResult r; r.ok = true; r.mode = m; r.requested = req; r.effective = req; r.deferred = true; r.message = msg; return r; }
    static ApplyResult rejected(ApplyMode m, int req, const std::string& msg) { ApplyResult r; r.ok = false; r.mode = m; r.requested = req; r.message = msg; return r; }
};

}} // namespace machino::power
