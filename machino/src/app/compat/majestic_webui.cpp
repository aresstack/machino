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

// W1: majestic <-> nativ fuer die drei umbenannten Image-Keys (eine Tabelle).
static const struct { const char* maj; const char* nat; bool boolean; } kImageAlias[] = {
    {"luminance", "brightness", false},
    {"mirror",    "hflip",      true},
    {"flip",      "vflip",      true},
};
const char* majestic_image_native(const std::string& majestic_key, bool* is_bool) {
    for (const auto& a : kImageAlias)
        if (majestic_key == a.maj) { if (is_bool) *is_bool = a.boolean; return a.nat; }
    return nullptr;
}
const char* majestic_image_alias(const std::string& native_key, bool* is_bool) {
    for (const auto& a : kImageAlias)
        if (native_key == a.nat) { if (is_bool) *is_bool = a.boolean; return a.maj; }
    return nullptr;
}

// W2: majestic nightMode.* <-> nativ night.* (Day / Night-Sektion).
static const struct { const char* maj; const char* nat; bool boolean; } kNightAlias[] = {
    {"irCut",             "ircut",               true},
    {"irCutPin1",         "ircut_pin1",          false},
    {"irCutPin2",         "ircut_pin2",          false},
    {"irCutSingleInvert", "ircut_single_invert", true},
    {"backlight",         "backlight",           true},
    {"backlightPin",      "backlight_pin",       false},
    {"lightSensorPin",    "light_sensor_pin",    false},
    {"lightSensorInvert", "light_sensor_invert", true},
    {"lightMonitor",      "light_monitor",       true},
    {"colorToGray",       "color_to_gray",       true},
    {"autoNightDelay",    "auto_night_delay",    false},
    {"autoDayDelay",      "auto_day_delay",      false},
};

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

    // AP9: the substream. mj-settings.js names exactly four sections -
    // 'image', 'sensor', 'video0', 'video1' - so upstream EXPECTS this one;
    // leaving it out is the deviation, not adding it.
    //
    // But NOT as "live". Unlike video0, a sub-stream change is not carried by
    // the POST: the unit is (re)built from the reloaded config. x-reload
    // "pipeline" is exactly that contract - mj-settings.js then tells the
    // operator "After Save, a reload restarts the video streams" and offers
    // Apply, whose SIGHUP main.cpp now answers by reconfiguring the unit.
    //
    // Both halves were needed. An earlier attempt exposed this as "live",
    // which would have reported a change as applied when nothing happened;
    // pulling it back out then hid a section upstream asks for.
    Json video1 = Json::object();
    add_range(video1, "fps", "Sub-stream frame rate", controls ? controls->get("stream_fps") : nullptr);
    add_range(video1, "bitrate_kbps", "Sub-stream bitrate (kbit/s)", controls ? controls->get("bitrate") : nullptr);
    add_range(video1, "gop", "Sub-stream keyframe interval (frames)", controls ? controls->get("gop") : nullptr);
    for (const char* k : {"fps", "bitrate_kbps", "gop"})
        if (const Json* f = video1.get(k); f && f->is_object()) {
            Json copy = *f;
            copy.set("x-reload", Json::string("pipeline"));
            video1.set(k, copy);
        }
    add_section(properties, "video1", video1);

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
                // W1: die Stock-UI haengt Features an majestic-NAMEN und
                // -TYPEN: der Tone-Strip und der Stock-Knopf kennen
                // "luminance" (nicht brightness), das Orientation-Pad
                // erscheint nur fuer BOOLSCHE mirror/flip. Also spricht das
                // Schema majestic; die native Flaeche bleibt unveraendert.
                bool as_bool = false;
                const char* alias = majestic_image_alias(kv.first, &as_bool);
                const std::string key = alias ? alias : kv.first;
                const Json* values = cap.get("values");
                Json f;
                if (as_bool) {
                    f = Json::object();
                    f.set("type", Json::string("boolean"));
                    f.set("title", Json::string(title_for(key)));
                } else if (values && values->is_array()) {
                    f = enum_field(title_for(key), *values);
                } else {
                    f = integer_field(title_for(key), &cap);
                }
                f.set("x-reload", Json::string(xr));
                // AP10: x-live is what makes the settings page push this knob
                // to POST /api/v1/image as it is dragged, instead of only on
                // Save. Without it the sliders move and the picture does not,
                // which is the gap the acceptance recorded as the one visible
                // drop-in deviation.
                f.set("x-live", Json::boolean(true));
                image_fields.set(key, f);
            }
        }
    }
    // W1: Defaults fuer die vier Tone-Knoepfe -- 128 ist der dokumentierte
    // IMP-Neutralwert (0..255, Mitte = keine Verschiebung), also derselbe
    // Bildzustand wie "unset auf Tuning-Bin-Default". Erst damit tun der
    // Stock-Knopf und die Reihen-Resets der Stock-UI etwas.
    for (const char* k : {"luminance", "contrast", "saturation", "hue"})
        set_default(image_fields, "image", k);
    add_section(properties, "image", image_fields);

    // W2: die Day / Night-Sektion (Stock-Label "nightMode"). Nur wenn dieser
    // Build den Dienst hat -- eine Plattform ohne ihn bekaeme sonst eine
    // Seite voller toter Felder. Der IR-Cut/Licht-Schalter der Live-Seite
    // liest seine Freigabe (und der Tooltip seinen Text) aus genau irCut/
    // backlight. Die Automatik ist die des Fotosensors (lightMonitor +
    // lightSensorPin + autoNight/DayDelay); die Schwellen fuer eine Automatik
    // aus der Sensorverstaerkung (minThreshold/maxThreshold, autoNight/DayGain)
    // fehlen ABSICHTLICH: die ist nicht implementiert, und ein Feld, das
    // nichts tut, ist eine Fake-Capability.
    if (const Json* night = capabilities.get("night")) {
        const Json* av = night->get("available");
        if (av && av->is_bool() && av->as_bool()) {
            auto boolf = [](const char* title) {
                Json f = Json::object();
                f.set("type", Json::string("boolean"));
                f.set("title", Json::string(title));
                return f;
            };
            auto strf = [](const char* title) {
                Json f = Json::object();
                f.set("type", Json::string("string"));
                f.set("title", Json::string(title));
                return f;
            };
            Json nf = Json::object();
            nf.set("irCut", boolf("IR-cut filter"));
            nf.set("irCutPin1", strf("IR-cut pin 1 (e.g. PB18)"));
            nf.set("irCutPin2", strf("IR-cut pin 2 (empty = single-pin filter)"));
            nf.set("irCutSingleInvert", boolf("Invert single-pin level"));
            nf.set("backlight", boolf("Camera light"));
            nf.set("backlightPin", strf("Light pin"));
            nf.set("lightSensorPin", strf("Daylight sensor pin (photocell)"));
            nf.set("lightSensorInvert", boolf("Invert daylight sensor level"));
            nf.set("lightMonitor", boolf("Automatic day/night (daylight sensor)"));
            nf.set("colorToGray", boolf("Colorless night mode"));
            auto secf = [](const char* title) {
                Json f = Json::object();
                f.set("type", Json::string("integer"));
                f.set("title", Json::string(title));
                f.set("minimum", Json::integer(0));
                f.set("maximum", Json::integer(3600));
                return f;
            };
            nf.set("autoNightDelay", secf("Seconds of darkness before night"));
            nf.set("autoDayDelay", secf("Seconds of daylight before day"));
            add_section(properties, "nightMode", nf);
        }
    }

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
    Json motion_fields = Json::object();
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

            // Die ORIGINAL-Seite "Motion detection" der Stock-WebUI (Gruppe
            // "Events", wie bei majestic). Nur sie zeigt, was die Detektoren
            // sehen: Livebild plus Kaestchen aus /ws/analytics. Die WebUI
            // rendert sie, sobald das Schema diesen Abschnitt traegt -- ohne
            // ihn gab es unter machino keinen Ort, an dem eine erkannte Person
            // sichtbar wird (2026-10-01). Die Felder sind DIESELBEN Schalter
            // wie ai.enabled / ai.detector (eine Wahrheit, zwei Seiten).
            //
            // roi muss dabei sein, auch leer: erst mit diesem Feld fragt die
            // Seite /api/v1/osd nach der Stream-Geometrie, und ohne die zeichnet
            // ihr Overlay keine Box ("do not draw yet"). Bereiche selbst kann
            // machino noch nicht: es beobachtet das ganze Bild, und ein Save mit
            // Bereichen wird mit genau diesem Satz abgelehnt.
            Json md = Json::object();
            Json mden = bool_field("Enable"); mden.set("x-reload", Json::string("live"));
            md.set("enabled", mden);
            if (const Json* de = ai_fields.get("detector")) md.set("detector", *de);
            Json roi = Json::object();
            roi.set("type", Json::string("array"));
            roi.set("title", Json::string("Region of interest"));
            roi.set("default", Json::array());
            roi.set("hint", Json::string("machino watches the whole picture; regions are not supported yet."));
            md.set("roi", roi);
            set_default(md, "motionDetect", "enabled");
            motion_fields = md;
        }
    }
    add_section(properties, "ai", ai_fields);
    if (motion_fields.is_object() && motion_fields.size() > 0)
        add_section(properties, "motionDetect", motion_fields);

    // Audio: microphone and speaker, each only when the platform has it.
    // Every field applies inside the POST (srate at the next open of the
    // device, which is when a rate can change at all), so all of them are
    // "live" in majestic's sense.
    Json audio_fields = Json::object();
    if (const Json* au = capabilities.get("audio"); au && au->is_object()) {
        const Json* outc = au->get("output");
        if (outc && outc->is_bool() && outc->as_bool()) {
            Json oe = bool_field("Enable speaker"); oe.set("x-reload", Json::string("live"));
            audio_fields.set("outputEnabled", oe);
            Json vr = Json::object(); vr.set("min", Json::integer(0)); vr.set("max", Json::integer(100));
            Json ov = integer_field("Speaker volume", &vr); ov.set("x-reload", Json::string("live"));
            audio_fields.set("outputVolume", ov);
        }
        const Json* in = au->get("input");
        if (in && in->is_bool() && in->as_bool()) {
            Json en = bool_field("Enable microphone"); en.set("x-reload", Json::string("live"));
            audio_fields.set("enabled", en);
            Json sr = Json::object();
            sr.set("type", Json::string("integer"));
            sr.set("title", Json::string("Sample rate (Hz)"));
            if (const Json* rates = au->get("sample_rates")) sr.set("enum", *rates);
            sr.set("x-reload", Json::string("live"));
            audio_fields.set("srate", sr);
            Json vr = Json::object(); vr.set("min", Json::integer(0)); vr.set("max", Json::integer(100));
            Json vol = integer_field("Microphone volume", &vr); vol.set("x-reload", Json::string("live"));
            audio_fields.set("volume", vol);
        }
    }
    add_section(properties, "audio", audio_fields);

    // The dashboard's camera tile. Two settings, both live:
    //   jpeg.enabled           the JPEG snapshot path (OpenIPC's own tile,
    //                          /image.jpg, /snapshot). Off by default on the
    //                          T40NN: building that encoder wedged the daemon
    //                          (machino-t40nn-jpeg-wedge). The title says so;
    //                          the switch is the operator's.
    //   webui.dashboard_preview auto = OpenIPC's way (JPEG if enabled, else
    //                          the notice), live = a muted stream player in
    //                          the tile (no JPEG needed), off = tile dark.
    Json jpeg_fields = Json::object();
    if (const Json* jc = capabilities.get("jpeg"); jc && jc->is_object()) {
        const Json* st = jc->get("status");
        if (st && st->is_string() && st->as_string() == "supported") {
            Json en = bool_field("JPEG snapshots (dashboard tile, /image.jpg) - WARNING: on the T40NN the JPEG encoder has wedged the daemon until power-cycle; keep off unless verified on your camera");
            en.set("x-reload", Json::string("live"));
            jpeg_fields.set("enabled", en);
            set_default(jpeg_fields, "jpeg", "enabled");
        }
    }
    add_section(properties, "jpeg", jpeg_fields);
    Json webui_fields = Json::object();
    {
        Json modes = Json::array();
        for (const char* m : {"auto", "live", "off"}) modes.push(Json::string(m));
        Json pv = enum_field("Dashboard preview (auto = JPEG snapshot when enabled, live = stream player in the tile, off = none)", modes);
        pv.set("x-reload", Json::string("live"));
        webui_fields.set("dashboard_preview", pv);
        set_default(webui_fields, "webui", "dashboard_preview");
    }
    add_section(properties, "webui", webui_fields);

    schema.set("properties", properties);

    Json groups = Json::array();
    const char* media_sections[] = {"video0", "video1", "sensor", "latency"};
    const char* image_sections[] = {"image", "nightMode"};   // W2: Day / Night neben Image
    const char* runtime_sections[] = {"performance", "lifecycle", "rtsp", "ai", "audio", "jpeg", "webui"};
    Json media = group("media", "Media", properties, media_sections, 4);
    Json image = group("image", "Image", properties, image_sections, 2);
    Json runtime = group("runtime", "Runtime", properties, runtime_sections, 7);
    const char* events_sections[] = {"motionDetect"};      // wie majestic: eigene Gruppe "Events"
    Json events = group("events", "Events", properties, events_sections, 1);
    if (media.get("sections")->size()) groups.push(media);
    if (image.get("sections")->size()) groups.push(image);
    if (events.get("sections")->size()) groups.push(events);
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
    // Die Motion-Seite liest dieselben Werte unter majestics Namen.
    if (const Json* ai = native_config.get("ai"); ai && ai->is_object()) {
        Json md = Json::object();
        if (const Json* e = ai->get("enabled"))  md.set("enabled", *e);
        if (const Json* d = ai->get("detector")) md.set("detector", *d);
        md.set("roi", Json::array());
        out.set("motionDetect", md);
    }

    // AP14: the snapshot gate, said out loud.
    //
    // dashboard.js polls /image.jpg every 5 s, but ONLY after checking the
    // config first:
    //
    //   // /image.jpg is the independent JPEG channel - jpeg.enabled is the
    //   // only gate. A camera streaming sub-only still has its snapshot.
    //   if (mjGet(cfg, 'jpeg.enabled') !== true) {
    //       off.textContent = 'Snapshots are disabled - open Live for video';
    //       return;
    //   }
    //
    // With no jpeg section at all the tile behaved correctly by ACCIDENT -
    // undefined is not true - but the gate was invisible, and a camera that
    // later enabled JPEG would still never be polled. Reporting the real value
    // makes the page's own message the honest one, and keeps the tile from
    // requesting a path this camera must not be asked for: the T40NN JPEG
    // encoder wedges the whole daemon (machino-t40nn-jpeg-wedge), which is why
    // jpeg.enabled defaults to false here.
    // webui.dashboard_preview ist die EXKLUSIVE Wahl des Betreibers, wie das
    // Dashboard-Tile seine Vorschau bekommt; das Gate der Seite ist
    // jpeg.enabled, also meldet die majestic-Sicht den Schalter, auf den das
    // Dashboard reagieren soll:
    //   auto -> echter Wert (OpenIPCs eigener JPEG-Weg, der Default)
    //   live -> false: der injizierte Stream-Player IST die Vorschau; die
    //           Seite darf /image.jpg nicht zusaetzlich pollen (live ersetzt
    //           JPEG, es kombiniert nicht)
    //   off  -> false: Tile dunkel, auch bei JPEG an
    // Das native /api/v1/config behaelt in allen Faellen den rohen Wert.
    copy_if(native_config, out, "webui");
    const Json* wb = native_config.get("webui");
    const Json* pv = wb && wb->is_object() ? wb->get("dashboard_preview") : nullptr;
    const std::string pvm = pv && pv->is_string() ? pv->as_string() : std::string("auto");
    const bool tile_no_jpeg = (pvm == "off" || pvm == "live");
    if (const Json* j = native_config.get("jpeg"); j && j->is_object()) {
        Json jp = *j;
        if (tile_no_jpeg) jp.set("enabled", Json::boolean(false));
        out.set("jpeg", jp);
    } else {
        Json jp = Json::object();
        jp.set("enabled", Json::boolean(false));
        out.set("jpeg", jp);
    }

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
            // W1: die View spricht majestic -- luminance statt brightness,
            // mirror/flip als Boolean statt hflip/vflip 0/1 (dasselbe
            // Vokabular wie das Schema, sonst findet die Seite ihre Werte
            // nicht wieder).
            bool as_bool = false;
            const char* alias = majestic_image_alias(kv.first, &as_bool);
            const std::string key = alias ? alias : kv.first;
            const Json* v = nullptr;
            if (!kv.second.is_null()) v = &kv.second;
            else if (effective_image) {
                if (const Json* eff = effective_image->get(kv.first); eff && !eff->is_null()) v = eff;
            }
            if (!v) continue;
            if (as_bool && v->is_number()) image.set(key, Json::boolean(v->as_int() != 0));
            else                           image.set(key, *v);
        }
    }
    if (!image.members().empty()) out.set("image", image);

    // W2: night.* (nativ) -> nightMode.* (majestic-View, dieselben Namen wie
    // das Schema, sonst findet die Seite ihre Werte nicht).
    if (const Json* native_night = native_config.get("night");
        native_night && native_night->is_object()) {
        Json nm = Json::object();
        for (const auto& a : kNightAlias)
            if (const Json* v = native_night->get(a.nat)) nm.set(a.maj, *v);
        if (!nm.members().empty()) out.set("nightMode", nm);
    }

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

    // W2: der NightService faehrt den Filter jetzt wirklich; die echte
    // Sektion (oben, aus night.*) hat Vorrang. NUR wenn dieser Build keinen
    // Night-Dienst hat (kein native "night"-Abschnitt), bleibt der geparkte
    // Ehrlichkeitszustand von frueher: irCut=="off" heisst fuer upstreams
    // ircut-check.js "eine Entscheidung, kein Defekt" -- ein ABSENTES
    // nightMode.irCutPin1 wuerde dagegen als roter Hardwarefehler gelesen.
    if (!out.get("nightMode")) {
        Json nm = Json::object();
        nm.set("irCut", Json::string("off"));
        out.set("nightMode", nm);
    }

    // Audio. upstream's audio-check.js treats silence and "off" as different
    // answers and says why in its own words:
    //
    //   "Absent is not false. A camera that never sent the key has not said
    //    its microphone is off - it has said nothing - and a panel that turns
    //    silence into 'switched off' sends somebody looking for a control to
    //    change that may not even be there."
    //
    // So both switches are ALWAYS reported: from the native section when this
    // build has an audio service, false otherwise. The live player is safe
    // either way: preview.js drops to muted when the video init names no
    // audio codec, which /ws/video does not (yet).
    {
        Json au = Json::object();
        const Json* na = native_config.get("audio");
        if (na && na->is_object()) {
            copy_if(*na, au, "enabled");
            copy_if(*na, au, "srate");
            copy_if(*na, au, "volume");
            if (const Json* v = na->get("output_enabled")) au.set("outputEnabled", *v);
            if (const Json* v = na->get("output_volume")) au.set("outputVolume", *v);
        }
        if (!au.get("enabled")) au.set("enabled", Json::boolean(false));
        if (!au.get("outputEnabled")) au.set("outputEnabled", Json::boolean(false));
        out.set("audio", au);
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
        if (name == "image" && value.is_object()) {
            // W1: majestic-Vokabular zurueckuebersetzen -- luminance ->
            // brightness, mirror/flip (Boolean) -> hflip/vflip (0/1). Die
            // native Validierung bleibt strikt, uebersetzt wird NUR hier.
            Json native_img = Json::object();
            for (const auto& kv : value.members()) {
                bool as_bool = false;
                const char* nat = majestic_image_native(kv.first, &as_bool);
                const std::string key = nat ? nat : kv.first;
                if (as_bool && kv.second.is_bool())
                    native_img.set(key, Json::integer(kv.second.as_bool() ? 1 : 0));
                else
                    native_img.set(key, kv.second);
            }
            patch.set("image", native_img);
            continue;
        }
        if (name == "nightMode" && value.is_object()) {
            // W2: majestic nightMode.* -> nativ night.* (eine Tabelle).
            // Unbekannte Leafs laufen unveraendert weiter und scheitern in
            // der nativen Validierung MIT NAMEN.
            Json native_night = Json::object();
            for (const auto& kv : value.members()) {
                const char* nat = nullptr;
                for (const auto& a : kNightAlias)
                    if (kv.first == a.maj) { nat = a.nat; break; }
                native_night.set(nat ? nat : kv.first.c_str(), kv.second);
            }
            patch.set("night", native_night);
            continue;
        }
        // motionDetect (die Original-Seite "Motion detection") ist eine zweite
        // Sicht auf ai.*: enabled/detector landen dort, ZUSAMMEN mit einem
        // ai-Abschnitt im selben POST (keiner ueberschreibt den anderen).
        if (name == "motionDetect" || name == "ai") {
            if (!value.is_object()) {
                r.code = "unknown_field"; r.path = name; r.message = "section must be an object"; return r;
            }
            Json ai;
            if (const Json* existing = patch.get("ai"); existing && existing->is_object()) ai = *existing;
            else ai = Json::object();
            for (const auto& kv : value.members()) {
                if (name == "motionDetect") {
                    if (kv.first == "roi") {
                        if (kv.second.is_array() && kv.second.size() == 0) continue;   // ganzes Bild = Ist-Zustand
                        r.status = 422; r.code = "invalid_value"; r.path = "motionDetect.roi";
                        r.message = "machino watches the whole picture; regions are not supported yet";
                        return r;
                    }
                    if (kv.first != "enabled" && kv.first != "detector") {
                        r.code = "unknown_field"; r.path = "motionDetect." + kv.first;
                        r.message = "machino's motion page takes enabled, detector and an empty roi";
                        return r;
                    }
                }
                ai.set(kv.first, kv.second);
            }
            if (ai.size() > 0) patch.set("ai", ai);
            continue;
        }
        if (name == "performance" || name == "sensor" || name == "image" ||
            name == "latency" || name == "rtsp" || name == "lifecycle" || name == "power" ||
            name == "jpeg" || name == "webui") {
            patch.set(name, value);
            continue;
        }
        // (AP18/AP19-Ablehnung fuer nightMode entfernt: seit W2 faehrt der
        // NightService IR-Cut/Licht wirklich -- die Sektion wird oben
        // uebersetzt; ob der ZIEL-Build sie hat, entscheidet die native
        // Validierung mit "day/night is not wired on this platform".)
        // Audio: majestic's camelCase speaker keys onto the native ones; the
        // native validation decides (and names) everything else.
        if (name == "audio" && value.is_object()) {
            Json native_audio = Json::object();
            for (const auto& kv : value.members()) {
                const std::string key = kv.first == "outputEnabled" ? "output_enabled"
                                      : kv.first == "outputVolume"  ? "output_volume" : kv.first;
                native_audio.set(key, kv.second);
            }
            patch.set("audio", native_audio);
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
    // Bool, not number, in the telemetry -- and only published when measured.
    // The WebUI's video-check needs BOTH isp_avelum and isp_exposureismax to
    // trust the sensor; missing either, it convicts from the picture alone,
    // and a player stalled on a bad uplink then reads as a blind camera.
    if (const Json* ex = telemetry.get("exposure"); ex && ex->is_object())
        if (const Json* im = ex->get("is_max"); im && im->is_bool())
            g("isp_exposureismax", im->as_bool() ? 1 : 0);
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
    if (key == "motionDetect.enabled")    { out = Json::boolean(def.ai.enabled); return true; }
    if (key == "ai.inference_fps")        { out = Json::integer(def.ai.inference_fps); return true; }
    if (key == "jpeg.enabled")            { out = Json::boolean(def.jpeg.enabled); return true; }
    if (key == "webui.dashboard_preview") { out = Json::string(def.webui.dashboard_preview); return true; }
    // W1: die vier Tone-Knoepfe. 128 = dokumentierter IMP-Neutralwert
    // (SetBrightness/Contrast/Saturation/Hue, 0..255, Mitte = keine
    // Verschiebung) -- bildgleich mit dem Tuning-Bin-Default, aber ohne den
    // Pipeline-Restart des Unset-Pfads. Genau die Menge, die der
    // Stock-Knopf der Stock-UI zuruecksetzt (TONE_KEYS).
    if (key == "image.luminance" || key == "image.contrast" ||
        key == "image.saturation" || key == "image.hue") {
        out = Json::integer(128); return true;
    }
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
        // W1: das Schema spricht majestic (image.luminance); der native PATCH
        // braucht den nativen Leaf (image.brightness).
        std::string leaf_name = key.substr(dot + 1);
        if (key.rfind("image.", 0) == 0)
            if (const char* nat = majestic_image_native(leaf_name)) leaf_name = nat;
        Json leaf = Json::object(); leaf.set(leaf_name, dv);
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
        {"motionDetect.detector",       "ai.detector"},
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
        std::string leaf = key.substr(6);
        // W1: majestic-Vokabular auf den nativen conf-Key abbilden.
        if (const char* nat = majestic_image_native(leaf)) leaf = nat;
        for (const char* k : IMAGE_KEYS)
            if (leaf == k) { r.ok = true; r.status = 200; r.unset.push_back("image." + leaf); return r; }
    }
    r.status = 404; r.code = "unknown_field"; r.path = key;
    r.message = "no such setting";
    return r;
}

