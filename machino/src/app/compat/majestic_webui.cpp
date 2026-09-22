#include "app/compat/majestic_webui.hpp"
#include "core/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace machino { namespace compat {

namespace {

bool supported(const Json* cap) {
    if (!cap || !cap->is_object()) return false;
    const Json* status = cap->get("status");
    return status && status->is_string() && status->as_string() == "supported";
}

std::string title_for(std::string key) {
    for (char& c : key) if (c == '_') c = ' ';
    if (!key.empty() && key[0] >= 'a' && key[0] <= 'z') key[0] = (char)(key[0] - 'a' + 'A');
    return key;
}

Json integer_field(const std::string& title, const Json* cap = nullptr) {
    Json f = Json::object();
    f.set("type", Json::string("integer"));
    f.set("title", Json::string(title));
    if (cap) {
        const Json* min = cap->get("min");
        const Json* max = cap->get("max");
        if (min && min->is_number()) f.set("minimum", *min);
        if (max && max->is_number()) f.set("maximum", *max);
    }
    return f;
}

Json enum_field(const std::string& title, const Json& values) {
    Json f = Json::object();
    f.set("type", Json::string("string"));
    f.set("title", Json::string(title));
    f.set("enum", values);
    return f;
}

Json bool_field(const std::string& title) {
    Json f = Json::object();
    f.set("type", Json::string("boolean"));
    f.set("title", Json::string(title));
    return f;
}

// x-reload answers the stock settings page's question "is anything left to do
// AFTER a successful POST?" (mj-settings.js changeCost(): "none"/"live" = the
// save carried it, "pipeline" = the operator is owed Apply-now). Machino's
// PATCH applies EVERY exposed change inside the POST - including the ones
// whose internal class is pipeline_restart (the restart happens during the
// request). So every exposed field is "live" in Majestic semantics; offering
// Apply-now afterwards would prompt for work that is already done. Fields
// whose class is daemon_restart/boot_only are NOT exposed at all: their POST
// only persists, and the stock Apply (`killall -HUP majestic`) cannot restart
// the daemon to deliver them.
const char* xreload_for(const Json* cap) {
    const Json* a = cap ? cap->get("apply") : nullptr;
    const std::string cls = a && a->is_string() ? a->as_string() : "";
    if (cls == "daemon_restart" || cls == "boot_only") return nullptr;   // do not expose
    return "live";
}

bool add_range(Json& fields, const char* key, const char* title, const Json* cap) {
    if (!supported(cap)) return false;
    const char* xr = xreload_for(cap);
    if (!xr) return false;
    Json f = integer_field(title, cap);
    f.set("x-reload", Json::string(xr));
    fields.set(key, f);
    return true;
}

void add_section(Json& properties, const char* id, const Json& fields) {
    if (fields.members().empty()) return;
    Json s = Json::object();
    s.set("type", Json::string("object"));
    s.set("properties", fields);
    properties.set(id, s);
}

Json group(const char* id, const char* label, const Json& properties, const char* const* sections, size_t count) {
    Json g = Json::object();
    g.set("id", Json::string(id));
    g.set("label", Json::string(label));
    Json ss = Json::array();
    for (size_t i = 0; i < count; ++i)
        if (properties.get(sections[i])) ss.push(Json::string(sections[i]));
    g.set("sections", ss);
    return g;
}

void copy_if(const Json& src, Json& dst, const char* key) {
    if (const Json* v = src.get(key)) dst.set(key, *v);
}

} // namespace

bool compiled_default(const std::string& key, Json& out);   // defined with majestic_reset below

// Attach the shared compiled-in default to an already-built schema field, so
// the stock UI enables its reset button exactly where /api/v1/reset works.
static void set_default(Json& section, const char* section_name, const char* leaf) {
    Json dv;
    if (!compiled_default(std::string(section_name) + "." + leaf, dv)) return;
    const Json* f = section.get(leaf);
    if (!f || !f->is_object()) return;
    Json copy = *f;
    copy.set("default", dv);
    section.set(leaf, copy);
}

