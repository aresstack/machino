// Application: the RTP fixed header (RFC 3550 5.1) and the G.711 payloads
// (RFC 3551 static types 0 and 8) - ONE place for what RTSP and WebRTC both
// speak. Every transport used to carry its own copy of the 12-byte layout
// and its own header parser; the parser in particular sits on the path that
// untrusted peers reach (backchannel, talkback), so there is exactly one.
// Pure, host-tested.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace machino { namespace rtp {

constexpr size_t  kHeaderBytes = 12;
constexpr uint8_t kPayloadPcmu = 0;
constexpr uint8_t kPayloadPcma = 8;

// The 12-byte header: v=2, no padding, no extension, no CSRC.
void write_header(uint8_t* h, uint8_t pt, bool marker, uint16_t seq, uint32_t ts, uint32_t ssrc);

// Length of the header as sent: fixed part, CSRC list, extension (RFC 8285
// one/two-byte, or any other profile - only the length word matters). 0 when
// the packet is malformed or the header runs past `n`.
size_t header_length(const uint8_t* p, size_t n);

struct Header {
    uint8_t  pt = 0;
    bool     marker = false;
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0;
    size_t   payload_at = 0;          // header_length
    size_t   payload_len = 0;         // without padding
};
// Full parse including padding. False on a malformed packet.
bool parse(const uint8_t* p, size_t n, Header& out);

// A G.711 payload (PT 0 PCMU / 8 PCMA) to 8 kHz PCM, appended to `out`.
// False for any other payload type.
bool decode_g711(uint8_t pt, const uint8_t* p, size_t n, std::vector<int16_t>& out);

}} // namespace machino::rtp
