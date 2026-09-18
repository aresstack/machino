#include "app/compat/majestic_webui.hpp"

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

    schema.set("properties", properties);

    Json groups = Json::array();
    const char* media_sections[] = {"video0", "sensor", "latency"};
    const char* image_sections[] = {"image"};
    const char* runtime_sections[] = {"performance", "lifecycle", "rtsp"};
    Json media = group("media", "Media", properties, media_sections, 3);
    Json image = group("image", "Image", properties, image_sections, 1);
    Json runtime = group("runtime", "Runtime", properties, runtime_sections, 3);
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

    if (const Json* video = native_config.get("video")) {
        if (const Json* v0 = video->get("0")) out.set("video0", *v0);
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
            name == "latency" || name == "rtsp" || name == "lifecycle" || name == "power") {
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

}} // namespace machino::compat
