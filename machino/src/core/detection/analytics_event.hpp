// What /ws/analytics sends: the live detection feed the stock WebUI's
// analytics-overlay.js draws over the preview (the Ai tab of camera.cgi).
//
// One JSON message per analysed frame, plus a heartbeat while a detector is
// running but quiet. The shape is the overlay's contract, read from its
// source (2026-09-30):
//
//   {"src":"person","active":true,"w":1920,"h":1080,"total":2,
//    "r":[[x,y,w,h,cls,score], ...]}
//
//   src     which detector spoke; later messages from one src replace
//           earlier ones ("what is there now", not a log)
//   active  the detector is running; false clears its boxes
//   w, h    the frame the boxes are measured in. The overlay expects
//           MAIN-STREAM PIXELS (it maps main -> shown stream through the
//           /api/v1/osd report), so the detector's normalised boxes are
//           scaled by the main stream's size here.
//   total   detections in this frame, boxed or not (motion is boxless)
//   r       one entry per boxed detection: pixels, class id, score 0..1
//
// Pure: no I/O, host-tested (tests/test_analytics_event.cpp).
#pragma once
#include "core/detection/types.hpp"
#include <string>

namespace machino { namespace detection {

std::string analytics_message(const std::string& src, const DetectionResult& r, int main_w, int main_h, bool active);

}} // namespace machino::detection
