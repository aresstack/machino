#include "app/api/api_service.hpp"
#include "core/log.hpp"
#include <algorithm>
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

ApiService::ApiService(power::PerformanceService& perf, lifecycle::PipelineManager& pipeline, ConfigStore& store,
                       EventBus& bus, const hw::ResolvedHardware& hw, const AppConfig& cfg)
    : perf_(perf), pipeline_(pipeline), store_(store), bus_(bus), hw_(hw), cfg_(cfg) {}

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
    j.set("video", v);
    Json ctl = Json::object();
    ctl.set("sensor_fps", range_control(c.sensor.fps));
    ctl.set("stream_fps", range_control(c.video.fps));
    ctl.set("bitrate", range_control(c.video.bitrate));
    ctl.set("isp_clock", perf_control(c.isp.performance));
    ctl.set("encoder_clock", perf_control(c.encoder.performance));
    ctl.set("cpu_frequency", perf_control(c.power.cpu_frequency));
    Json lg = Json::object(); lg.set("status", Json::string("supported")); lg.set("apply", Json::string("daemon_restart")); ctl.set("idle_grace_ms", lg);
    j.set("controls", ctl);
    Json profiles = Json::array(); for (const char* n : {"performance", "balanced", "battery", "custom"}) profiles.push(Json::string(n));
    j.set("profiles", profiles);
    Json vf = Json::array(); for (int f : perf_.verified_fps()) vf.push(Json::integer(f));
    j.set("verified_fps", vf);
    return j;
}
Response ApiService::capabilities() const { return Response{200, capabilities_json()}; }

Json ApiService::state_json() {
    lifecycle::Stats st = pipeline_.stats();
    power::EffectiveState e = perf_.effective_state();
    EffectiveStream s = pipeline_.stream();
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
    m.set("width", Json::integer(s.width)); m.set("height", Json::integer(s.height));
    m.set("encoder_active", Json::boolean(running));
    j.set("media", m);
    j.set("revision", Json::integer(store_.revision()));
    return j;
}
Response ApiService::state() { return Response{200, state_json()}; }

Json ApiService::config_json() {
    power::EffectiveState e = perf_.effective_state();
    EffectiveStream s = pipeline_.stream();
    Json j = Json::object();
    j.set("revision", Json::integer(store_.revision()));
    Json perf = Json::object(); perf.set("profile", Json::string(power::profile_name(e.profile))); j.set("performance", perf);
    Json sen = Json::object(); sen.set("fps", Json::integer(e.sensor_fps_requested)); j.set("sensor", sen);
    Json v0 = Json::object(); v0.set("fps", Json::integer(s.fps)); v0.set("bitrate_kbps", Json::integer(s.bitrate_kbps));
    v0.set("width", Json::integer(s.width)); v0.set("height", Json::integer(s.height)); v0.set("gop", Json::integer(s.gop));
    Json video = Json::object(); video.set("0", v0); j.set("video", video);
    Json lc = Json::object(); lc.set("idle_grace_ms", Json::integer(cfg_.pipeline.idle_grace_ms)); lc.set("always_on", Json::boolean(cfg_.pipeline.always_on)); j.set("lifecycle", lc);
    Json pw = Json::object(); pw.set("isp_performance", Json::string(power::perf_level_name(e.isp)));
    pw.set("encoder_performance", Json::string(power::perf_level_name(e.encoder))); pw.set("cpu_performance", Json::string(power::perf_level_name(e.cpu)));
    j.set("power", pw);
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
    m.set("bitrate_kbps_requested", Json::integer(t.requested_bitrate_kbps)); j.set("media", m);
    Json pw = Json::object(); pw.set("sensor_fps", opt(t.effective_sensor_fps)); pw.set("sensor_fps_requested", Json::integer(t.requested_sensor_fps));
    pw.set("isp_clock_hz", opt(t.isp_clock_hz)); pw.set("encoder_clock_hz", opt(t.encoder_clock_hz));
    pw.set("cpu_frequency_hz", t.cpu_freq_khz.available ? Json::number((double)t.cpu_freq_khz.value * 1000.0) : Json::null());
    pw.set("profile", Json::string(power::profile_name(t.profile)));
    j.set("power", pw);
    return j;
}
Response ApiService::telemetry() { return Response{200, telemetry_json()}; }

// ---- PATCH /config ----------------------------------------------------------
namespace {
struct Change { std::string path; Json requested; ApplyResult r; bool has_result = false; std::string key, value; };

bool get_int(const Json& v, long long& out) { if (!v.is_integer()) return false; out = v.as_int(); return true; }

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
                    else return bad(400, "unknown_field", d.path, "unknown field");
                    changes.push_back(d);
                }
                continue;
            } else if (s == "lifecycle" && kv.first == "idle_grace_ms") {
                long long n; if (!get_int(val, n) || n < 0 || n > 600000) return bad(422, "invalid_value", path, "idle_grace_ms must be an integer in 0..600000");
                c.key = "lifecycle.idle_grace_ms"; c.value = std::to_string(n);
            } else if (s == "power" && (kv.first == "isp_performance" || kv.first == "encoder_performance" || kv.first == "cpu_performance")) {
                if (!val.is_string()) return bad(422, "invalid_value", path, "level must be a string (auto|low|high)");
                PerfLevel l; if (!power::parse_perf_level(val.as_string(), l)) return bad(422, "invalid_value", path, "unknown level (auto|low|high)");
                c.key = "power." + kv.first; c.value = val.as_string();
            } else return bad(400, "unknown_field", path, "unknown field");
            changes.push_back(c);
        }
    }
    if (changes.empty()) return bad(400, "invalid_value", "", "no changes given");
    // profile first, then explicit values (explicit user values win over the profile)
    std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return (a.key == "performance.profile") > (b.key == "performance.profile"); });

    // ---- phase 2: apply through the services (the PipelineManager owns any restart)
    bool any_rejected = false; const Change* first_rejected = nullptr;
    for (auto& c : changes) {
        long long n = 0; if (c.requested.is_number()) n = c.requested.as_int();
        if (c.key == "performance.profile")      { Profile p; power::parse_profile(c.value, p); c.r = perf_.apply_profile(p); }
        else if (c.key == "sensor.fps")          c.r = perf_.set_sensor_fps((int)n);
        else if (c.key == "video.fps")           c.r = perf_.set_stream_fps((int)n);
        else if (c.key == "video.bitrate")       c.r = perf_.set_bitrate((int)n);
        else if (c.key == "lifecycle.idle_grace_ms") { c.r = ApplyResult::stored(ApplyMode::DaemonRestart, (int)n, "persisted; takes effect after daemon restart"); }
        else if (c.key == "power.isp_performance")     { PerfLevel l; power::parse_perf_level(c.value, l); c.r = perf_.set_isp_performance(l); }
        else if (c.key == "power.encoder_performance") { PerfLevel l; power::parse_perf_level(c.value, l); c.r = perf_.set_encoder_performance(l); }
        else if (c.key == "power.cpu_performance")     { PerfLevel l; power::parse_perf_level(c.value, l); c.r = perf_.set_cpu_performance(l); }
        c.has_result = true;
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