namespace {
// A refusal frame: the enumerated marker on its OWN first line (so update.js's
// anchored /^ERROR: .../mi matches), then free reason text for the log pane.
UpgradePlan refuse(const char* marker, const std::string& reason) {
    UpgradePlan p;
    p.refusal = std::string(marker) + "\n\n" + reason;
    return p;   // argv stays empty => caller refuses
}
// A --url source must be a bare http(s) URL with no whitespace or control
// bytes: it goes into argv unquoted (no shell), but a newline would still let
// it forge a marker line in the streamed transcript.
bool clean_token(const std::string& s, bool allow_slash_colon) {
    if (s.empty() || s.size() > 512) return false;
    for (unsigned char ch : s) {
        if (ch <= 0x20 || ch == 0x7f) return false;             // no ws/control
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                        ch == '-' ||
                        (allow_slash_colon && (ch == '/' || ch == ':' ||
                                               ch == '?' || ch == '=' ||
                                               ch == '&' || ch == '%'));
        if (!ok) return false;
    }
    return true;
}
} // namespace

UpgradePlan upgrade_plan(const std::string& params_json) {
    Json j; std::string jerr;
    if (!Json::parse(params_json, j, jerr) || !j.is_object())
        return refuse("ERROR: invalid upgrade parameters",
                      "The start message was not the JSON the Update page sends.");

    auto flag = [&](const char* key) {
        const Json* v = j.get(key);
        return v && v->is_bool() && v->as_bool();
    };
    const bool kernel = flag("kernel");
    const bool rootfs = flag("rootfs");
    const bool reset  = flag("reset");
    const bool force  = flag("force");

    // The one thing this daemon will not do. reset == wipe_overlay == "-n":
    // machino, its config and the AI model all live on the overlay, so a
    // wiping update would erase the very daemon streaming this log. Refuse it
    // in the page's own words rather than quietly dropping the flag - the
    // person asked for a wipe and has to be told it was declined.
    if (reset)
        return refuse("ERROR: invalid upgrade parameters",
                      "Overlay wipe (\"reset\") is refused: machino, its "
                      "settings and the AI model all live on the overlay, and "
                      "wiping it would erase this daemon mid-update. Uncheck "
                      "the reset/wipe option and update again - the base "
                      "firmware still updates, and majestic stays disabled.");

    if (!kernel && !rootfs)
        return refuse("ERROR: invalid upgrade parameters",
                      "Nothing selected to update: pick kernel and/or rootfs.");

    UpgradePlan p;
    // --web is mandatory: without it sysupgrade SIGQUITs the web daemon (this
    // process) before flashing (sysupgrade line ~347), and the log the page is
    // reading dies with it. With it, machino survives the quiet phases and
    // streams to the point of no return.
    p.argv = {"/usr/sbin/sysupgrade", "--web"};
    if (rootfs) p.argv.push_back("-r");
    if (kernel) p.argv.push_back("-k");
    if (force)  p.argv.push_back("-f");
    // NOTE: "-n"/"--wipe_overlay" is intentionally never added. See reset above.

    const Json* src = j.get("source");
    const std::string source = (src && src->is_string()) ? src->as_string() : "";
    if (source.empty() || source == "github") {
        // Default online update: no source flag -> sysupgrade pulls the latest
        // build for this SoC from the OpenIPC release channel. This is the
        // banner button's path.
    } else if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
        if (!clean_token(source, /*allow_slash_colon=*/true))
            return refuse("ERROR: invalid upgrade parameters",
                          "The update URL contains characters that are not allowed.");
        p.argv.push_back("--url=" + source);
    } else if (source == "/tmp/firmware.tgz") {
        // Local upload: the page POSTed the .tgz to /upload first.
        p.argv.push_back("--archive=/tmp/firmware.tgz");
    } else if (clean_token(source, /*allow_slash_colon=*/false)) {
        // A named release channel from the manifest (nightly, stable, ...).
        p.argv.push_back("--channel=" + source);
    } else {
        return refuse("ERROR: invalid upgrade parameters",
                      "The update source was not recognised.");
    }
    return p;
}

}} // namespace machino::compat