Json majestic_schema(const Json& capabilities) {
    Json schema = Json::object();
    schema.set("$schema", Json::string("http://json-schema.org/draft-04/schema#"));
    schema.set("type", Json::string("object"));
    Json properties = Json::object();

    const Json* controls = capabilities.get("controls");

    Json video0 = Json::object();
    add_range(video0, "fps", "Frame rate", controls ? controls->get("stream_fps") : nullptr);
    add_range(video0, "bitrate_kbps", "Bitrate (kbit/s)", controls ? controls->get("bitrate") : nullptr);
    add_range(video0, "gop", "Keyframe interval (frames)", controls ? controls->get("gop") : nullptr);
    set_default(video0, "video0", "bitrate_kbps");
    set_default(video0, "video0", "gop");
    add_section(properties, "video0", video0);

    // AP6: the substream is deliberately NOT in this schema, and that follows
    // this file's own rule rather than being an omission.
    //
    // xreload_for() refuses to expose anything of daemon_restart class,
    // because the stock Apply is `killall -HUP majestic` and cannot restart the
    // daemon to deliver it - a field the page can set but not make true is
    // worse than one it does not offer. video.1.* is exactly that class: the
    // substream is built at daemon start, and PipelineManager::update_sub_stream
    // has no caller yet.
    //
    // An earlier version of this change DID add the section, with the same
    // "live" semantics as video0. That would have had the stock settings page
    // report a sub-stream change as applied when nothing had happened.
    // It is reachable through /api/v1/config, which reports it honestly as
    // persisted-until-restart, and through machino.conf.

    Json sensor = Json::object();
    add_range(sensor, "fps", "Sensor frame rate", controls ? controls->get("sensor_fps") : nullptr);
    add_section(properties, "sensor", sensor);

    Json image_fields = Json::object();
    if (const Json* image = capabilities.get("image")) {
        if (image->is_object()) {
            for (const auto& kv : image->members()) {
                const Json& cap = kv.second;
                if (!supported(&cap)) continue;
                const char* xr = xreload_for(&cap);
                if (!xr) continue;
                const Json* values = cap.get("values");
                Json f = (values && values->is_array()) ? enum_field(title_for(kv.first), *values)
                                                        : integer_field(title_for(kv.first), &cap);
                f.set("x-reload", Json::string(xr));
                image_fields.set(kv.first, f);
            }
        }
    }
    add_section(properties, "image", image_fields);

    Json latency_fields = Json::object();
    if (const Json* latency = capabilities.get("latency")) {
        if (const Json* profiles = latency->get("profiles"); profiles && profiles->is_array()) {
            Json lp = enum_field("Latency profile", *profiles);
            lp.set("x-reload", Json::string("live"));   // applied during the POST
            latency_fields.set("profile", lp);
        }
        add_range(latency_fields, "gop", "Keyframe interval (frames)", latency->get("gop"));
        add_range(latency_fields, "framesource_buffers", "FrameSource buffers", latency->get("framesource_buffers"));
        add_range(latency_fields, "encoder_buffers", "Encoder buffers", latency->get("encoder_buffers"));
        add_range(latency_fields, "consumer_queue_depth", "Consumer queue depth", latency->get("consumer_queue_depth"));
        add_range(latency_fields, "socket_send_buffer_bytes", "Socket send buffer (bytes)", latency->get("socket_send_buffer_bytes"));
        add_range(latency_fields, "send_stall_ms", "Send stall limit (ms)", latency->get("send_stall_ms"));
    }
    add_section(properties, "latency", latency_fields);

    Json performance = Json::object();
    if (const Json* profiles = capabilities.get("profiles"); profiles && profiles->is_array()) {
        Json pp = enum_field("Performance profile", *profiles);
        pp.set("x-reload", Json::string("live"));   // the operating-point switch runs during the POST
        performance.set("profile", pp);
    }
    add_section(properties, "performance", performance);

    // lifecycle.idle_grace_ms is daemon_restart-class: the stock Apply (a
    // SIGHUP) cannot deliver it, so it goes through add_range's class filter
    // and is only exposed if the platform ever reclassifies it.
    Json lifecycle = Json::object();
    add_range(lifecycle, "idle_grace_ms", "Idle grace period (ms)", controls ? controls->get("idle_grace_ms") : nullptr);
    add_section(properties, "lifecycle", lifecycle);

    // rtsp.max_clients is NOT exposed: its native class is DaemonRestart (the
    // running RtspServer holds a config copy that neither the POST nor a
    // SIGHUP updates), so the stock UI could never truthfully apply it.

    // M9/M10: detection is advertised only when the platform proved it.
    Json ai_fields = Json::object();
    if (const Json* ai = capabilities.get("ai"); ai && ai->is_object()) {
        const Json* avail = ai->get("available");
        if (avail && avail->is_string() && avail->as_string() == "supported") {
            // The detection subsystem declares itself live (caps ai.inference_fps
            // apply=live; the detector is started/stopped as a consumer).
            Json en = bool_field("Enable detection"); en.set("x-reload", Json::string("live"));
            ai_fields.set("enabled", en);
            if (const Json* dets = ai->get("detectors"); dets && dets->is_array() && dets->size() > 0) {
                Json de = enum_field("Detector", *dets); de.set("x-reload", Json::string("live"));
                ai_fields.set("detector", de);
            }
            if (const Json* fps = ai->get("inference_fps")) {
                Json ff = integer_field("Inference rate (fps)", fps);
                if (const char* xr = xreload_for(fps)) ff.set("x-reload", Json::string(xr));
                ai_fields.set("inference_fps", ff);
            }
            set_default(ai_fields, "ai", "enabled");
            set_default(ai_fields, "ai", "inference_fps");
        }
    }
    add_section(properties, "ai", ai_fields);

    schema.set("properties", properties);

    Json groups = Json::array();
    const char* media_sections[] = {"video0", "sensor", "latency"};
    const char* image_sections[] = {"image"};
    const char* runtime_sections[] = {"performance", "lifecycle", "rtsp", "ai"};
    Json media = group("media", "Media", properties, media_sections, 3);
    Json image = group("image", "Image", properties, image_sections, 1);
    Json runtime = group("runtime", "Runtime", properties, runtime_sections, 4);
    if (media.get("sections")->size()) groups.push(media);
    if (image.get("sections")->size()) groups.push(image);
    if (runtime.get("sections")->size()) groups.push(runtime);
    schema.set("x-groups", groups);
    return schema;
}

