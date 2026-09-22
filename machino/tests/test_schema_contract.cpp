// AP4: the invariants the stock Settings page depends on when it builds its
// controls purely from /api/v1/config.schema.json. Individual fields are
// checked elsewhere; this walks the WHOLE document and asserts the structural
// contract, so a future field cannot quietly ship a control the unmodified UI
// would render wrong (or not at all).
#include "app/compat/majestic_webui.hpp"
#include "core/json.hpp"
#include <cstdio>
#include <set>
#include <string>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define SCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// One supported control entry as the capability document carries it.
Json ctl(long long lo, long long hi, const char* apply = "live") {
    Json c = Json::object();
    c.set("status", Json::string("supported"));
    c.set("apply", Json::string(apply));
    c.set("min", Json::integer(lo));
    c.set("max", Json::integer(hi));
    return c;
}

// A capability document rich enough that every optional section is emitted -
// a thin one silently produces a schema with no video0 at all, which is
// exactly the kind of empty Settings page this test exists to catch.
Json caps_all() {
    Json caps = Json::object();
    Json pr = Json::array();
    pr.push(Json::string("performance")); pr.push(Json::string("balanced")); pr.push(Json::string("battery"));
    caps.set("profiles", pr);

    Json controls = Json::object();
    controls.set("stream_fps", ctl(1, 30));
    controls.set("bitrate", ctl(64, 20000));
    controls.set("gop", ctl(1, 300));
    controls.set("sensor_fps", ctl(10, 20));
    caps.set("controls", controls);

    Json image = Json::object();
    Json br = Json::object(); br.set("status", Json::string("supported"));
    br.set("min", Json::integer(0)); br.set("max", Json::integer(255));
    image.set("brightness", br);
    caps.set("image", image);
    return caps;
}

bool is_known_type(const std::string& t) {
    return t == "boolean" || t == "integer" || t == "number" || t == "string" || t == "object";
}

} // namespace

void run_schema_contract_tests() {
    const Json schema = compat::majestic_schema(caps_all());

    // the document itself
    SCHECK(schema.get("type") && schema.get("type")->as_string() == "object");
    const Json* props = schema.get("properties");
    SCHECK(props && props->is_object() && props->size() > 0);
    if (!props || !props->is_object()) return;

    std::set<std::string> section_ids;

    for (const auto& sec : props->members()) {
        const std::string sid = sec.first;
        section_ids.insert(sid);
        const Json& s = sec.second;

        // every section is an object with a properties bag
        SCHECK(s.is_object());
        SCHECK(s.get("type") && s.get("type")->as_string() == "object");
        const Json* fields = s.get("properties");
        SCHECK(fields && fields->is_object());
        if (!fields || !fields->is_object()) continue;
        SCHECK(fields->size() > 0);          // an empty section renders as a blank card

        for (const auto& fld : fields->members()) {
            const std::string fid = sid + "." + fld.first;
            const Json& f = fld.second;
            SCHECK(f.is_object());

            // a control the UI can render needs at least a type and a label
            const Json* type = f.get("type");
            SCHECK(type && type->is_string());
            if (!type || !type->is_string()) continue;
            const std::string t = type->as_string();
            if (!is_known_type(t)) { ++g_fail_ext; fprintf(stderr, "FAIL unknown type %s for %s\n", t.c_str(), fid.c_str()); }
            const Json* title = f.get("title");
            SCHECK(title && title->is_string() && !title->as_string().empty());

            // x-reload is the apply PROMISE, and it must not be bigger than
            // what actually happens.
            //
            //   "live"      the POST carried it - true for everything Machino
            //               applies inside the request
            //   "pipeline"  saved, and a reload is still owed. mj-settings.js
            //               changeCost() maps any value that is not none/live
            //               and carries no service:/channel: prefix to
            //               'pipeline', and then tells the operator "After
            //               Save, a reload restarts the video streams".
            //
            // video1 is the second case: the sub-stream unit is rebuilt from
            // the reloaded config, which is what the Apply button's SIGHUP
            // delivers. Advertising it as "live" would report a change as
            // applied when nothing had happened.
            if (const Json* xr = f.get("x-reload")) {
                const bool known = xr->is_string() &&
                                   (xr->as_string() == "live" || xr->as_string() == "pipeline");
                SCHECK(known);
                if (xr->is_string() && xr->as_string() == "pipeline")
                    SCHECK(sid == "video1");        // nothing else may claim it yet
            }

            // an enum must be a non-empty array of strings, and the default
            // (when present) must be one of its values
            if (const Json* en = f.get("enum")) {
                SCHECK(en->is_array() && en->size() > 0);
                bool all_strings = true, default_in_enum = false;
                const Json* def = f.get("default");
                for (size_t i = 0; i < en->size(); ++i) {
                    if (!en->at(i).is_string()) all_strings = false;
                    if (def && def->is_string() && en->at(i).is_string() &&
                        en->at(i).as_string() == def->as_string()) default_in_enum = true;
                }
                SCHECK(all_strings);
                if (def && def->is_string()) SCHECK(default_in_enum);
                SCHECK(t == "string");            // an enum control is a string control
            }

            // a range must be ordered, and a default inside it
            const Json* mn = f.get("minimum");
            const Json* mx = f.get("maximum");
            if (mn || mx) {
                SCHECK(t == "integer" || t == "number");
                SCHECK(mn && mn->is_number());
                SCHECK(mx && mx->is_number());
                if (mn && mx && mn->is_number() && mx->is_number()) {
                    SCHECK(mn->as_int() <= mx->as_int());
                    if (const Json* def = f.get("default"); def && def->is_number()) {
                        SCHECK(def->as_int() >= mn->as_int() && def->as_int() <= mx->as_int());
                    }
                }
            }

            // a default must match the declared type - a string default on an
            // integer control is how a dynamic form ends up with an empty box
            if (const Json* def = f.get("default")) {
                if (t == "integer" || t == "number") SCHECK(def->is_number());
                else if (t == "boolean")             SCHECK(def->is_bool());
                else if (t == "string")              SCHECK(def->is_string());
            }
        }
    }

    // groups: every referenced section must exist, ids and labels present.
    // A group pointing at a section that was not emitted leaves an empty tab.
    if (const Json* groups = schema.get("x-groups")) {
        SCHECK(groups->is_array() && groups->size() > 0);
        for (size_t i = 0; i < groups->size(); ++i) {
            const Json& g = groups->at(i);
            SCHECK(g.is_object());
            SCHECK(g.get("id") && g.get("id")->is_string() && !g.get("id")->as_string().empty());
            SCHECK(g.get("label") && g.get("label")->is_string() && !g.get("label")->as_string().empty());
            const Json* ss = g.get("sections");
            SCHECK(ss && ss->is_array());
            if (!ss || !ss->is_array()) continue;
            for (size_t k = 0; k < ss->size(); ++k) {
                SCHECK(ss->at(k).is_string());
                if (ss->at(k).is_string())
                    SCHECK(section_ids.count(ss->at(k).as_string()) == 1);
            }
        }
    }

    // fields whose native class is daemon_restart/boot_only must stay OUT:
    // the stock UI would show an Apply that cannot be honoured
    SCHECK(!props->get("lifecycle"));
    SCHECK(!props->get("rtsp"));

    // and the sections the dashboard/live pages rely on must be IN
    SCHECK(props->get("video0"));
    SCHECK(props->get("performance"));
}
