#include "app/compat/majestic_migrate.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>

namespace machino { namespace compat {

const char* disposition_name(Disposition d) {
    switch (d) {
        case Disposition::Mapped:      return "mapped";
        case Disposition::Converted:   return "converted";
        case Disposition::Ignored:     return "ignored";
        case Disposition::Unsupported: return "unsupported";
        case Disposition::Invalid:     return "invalid";
    }
    return "?";
}

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out; size_t start = 0;
    while (true) {
        size_t comma = s.find(',', start);
        out.push_back(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

bool is_truthy(const std::string& v) {
    std::string l = lower(v);
    return l == "true" || l == "1" || l == "yes" || l == "on" || l == "enable" || l == "enabled";
}

bool to_int(const std::string& s, long long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') return false;
    out = v; return true;
}

// majestic image controls are 0..100; machino image controls are 0..255.
int scale100_to255(long long v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return (int)((v * 255 + 50) / 100);
}

std::string strip_value(std::string v) {
    v = trim(v);
    // inline comment (only when not inside quotes; majestic values with '#' are quoted)
    if (!v.empty() && v.front() != '"' && v.front() != '\'') {
        size_t h = v.find(" #");
        if (h != std::string::npos) v = trim(v.substr(0, h));
    }
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
        v = v.substr(1, v.size() - 2);
    return v;
}

struct Flat {
    std::vector<std::pair<std::string, std::string>> kv;   // dotted leaf keys, in document order
    std::vector<std::string> structural;                    // dotted keys whose value is a list/flow map
};

// Indentation-based subset parser. Good enough for majestic.yaml (2-space
// nested maps, scalar leaves, comments, quotes). Lists/flow are recorded, not
// guessed at.
Flat parse_yaml_flat(const std::string& text) {
    Flat f;
    std::vector<std::pair<int, std::string>> stack;   // (indent, key)
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        int indent = 0; while (indent < (int)line.size() && line[indent] == ' ') ++indent;
        std::string body = trim(line);
        if (body.empty() || body[0] == '#' || body == "---" || body == "...") continue;

        auto dotted = [&](const std::string& key) {
            std::string p;
            for (auto& e : stack) { if (!p.empty()) p += "."; p += e.second; }
            if (!p.empty()) p += ".";
            return p + key;
        };

        if (body[0] == '-') {                              // list item: mark the parent, don't descend
            std::string p; for (auto& e : stack) { if (!p.empty()) p += "."; p += e.second; }
            if (!p.empty() && (f.structural.empty() || f.structural.back() != p)) f.structural.push_back(p);
            continue;
        }
        size_t colon = body.find(':');
        if (colon == std::string::npos) continue;          // not a mapping line
        std::string key = trim(body.substr(0, colon));
        std::string rest = body.substr(colon + 1);
        while (!stack.empty() && stack.back().first >= indent) stack.pop_back();

        std::string val = strip_value(rest);
        if (val.empty()) { stack.push_back({indent, key}); continue; }   // parent map
        if (val[0] == '[' || val[0] == '{') { f.structural.push_back(dotted(key)); continue; }
        f.kv.emplace_back(dotted(key), val);
    }
    return f;
}

// ---- the mapping ------------------------------------------------------------

class Mapper {
public:
    explicit Mapper(const Flat& flat) : flat_(flat) { for (auto& p : flat.kv) m_[p.first] = p.second; }

    MigrationResult run() {
        MigrationResult r; r.ok = true;
        for (const auto& p : flat_.kv) add(r, map_one(p.first, p.second));
        for (const auto& s : flat_.structural) {
            MigrationEntry e; e.source_key = s; e.source_value = "<list/flow>";
            e.disp = Disposition::Unsupported; e.note = "structured value not migrated";
            add(r, e);
        }
        return r;
    }

private:
    const Flat& flat_;
    std::map<std::string, std::string> m_;

    static void add(MigrationResult& r, const MigrationEntry& e) {
        r.entries.push_back(e);
        switch (e.disp) {
            case Disposition::Mapped:      ++r.mapped; break;
            case Disposition::Converted:   ++r.converted; break;
            case Disposition::Ignored:     ++r.ignored; break;
            case Disposition::Unsupported: ++r.unsupported; break;
            case Disposition::Invalid:     ++r.invalid; break;
        }
        if ((e.disp == Disposition::Mapped || e.disp == Disposition::Converted) && !e.target_key.empty()) {
            // target_key and value are positionally-paired comma lists (one pair
            // normally; two for size-style splits into width/height).
            std::vector<std::string> keys = split_commas(e.target_key);
            std::vector<std::string> vals = split_commas(e.value);
            for (size_t i = 0; i < keys.size() && i < vals.size(); ++i)
                r.config.emplace_back(trim(keys[i]), trim(vals[i]));
        }
    }

    static MigrationEntry mapped(const std::string& sk, const std::string& sv, const std::string& tk, const std::string& tv, const char* note = "") {
        MigrationEntry e; e.source_key = sk; e.source_value = sv; e.target_key = tk; e.value = tv; e.disp = Disposition::Mapped; e.note = note; return e;
    }
    static MigrationEntry converted(const std::string& sk, const std::string& sv, const std::string& tk, const std::string& tv, const std::string& note) {
        MigrationEntry e; e.source_key = sk; e.source_value = sv; e.target_key = tk; e.value = tv; e.disp = Disposition::Converted; e.note = note; return e;
    }
    static MigrationEntry ignored(const std::string& sk, const std::string& sv, const char* note) {
        MigrationEntry e; e.source_key = sk; e.source_value = sv; e.disp = Disposition::Ignored; e.note = note; return e;
    }
    static MigrationEntry unsupported(const std::string& sk, const std::string& sv, const char* note) {
        MigrationEntry e; e.source_key = sk; e.source_value = sv; e.disp = Disposition::Unsupported; e.note = note; return e;
    }
    static MigrationEntry invalid(const std::string& sk, const std::string& sv, const std::string& note) {
        MigrationEntry e; e.source_key = sk; e.source_value = sv; e.disp = Disposition::Invalid; e.note = note; return e;
    }

    // majestic GOP is expressed in seconds; machino wants frames = gop_s * fps.
    MigrationEntry map_gop(const std::string& sk, const std::string& sv, const std::string& fps_key, const std::string& target) {
        long long g; if (!to_int(sv, g) || g <= 0) return invalid(sk, sv, "GOP must be a positive integer");
        auto it = m_.find(fps_key);
        long long fps;
        if (it == m_.end() || !to_int(it->second, fps) || fps <= 0)
            return invalid(sk, sv, "cannot convert GOP (seconds) without a numeric " + fps_key);
        long long frames = g * fps; if (frames < 1) frames = 1; if (frames > 1000) frames = 1000;
        return converted(sk, sv, target, std::to_string(frames), "majestic GOP is seconds; converted to frames using " + fps_key);
    }

    MigrationEntry map_size(const std::string& sk, const std::string& sv, const std::string& wkey, const std::string& hkey) {
        size_t x = sv.find('x'); if (x == std::string::npos) x = sv.find('X');
        long long w, h;
        if (x == std::string::npos || !to_int(sv.substr(0, x), w) || !to_int(sv.substr(x + 1), h) || w <= 0 || h <= 0)
            return invalid(sk, sv, "resolution must look like WIDTHxHEIGHT");
        return converted(sk, sv, wkey + "," + hkey, std::to_string(w) + "," + std::to_string(h), "split into " + wkey + "/" + hkey);
    }

    MigrationEntry map_image(const std::string& sk, const std::string& leaf, const std::string& sv) {
        if (leaf == "mirror")  return converted(sk, sv, "image.hflip", is_truthy(sv) ? "1" : "0", "boolean -> 0/1");
        if (leaf == "flip")    return converted(sk, sv, "image.vflip", is_truthy(sv) ? "1" : "0", "boolean -> 0/1");
        static const std::map<std::string, std::string> scale = {
            {"contrast", "image.contrast"}, {"hue", "image.hue"}, {"saturation", "image.saturation"},
            {"luminance", "image.brightness"}, {"brightness", "image.brightness"}, {"sharpness", "image.sharpness"}};
        auto it = scale.find(leaf);
        if (it != scale.end()) {
            long long v; if (!to_int(sv, v)) return invalid(sk, sv, "image control must be an integer");
            return converted(sk, sv, it->second, std::to_string(scale100_to255(v)), "rescaled 0..100 -> 0..255");
        }
        return unsupported(sk, sv, "no machino image equivalent");
    }

    // gop is handled in map_one (needs the paired fps); everything else here.
    MigrationEntry map_video(const std::string& sk, const std::string& leaf, const std::string& sv, bool sub_stream) {
        const std::string vp = sub_stream ? "video.1." : "video.";
        if (leaf == "fps")       return mapped(sk, sv, vp + "fps", sv);
        if (leaf == "bitrate")   return mapped(sk, sv, vp + "bitrate", sv);
        if (leaf == "size")      return map_size(sk, sv, vp + "width", vp + "height");
        if (leaf == "rcmode" || leaf == "rc") {
            std::string l = lower(sv);
            if (l == "cbr" || l == "vbr" || l == "fixqp") return mapped(sk, sv, sub_stream ? "video.1.rc_mode" : "video.rc_mode", l);
            if (l == "avbr") return converted(sk, sv, sub_stream ? "video.1.rc_mode" : "video.rc_mode", "vbr", "avbr approximated as vbr");
            return unsupported(sk, sv, "unknown rate-control mode");
        }
        if (leaf == "codec" || leaf == "type") {
            std::string l = lower(sv);
            if (l == "h264" || l == "avc") return ignored(sk, sv, "machino streams H.264");
            return unsupported(sk, sv, "machino builds H.264 only");
        }
        if (leaf == "enabled") {
            if (sub_stream) return is_truthy(sv) ? mapped(sk, sv, "video.1.enabled", "true")
                                                 : mapped(sk, sv, "video.1.enabled", "false");
            return is_truthy(sv) ? ignored(sk, sv, "main stream is demand-driven, always available")
                                 : unsupported(sk, sv, "machino cannot disable the main stream");
        }
        return unsupported(sk, sv, "no machino equivalent");
    }

    MigrationEntry map_one(const std::string& key, const std::string& val) {
        // section.leaf split
        size_t dot = key.find('.');
        std::string sec = dot == std::string::npos ? key : key.substr(0, dot);
        std::string leaf = dot == std::string::npos ? std::string() : key.substr(dot + 1);
        std::string sec_l = lower(sec), leaf_l = lower(leaf);

        if (sec_l == "system") {
            if (leaf_l == "loglevel") {
                std::string l = lower(val); int lvl = 2;
                if (l == "error" || l == "err") lvl = 0; else if (l == "warn" || l == "warning") lvl = 1;
                else if (l == "info") lvl = 2; else if (l == "debug" || l == "trace") lvl = 3;
                return converted(key, val, "log.level", std::to_string(lvl), "majestic log level -> machino 0..3");
            }
            return ignored(key, val, "machino WebUI is served by the selector httpd (fixed port 80)");
        }
        if (sec_l == "isp")   return ignored(key, val, "sensor/ISP wiring comes from the machino board profile");
        if (sec_l == "image") return map_image(key, leaf_l, val);
        if (sec_l == "video0") {
            if (leaf_l == "gop") return map_gop(key, val, "video0.fps", "latency.gop");
            return map_video(key, leaf_l, val, false);
        }
        if (sec_l == "video1") {
            if (leaf_l == "gop") return map_gop(key, val, "video1.fps", "video.1.gop");
            return map_video(key, leaf_l, val, true);
        }
        if (sec_l == "rtsp") {
            if (leaf_l == "port") return mapped(key, val, "rtsp.port", val);
            if (leaf_l == "enabled") return ignored(key, val, "RTSP is always available in machino");
            return ignored(key, val, "no machino equivalent");
        }
        if (sec_l == "jpeg") {
            if (leaf_l == "quality" || leaf_l == "qualitylevel") {
                long long q; if (!to_int(val, q)) return invalid(key, val, "quality must be an integer");
                if (q > 99) return converted(key, val, "jpeg.quality", "99", "clamped to machino max (99)");
                if (q < 1) q = 1;
                return mapped(key, val, "jpeg.quality", std::to_string(q));
            }
            return ignored(key, val, "no machino equivalent");
        }
        if (sec_l == "motiondetect") {
            if (leaf_l == "enabled")
                return converted(key, val, is_truthy(val) ? "ai.enabled,ai.detector" : "ai.enabled",
                                 is_truthy(val) ? "true,motion" : "false", "mapped to machino detection (IMP_IVS motion)");
            return unsupported(key, val, "machino motion uses a fixed grid; per-region config not migrated");
        }
        if (sec_l == "watchdog") return ignored(key, val, "handled by the OpenIPC init + streamerctl");
        if (sec_l == "audio")    return unsupported(key, val, "machino has no audio path");
        if (sec_l == "records" || sec_l == "record") return unsupported(key, val, "no on-device recording");
        if (sec_l == "mqtt")     return unsupported(key, val, "no MQTT integration");
        if (sec_l == "outgoing") return unsupported(key, val, "no outgoing/stream-push integration");
        if (sec_l == "onvif")    return unsupported(key, val, "no ONVIF service");
        if (sec_l == "nightmode" || sec_l == "night") return unsupported(key, val, "day/night switching not implemented");
        return unsupported(key, val, "unknown majestic key");
    }
};

} // namespace

MigrationResult migrate_majestic_yaml(const std::string& yaml_text) {
    if (trim(yaml_text).empty()) { MigrationResult r; r.ok = false; r.error = "empty document"; return r; }
    Flat flat = parse_yaml_flat(yaml_text);
    if (flat.kv.empty() && flat.structural.empty()) { MigrationResult r; r.ok = false; r.error = "no recognisable majestic keys"; return r; }
    return Mapper(flat).run();
}

std::string migration_report(const MigrationResult& r) {
    std::ostringstream o;
    if (!r.ok) { o << "migration failed: " << r.error << "\n"; return o.str(); }
    for (const auto& e : r.entries) {
        o << "[" << disposition_name(e.disp) << "] " << e.source_key << " = " << e.source_value;
        if (!e.target_key.empty()) o << " -> " << e.target_key << " = " << e.value;
        if (!e.note.empty()) o << "  (" << e.note << ")";
        o << "\n";
    }
    o << "summary: " << r.mapped << " mapped, " << r.converted << " converted, " << r.ignored
      << " ignored, " << r.unsupported << " unsupported, " << r.invalid << " invalid\n";
    return o.str();
}

std::string to_machino_conf(const MigrationResult& r) {
    std::ostringstream o;
    o << "# machino.conf migrated from majestic.yaml\n";
    o << "# " << r.mapped << " mapped, " << r.converted << " converted, "
      << r.unsupported << " unsupported, " << r.invalid << " invalid (see migration report)\n";
    for (const auto& kv : r.config) o << kv.first << " = " << kv.second << "\n";
    return o.str();
}

}} // namespace machino::compat