Json majestic_config(const Json& native_config, const Json& state) {
    Json out = Json::object();

    copy_if(native_config, out, "revision");
    copy_if(native_config, out, "performance");
    copy_if(native_config, out, "sensor");
    copy_if(native_config, out, "rtsp");
    copy_if(native_config, out, "ai");

    // majestic-webui's Dashboard/Streams read video0/video1 with the field
    // names size ("WxH"), codec, bitrate (kbit/s) and enabled, so alias the
    // native width/height/bitrate_kbps into those on top of the native fields.
    if (const Json* video = native_config.get("video")) {
        for (int u = 0; u < 2; ++u) {
            const Json* vu = video->get(std::to_string(u));
            if (!vu || !vu->is_object()) continue;
            Json v = *vu;
            long long w = 0, h = 0;
            if (const Json* x = vu->get("width");  x && x->is_number()) w = x->as_int();
            if (const Json* x = vu->get("height"); x && x->is_number()) h = x->as_int();
            if (w > 0 && h > 0) {
                char sz[48]; std::snprintf(sz, sizeof sz, "%lldx%lld", w, h);
                v.set("size", Json::string(sz));
            }
            if (const Json* br = vu->get("bitrate_kbps"); br && br->is_number()) v.set("bitrate", *br);
            if (const Json* codec = vu->get("codec"); codec && codec->is_string()) v.set("codec", *codec);
            else v.set("codec", Json::string("h264"));
            if (!v.get("enabled")) v.set("enabled", Json::boolean(true));
            out.set(u == 0 ? "video0" : "video1", v);
        }
    }

    Json image = Json::object();
    const Json* requested_image = native_config.get("image");
    const Json* effective_image = state.get("image");
    if (requested_image && requested_image->is_object()) {
        for (const auto& kv : requested_image->members()) {
            if (!kv.second.is_null()) image.set(kv.first, kv.second);
            else if (effective_image) {
                if (const Json* effective = effective_image->get(kv.first); effective && !effective->is_null())
                    image.set(kv.first, *effective);
            }
        }
    }
    if (!image.members().empty()) out.set("image", image);

    if (const Json* native_latency = native_config.get("latency")) {
        Json latency = Json::object();
        const Json* effective = native_latency->get("effective");
        const char* keys[] = {"profile", "gop", "framesource_buffers", "encoder_buffers",
                              "consumer_queue_depth", "socket_send_buffer_bytes", "send_stall_ms"};
        for (const char* key : keys) {
            const Json* value = native_latency->get(key);
            if (value && !value->is_null()) latency.set(key, *value);
            else if (effective) {
                if (const Json* fallback = effective->get(key); fallback && !fallback->is_null())
                    latency.set(key, *fallback);
            }
        }
        if (!latency.members().empty()) out.set("latency", latency);
    }

    if (const Json* native_lifecycle = native_config.get("lifecycle")) {
        Json lifecycle = Json::object();
        copy_if(*native_lifecycle, lifecycle, "idle_grace_ms");
        if (!lifecycle.members().empty()) out.set("lifecycle", lifecycle);
    }

    // Machino does not drive the IR-cut filter yet. To the stock WebUI an
    // ABSENT nightMode.irCutPin1 means "Majestic cannot move the IR-cut
    // filter" - a red hardware fault. Upstream ircut-check.js treats
    // irCut=="off" as "a decision, not a defect" (parked: with no pins
    // configured it raises NO finding at all), which is the truthful state
    // here. No GPIO pins are invented; real nightMode support comes from the
    // board profile later.
    {
        Json nm = Json::object();
        nm.set("irCut", Json::string("off"));
        out.set("nightMode", nm);
    }

    return out;
}

