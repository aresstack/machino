// Machino application layer: PerformanceService.
//
//   media configuration + profile
//           |
//           v
//   PerformanceService  (policy: validate, classify apply mode, keep
//           |            requested vs effective apart)
//           v
//   PipelineManager (single owner of start/stop/restart + live setters)
//   IPlatform / IEncoder / IPowerControl (adapter)
//
// The service is what the M6 API will expose; it never touches a vendor
// type. Profiles are presets over the same parameters, nothing hidden.
#pragma once
#include "core/capabilities.hpp"
#include "core/config.hpp"
#include "core/hw/resolve.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/power/apply.hpp"
#include "core/power/system_stats.hpp"
#include "core/telemetry.hpp"
#include "ports/iplatform.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace machino { namespace power {

struct EffectiveState {
    Profile   profile = Profile::Performance;
    int       sensor_fps_requested = 0;
    int       sensor_fps_effective = -1;      // -1 = not readable / pipeline cold
    bool      sensor_fps_readback = false;
    int       stream_fps = 0;
    int       bitrate_kbps = 0;
    PerfLevel isp = PerfLevel::Auto, encoder = PerfLevel::Auto, cpu = PerfLevel::Auto;
    ApplyMode sensor_fps_mode = ApplyMode::Unsupported;
    ApplyMode stream_fps_mode = ApplyMode::PipelineRestart;
    ApplyMode bitrate_mode = ApplyMode::Unsupported;
};

class PerformanceService {
public:
    PerformanceService(lifecycle::PipelineManager& pipeline, IPlatform& platform, ISystemStats& stats,
                       const hw::ResolvedHardware& hw, const StreamConfig& video);

    CapabilitySet  capabilities() const;
    EffectiveState effective_state();
    Telemetry      telemetry();

    // Presets resolved from the sensor's verified modes and the board profile.
    ApplyResult apply_profile(Profile p);
    ApplyResult set_sensor_fps(int fps);
    ApplyResult set_stream_fps(int fps);
    ApplyResult set_bitrate(int kbps);
    ApplyResult set_isp_performance(PerfLevel l);
    ApplyResult set_encoder_performance(PerfLevel l);
    ApplyResult set_cpu_performance(PerfLevel l);

    // Apply a (re)loaded configuration: only changed values are applied.
    std::vector<ApplyResult> apply_config(const PerformanceConfig& pc, const StreamConfig& video);

    // fps values a profile may choose from (verified sensor modes for the
    // current geometry, ascending) - empty when unknown
    std::vector<int> verified_fps() const;

private:
    ApplyResult apply_stream_restart(const EffectiveStream& s, const char* what, int requested);
    // Policy validation that touches nothing. Returns "" when the value would
    // be accepted, otherwise the reason. The setters and apply_profile share
    // these so the rules cannot drift apart.
    std::string check_stream_fps(int fps) const;
    std::string check_bitrate(int kbps) const;
    std::string check_sensor_fps(int fps) const;
    int  preset_fps(Profile p, std::string& note) const;
    int  preset_bitrate(Profile p) const;
    ApplyMode sensor_fps_mode() const;

    lifecycle::PipelineManager& pipeline_;
    IPlatform&      platform_;
    ISystemStats&   stats_;
    hw::ResolvedHardware hw_;
    CapabilitySet   caps_;
    std::mutex      m_;
    Profile         profile_ = Profile::Performance;
    int             sensor_fps_req_ = 0;
    int             bitrate_default_ = 0;
    PerfLevel       isp_ = PerfLevel::Auto, enc_ = PerfLevel::Auto, cpu_ = PerfLevel::Auto;
    bool            allow_unverified_ = false;
};

}} // namespace machino::power
