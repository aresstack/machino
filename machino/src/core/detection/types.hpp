// Machino core: detection results, platform-neutral. A detector produces zero
// or more Detections per analysed frame; motion backends set `motion` and a
// coarse level, object/NNA backends fill class_id/label/confidence and a box.
// Coordinates are normalised [0,1] so a result is independent of the analysis
// resolution.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace detection {

struct Box {
    float x = 0, y = 0, w = 0, h = 0;   // normalised [0,1], top-left origin
};

struct Detection {
    int         class_id  = -1;         // -1 = not a classified object (e.g. motion)
    std::string label;                  // "motion", "person", ... ("" when unknown)
    int         confidence = 0;         // 0..100
    Box         box;                    // normalised region ({} when whole-frame)
};

struct DetectionResult {
    std::vector<Detection> detections;
    bool     motion = false;            // any motion at all this frame
    int      motion_level = 0;          // 0..100 coarse activity, backend-defined
    int64_t  frame_pts_us = 0;          // source frame timestamp (platform clock)
    bool     any() const { return motion || !detections.empty(); }
};

}} // namespace machino::detection
