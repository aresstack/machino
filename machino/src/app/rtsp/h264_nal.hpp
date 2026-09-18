// H.264 Annex-B helpers for the RTSP/RTP application layer.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace h264 {

struct Nal {
    const uint8_t* p;     // NAL payload start (after the start code)
    size_t         len;
    uint8_t        type;  // nal_unit_type (p[0] & 0x1f)
};

// Splits an Annex-B buffer into NAL units. Returns the count written (<= max).
size_t split(const uint8_t* data, size_t len, Nal* out, size_t max);

// Extracts SPS (7) and PPS (8) from an access unit if present.
bool extract_params(const uint8_t* data, size_t len, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps);

std::string base64(const uint8_t* p, size_t n);

}} // namespace machino::h264
