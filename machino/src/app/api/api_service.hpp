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
#include "core/detection/detection_service.hpp"
#include "core/events.hpp"
#include "core/hw/resolve.hpp"
#include "core/json.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "ports/rtsp_control.hpp"
#include "core/media/tuning_service.hpp"
#include "core/power/performance_service.hpp"
#include <cstdint>
#include <mutex>
#include <string>
#include <functional>
#include <vector>

namespace machino { namespace api {

struct Response {
    int  status = 200;
    Json body;
};

class ApiService {
public:
    ApiService(power::PerformanceService& perf, media::TuningService& tuning, lifecycle::PipelineManager& pipeline, ConfigStore& store,
               EventBus& bus, const hw::ResolvedHardware& hw, const AppConfig& cfg,
               detection::DetectionService* detection = nullptr,
               IRtspControl* rtsp = nullptr);

    Response discovery() const;
    Response capabilities() const;
    Response state();
    Response config();
    Response telemetry();
    // Partial update. `if_match` = expected revision ("" = none). Serialised.
    Response patch_config(const std::string& body, const std::string& if_match);

    // Unset: the majestic-webui reset contract for schema fields WITHOUT a
    // default ("a key it declares none for is REMOVED, which is the unset
    // state", mj-settings.js #416). Validates every key first, persists the
    // removal, then applies the unconfigured state SYNCHRONOUSLY - a 200 means
    // the immediately following config.json read shows the reset. Serialised
    // with patch_config via patch_m_.
    Response unset_config(const std::vector<std::string>& conf_keys);

    // M8: one current frame as JPEG (binary, not JSON). Delegates to the
    // pipeline's snapshot path; the transport builds the image/jpeg response.
    // timeout_ms bounds the capture wait - streaming callers (MJPEG in the
    // single-threaded HTTP loop) pass a SHORT value and skip the tick on
    // Timeout, so a slow/never-arriving frame can not stall the server.
    Result snapshot(std::vector<uint8_t>& out, std::string& err, int timeout_ms = 5000);

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
    detection::DetectionService* detection_ = nullptr;   // M9: optional, null when no AI subsystem
    IRtspControl* rtsp_ = nullptr;                       // AP2: live rtsp.enabled/rtsp.port; null = not wired
    std::mutex                  patch_m_;          // PATCHes are serialised
};

}} // namespace machino::api
