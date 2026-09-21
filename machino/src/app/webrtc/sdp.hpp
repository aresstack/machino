// WebRTC SDP for the majestic /ws/webrtc contract (see the stock webui's
// preview-webrtc.js): the browser offers recvonly video (+ optionally audio),
// the camera answers sendonly H.264 from the existing StreamHub - no second
// encoder, no transcoding. The camera is ICE-lite with one host candidate and
// a passive DTLS role; declined m-lines answer with port 0 (that is how the
// client's sdpPort() reads "not served"). Pure string work: fully host-testable.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace webrtc {

// One m-line of the browser's offer, in offer order (the answer must mirror
// every m-line in the same order or setRemoteDescription rejects it).
struct OfferMedia {
    std::string kind;        // "video" / "audio" / other
    std::string mid;
    int         h264_pt = -1;          // payload type with packetization-mode=1 (video only)
    std::string h264_profile;          // its profile-level-id (verbatim, may be empty)
};

struct Offer {
    bool ok = false;
    std::string error;                 // why parsing refused
    std::string ice_ufrag, ice_pwd;    // session- or media-level, first wins
    std::string fingerprint_alg;       // e.g. "sha-256"
    std::string fingerprint;           // upper/lower as offered, colon-separated
    std::vector<OfferMedia> media;     // every m-line, offer order
    int video_index = -1;              // index into media of the first usable video
};

// Parses a browser SDP offer. `ok` requires: at least one video m-line with an
// H264 payload of packetization-mode=1, ice credentials and a DTLS fingerprint.
Offer parse_offer(const std::string& sdp);

// Everything the answer needs from the camera side.
struct AnswerParams {
    std::string ice_ufrag, ice_pwd;    // ours (ICE-lite)
    std::string fingerprint;           // our DTLS cert, "AA:BB:..." (sha-256)
    std::string host_ip;               // the one LAN candidate
    uint16_t    port = 0;              // our single UDP port (rtcp-mux, BUNDLE)
    uint32_t    ssrc = 0;
    std::string cname = "machino";
};

// Builds the SDP answer: video sendonly on the offered H264 payload type,
// every other m-line mirrored back with port 0. Empty string when the offer
// was not parseable (callers should have checked offer.ok).
std::string build_answer(const Offer& offer, const AnswerParams& p);

}} // namespace machino::webrtc