// The stock WebUI sends every form value as a STRING ("Values are always sent
// as strings; the camera coerces" - upstream docs/settings-page.md). Coerce
// whole-string integers/decimals and true/false back to native JSON types so
// the native validator sees 20, not "20". Real string fields (codec, detector,
// profile names) are never purely numeric, so they pass through untouched.
// Json exposes const traversal only, so this is a pure copy-transform.
static Json coerce_strings(const Json& v) {
    if (v.is_object()) {
        Json o = Json::object();
        for (const auto& m : v.members()) o.set(m.first, coerce_strings(m.second));
        return o;
    }
    if (v.is_array()) {
        Json a = Json::array();
        for (size_t i = 0; i < v.size(); ++i) a.push(coerce_strings(v.at(i)));
        return a;
    }
    if (!v.is_string()) return v;
    const std::string& s = v.as_string();
    if (s == "true")  return Json::boolean(true);
    if (s == "false") return Json::boolean(false);
    if (s.empty()) return v;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return v;
    bool digits = true, dot = false;
    for (size_t k = i; k < s.size(); ++k) {
        if (s[k] == '.' && !dot && k > i && k + 1 < s.size()) { dot = true; continue; }
        if (s[k] < '0' || s[k] > '9') { digits = false; break; }
    }
    if (!digits) return v;
    if (dot) return Json::number(strtod(s.c_str(), nullptr));
    return Json::integer(strtoll(s.c_str(), nullptr, 10));
}

