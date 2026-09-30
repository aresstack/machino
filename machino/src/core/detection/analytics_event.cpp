#include "core/detection/analytics_event.hpp"

#include <cmath>
#include <cstdio>

namespace machino { namespace detection {

std::string analytics_message(const std::string& src, const DetectionResult& r, int main_w, int main_h, bool active) {
    // src lands in a JSON string and later in a DOM attribute: only the plain
    // detector names pass, anything else is flattened. The page is careful
    // about it too, but the camera is the one that promised three literals.
    std::string s;
    for (char ch : src) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        s.push_back(ok ? ch : '_');
    }
    if (s.empty()) s = "detector";
    if (main_w <= 0) main_w = 1;
    if (main_h <= 0) main_h = 1;

    std::string out;
    out.reserve(96 + r.detections.size() * 40);
    char buf[128];
    snprintf(buf, sizeof buf, "{\"src\":\"%s\",\"active\":%s,\"w\":%d,\"h\":%d,\"total\":%u,\"r\":[",
             s.c_str(), active ? "true" : "false", main_w, main_h, (unsigned)r.detections.size());
    out += buf;
    bool first = true;
    for (const Detection& d : r.detections) {
        // Motion reports the whole frame as {} and gets no box: an invented
        // 0/0/0/0 would be a statement about where nothing was seen.
        if (!(d.box.w > 0.f && d.box.h > 0.f)) continue;
        const long x = std::lround((double)d.box.x * main_w);
        const long y = std::lround((double)d.box.y * main_h);
        const long w = std::lround((double)d.box.w * main_w);
        const long h = std::lround((double)d.box.h * main_h);
        int conf = d.confidence; if (conf < 0) conf = 0; if (conf > 100) conf = 100;
        snprintf(buf, sizeof buf, "%s[%ld,%ld,%ld,%ld,%d,%d.%02d]", first ? "" : ",", x, y, w, h,
                 d.class_id < 0 ? 0 : d.class_id, conf / 100, conf % 100);
        out += buf;
        first = false;
    }
    out += "]}";
    return out;
}

}} // namespace machino::detection
