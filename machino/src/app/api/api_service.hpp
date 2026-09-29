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
#include "core/audio/audio_service.hpp"
#include "core/detection/detection_service.hpp"
#include "core/detection/detector_availability.hpp"
#include "core/events.hpp"
#include "core/hw/resolve.hpp"
#include "core/json.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "ports/rtsp_control.hpp"
#include "core/media/tuning_service.hpp"
#include "core/net/ipsec_service.hpp"
#include "core/night/night_service.hpp"
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
    // webui.dashboard_preview as of now (store first, then the loaded config):
    // the front door reads it per relayed dashboard page.
    std::string dashboard_preview() const;
    Response telemetry();
    // AP-NNA5: der Availability-Vertrag als API. Provider kommt aus main
    // (die Plattform kennt die Fakten); ohne Provider antwortet die Route
    // ehrlich mit dem, was die Capabilities hergeben (nur motion).
    void set_detector_status_provider(
        std::function<std::vector<detection::DetectorStatus>(const std::string&)> f)
    { det_status_ = std::move(f); }
    Response ai_detectors();

    // AP3 (Feature 2): IPsec/VPN. Der Service kommt aus main (null = das
    // Feature ist auf dieser Plattform nicht verdrahtet -> 404, ehrlich).
    // Der PSK ist write-only: PUT nimmt ihn an, KEINE Route gibt ihn zurueck,
    // er erscheint in keiner Fehlermeldung.
    void set_ipsec_service(ipsec::IpsecService* s) { ipsec_ = s; }

    // W2 (Day/Night): die majestic-/night/*-Flaeche. cmd = on|off|toggle|
    // ircut|light; Antwort ist das nackte JSON-Boolean des neuen Zustands
    // (so liest es die Stock-Seite). night_metric liefert die "0"/"1"-
    // Textwerte fuer /metrics/night?value=... Ohne verdrahteten Service 404.
    void set_night_service(night::NightService* s) { night_ = s; }
    // Audio: config section `audio`, telemetry `audio`. Null = not wired;
    // the section is then absent and a PATCH to it says why.
    void set_audio_service(audio::AudioService* a) { audio_ = a; }
    Response night_action(const std::string& cmd);
    bool night_metric(const std::string& value_name, std::string& out);
    // W4: GET /api/v1/gpio -- die Pin-Landkarte, aus der die Day/Night-Seite
    // ihren Chip zeichnet (Baenke, gehaltene Pins, Rollen-Zuordnung).
    Response gpio_map();
    Response audio_tone(const std::string& body);      // POST /api/v1/audio/tone
    Response audio_monitor(const std::string& body);   // POST /api/v1/audio/monitor
    Response ipsec_get();                              // GET  /api/v1/ipsec
    Response ipsec_put_config(const std::string& body);// PUT  /api/v1/ipsec/config
    Response ipsec_connect();                          // POST /api/v1/ipsec/connect
    Response ipsec_disconnect();                       // POST /api/v1/ipsec/disconnect
    Response ipsec_rekey(bool ike_sa);                 // POST /api/v1/ipsec/rekey | rekey-ike (AP11)
    Response ipsec_status();                           // GET  /api/v1/ipsec/status
    // Partial update. `if_match` = expected revision ("" = none). Serialised.
    Response patch_config(const std::string& body, const std::string& if_match);

    // Unset: the majestic-webui reset contract for schema fields WITHOUT a
    // default ("a key it declares none for is REMOVED, which is the unset
    // state", mj-settings.js #416). Validates every key first, persists the
    // removal, then applies the unconfigured state SYNCHRONOUSLY - a 200 means
    // the immediately following config.json read shows the reset. Serialised
    // with patch_config via patch_m_.
    Response unset_config(const std::vector<std::string>& conf_keys);

    // AP10: the live image preview the stock settings page drives while a
    // slider is being dragged.
    //
    //   POST /api/v1/image?brightness=128&contrast=100&hflip=1
    //
    // The contract is read off mj-settings.js, not invented: leaf names only
    // (`f.dot.split('.').pop()`), EVERY live field sent together on every push
    // - "sending them together is what lets the backend apply combined
    // settings (mirror and flip need each other)" - and the reply is only ever
    // inspected for `r.ok`.
    //
    // NOTHING IS PERSISTED. The page saves separately; this is the preview,
    // and a drag that wrote config would put every intermediate slider
    // position on flash. It is also why this is not patch_config with a flag:
    // two paths that must never share a persistence decision.
    Response live_image(const std::string& query);

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
    IRtspControl* rtsp_ = nullptr;                 // AP2: live rtsp.enabled/rtsp.port; null = not wired
    std::function<std::vector<detection::DetectorStatus>(const std::string&)> det_status_;
    ipsec::IpsecService*        ipsec_ = nullptr;  // AP3: optional, null = nicht verdrahtet
    night::NightService*        night_ = nullptr;  // W2: optional, null = nicht verdrahtet
    audio::AudioService*        audio_ = nullptr;
    std::mutex                  patch_m_;          // PATCHes are serialised
};

}} // namespace machino::api
