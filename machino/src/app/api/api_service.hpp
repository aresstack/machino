// Application: the stable /api/v1 surface, transport-independent.
//
//   WebUI -> HTTP -> ApiService -> PerformanceService / PipelineManager / ConfigStore / EventBus
//
// Builds JSON documents (capabilities, state, config, telemetry) and applies
// partial config patches exclusively through the services. No vendor calls,
// no platform names in logic; everything comes from the capability model and
// the resolved hardware description. Reads never create media demand.
#pragma once
#include "core/config.hpp"
#include "core/config_store.hpp"
#include "core/events.hpp"
#include "core/hw/resolve.hpp"
#include "core/json.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/media/tuning_service.hpp"
#include "core/power/performance_service.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace machino { namespace api {

struct Response {
    int  status = 200;
    Json body;
};

class ApiService {
public:
    ApiService(power::PerformanceService& perf, media::TuningService& tuning, lifecycle::PipelineManager& pipeline, ConfigStore& store,
               EventBus& bus, const hw::ResolvedHardware& hw, const AppConfig& cfg);

    Response discovery() const;
    Response capabilities() const;
    Response state();
    Response config();
    Response telemetry();
    // Partial update. `if_match` = expected revision ("" = none). Serialised.
    Response patch_config(const std::string& body, const std::string& if_match);

    // M8: one current frame as JPEG (binary, not JSON). Delegates to the
    // pipeline's snapshot path; the transport builds the image/jpeg response.
    Result snapshot(std::vector<uint8_t>& out, std::string& err);

    Json telemetry_json();                         // also used for SSE telemetry events
    static Json error(const char* code, const std::string& path, const std::string& message);
    static Response fail(int status, const char* code, const std::string& path, const std::string& message);

private:
    Json capabilities_json() const;
    Json state_json();
    Json config_json();

    power::PerformanceService&  perf_;
    media::TuningService&       tuning_;
    lifecycle::PipelineManager& pipeline_;
    ConfigStore&                store_;
    EventBus&                   bus_;
    hw::ResolvedHardware        hw_;
    AppConfig                   cfg_;              // startup snapshot (for non-runtime keys)
    std::mutex                  patch_m_;          // PATCHes are serialised
};

}} // namespace machino::api
