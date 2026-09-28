#include "app/rtsp/rtp_audio.hpp"
#include <cstdlib>

namespace machino { namespace rtsp {

std::string sdp_audio_section() {
    return "m=audio 0 RTP/AVP 8\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:8 PCMA/8000\r\n"
           "a=control:trackID=" + std::to_string(kAudioTrack) + "\r\n";
}

int track_from_url(const std::string& url) {
    const size_t p = url.rfind("trackID=");
    if (p == std::string::npos) return 0;
    return atoi(url.c_str() + p + 8);
}

void rtp_header(uint8_t* h, uint8_t pt, bool marker, uint16_t seq, uint32_t ts, uint32_t ssrc) {
    h[0] = 0x80;
    h[1] = (uint8_t)((pt & 0x7f) | (marker ? 0x80 : 0));
    h[2] = (uint8_t)(seq >> 8);  h[3] = (uint8_t)seq;
    h[4] = (uint8_t)(ts >> 24);  h[5] = (uint8_t)(ts >> 16);  h[6] = (uint8_t)(ts >> 8);  h[7] = (uint8_t)ts;
    h[8] = (uint8_t)(ssrc >> 24); h[9] = (uint8_t)(ssrc >> 16); h[10] = (uint8_t)(ssrc >> 8); h[11] = (uint8_t)ssrc;
}

bool interleaved_channels(const std::string& transport, int& rtp, int& rtcp) {
    const size_t p = transport.find("interleaved=");
    if (p == std::string::npos) return false;
    rtp = atoi(transport.c_str() + p + 12);
    const size_t dash = transport.find('-', p + 12);
    const size_t semi = transport.find(';', p + 12);
    rtcp = (dash != std::string::npos && (semi == std::string::npos || dash < semi))
         ? atoi(transport.c_str() + dash + 1) : rtp + 1;
    return true;
}

}} // namespace machino::rtsp
