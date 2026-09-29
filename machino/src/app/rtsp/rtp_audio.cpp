#include "app/rtsp/rtp_audio.hpp"
#include <cctype>
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

std::string sdp_backchannel_section() {
    return "m=audio 0 RTP/AVP 0 8\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\n"
           "a=sendonly\r\na=control:trackID=" + std::to_string(kBackchannelTrack) + "\r\n";
}

bool wants_backchannel(const std::string& require_header) {
    std::string h;
    for (char c : require_header) h += (char)tolower((unsigned char)c);
    return h.find("www.onvif.org/ver20/backchannel") != std::string::npos;
}

Interleaved take_interleaved(std::string& buf, int& channel, std::string& data) {
    if (buf.empty() || buf[0] != '$') return Interleaved::NotOne;
    if (buf.size() < 4) return Interleaved::Partial;
    const size_t len = ((size_t)(uint8_t)buf[2] << 8) | (uint8_t)buf[3];
    if (len > kMaxInterleaved) return Interleaved::TooLarge;
    if (buf.size() < 4 + len) return Interleaved::Partial;
    channel = (uint8_t)buf[1];
    data.assign(buf, 4, len);
    buf.erase(0, 4 + len);
    return Interleaved::Frame;
}

}} // namespace machino::rtsp