MajesticTranslation majestic_post_to_native(const std::string& body) {
    MajesticTranslation r;
    if (body.empty()) {
        r.code = "invalid_json"; r.message = "empty body"; return r;
    }

    Json doc; std::string err;
    if (!Json::parse(body, doc, err)) {
        r.code = "invalid_json"; r.message = err; return r;
    }
    if (!doc.is_object()) {
        r.code = "invalid_json"; r.message = "top-level value must be an object"; return r;
    }
    doc = coerce_strings(doc);

    Json patch = Json::object();
    for (const auto& sec : doc.members()) {
        const std::string& name = sec.first;
        const Json& value = sec.second;
        if (name == "revision") {
            patch.set(name, value);
            continue;
        }
        if (name == "video0" || name == "video1") {
            if (!value.is_object()) {
                r.code = "unknown_field"; r.path = name; r.message = "section must be an object"; return r;
            }
            // Both units land under the same "video" object, so a page that
            // sends video0 AND video1 in one POST must not have the second
            // overwrite the first.
            Json video;
            if (const Json* existing = patch.get("video"); existing && existing->is_object()) video = *existing;
            else video = Json::object();
            video.set(name == "video0" ? "0" : "1", value);
            patch.set("video", video);
            continue;
        }
        if (name == "performance" || name == "sensor" || name == "image" ||
            name == "latency" || name == "rtsp" || name == "lifecycle" || name == "power" || name == "ai") {
            patch.set(name, value);
            continue;
        }
        r.code = "unknown_field";
        r.path = name;
        r.message = "unknown majestic-webui section";
        return r;
    }

    r.ok = true;
    r.status = 200;
    r.patch = patch;
    return r;
}

namespace {

std::string num(double d) { char b[64]; std::snprintf(b, sizeof b, "%.10g", d); return b; }

// Read telemetry[section][key] as a number; returns false when absent/null so
// the metric is simply omitted (an absent value must not print as 0).
bool tel_num(const Json& root, const char* section, const char* key, double& out) {
    const Json* s = root.get(section);
    if (!s || !s->is_object()) return false;
    const Json* v = s->get(key);
    if (!v || !v->is_number()) return false;
    out = v->as_number();
    return true;
}

// state.media.streams["<u>"].total_bytes
bool stream_bytes(const Json& state, const char* unit, double& out) {
    const Json* m = state.get("media"); if (!m || !m->is_object()) return false;
    const Json* s = m->get("streams");  if (!s || !s->is_object()) return false;
    const Json* u = s->get(unit);       if (!u || !u->is_object()) return false;
    const Json* v = u->get("total_bytes");
    if (!v || !v->is_number()) return false;
    out = v->as_number();
    return true;
}

} // namespace

std::string majestic_metrics(const Json& telemetry, const Json& state, const LinuxSample& lin) {
    std::ostringstream o;
    auto g = [&](const char* name, double v) { o << name << ' ' << num(v) << '\n'; };

    // --- clock / uptime -----------------------------------------------------
    g("node_time_seconds", lin.now_unix);
    if (lin.have_boot)     g("node_boot_time_seconds", lin.boot_unix);
    if (lin.have_app_boot) g("app_boot_time_seconds", lin.app_boot_unix);
    if (lin.have_load) { g("node_load1", lin.load1); g("node_load5", lin.load5); g("node_load15", lin.load15); }

    // --- memory (node-exporter meminfo names, bytes) ------------------------
    if (lin.have_mem) {
        g("node_memory_MemTotal_bytes",       (double)lin.mem_total);
        g("node_memory_MemFree_bytes",        (double)lin.mem_free);
        g("node_memory_MemAvailable_bytes",   (double)lin.mem_avail);
        g("node_memory_SReclaimable_bytes",   (double)lin.mem_sreclaim);
        g("node_memory_Active_file_bytes",    (double)lin.mem_active_file);
        g("node_memory_Inactive_file_bytes",  (double)lin.mem_inactive_file);
    }

    // --- SoC temperature (real hardware value or nothing) -------------------
    if (lin.have_temp) g("node_hwmon_temp_celsius", lin.temp_c);

    // --- CPU (jiffies/USER_HZ -> seconds; the WebUI only needs idle/total) --
    for (const auto& c : lin.cpus) {
        const std::string cpu = std::to_string(c.index);
        auto cline = [&](const char* mode, uint64_t j) {
            o << "node_cpu_seconds_total{cpu=\"" << cpu << "\",mode=\"" << mode << "\"} "
              << num((double)j / 100.0) << '\n';
        };
        cline("user", c.user);    cline("nice", c.nice);   cline("system", c.system);
        cline("idle", c.idle);    cline("iowait", c.iowait); cline("irq", c.irq);
        cline("softirq", c.softirq); cline("steal", c.steal);
    }

    // --- network (per non-loopback interface) -------------------------------
    for (const auto& n : lin.nets) {
        o << "node_network_receive_bytes_total{device=\""  << n.dev << "\"} " << num((double)n.rx) << '\n';
        o << "node_network_transmit_bytes_total{device=\"" << n.dev << "\"} " << num((double)n.tx) << '\n';
    }

    // --- ISP / AE (from Machino telemetry.exposure + power) -----------------
    double v = 0;
    if (tel_num(telemetry, "exposure", "luma", v))             g("isp_avelum", v);
    if (tel_num(telemetry, "exposure", "analog_gain", v))      g("isp_again", v);
    if (tel_num(telemetry, "exposure", "digital_gain", v))     g("isp_dgain", v);
    if (tel_num(telemetry, "exposure", "isp_digital_gain", v)) g("isp_ispdgain", v);
    if (tel_num(telemetry, "exposure", "integration_time", v)) g("isp_exptime", v);
    if (tel_num(telemetry, "power", "sensor_fps", v))          g("isp_fps", v);

    // --- encoder throughput (monotonic byte counters, one per stream) -------
    if (stream_bytes(state, "0", v)) g("venc0_rcvd_bytes", v);
    if (stream_bytes(state, "1", v)) g("venc1_rcvd_bytes", v);

    return o.str();
}

