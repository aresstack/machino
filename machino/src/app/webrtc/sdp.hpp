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
    std::vector<int> pts;              // payload types in OFFER order = the
                                       // offerer's preference, which is the
                                       // only place that order exists
    int         h264_pt = -1;          // payload type with packetization-mode=1 (video only)
    std::string h264_profile;          // its profile-level-id (verbatim, may be empty)
    // Audio: the G.711 payloads the browser offers (every WebRTC browser does,
    // next to Opus) and the direction it asks for, from ITS side.
    int         pcma_pt = -1, pcmu_pt = -1;
    std::string direction = "sendrecv";   // sendrecv | sendonly | recvonly | inactive
};

struct Offer {
    bool ok = false;
    std::string error;                 // why parsing refused
    std::string ice_ufrag, ice_pwd;    // session- or media-level, first wins
    std::string fingerprint_alg;       // e.g. "sha-256"
    std::string fingerprint;           // upper/lower as offered, colon-separated
    std::vector<OfferMedia> media;     // every m-line, offer order
    int video_index = -1;              // index into media of the first usable video
    int audio_index = -1;              // first audio m-line offering PCMA or PCMU
};

// Parses a browser SDP offer. `ok` requires: at least one video m-line with an
// H264 payload of packetization-mode=1, ice credentials and a DTLS fingerprint.
//
// Among the offered mode-1 H264 payloads the choice is made in OFFER order,
// except that an exact match on `prefer_profile` (the profile-level-id this
// camera really emits, e.g. "640033" from the SPS) wins - then the answer
// names the stream instead of approximating it. Empty = no preference, which
// falls back to the browser's own order.
Offer parse_offer(const std::string& sdp, const std::string& prefer_profile = "");

// Everything the answer needs from the camera side.
struct AnswerParams {
    std::string ice_ufrag, ice_pwd;    // ours (ICE-lite)
    std::string fingerprint;           // our DTLS cert, "AA:BB:..." (sha-256)
    std::string host_ip;               // the one LAN candidate
    uint16_t    port = 0;              // our single UDP port (rtcp-mux, BUNDLE)
    uint32_t    ssrc = 0;
    std::string cname = "machino";
    // Audio, as settled by plan_audio(). Default: none - every audio m-line is
    // declined, exactly as before audio existed.
    int         audio_pt = -1;         // 8 PCMA / 0 PCMU, -1 = no audio
    bool        audio_send = false, audio_recv = false;
    uint32_t    audio_ssrc = 0;
};

// What the camera will do with the offered audio. The camera SENDS (its
// microphone) when it may and the browser receives; it RECEIVES (talkback to
// the speaker) when it may and the browser sends. PCMA is preferred: the same
// codec as the RTSP track. Neither direction = the m-line is declined.
struct AudioPlan { int pt = -1; bool send = false, recv = false; };
AudioPlan plan_audio(const Offer& offer, bool camera_can_send, bool camera_can_recv);

// Builds the SDP answer: video sendonly on the offered H264 payload type,
// every other m-line mirrored back with port 0. Empty string when the offer
// was not parseable (callers should have checked offer.ok).
std::string build_answer(const Offer& offer, const AnswerParams& p);

}} // namespace machino::webrtc
