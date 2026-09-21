// RTP H.264 packetization (RFC 6184, packetization-mode=1) and the RTCP bits
// the camera cares about. One access unit in, RTP packets out: single-NAL
// where a NAL fits, FU-A fragmentation where it does not; SPS/PPS travel
// in-band (WebRTC has no avcC), the marker bit closes the access unit. The
// SRTP layer encrypts these later - here they are plain. Pure and host-tested.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace machino { namespace webrtc {

struct RtpParams {
    uint8_t  payload_type = 102;
    uint32_t ssrc = 0;
    // Payload budget per packet BEFORE the 12-byte RTP header (and before the
    // SRTP tag): 1200 keeps header+tag under a 1280-safe MTU on any LAN.
    size_t   max_payload = 1200;
};

// Packetizes one Annex-B access unit stamped `ts90k`. `seq` is the caller's
// running RTP sequence number, advanced per packet. Access-unit delimiters
// (NAL type 9) are dropped; everything else - SPS, PPS, IDR, slices - is sent.
std::vector<std::vector<uint8_t>> packetize_h264(const uint8_t* au, size_t len,
                                                 uint32_t ts90k, uint16_t& seq,
                                                 const RtpParams& p);

// What an incoming (already decrypted) RTCP compound packet asks of us.
struct RtcpInfo {
    bool pli = false;        // Picture Loss Indication -> request an IDR
    bool receiver_report = false;
};
RtcpInfo parse_rtcp(const uint8_t* p, size_t n);

// True when a demuxed datagram is RTCP rather than RTP (rtcp-mux, RFC 5761:
// payload types 64..95 in the second byte).
bool is_rtcp(const uint8_t* p, size_t n);

}} // namespace machino::webrtc
