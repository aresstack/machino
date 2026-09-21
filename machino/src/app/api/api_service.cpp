#include "app/api/api_service.hpp"
#include "core/log.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace machino { namespace api {

static const char* MOD = "API";
using power::ApplyResult; using power::Profile; using power::PerfLevel;

static const char* lifecycle_name_lc(lifecycle::State s) {
    switch (s) {
        case lifecycle::State::ColdIdle:  return "cold_idle";
        case lifecycle::State::Starting:  return "starting";
        case lifecycle::State::Active:    return "active";
        case lifecycle::State::GraceIdle: return "grace_idle";
        case lifecycle::State::Stopping:  return "stopping";
        case lifecycle::State::Failed:    return "failed";
    }
    return "?";
}
static const char* apply_api_name(ApplyMode m) {
    switch (m) {
        case ApplyMode::Live:            return "live";
        case ApplyMode::PipelineRestart: return "pipeline_restart";
        case ApplyMode::DaemonRestart:   return "daemon_restart";
        case ApplyMode::BootOnly:        return "boot_only";
        case ApplyMode::Unsupported:     return "unsupported";
    }
    return "?";
}
static int64_t now_ms_realtime() { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

template <typename T> static Json opt(const Optional<T>& o) { return o.available ? Json::number((double)o.value) : Json::null(); }

ApiService::ApiService(power::PerformanceService& perf, media::TuningService& tuning, lifecycle::PipelineManager& pipeline, ConfigStore& store,
                       EventBus& bus, const hw::ResolvedHardware& hw, const AppConfig& cfg, detection::DetectionService* detection)
    : perf_(perf), tuning_(tuning), pipeline_(pipeline), store_(store), bus_(bus), hw_(hw), cfg_(cfg), detection_(detection) {}

Json ApiService::error(const char* code, const std::string& path, const std::string& message) {
    Json e = Json::object(); e.set("code", Json::string(code));
    if (!path.empty()) e.set("path", Json::string(path));
    e.set("message", Json::string(message));
    Json r = Json::object(); r.set("ok", Json::boolean(false)); r.set("error", e);
    return r;
}
Response ApiService::fail(int status, const char* code, const std::string& path, const std::string& message) {
    return Response{status, error(code, path, message)};
}

// ---- GET documents ----------------------------------------------------------
Response ApiService::discovery() const {
    Json j = Json::object();
    j.set("name", Json::string("Machino")); j.set("version", Json::string(MACHINO_VERSION)); j.set("api", Json::integer(1));
    Json e = Json::object();
    for (const char* n : {"capabilities", "state", "config", "telemetry", "events"}) e.set(n, Json::string(std::string("/api/v1/") + n));
    j.set("endpoints", e);
    return Response{200, j};
}

static Json range_control(const RangeCap& c) {
    Json j = Json::object();
    j.set("status", Json::string(cap_name(c.support)));
    j.set("apply", Json::string(apply_api_name(c.support == Cap::Supported ? c.apply : ApplyMode::Unsupported)));
    if (c.min >= 0) j.set("min", Json::integer(c.min));     // absent when unknown - never invented
    if (c.max >= 0) j.set("max", Json::integer(c.max));
    return j;
}
static Json perf_control(const PerfCap& c) {
    Json j = Json::object();
    j.set("status", Json::string(cap_name(c.support)));
    j.set("apply", Json::string(apply_api_name(c.support == Cap::Supported ? c.apply : ApplyMode::Unsupported)));
    j.set("readable", Json::boolean(c.readable));
    return j;
}

Json ApiService::capabilities_json() const {
    CapabilitySet c = perf_.capabilities();
    ImageCaps image_caps = tuning_.image_caps();
    Json j = Json::object();
    j.set("api", Json::integer(1));
    Json p = Json::object(); p.set("vendor", Json::string(hw_.platform.vendor)); p.set("family", Json::string(hw_.platform.family)); p.set("model", Json::string(hw_.platform.model));
    j.set("platform", p);
    Json b = Json::object(); b.set("id", hw_.board_id.empty() ? Json::null() : Json::string(hw_.board_id)); b.set("hardware_verified", Json::boolean(hw_.board_verified));
    j.set("board", b);
    Json s = Json::object(); s.set("model", Json::string(hw_.sensor.model));
    s.set("interface", Json::string(hw_.sensor.interface == hw::SensorInterface::MipiCsi ? "mipi-csi" : hw_.sensor.interface == hw::SensorInterface::Dvp ? "dvp" : "unknown"));
    Json nat = Json::object(); nat.set("width", Json::integer(hw_.sensor.native_width)); nat.set("height", Json::integer(hw_.sensor.native_height)); s.set("native", nat);
    // verified modes grouped by geometry: [{width,height,fps:[...]}]
    Json modes = Json::array();
    for (const auto& m : hw_.sensor.modes) {
        bool merged = false;
        for (size_t i = 0; i < modes.size(); ++i) {
            const Json& g = modes.at(i);
            if (g.get("width")->as_int() == m.width && g.get("height")->as_int() == m.height) {
                Json ng = g; Json fps = *g.get("fps"); fps.push(Json::integer(m.fps)); ng.set("fps", fps);
                Json rebuilt = Json::array(); for (size_t k = 0; k < modes.size(); ++k) rebuilt.push(k == i ? ng : modes.at(k));
                modes = rebuilt; merged = true; break;
            }
        }
        if (!merged) { Json g = Json::object(); g.set("width", Json::integer(m.width)); g.set("height", Json::integer(m.height)); Json f = Json::array(); f.push(Json::integer(m.fps)); g.set("fps", f); modes.push(g); }
    }
    s.set("modes", modes);
    j.set("sensor", s);
    Json v = Json::object(); v.set("h264", Json::string(cap_name(c.video.h264))); v.set("h265", Json::string(cap_name(c.video.h265)));
    v.set("max_streams", c.video.max_streams >= 0 ? Json::integer(c.video.max_streams) : Json::null());
    // Per-unit presence: which stream units this build actually wired. Modes
    // stay unknown (null) - they are not invented, only reported when known.
    Json streams = Json::object();
    for (int u = 0; u <= lifecycle::UNIT_SUB; ++u) {
        if (!pipeline_.unit_configured(u)) continue;
        EffectiveStream us = pipeline_.stream_unit(u);
        Json su = Json::object();
        su.set("configured", Json::boolean(true));
        su.set("width", us.width > 0 ? Json::integer(us.width) : Json::null());
        su.set("height", us.height > 0 ? Json::integer(us.height) : Json::null());
        su.set("supported_modes", Json::null());
        streams.set(std::to_string(u), su);
    }
    v.set("streams", streams);
    j.set("video", v);
    Json jp = Json::object();
    jp.set("status", Json::string(cap_name(c.jpeg.supported)));
    jp.set("max_width", c.jpeg.max_width >= 0 ? Json::integer(c.jpeg.max_width) : Json::null());
    jp.set("max_height", c.jpeg.max_height >= 0 ? Json::integer(c.jpeg.max_height) : Json::null());
    j.set("jpeg", jp);
    // M9: detection/AI. Detectors are listed only when the capability is proven;
    // person stays unknown until an NNA backend is actually built and verified.
    Json ai = Json::object();
    ai.set("available", Json::string(cap_name(c.ai.available)));
    ai.set("motion", Json::string(cap_name(c.ai.motion)));
    ai.set("person", Json::string(cap_name(c.ai.person)));
    Json detectors = Json::array();
    if (c.ai.motion == Cap::Supported) detectors.push(Json::string("motion"));
    ai.set("detectors", detectors);
    Json aifps = Json::object(); aifps.set("status", Json::string(cap_name(c.ai.available)));
    aifps.set("apply", Json::string(c.ai.available == Cap::Supported ? "live" : "unsupported"));
    aifps.set("min", Json::integer(1)); aifps.set("max", Json::integer(60));
    ai.set("inference_fps", aifps);
    j.set("ai", ai);
    Json ctl = Json::object();
    ctl.set("sensor_fps", range_control(c.sensor.fps));
    ctl.set("stream_fps", range_control(c.video.fps));
    ctl.set("bitrate", range_control(c.video.bitrate));
    ctl.set("gop", range_control(c.video.gop));
    ctl.set("framesource_buffers", range_control(c.video.framesource_buffers));
    ctl.set("encoder_buffers", range_control(c.video.encoder_buffers));
    ctl.set("isp_clock", perf_control(c.isp.performance));
    ctl.set("encoder_clock", perf_control(c.encoder.performance));
    ctl.set("cpu_frequency", perf_control(c.power.cpu_frequency));
    Json lg = Json::object(); lg.set("status", Json::string("supported")); lg.set("apply", Json::string("daemon_restart")); ctl.set("idle_grace_ms", lg);
    j.set("controls", ctl);
    Json image = Json::object();
    for (int i = 0; i < (int)ImageControl::COUNT; ++i) {
        ImageControl k = (ImageControl)i;
        Json cap = range_control(image_caps.control[i]);
        if (k == ImageControl::AntiFlicker) {
            Json values = Json::array(); values.push(Json::string("off")); values.push(Json::string("50hz")); values.push(Json::string("60hz"));
            cap.set("values", values);
        }
        if (k == ImageControl::WhiteBalanceMode) {
            Json values = Json::array();
            for (const char* n : {"auto", "manual", "daylight", "cloudy", "incandescent", "fluorescent", "twilight", "shade", "warm_fluorescent", "color_tendency"})
                values.push(Json::string(n));
            cap.set("values", values);
        }
        image.set(image_control_name(k), cap);
    }
    j.set("image", image);
    Json latency = Json::object();
    Json lp = Json::array(); for (const char* n : {"normal", "low", "custom"}) lp.push(Json::string(n));
    latency.set("profiles", lp);
    latency.set("gop", range_control(c.video.gop));
    latency.set("framesource_buffers", range_control(c.video.framesource_buffers));
    latency.set("encoder_buffers", range_control(c.video.encoder_buffers));
    Json q = Json::object(); q.set("status", Json::string("supported")); q.set("apply", Json::string("live")); q.set("min", Json::integer(1)); q.set("max", Json::integer(32));
    latency.set("consumer_queue_depth", q);
    Json sb = Json::object(); sb.set("status", Json::string("supported")); sb.set("apply", Json::string("daemon_restart")); sb.set("min", Json::integer(4096)); sb.set("max", Json::integer(1048576));
    latency.set("socket_send_buffer_bytes", sb);
    Json stall = Json::object(); stall.set("status", Json::string("supported")); stall.set("apply", Json::string("daemon_restart")); stall.set("min", Json::integer(50)); stall.set("max", Json::integer(10000));
    latency.set("send_stall_ms", stall);
    Json unsupported = Json::object(); unsupported.set("status", Json::string("unsupported")); unsupported.set("apply", Json::string("unsupported"));
    latency.set("b_frames", unsupported); latency.set("sdk_low_latency_mode", unsupported);
    j.set("latency", latency);
    Json profiles = Json::array(); for (const char* n : {"performance", "balanced", "battery", "custom"}) profiles.push(Json::string(n));
    j.set("profiles", profiles);
    Json vf = Json::array(); for (int f : perf_.verified_fps()) vf.push(Json::integer(f));
    j.set("verified_fps", vf);
    return j;
}
Response ApiService::capabilities() const { return Response{200, capabilities_json()}; }

Result ApiService::snapshot(std::vector<uint8_t>& out, std::string& err, int timeout_ms) {
    return pipeline_.snapshot(out, err, timeout_ms);
}

Json ApiService::state_json() {
    lifecycle::Stats st = pipeline_.stats();
    power::EffectiveState e = perf_.effective_state();
    EffectiveStream s = pipeline_.stream();
    media::TuningState tune = tuning_.state();
    bool running = st.state == lifecycle::State::Active || st.state == lifecycle::State::GraceIdle;
    Json j = Json::object();
    j.set("lifecycle", Json::string(lifecycle_name_lc(st.state)));
    j.set("profile", Json::string(power::profile_name(e.profile)));
    j.set("pipeline_generation", Json::integer(st.generation));
    j.set("restarts", Json::integer(st.restart_count));
    j.set("last_error", st.last_error.empty() ? Json::null() : Json::string(st.last_error));
    Json cons = Json::object(); cons.set("total", Json::integer(st.total_demand));
    for (int i = 0; i < (int)lifecycle::ConsumerType::COUNT; ++i) if (st.demand[i] > 0) cons.set(lifecycle::consumer_name((lifecycle::ConsumerType)i), Json::integer(st.demand[i]));
    j.set("consumers", cons);
    Json m = Json::object();
    m.set("sensor_fps_requested", Json::integer(e.sensor_fps_requested));
    m.set("sensor_fps", e.sensor_fps_readback ? Json::integer(e.sensor_fps_effective) : Json::null());   // effective (hardware) or null when cold/unreadable
    m.set("stream_fps", Json::integer(s.fps));
    m.set("bitrate_kbps", Json::integer(s.bitrate_kbps));
    m.set("gop", Json::integer(s.gop));
    m.set("framesource_buffers", Json::integer(s.buffers));
    m.set("encoder_buffers", s.encoder_buffers > 0 ? Json::integer(s.encoder_buffers) : Json::null());
    m.set("width", Json::integer(s.width)); m.set("height", Json::integer(s.height));
    m.set("encoder_active", Json::boolean(st.unit_active[lifecycle::UNIT_MAIN]));   // main encoder, not the base
    m.set("sensor_active", Json::boolean(running));                                 // base (sensor/ISP)
    // M8: per-unit runtime view. The flat fields above stay for compatibility
    // and describe the main stream (unit 0).
    Json streams = Json::object();
    for (int u = 0; u <= lifecycle::UNIT_SUB; ++u) {
        if (!pipeline_.unit_configured(u)) continue;
        EffectiveStream us = pipeline_.stream_unit(u);
        Json su = Json::object();
        su.set("active", Json::boolean(st.unit_active[u]));
        su.set("consumers", Json::integer(st.unit_demand[u]));
        su.set("width", Json::integer(us.width)); su.set("height", Json::integer(us.height));
        su.set("fps", Json::integer(us.fps)); su.set("bitrate_kbps", Json::integer(us.bitrate_kbps));
        su.set("total_bytes", Json::integer((int64_t)st.total_bytes[u]));   // monotonic, for /metrics
        streams.set(std::to_string(u), su);
    }
    m.set("streams", streams);
    Json mj = Json::object();
    mj.set("active", Json::boolean(st.unit_active[lifecycle::UNIT_JPEG]));
    mj.set("consumers", Json::integer(st.unit_demand[lifecycle::UNIT_JPEG]));
    m.set("jpeg", mj);
    j.set("media", m);
    Json lat = Json::object(); lat.set("profile", Json::string(media::latency_profile_name(tune.latency.profile)));
    lat.set("gop", Json::integer(tune.latency.gop)); lat.set("framesource_buffers", Json::integer(tune.latency.framesource_buffers));
    lat.set("encoder_buffers", tune.latency.encoder_buffers > 0 ? Json::integer(tune.latency.encoder_buffers) : Json::null());
    lat.set("consumer_queue_depth", Json::integer(tune.latency.consumer_queue_depth)); j.set("latency", lat);
    Json image = Json::object();
    for (int i = 0; i < (int)ImageControl::COUNT; ++i)
        image.set(image_control_name((ImageControl)i), tune.image_effective[i] >= 0 ? Json::integer(tune.image_effective[i]) : Json::null());
    ExposureReadback ex; Result er = tuning_.exposure(ex);
    Json ae = Json::object();
    ae.set("available", Json::boolean((bool)er && ex.available));
    ae.set("luma", ex.available ? Json::integer(ex.luma) : Json::null()); ae.set("target", ex.available ? Json::integer(ex.target) : Json::null());
    ae.set("stable", ex.available ? Json::boolean(ex.stable) : Json::null()); ae.set("integration_time", ex.available ? Json::integer(ex.integration_time) : Json::null());
    ae.set("analog_gain", ex.available ? Json::integer(ex.again) : Json::null()); ae.set("digital_gain", ex.available ? Json::integer(ex.dgain) : Json::null());
    ae.set("isp_digital_gain", ex.available ? Json::integer(ex.isp_dgain) : Json::null()); ae.set("total_gain_db", ex.available ? Json::integer(ex.total_gain_db) : Json::null());
    image.set("exposure", ae); j.set("image", image);
    if (detection_) {
        detection::AiTelemetry ai = detection_->telemetry();
        Json a = Json::object();
        a.set("state", Json::string(detection::ai_state_name(ai.state)));
        a.set("enabled", Json::boolean(ai.enabled));
        a.set("detector", Json::string(ai.detector));
        a.set("backend", ai.backend.empty() ? Json::null() : Json::string(ai.backend));
        a.set("motion", Json::boolean(ai.motion_now));
        a.set("last_error", ai.last_error.empty() ? Json::null() : Json::string(ai.last_error));
        j.set("ai", a);
    }
    j.set("revision", Json::integer(store_.revision()));
    return j;
}
Response ApiService::state() { return Response{200, state_json()}; }

Json ApiService::config_json() {
    power::EffectiveState e = perf_.effective_state();
    EffectiveStream s = pipeline_.stream();
    media::TuningState tune = tuning_.state();
    Json j = Json::object();
    j.set("revision", Json::integer(store_.revision()));
    Json perf = Json::object(); perf.set("profile", Json::string(power::profile_name(e.profile))); j.set("performance", perf);
    Json sen = Json::object(); sen.set("fps", Json::integer(e.sensor_fps_requested)); j.set("sensor", sen);
    Json v0 = Json::object(); v0.set("fps", Json::integer(s.fps)); v0.set("bitrate_kbps", Json::integer(s.bitrate_kbps));
    v0.set("width", Json::integer(s.width)); v0.set("height", Json::integer(s.height)); v0.set("gop", Json::integer(s.gop));
    Json video = Json::object(); video.set("0", v0); j.set("video", video);
    Json lat = Json::object(); lat.set("profile", Json::string(media::latency_profile_name(tune.requested_latency.profile)));
    lat.set("gop", tune.requested_latency.gop ? Json::integer(*tune.requested_latency.gop) : Json::null());
    lat.set("framesource_buffers", tune.requested_latency.framesource_buffers ? Json::integer(*tune.requested_latency.framesource_buffers) : Json::null());
    lat.set("encoder_buffers", tune.requested_latency.encoder_buffers ? Json::integer(*tune.requested_latency.encoder_buffers) : Json::null());
    lat.set("consumer_queue_depth", tune.requested_latency.consumer_queue_depth ? Json::integer(*tune.requested_latency.consumer_queue_depth) : Json::null());
    lat.set("socket_send_buffer_bytes", Json::integer(cfg_.rtsp.send_buffer_bytes));
    lat.set("send_stall_ms", Json::integer(cfg_.rtsp.send_stall_ms));
    Json le = Json::object(); le.set("gop", Json::integer(tune.latency.gop)); le.set("framesource_buffers", Json::integer(tune.latency.framesource_buffers));
    le.set("encoder_buffers", tune.latency.encoder_buffers > 0 ? Json::integer(tune.latency.encoder_buffers) : Json::null());
    le.set("consumer_queue_depth", Json::integer(tune.latency.consumer_queue_depth));
    le.set("socket_send_buffer_bytes", Json::integer(cfg_.rtsp.send_buffer_bytes)); le.set("send_stall_ms", Json::integer(cfg_.rtsp.send_stall_ms));
    lat.set("effective", le); j.set("latency", lat);
    Json image = Json::object();
    for (int i = 0; i < (int)ImageControl::COUNT; ++i)
        image.set(image_control_name((ImageControl)i), tune.image_requested[i] >= 0 ? Json::integer(tune.image_requested[i]) : Json::null());
    j.set("image", image);
    Json rt = Json::object(); rt.set("max_clients", Json::integer(cfg_.rtsp.max_clients)); j.set("rtsp", rt);
    Json lc = Json::object(); lc.set("idle_grace_ms", Json::integer(cfg_.pipeline.idle_grace_ms)); lc.set("always_on", Json::boolean(cfg_.pipeline.always_on)); j.set("lifecycle", lc);
    Json pw = Json::object(); pw.set("isp_performance", Json::string(power::perf_level_name(e.isp)));
    pw.set("encoder_performance", Json::string(power::perf_level_name(e.encoder))); pw.set("cpu_performance", Json::string(power::perf_level_name(e.cpu)));
    j.set("power", pw);
    Json ai = Json::object();
    if (detection_) {
        detection::AiTelemetry a = detection_->telemetry();
        ai.set("enabled", Json::boolean(a.enabled));
        ai.set("detector", Json::string(a.detector));
        ai.set("inference_fps", Json::integer(a.requested_fps));
    } else {
        ai.set("enabled", Json::boolean(cfg_.ai.enabled));
        ai.set("detector", Json::string(cfg_.ai.detector));
        ai.set("inference_fps", Json::integer(cfg_.ai.inference_fps));
    }
    j.set("ai", ai);
    return j;
}
Response ApiService::config() { return Response{200, config_json()}; }


Json ApiService::telemetry_json() {
    Telemetry t = perf_.telemetry();
    Json j = Json::object();
    j.set("timestamp_ms", Json::integer(now_ms_realtime()));
    j.set("lifecycle", Json::string(lifecycle_name_lc(t.state)));
    j.set("pipeline_generation", Json::integer(t.generation));
    Json pr = Json::object(); pr.set("cpu_percent", opt(t.cpu_percent)); pr.set("rss_kb", opt(t.rss_kb)); pr.set("threads", opt(t.threads)); j.set("process", pr);
    Json m = Json::object(); m.set("encoded_fps", opt(t.measured_encoded_fps)); m.set("bitrate_kbps", opt(t.measured_bitrate_kbps));
    m.set("dropped_frames", Json::integer(t.dropped_frames)); m.set("stream_fps_requested", Json::integer(t.requested_stream_fps));
    m.set("bitrate_kbps_requested", Json::integer(t.requested_bitrate_kbps));
    // M8: per-stream and jpeg counters, straight from the pipeline stats.
    lifecycle::Stats st = pipeline_.stats();
    Json streams = Json::object();
    for (int u = 0; u <= lifecycle::UNIT_SUB; ++u) {
        if (!pipeline_.unit_configured(u)) continue;
        lifecycle::Measurement mu = pipeline_.measurement_unit(u);
        Json su = Json::object();
        su.set("active", Json::boolean(st.unit_active[u]));
        su.set("encoded_fps", mu.valid ? Json::number(mu.encoded_fps) : Json::null());
        su.set("bitrate_kbps", mu.valid ? Json::number(mu.bitrate_kbps) : Json::null());
        su.set("dropped_frames", Json::integer(mu.dropped_frames));
        streams.set(std::to_string(u), su);
    }
    m.set("streams", streams);
    j.set("media", m);
    Json tj = Json::object();
    tj.set("active", Json::boolean(st.unit_active[lifecycle::UNIT_JPEG]));
    tj.set("captures_total", Json::integer(st.jpeg_captures));
    tj.set("capture_failures", Json::integer(st.jpeg_failures));
    tj.set("last_capture_ms", st.jpeg_last_capture_ms >= 0 ? Json::integer(st.jpeg_last_capture_ms) : Json::null());
    j.set("jpeg", tj);
    LatencyStats ls = tuning_.latency_stats();
    Json lat = Json::object(); lat.set("available", Json::boolean(ls.valid)); lat.set("samples", Json::integer(ls.samples));
    lat.set("capture_to_encoder_output_avg_ms", ls.valid ? Json::number(ls.capture_to_out_avg_ms) : Json::null());
    lat.set("capture_to_encoder_output_max_ms", ls.valid ? Json::number(ls.capture_to_out_max_ms) : Json::null());
    lat.set("encoder_output_to_socket_avg_ms", ls.valid ? Json::number(ls.out_to_send_avg_ms) : Json::null());
    lat.set("encoder_output_to_socket_max_ms", ls.valid ? Json::number(ls.out_to_send_max_ms) : Json::null());
    lat.set("discontinuities", Json::integer(ls.discontinuities)); j.set("latency", lat);
    ExposureReadback ex; Result xr = tuning_.exposure(ex);
    Json ae = Json::object(); ae.set("available", Json::boolean((bool)xr && ex.available));
    ae.set("luma", ex.available ? Json::integer(ex.luma) : Json::null()); ae.set("target", ex.available ? Json::integer(ex.target) : Json::null());
    ae.set("stable", ex.available ? Json::boolean(ex.stable) : Json::null()); ae.set("integration_time", ex.available ? Json::integer(ex.integration_time) : Json::null());
    ae.set("analog_gain", ex.available ? Json::integer(ex.again) : Json::null()); ae.set("digital_gain", ex.available ? Json::integer(ex.dgain) : Json::null());
    ae.set("isp_digital_gain", ex.available ? Json::integer(ex.isp_dgain) : Json::null()); ae.set("total_gain_db", ex.available ? Json::integer(ex.total_gain_db) : Json::null());
    j.set("exposure", ae);
    Json pw = Json::object(); pw.set("sensor_fps", opt(t.effective_sensor_fps)); pw.set("sensor_fps_requested", Json::integer(t.requested_sensor_fps));
    pw.set("isp_clock_hz", opt(t.isp_clock_hz)); pw.set("encoder_clock_hz", opt(t.encoder_clock_hz));
    pw.set("cpu_frequency_hz", t.cpu_freq_khz.available ? Json::number((double)t.cpu_freq_khz.value * 1000.0) : Json::null());
    pw.set("profile", Json::string(power::profile_name(t.profile)));
    j.set("power", pw);
    if (detection_) {
        detection::AiTelemetry ai = detection_->telemetry();
        Json a = Json::object();
        a.set("state", Json::string(detection::ai_state_name(ai.state)));
        a.set("enabled", Json::boolean(ai.enabled));
        a.set("detector", Json::string(ai.detector));
        a.set("backend", ai.backend.empty() ? Json::null() : Json::string(ai.backend));
        a.set("inference_fps_requested", Json::integer(ai.requested_fps));
        a.set("effective_fps", ai.effective_fps > 0.0 ? Json::number(ai.effective_fps) : Json::null());
        a.set("completed", Json::integer((long long)ai.completed));
        a.set("failed", Json::integer((long long)ai.failed));
        a.set("detections_total", Json::integer((long long)ai.detections_total));
        a.set("skipped", Json::integer((long long)ai.skipped));
        a.set("motion", Json::boolean(ai.motion_now));
        a.set("last_inference_ms", ai.last_inference_ms >= 0 ? Json::integer(ai.last_inference_ms) : Json::null());
        a.set("last_detection_ms", ai.last_detection_ms >= 0 ? Json::integer(ai.last_detection_ms) : Json::null());
        j.set("ai", a);
    }
    return j;
}
Response ApiService::telemetry() { return Response{200, telemetry_json()}; }

// ---- PATCH /config ----------------------------------------------------------
namespace {
struct Change { std::string path; Json requested; ApplyResult r; bool has_result = false; std::string key, value; };

bool get_int(const Json& v, long long& out) { if (!v.is_integer()) return false; out = v.as_int(); return true; }

bool image_control_from_name(const std::string& n, ImageControl& out) {
    for (int i = 0; i < (int)ImageControl::COUNT; ++i) {
        ImageControl c = (ImageControl)i;
        if (n == image_control_name(c)) { out = c; return true; }
    }
    return false;
}

bool wb_mode(const std::string& s, int& out) {
    const char* names[] = {"auto", "manual", "daylight", "cloudy", "incandescent", "fluorescent", "twilight", "shade", "warm_fluorescent", "color_tendency"};
    for (int i = 0; i < 10; ++i) if (s == names[i]) { out = i; return true; }
    return false;
}

Json change_json(const Change& c) {
    Json j = Json::object();
    j.set("path", Json::string(c.path));
    j.set("requested", c.requested);
    if (c.has_result) {
        j.set("effective", c.r.effective >= 0 && c.r.mode != ApplyMode::Unsupported && c.r.ok ? Json::integer(c.r.effective) : Json::null());
        j.set("apply", Json::string(apply_api_name(c.r.mode)));
        j.set("status", Json::string(!c.r.ok ? "rejected" : c.r.deferred ? "stored" : "applied"));
        j.set("effective_on_next_start", Json::boolean(c.r.ok && c.r.deferred));
        j.set("pipeline_restarted", Json::boolean(c.r.ok && !c.r.deferred && c.r.mode == ApplyMode::PipelineRestart));
        if (!c.r.message.empty()) j.set("message", Json::string(c.r.message));
    }
    return j;
}
} // namespace

Response ApiService::unset_config(const std::vector<std::string>& conf_keys) {
    // Serialised with PATCH: a reset racing a save must not interleave
    // runtime state and ConfigStore.
    std::lock_guard<std::mutex> lk(patch_m_);

    // Phase 1 - VALIDATE: every key must resolve to an action before anything
    // is written or applied; an unknown key answers 404 with nothing changed.
    std::vector<ImageControl> images;
    bool fs = false, enc = false, qd = false, vfps = false, sfps = false, prof = false, latprof = false, det = false;
    for (const std::string& k : conf_keys) {
        if (k.rfind("image.", 0) == 0) {
            ImageControl c;
            if (!image_control_from_name(k.substr(6), c))
                return fail(404, "unknown_field", k, "unknown image control");
            images.push_back(c);
        }
        else if (k == "latency.framesource_buffers") fs = true;
        else if (k == "latency.encoder_buffers")     enc = true;
        else if (k == "latency.queue_depth")         qd = true;
        else if (k == "video.fps")                   vfps = true;
        else if (k == "sensor.fps")                  sfps = true;
        else if (k == "performance.profile")         prof = true;
        else if (k == "latency.profile")             latprof = true;
        else if (k == "ai.detector") {
            if (!detection_) return fail(404, "unknown_field", k, "no detection subsystem on this build");
            det = true;
        }
        else return fail(404, "unknown_field", k, "no such resettable key");
    }

    // Phase 2 - PERSIST first: if the flash write fails, nothing has moved.
    // If a runtime step fails afterwards, the DISK already holds the reset,
    // so the next start converges to the requested state instead of
    // resurrecting the old override.
    std::string err;
    if (!store_.commit_remove(conf_keys, err))
        return fail(500, "internal", "", err);

    // Phase 3 - APPLY synchronously: a 200 means "effective now"; the stock
    // UI re-reads config.json immediately. No SIGHUP here - the signal stays
    // the EXTERNAL compatibility interface (`killall -HUP majestic`), not an
    // internal completion mechanism racing this HTTP response.
    for (ImageControl c : images) {
        power::ApplyResult ar = tuning_.clear_image(c);
        if (!ar.ok) return fail(500, "internal", image_control_name(c),
                                ar.message + " (persisted; fully effective at next start)");
    }
    if (fs)  { power::ApplyResult ar = tuning_.clear_framesource_buffers(); if (!ar.ok) return fail(500, "internal", "latency.framesource_buffers", ar.message + " (persisted)"); }
    if (enc) { power::ApplyResult ar = tuning_.clear_encoder_buffers();     if (!ar.ok) return fail(500, "internal", "latency.encoder_buffers", ar.message + " (persisted)"); }
    if (qd)  { power::ApplyResult ar = tuning_.clear_queue_depth();         if (!ar.ok) return fail(500, "internal", "latency.queue_depth", ar.message + " (persisted)"); }
    // Explicit, profile-independent clears with CHECKED results: a 200 must
    // never paper over a rejected re-apply (false-200), and the generic
    // apply_config path deliberately skips absent keys under Custom - the
    // opposite of what a reset means.
    if (vfps) { power::ApplyResult ar = perf_.clear_stream_fps(); if (!ar.ok) return fail(500, "internal", "video.fps", ar.message + " (persisted; fully effective at next start)"); }
    if (sfps) { power::ApplyResult ar = perf_.clear_sensor_fps(); if (!ar.ok) return fail(500, "internal", "sensor.fps", ar.message + " (persisted; fully effective at next start)"); }
    if (prof) {
        power::ApplyResult ar = perf_.apply_profile(PerformanceConfig{}.profile);   // compiled default
        if (!ar.ok) return fail(500, "internal", "performance.profile", ar.message + " (persisted; fully effective at next start)");
    }
    if (latprof) {
        power::ApplyResult ar = tuning_.set_latency_profile(media::LatencySettings{}.profile);   // compiled default
        if (!ar.ok) return fail(500, "internal", "latency.profile", ar.message + " (persisted; fully effective at next start)");
    }
    if (det) {
        Result r = detection_->set_detector(AiConfig{}.detector);                    // compiled default
        if (!r) return fail(500, "internal", "ai.detector",
                            std::string("detector reset failed (") + status_name(r.status) + ") (persisted; fully effective at next start)");
    }

    Json j = Json::object();
    j.set("ok", Json::boolean(true));
    j.set("revision", Json::integer(store_.revision()));
    return Response{200, j};
}

Response ApiService::patch_config(const std::string& body, const std::string& if_match) {
    std::lock_guard<std::mutex> lk(patch_m_);
    if (body.empty()) return fail(400, "invalid_json", "", "empty body");
    Json doc; std::string perr;
    if (!Json::parse(body, doc, perr)) return fail(400, "invalid_json", "", perr);
    if (!doc.is_object()) return fail(400, "invalid_json", "", "top-level value must be an object");
    if (!if_match.empty()) {
        unsigned want = (unsigned)strtoul(if_match.c_str(), nullptr, 10);
        if (want != store_.revision()) return fail(409, "conflict", "revision", "stale revision " + if_match + ", current is " + std::to_string(store_.revision()));
    }
    if (const Json* rv = doc.get("revision")) {          // optional in-body optimistic concurrency
        long long want; if (!get_int(*rv, want)) return fail(422, "invalid_value", "revision", "revision must be an integer");
        if ((unsigned)want != store_.revision()) return fail(409, "conflict", "revision", "stale revision " + std::to_string(want) + ", current is " + std::to_string(store_.revision()));
    }

    // ---- phase 1: structural validation (unknown fields, types) - nothing applied yet
    std::vector<Change> changes;
    CapabilitySet caps = perf_.capabilities();
    auto bad = [&](int status, const char* code, const std::string& path, const std::string& msg) { return fail(status, code, path, msg); };
    for (const auto& sec : doc.members()) {
        const std::string& s = sec.first; const Json& v = sec.second;
        if (s == "revision") continue;
        if (!v.is_object()) return bad(400, "unknown_field", s, "section must be an object");
        for (const auto& kv : v.members()) {
            std::string path = s + "." + kv.first; const Json& val = kv.second;
            Change c; c.path = path; c.requested = val;
            if (s == "performance" && kv.first == "profile") {
                if (!val.is_string()) return bad(422, "invalid_value", path, "profile must be a string");
                Profile p; if (!power::parse_profile(val.as_string(), p)) return bad(422, "invalid_value", path, "unknown profile (performance|balanced|battery|custom)");
                c.key = "performance.profile"; c.value = val.as_string();
            } else if (s == "sensor" && kv.first == "fps") {
                long long n; if (!get_int(val, n) || n <= 0 || n > 240) return bad(422, "invalid_value", path, "fps must be an integer in 1..240");
                if (caps.sensor.fps.support == Cap::Unsupported) return bad(422, "unsupported_control", path, "sensor fps control is not available on this platform");
                c.key = "sensor.fps"; c.value = std::to_string(n);
            } else if (s == "video" && kv.first == "0") {
                if (!val.is_object()) return bad(400, "unknown_field", path, "video.0 must be an object");
                for (const auto& f : val.members()) {
                    Change d; d.path = path + "." + f.first; d.requested = f.second; long long n;
                    if (f.first == "fps") { if (!get_int(f.second, n) || n <= 0 || n > 240) return bad(422, "invalid_value", d.path, "fps must be an integer in 1..240"); d.key = "video.fps"; d.value = std::to_string(n); }
                    else if (f.first == "bitrate_kbps") { if (!get_int(f.second, n) || n <= 0 || n > 200000) return bad(422, "invalid_value", d.path, "bitrate_kbps must be an integer in 1..200000"); d.key = "video.bitrate"; d.value = std::to_string(n); }
                    else if (f.first == "gop") { if (!get_int(f.second, n) || n < 1 || n > 1000) return bad(422, "invalid_value", d.path, "gop must be an integer in 1..1000"); d.key = "latency.gop"; d.value = std::to_string(n); }
                    else return bad(400, "unknown_field", d.path, "unknown field");
                    changes.push_back(d);
                }
                continue;
            } else if (s == "latency") {
                if (kv.first == "profile") {
                    if (!val.is_string()) return bad(422, "invalid_value", path, "profile must be normal|low|custom");
                    media::LatencyProfile p; if (!media::parse_latency_profile(val.as_string(), p)) return bad(422, "invalid_value", path, "unknown latency profile (normal|low|custom)");
                    c.key = "latency.profile"; c.value = val.as_string();
                } else {
                    long long n; if (!get_int(val, n)) return bad(422, "invalid_value", path, "value must be an integer");
                    if (kv.first == "gop") { if (n < 1 || n > 1000) return bad(422, "invalid_value", path, "gop must be in 1..1000"); c.key = "latency.gop"; }
                    else if (kv.first == "framesource_buffers") { if (n < 1 || n > 8) return bad(422, "invalid_value", path, "framesource_buffers must be in 1..8"); c.key = "latency.framesource_buffers"; }
                    else if (kv.first == "encoder_buffers") { if (n < 1 || n > 8) return bad(422, "invalid_value", path, "encoder_buffers must be in 1..8"); c.key = "latency.encoder_buffers"; }
                    else if (kv.first == "consumer_queue_depth") { if (n < 1 || n > 32) return bad(422, "invalid_value", path, "consumer_queue_depth must be in 1..32"); c.key = "latency.queue_depth"; }
                    else if (kv.first == "socket_send_buffer_bytes") { if (n < 4096 || n > 1048576) return bad(422, "invalid_value", path, "socket_send_buffer_bytes must be in 4096..1048576"); c.key = "rtsp.send_buffer_bytes"; }
                    else if (kv.first == "send_stall_ms") { if (n < 50 || n > 10000) return bad(422, "invalid_value", path, "send_stall_ms must be in 50..10000"); c.key = "rtsp.send_stall_ms"; }
                    else return bad(400, "unknown_field", path, "unknown field");
                    c.value = std::to_string(n);
                }
            } else if (s == "image") {
                ImageControl control;
                if (!image_control_from_name(kv.first, control)) return bad(400, "unknown_field", path, "unknown image control");
                ImageCaps ic = tuning_.image_caps(); const RangeCap cap = ic.control[(int)control];
                if (cap.support != Cap::Supported) return bad(422, "unsupported_control", path, "image control is not supported by this adapter/mode");
                int iv = -1;
                if (control == ImageControl::AntiFlicker) {
                    if (!val.is_string()) return bad(422, "invalid_value", path, "anti_flicker must be off|50hz|60hz");
                    iv = val.as_string() == "off" ? 0 : val.as_string() == "50hz" ? 50 : val.as_string() == "60hz" ? 60 : -1;
                    if (iv < 0) return bad(422, "invalid_value", path, "anti_flicker must be off|50hz|60hz");
                    c.value = val.as_string();
                } else if (control == ImageControl::WhiteBalanceMode && val.is_string()) {
                    if (!wb_mode(val.as_string(), iv)) return bad(422, "invalid_value", path, "unknown white-balance mode");
                    c.value = std::to_string(iv);
                } else {
                    long long n; if (!get_int(val, n)) return bad(422, "invalid_value", path, "image control must be an integer"); iv = (int)n;
                    if (!cap.in_range(iv)) return bad(422, "invalid_value", path, "image control outside capability range");
                    c.value = std::to_string(iv);
                }
                c.key = "image." + kv.first;
            } else if (s == "rtsp" && kv.first == "max_clients") {
                long long n; if (!get_int(val, n) || n < 1 || n > 16) return bad(422, "invalid_value", path, "max_clients must be an integer in 1..16");
                c.key = "rtsp.max_clients"; c.value = std::to_string(n);
            } else if (s == "lifecycle" && kv.first == "idle_grace_ms") {
                long long n; if (!get_int(val, n) || n < 0 || n > 600000) return bad(422, "invalid_value", path, "idle_grace_ms must be an integer in 0..600000");
                c.key = "lifecycle.idle_grace_ms"; c.value = std::to_string(n);
            } else if (s == "power" && (kv.first == "isp_performance" || kv.first == "encoder_performance" || kv.first == "cpu_performance")) {
                if (!val.is_string()) return bad(422, "invalid_value", path, "level must be a string (auto|low|high)");
                PerfLevel l; if (!power::parse_perf_level(val.as_string(), l)) return bad(422, "invalid_value", path, "unknown level (auto|low|high)");
                c.key = "power." + kv.first; c.value = val.as_string();
            } else if (s == "ai") {
                if (!detection_ || caps.ai.available != Cap::Supported) return bad(422, "unsupported_control", path, "detection/AI is not available on this platform");
                if (kv.first == "enabled") {
                    if (!val.is_bool()) return bad(422, "invalid_value", path, "enabled must be a boolean");
                    c.key = "ai.enabled"; c.value = val.as_bool() ? "true" : "false";
                } else if (kv.first == "detector") {
                    if (!val.is_string()) return bad(422, "invalid_value", path, "detector must be a string");
                    if (val.as_string() != "motion") return bad(422, "invalid_value", path, "unknown detector (motion)");
                    c.key = "ai.detector"; c.value = val.as_string();
                } else if (kv.first == "inference_fps") {
                    long long n; if (!get_int(val, n) || n < 1 || n > 60) return bad(422, "invalid_value", path, "inference_fps must be an integer in 1..60");
                    c.key = "ai.inference_fps"; c.value = std::to_string(n);
                } else return bad(400, "unknown_field", path, "unknown field");
            } else return bad(400, "unknown_field", path, "unknown field");
            changes.push_back(c);
        }
    }
    if (changes.empty()) return bad(400, "invalid_value", "", "no changes given");
    // profile first, then explicit values (explicit user values win over the profile)
    std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return (a.key == "performance.profile") > (b.key == "performance.profile"); });

    // ---- phase 2: apply through the services (the PipelineManager owns any restart)
    bool any_rejected = false; const Change* first_rejected = nullptr;
    // AI setters return a Result (ok / unsupported / error); fold that into the
    // ApplyResult the rest of the pipeline speaks. Live-applied.
    auto ai_apply = [&](Result r, int eff) -> ApplyResult {
        if (r) return ApplyResult::applied(ApplyMode::Live, eff, eff);
        std::string msg = detection_ ? detection_->telemetry().last_error : std::string("detector error");
        ApplyMode m = (r.status == Status::Unsupported) ? ApplyMode::Unsupported : ApplyMode::Live;
        return ApplyResult::rejected(m, eff, msg.empty() ? "detector could not be applied" : msg);
    };
    for (auto& c : changes) {
        long long n = 0; if (c.requested.is_number()) n = c.requested.as_int();
        if (c.key == "performance.profile")      { Profile p; power::parse_profile(c.value, p); c.r = perf_.apply_profile(p); }
        else if (c.key == "sensor.fps")          c.r = perf_.set_sensor_fps((int)n);
        else if (c.key == "video.fps")           c.r = perf_.set_stream_fps((int)n);
        else if (c.key == "video.bitrate")       c.r = perf_.set_bitrate((int)n);
        else if (c.key == "latency.profile")     { media::LatencyProfile p; media::parse_latency_profile(c.value, p); c.r = tuning_.set_latency_profile(p); }
        else if (c.key == "latency.gop")         c.r = tuning_.set_gop(atoi(c.value.c_str()));
        else if (c.key == "latency.framesource_buffers") c.r = tuning_.set_framesource_buffers(atoi(c.value.c_str()));
        else if (c.key == "latency.encoder_buffers")     c.r = tuning_.set_encoder_buffers(atoi(c.value.c_str()));
        else if (c.key == "latency.queue_depth")         c.r = tuning_.set_queue_depth(atoi(c.value.c_str()));
        else if (c.key == "rtsp.send_buffer_bytes" || c.key == "rtsp.send_stall_ms")
            c.r = ApplyResult::stored(ApplyMode::DaemonRestart, atoi(c.value.c_str()), "persisted; applies to sockets after daemon restart");
        else if (c.key == "rtsp.max_clients")
            c.r = ApplyResult::stored(ApplyMode::DaemonRestart, atoi(c.value.c_str()), "persisted; the accept loop picks it up after daemon restart");
        else if (c.key.rfind("image.", 0) == 0) {
            ImageControl control; image_control_from_name(c.key.substr(6), control);
            int iv = control == ImageControl::AntiFlicker ? (c.value == "off" ? 0 : c.value == "50hz" ? 50 : 60) : atoi(c.value.c_str());
            c.r = tuning_.set_image(control, iv);
        }
        else if (c.key == "lifecycle.idle_grace_ms") { c.r = ApplyResult::stored(ApplyMode::DaemonRestart, (int)n, "persisted; takes effect after daemon restart"); }
        else if (c.key == "power.isp_performance")     { PerfLevel l; power::parse_perf_level(c.value, l); c.r = perf_.set_isp_performance(l); }
        else if (c.key == "power.encoder_performance") { PerfLevel l; power::parse_perf_level(c.value, l); c.r = perf_.set_encoder_performance(l); }
        else if (c.key == "power.cpu_performance")     { PerfLevel l; power::parse_perf_level(c.value, l); c.r = perf_.set_cpu_performance(l); }
        else if (c.key == "ai.enabled")       { c.r = ai_apply(detection_->set_enabled(c.value == "true"), c.value == "true" ? 1 : 0); }
        else if (c.key == "ai.detector")      { c.r = ai_apply(detection_->set_detector(c.value), -1); }
        else if (c.key == "ai.inference_fps") { int n2 = atoi(c.value.c_str()); c.r = ai_apply(detection_->set_inference_fps(n2), n2); }
        c.has_result = true;
        if (c.r.ok) {
            if (c.key == "rtsp.send_buffer_bytes") cfg_.rtsp.send_buffer_bytes = atoi(c.value.c_str());
            else if (c.key == "rtsp.send_stall_ms") cfg_.rtsp.send_stall_ms = atoi(c.value.c_str());
            else if (c.key == "rtsp.max_clients") cfg_.rtsp.max_clients = atoi(c.value.c_str());
            else if (c.key == "lifecycle.idle_grace_ms") cfg_.pipeline.idle_grace_ms = atoi(c.value.c_str());
        }
        if (c.r.ok && (c.key == "performance.profile" || c.key == "video.fps")) {
            ApplyResult lr = tuning_.refresh_after_stream_change();
            if (!lr.ok) LOGW(MOD, "latency preset refresh after %s failed: %s", c.key.c_str(), lr.message.c_str());
        }
        if (!c.r.ok) { any_rejected = true; if (!first_rejected) first_rejected = &c; }
        LOGI(MOD, "PATCH %s=%s -> %s (%s, effective=%d%s) %s", c.path.c_str(), c.value.c_str(), c.r.ok ? "ok" : "REJECTED",
             apply_api_name(c.r.mode), c.r.effective, c.r.deferred ? ", deferred" : "", c.r.message.c_str());
    }

    // ---- phase 3: persist what was applied/stored (atomic), bump revision, notify
    KeyValues kv;
    for (const auto& c : changes) if (c.r.ok) kv.emplace_back(c.key, c.value);
    if (!kv.empty()) {
        std::string err;
        if (!store_.commit(kv, err)) {
            Response r = fail(500, "internal_error", "", "settings applied but persisting failed: " + err);
            Json ch = Json::array(); for (const auto& c : changes) ch.push(change_json(c)); r.body.set("changes", ch);
            return r;
        }
        Json ev = Json::object(); ev.set("revision", Json::integer(store_.revision()));
        Json paths = Json::array(); for (const auto& c : changes) if (c.r.ok) paths.push(Json::string(c.path)); ev.set("paths", paths);
        bus_.publish("config_changed", ev.dump());
    }

    Json changes_json = Json::array(); for (const auto& c : changes) changes_json.push(change_json(c));
    if (any_rejected) {
        const ApplyResult& r = first_rejected->r;
        const char* code = r.mode == ApplyMode::Unsupported ? "unsupported_control"
                         : r.message.find("restart failed") != std::string::npos ? "pipeline_restart_failed"
                         : r.message.find("platform rejected") != std::string::npos ? "apply_failed" : "invalid_value";
        int status = !strcmp(code, "pipeline_restart_failed") ? 500 : !strcmp(code, "apply_failed") ? 500 : 422;
        Response resp = fail(status, code, first_rejected->path, r.message);
        resp.body.set("revision", Json::integer(store_.revision()));
        resp.body.set("changes", changes_json);
        return resp;
    }
    Json ok = Json::object(); ok.set("ok", Json::boolean(true)); ok.set("revision", Json::integer(store_.revision()));
    ok.set("lifecycle", Json::string(lifecycle_name_lc(pipeline_.state())));
    ok.set("changes", changes_json);
    return Response{200, ok};
}

}} // namespace machino::api
