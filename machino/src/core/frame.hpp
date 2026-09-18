// Machino core: an encoded access unit (one frame, Annex-B byte stream with
// start codes). Produced by an encoder adapter, fanned out by StreamHub.
// Shared immutably between consumers; the vector is the only allocation.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace machino {

struct AccessUnit {
    std::vector<uint8_t> data;   // Annex-B (00 00 00 01 NAL ...)
    int64_t  pts_us = 0;         // platform timestamp, microseconds
    bool     key    = false;     // contains an IDR slice
    uint32_t seq    = 0;         // encoder frame sequence
};

using AuPtr = std::shared_ptr<const AccessUnit>;

} // namespace machino
