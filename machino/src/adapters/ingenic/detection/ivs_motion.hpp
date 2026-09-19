// Ingenic adapter: IMP_IVS motion detector (the "motion" backend). A
// BoundSource detector - it binds a dedicated low-resolution analysis
// FrameSource channel to an IVS group running the move algorithm and polls
// per-cell motion results. No encoder, no model file. This is the first real
// detector; NNA/person detection is a separate backend and is not built here.
#pragma once
#include "ports/idetector.hpp"
#include <memory>

namespace machino { namespace ingenic {

// Build the IMP_IVS move pipeline on analysis channel `chn` (its own
// FrameSource channel, distinct from main/sub/jpeg). Returns nullptr when any
// SDK construction step is refused - the backend is then simply unavailable,
// never fatal. native_w/native_h are the sensor's full resolution, used to
// decide whether the analysis channel needs the scaler.
std::unique_ptr<IDetector> create_motion_detector(int chn, const DetectorParams& p, int native_w, int native_h);

}} // namespace machino::ingenic
