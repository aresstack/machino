// Application: the microphone as an RTSP track. G.711 A-law, static payload
// type 8 (RFC 3551), 8 kHz - the one audio codec every NVR, VLC and ONVIF
// client decodes, and one this build can produce without an encoder library.
// One RTP packet per 40 ms capture frame (320 bytes at 8 kHz); a 16 kHz
// microphone is decimated 2:1 on the way (http::audio_encode).
//
// Pure helpers, host-tested; the server owns sockets and sessions.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace machino { namespace rtsp {

static const int kAudioTrack = 1;          // trackID of the audio m-line (video is 0)
static const uint8_t kPayloadPcma = 8;
static const int kPcmaClock = 8000;

// The audio m-section of the DESCRIBE answer.
std::string sdp_audio_section();
// trackID from a SETUP url ("rtsp://h/ch0/trackID=1" -> 1); 0 when absent,
// which is what every single-track client sends for the video.
int track_from_url(const std::string& url);
// 12-byte RTP header into h.
void rtp_header(uint8_t* h, uint8_t pt, bool marker, uint16_t seq, uint32_t ts, uint32_t ssrc);
// interleaved=a-b out of a Transport header; false when absent.
bool interleaved_channels(const std::string& transport, int& rtp, int& rtcp);

}} // namespace machino::rtsp
