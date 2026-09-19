#include "app/compat/majestic_webui.hpp"

#include <cstdio>
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

Json integer_field(const std::string& title, long long min, long long max) {
    Json f = integer_field(title);
    f.set("minimum", Json::integer(min));
    f.set("maximum", Json::integer(max));
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

bool add_range(Json& fields, const char* key, const char* title, const Json* cap) {
    if (!supported(cap)) return false;
    fields.set(key, integer_field(title, cap));
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
    add_section(properties, "video0", video0);

    Json sensor = Json::object();
    add_range(sensor, "fps", "Sensor frame rate", controls ? controls->get("sensor_fps") : nullptr);
    add_section(properties, "sensor", sensor);

    Json image_fields = Json::object();
    if (const Json* image = capabilities.get("image")) {
        if (image->is_object()) {
            for (const auto& kv : image->members()) {
                const Json& cap = kv.second;
                if (!supported(&cap)) continue;
                const Json* values = cap.get("values");
                if (values && values->is_array()) image_fields.set(kv.first, enum_field(title_for(kv.first), *values));
                else image_fields.set(kv.first, integer_field(title_for(kv.first), &cap));
            }
        }
    }
    add_section(properties, "image", image_fields);

    Json latency_fields = Json::object();
    if (const Json* latency = capabilities.get("latency")) {
        if (const Json* profiles = latency->get("profiles"); profiles && profiles->is_array())
            latency_fields.set("profile", enum_field("Latency profile", *profiles));
        add_range(latency_fields, "gop", "Keyframe interval (frames)", latency->get("gop"));
        add_range(latency_fields, "framesource_buffers", "FrameSource buffers", latency->get("framesource_buffers"));
        add_range(latency_fields, "encoder_buffers", "Encoder buffers", latency->get("encoder_buffers"));
        add_range(latency_fields, "consumer_queue_depth", "Consumer queue depth", latency->get("consumer_queue_depth"));
        add_range(latency_fields, "socket_send_buffer_bytes", "Socket send buffer (bytes)", latency->get("socket_send_buffer_bytes"));
        add_range(latency_fields, "send_stall_ms", "Send stall limit (ms)", latency->get("send_stall_ms"));
    }
    add_section(properties, "latency", latency_fields);

    Json performance = Json::object();
    if (const Json* profiles = capabilities.get("profiles"); profiles && profiles->is_array())
        performance.set("profile", enum_field("Performance profile", *profiles));
    add_section(properties, "performance", performance);

    Json lifecycle = Json::object();
    if (!controls || supported(controls->get("idle_grace_ms")))
        lifecycle.set("idle_grace_ms", integer_field("Idle grace period (ms)", 0, 600000));
    add_section(properties, "lifecycle", lifecycle);

    Json rtsp = Json::object();
    rtsp.set("max_clients", integer_field("Maximum RTSP clients", 1, 16));
    add_section(properties, "rtsp", rtsp);

    // M9/M10: detection is advertised only when the platform proved it.
    Json ai_fields = Json::object();
    if (const Json* ai = capabilities.get("ai"); ai && ai->is_object()) {
        const Json* avail = ai->get("available");
        if (avail && avail->is_string() && avail->as_string() == "supported") {
            ai_fields.set("enabled", bool_field("Enable detection"));
            if (const Json* dets = ai->get("detectors"); dets && dets->is_array() && dets->size() > 0)
                ai_fields.set("detector", enum_field("Detector", *dets));
            if (const Json* fps = ai->get("inference_fps"))
                ai_fields.set("inference_fps", integer_field("Inference rate (fps)", fps));
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
                char sz[32]; std::snprintf(sz, sizeof sz, "%lldx%lld", w, h);
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

    return out;
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

    Json patch = Json::object();
    for (const auto& sec : doc.members()) {
        const std::string& name = sec.first;
        const Json& value = sec.second;
        if (name == "revision") {
            patch.set(name, value);
            continue;
        }
        if (name == "video0") {
            if (!value.is_object()) {
                r.code = "unknown_field"; r.path = "video0"; r.message = "section must be an object"; return r;
            }
            Json video = Json::object();
            video.set("0", value);
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

Json majestic_sources(const Json& majestic_config) {
    Json arr = Json::array();
    const char* keys[] = {"video0", "video1"};
    for (int u = 0; u < 2; ++u) {
        const Json* vu = majestic_config.get(keys[u]);
        if (!vu || !vu->is_object()) continue;
        if (const Json* en = vu->get("enabled"); en && en->is_bool() && !en->as_bool()) continue;
        Json s = Json::object();
        s.set("id", Json::string(u == 0 ? "video0" : "video1"));
        s.set("camera", Json::integer(u + 1));   // 1-based; the WebUI keeps camera>0
        s.set("channel", Json::integer(u));
        s.set("name", Json::string(u == 0 ? "Main stream" : "Sub stream"));
        for (const char* k : {"codec", "size", "fps", "bitrate", "width", "height"})
            if (const Json* x = vu->get(k)) s.set(k, *x);
        arr.push(s);
    }
    Json out = Json::object();
    out.set("sources", arr);
    return out;
}

}} // namespace machino::compat