// Wire format pinned by upstream tests/sources.test.js ("copied from a camera,
// not from what this UI wishes were there"): subtype is a NAME, id is
// 3*camera+subtypeIndex, `flowing` appears on h264 streams only, mjpeg
// streams carry rtsp:false. Machino has no JPEG stream (deliberately off on
// the T40NN), so only main/sub are listed.
Json majestic_sources(const Json& majestic_config, const Json& state) {
    bool encoder_active = false;
    if (const Json* med = state.get("media"))
        if (const Json* ea = med->get("encoder_active"))
            if (ea->is_bool()) encoder_active = ea->as_bool();

    Json streams = Json::array();
    const struct { const char* section; int subtype; const char* name; } units[] = {
        {"video0", 0, "main"}, {"video1", 1, "sub"},
    };
    for (const auto& u : units) {
        const Json* vu = majestic_config.get(u.section);
        if (!vu || !vu->is_object()) continue;
        if (const Json* en = vu->get("enabled"); en && en->is_bool() && !en->as_bool()) continue;
        Json s = Json::object();
        s.set("id", Json::integer(u.subtype));           // camera 0: id == subtype index
        s.set("subtype", Json::string(u.name));
        const Json* codec = vu->get("codec");
        s.set("codec", codec && codec->is_string() ? *codec : Json::string("h264"));
        for (const char* k : {"fps", "width", "height"})
            if (const Json* x = vu->get(k); x && x->is_number()) s.set(k, *x);
        // h264 only; "absent" would mean MJPEG semantics. Only the main unit's
        // liveness is known (media.encoder_active); the sub unit reports false
        // until it runs, which is exactly what the fixture shows for an idle
        // second stream.
        s.set("flowing", Json::boolean(u.subtype == 0 ? encoder_active : false));
        s.set("configured", Json::boolean(true));
        s.set("present", Json::boolean(true));
        s.set("rtsp", Json::boolean(true));
        streams.push(s);
    }
    Json sensor = Json::object();
    sensor.set("camera", Json::integer(0));
    sensor.set("kind", Json::string("sensor"));
    sensor.set("streams", streams);
    Json arr = Json::array(); arr.push(sensor);
    Json out = Json::object(); out.set("sources", arr);
    return out;
}

