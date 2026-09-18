#include "core/hw/board_profile_parser.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace machino { namespace hw {

static void trim(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
}

static bool to_int(const std::string& v, int& out) {
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 0);
    if (end == v.c_str() || *end) return false;
    out = (int)n; return true;
}

bool parse_mode(const std::string& s, SensorMode& out) {
    int w = 0, h = 0, f = 0;
    if (sscanf(s.c_str(), "%dx%d@%d", &w, &h, &f) != 3) return false;
    if (w <= 0 || h <= 0 || f <= 0) return false;
    out = SensorMode{w, h, f};
    return true;
}

// "none" / "-1" -> -1 (explicitly no pin); otherwise a non-negative pin number.
static bool to_gpio(const std::string& v, int& out) {
    if (v == "none" || v == "-1") { out = -1; return true; }
    if (!to_int(v, out)) return false;
    return out >= 0;
}

bool parse_board_profile(const std::string& text, BoardProfile& out, std::string& err, std::string* warnings) {
    BoardProfile p;
    size_t pos = 0; int line = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string s = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        ++line;
        size_t hash = s.find('#'); if (hash != std::string::npos) s.erase(hash);
        trim(s); if (s.empty()) continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) { err = "line " + std::to_string(line) + ": no '='"; return false; }
        std::string k = s.substr(0, eq), v = s.substr(eq + 1); trim(k); trim(v);
        int n = 0; SensorMode m;
        auto bad = [&](const char* what) { err = "line " + std::to_string(line) + ": bad " + what + " '" + v + "'"; return false; };
        if      (k == "board_id")   p.board_id = v;
        else if (k == "platform")   p.platform = v;
        else if (k == "sensor")     p.sensor = v;
        else if (k == "i2c_bus")    { if (!to_int(v, n) || n < 0 || n > 15) return bad("i2c_bus"); p.wiring.i2c_bus = n; }
        else if (k == "i2c_addr")   { if (!to_int(v, n) || n < 0 || n > 0x7f) return bad("i2c_addr"); p.wiring.i2c_addr = n; }
        else if (k == "mclk")       { if (!to_int(v, n) || n < 0 || n > 7) return bad("mclk"); p.wiring.mclk = n; }
        else if (k == "reset_gpio") { if (!to_gpio(v, n)) return bad("reset_gpio"); p.wiring.reset_gpio = n; }
        else if (k == "pwdn_gpio")  { if (!to_gpio(v, n)) return bad("pwdn_gpio"); p.wiring.pwdn_gpio = n; }
        else if (k == "mode")       { if (!parse_mode(v, m)) return bad("mode (WxH@fps)"); p.default_mode = m; }
        else if (k == "preset.balanced_fps")     { if (!to_int(v, n) || n <= 0) return bad("preset.balanced_fps");     p.presets.balanced_fps = n; }
        else if (k == "preset.battery_fps")      { if (!to_int(v, n) || n <= 0) return bad("preset.battery_fps");      p.presets.battery_fps = n; }
        else if (k == "preset.balanced_bitrate") { if (!to_int(v, n) || n <= 0) return bad("preset.balanced_bitrate"); p.presets.balanced_bitrate = n; }
        else if (k == "preset.battery_bitrate")  { if (!to_int(v, n) || n <= 0) return bad("preset.battery_bitrate");  p.presets.battery_bitrate = n; }
        else if (k == "hardware_verified") p.hardware_verified = (v == "1" || v == "yes" || v == "true");
        else if (k == "notes")      p.notes = v;
        else if (warnings)          *warnings += "line " + std::to_string(line) + ": unknown key '" + k + "'\n";
    }
    if (p.board_id.empty()) { err = "board_id missing"; return false; }
    if (p.platform.empty()) { err = "platform missing"; return false; }
    if (p.sensor.empty())   { err = "sensor missing"; return false; }
    out = p;
    return true;
}

bool load_board_profile_file(const char* path, BoardProfile& out, std::string& err, std::string* warnings) {
    FILE* f = fopen(path, "r");
    if (!f) { err = std::string("cannot open ") + path; return false; }
    std::string text; char buf[512];
    while (fgets(buf, sizeof buf, f)) text += buf;
    fclose(f);
    return parse_board_profile(text, out, err, warnings);
}

}} // namespace machino::hw
