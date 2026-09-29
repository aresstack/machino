// Application: JPEG over RTP (RFC 2435) for the MJPEG RTSP mount
// (rtsp://CAM/stream=2) - pure, host-tested.
//
// RFC 2435 does not carry the JPEG headers: the receiver rebuilds them from
// the type (sampling), Q, width/height and - with Q >= 128 - the quantisation
// tables, which travel in-band in the first packet of each frame. So the
// packetiser parses the frame the encoder made, keeps only the entropy-coded
// scan, and says in the RTP payload header what the rebuilt headers must be.
// Two assumptions the format itself makes: 8-bit baseline, and the standard
// Huffman tables of ISO 10918-1 Annex K (hardware JPEG encoders use them).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace rtsp {

struct JpegFrame {
    int     width = 0, height = 0;
    uint8_t type = 0;                    // 0 = 4:2:2, 1 = 4:2:0; +64 with restart markers
    uint16_t restart_interval = 0;
    std::vector<uint8_t> qtables;        // luma then chroma, 64 bytes each, zig-zag as in DQT
    const uint8_t* scan = nullptr;       // entropy-coded data, EOI stripped
    size_t   scan_len = 0;
};

// Parses a baseline JPEG. False (with the reason) for what RFC 2435 cannot
// carry: progressive/12-bit, not 3-component YCbCr at 4:2:0/4:2:2, over
// 2040 pixels in either direction, or no SOS.
bool parse_jpeg(const uint8_t* p, size_t n, JpegFrame& out, std::string& why);

// The RTP payloads for one frame (no RTP header), each at most `mtu` bytes.
// The caller sets the marker on the last one; all share one timestamp.
std::vector<std::vector<uint8_t>> jpeg_rtp_payloads(const JpegFrame& f, size_t mtu);

// SDP media section for the MJPEG track (static payload type 26).
std::string sdp_jpeg_section();

constexpr uint8_t kPayloadJpeg = 26;

}} // namespace machino::rtsp
