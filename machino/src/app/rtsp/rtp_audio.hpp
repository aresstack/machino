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
#include <vector>

namespace machino { namespace rtsp {

static const int kAudioTrack = 1;          // trackID of the audio m-line (video is 0)
static const int kBackchannelTrack = 2;    // ONVIF backchannel: the client talks, the camera plays
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

// The ONVIF backchannel m-section (ONVIF Streaming Spec 5.3): audio the
// CLIENT sends, marked a=sendonly from the server's side, PCMU and PCMA.
std::string sdp_backchannel_section();
// Does a request carry "Require: www.onvif.org/ver20/backchannel"?
bool wants_backchannel(const std::string& require_header);

// Interleaved RTP/RTCP on the RTSP connection (RFC 2326 10.12): "$", channel,
// 16-bit length, data. The buffer holds whatever the socket delivered.
enum class Interleaved { NotOne, Partial, Frame };
// Frame: the frame is removed from `buf` into `channel`/`data`. Partial: wait
// for more bytes. NotOne: `buf` does not start with '$' (a text request).
Interleaved take_interleaved(std::string& buf, int& channel, std::string& data);

// Plain RTP header parse: payload type and where the payload is (CSRCs, a
// header extension and padding accounted for). False on a malformed packet.
bool rtp_payload(const uint8_t* p, size_t n, uint8_t& pt, size_t& off, size_t& len);
// G.711 payload (PT 0 PCMU / 8 PCMA) to 8 kHz PCM, appended. False for any
// other payload type.
bool decode_g711(uint8_t pt, const uint8_t* p, size_t n, std::vector<int16_t>& out);

}} // namespace machino::rtsp
