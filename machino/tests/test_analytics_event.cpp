// /ws/analytics message shape: what the stock overlay draws from.
#include "core/detection/analytics_event.hpp"
#include <cstdio>
#include <string>

using namespace machino::detection;
extern int g_fail_ext, g_pass_ext;
#define ACHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void run_analytics_event_tests() {
    // Two persons, boxes normalised on a 1920x1080 main stream -> main pixels,
    // class id and a 0..1 score, total counts both.
    DetectionResult r;
    Detection a; a.class_id = 0; a.label = "person"; a.confidence = 87; a.box = {0.25f, 0.5f, 0.1f, 0.2f};
    Detection b; b.class_id = 0; b.label = "person"; b.confidence = 100; b.box = {0.f, 0.f, 0.5f, 0.5f};
    r.detections = {a, b};
    const std::string m = analytics_message("person", r, 1920, 1080, true);
    ACHECK(m == "{\"src\":\"person\",\"active\":true,\"w\":1920,\"h\":1080,\"total\":2,"
                "\"r\":[[480,540,192,216,0,0.87],[0,0,960,540,0,1.00]]}");

    // Motion: one boxless detection -> counted, not drawn; active follows the
    // detector's motion flag.
    DetectionResult mo; mo.motion = true;
    Detection md; md.label = "motion"; md.confidence = 60;   // box {} = whole frame
    mo.detections = {md};
    ACHECK(analytics_message("motion", mo, 1280, 720, mo.motion)
           == "{\"src\":\"motion\",\"active\":true,\"w\":1280,\"h\":720,\"total\":1,\"r\":[]}");

    // Heartbeat while quiet: nothing seen, still speaking.
    DetectionResult empty;
    ACHECK(analytics_message("person", empty, 1920, 1080, true)
           == "{\"src\":\"person\",\"active\":true,\"w\":1920,\"h\":1080,\"total\":0,\"r\":[]}");
    // Stopped: active false clears the overlay's boxes for that src.
    ACHECK(analytics_message("person", empty, 1920, 1080, false).find("\"active\":false") != std::string::npos);

    // The src reaches a DOM attribute: only plain names pass.
    ACHECK(analytics_message("we ird\"", empty, 1, 1, true).rfind("{\"src\":\"we_ird_\"", 0) == 0);
    ACHECK(analytics_message("", empty, 1, 1, true).rfind("{\"src\":\"detector\"", 0) == 0);

    // Confidence is clamped and a negative class id reads as 0; a zero frame
    // size never divides or multiplies into nonsense.
    Detection odd; odd.class_id = -1; odd.confidence = 250; odd.box = {0.5f, 0.5f, 0.5f, 0.5f};
    DetectionResult o; o.detections = {odd};
    ACHECK(analytics_message("person", o, 0, 0, true).find("[1,1,1,1,0,1.00]") != std::string::npos);
    ACHECK(analytics_message("person", o, 100, 50, true).find("[50,25,50,25,0,1.00]") != std::string::npos);
}
