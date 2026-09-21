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

// Rewrites an SPS so its VUI carries an explicit bitstream_restriction with
// the given reorder/DPB bounds. The Ingenic encoder emits High@L5.1 without
// one, so an MSE browser derives its decoded-picture buffer from the level -
// up to 16 frames (~800 ms at 20 fps) of permitted decoder-side delay for a
// stream that factually has 1 reference and no B-frames. Stating the truth
// (num_reorder_frames=0, max_dec_frame_buffering=1) lets the decoder present
// immediately. Every other SPS bit is preserved; if the SPS cannot be parsed,
// has no VUI, or already carries a restriction, the input is returned as is.
std::vector<uint8_t> sps_with_bitstream_restriction(const std::vector<uint8_t>& sps,
                                                    unsigned num_reorder_frames = 0,
                                                    unsigned max_dec_frame_buffering = 1);

}} // namespace machino::h264