// GET /api/v1/get?key= - plain-text value of a dotted key in the flattened
// majestic document; miss (absent or null) = 404, exactly what the stock
// mj_cfg() in www/cgi-bin/p/majestic.sh distinguishes.
bool majestic_get(const Json& majestic_config, const std::string& key, std::string& out_text) {
    const Json* v = &majestic_config;
    size_t p = 0;
    while (p <= key.size()) {
        size_t dot = key.find('.', p);
        if (dot == std::string::npos) dot = key.size();
        if (dot == p || !v->is_object()) return false;
        v = v->get(key.substr(p, dot - p));
        if (!v) return false;
        p = dot + 1;
    }
    if (v->is_null()) return false;
    if (v->is_string()) { out_text = v->as_string(); return true; }
    out_text = v->dump();     // numbers/bools print bare; objects/arrays as JSON
    return true;
}

// ONE source of truth for the schema's "default" fields AND /api/v1/reset.
// Upstream contract (docs/settings-page.md): "Reset is disabled where the
// schema declares no `default`" and a reset 404 disables the button as "no
// such setting". So: every key that declares a default here MUST reset, and
// keys without a fixed compiled-in default (the fps values are "follow the
// sensor mode") declare none - the stock UI then never calls reset for them.
bool compiled_default(const std::string& key, Json& out) {
    static const AppConfig def;
    if (key == "video0.bitrate_kbps")     { out = Json::integer(def.video.bitrate_kbps); return true; }
    if (key == "video0.gop")              { out = Json::integer(def.video.gop); return true; }
    if (key == "ai.enabled")              { out = Json::boolean(def.ai.enabled); return true; }
    if (key == "ai.inference_fps")        { out = Json::integer(def.ai.inference_fps); return true; }
    return false;
}

// GET /api/v1/reset?key= - the CURRENT mj-settings.js contract (#416): a key
// whose schema declares a default goes back to that value; a key it declares
// NONE for is REMOVED, returning the camera to the unconfigured state; 404
// means the camera has no such setting at all.
MajesticTranslation majestic_reset(const std::string& key) {
    Json dv;
    size_t dot = key.find('.');
    if (dot != std::string::npos && compiled_default(key, dv)) {
        Json leaf = Json::object(); leaf.set(key.substr(dot + 1), dv);
        Json top = Json::object();  top.set(key.substr(0, dot), leaf);
        return majestic_post_to_native(top.dump());
    }

    // No default: map the schema-exposed majestic key to the machino.conf
    // line(s) whose REMOVAL is the unset state. Only keys the schema can
    // actually expose appear here; everything else is honestly 404.
    static const struct { const char* mkey; const char* conf; } UNSET[] = {
        {"video0.fps",                  "video.fps"},
        {"sensor.fps",                  "sensor.fps"},
        {"performance.profile",         "performance.profile"},
        {"latency.profile",             "latency.profile"},
        {"latency.framesource_buffers", "latency.framesource_buffers"},
        {"latency.encoder_buffers",     "latency.encoder_buffers"},
        {"latency.consumer_queue_depth","latency.queue_depth"},
        {"ai.detector",                 "ai.detector"},
    };
    // image.* controls: the conf key mirrors the majestic key; the name list
    // mirrors media::ImageControl (an unknown name must stay 404).
    static const char* IMAGE_KEYS[] = {
        "brightness", "contrast", "saturation", "sharpness", "hue",
        "hflip", "vflip", "anti_flicker", "ae_compensation", "highlight_depress",
        "backlight_comp", "white_balance_mode", "running_mode",
        "temporal_nr", "spatial_nr", "dpc", "defog",
    };

    MajesticTranslation r;
    for (const auto& u : UNSET)
        if (key == u.mkey) { r.ok = true; r.status = 200; r.unset.push_back(u.conf); return r; }
    if (key.rfind("image.", 0) == 0) {
        const std::string leaf = key.substr(6);
        for (const char* k : IMAGE_KEYS)
            if (leaf == k) { r.ok = true; r.status = 200; r.unset.push_back("image." + leaf); return r; }
    }
    r.status = 404; r.code = "unknown_field"; r.path = key;
    r.message = "no such setting";
    return r;
}

}} // namespace machino::compat
