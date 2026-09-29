#include "app/rtsp/rtp_audio.hpp"
#include "core/audio/g711.hpp"
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
    if (buf.size() < 4 + len) return Interleaved::Partial;
    channel = (uint8_t)buf[1];
    data.assign(buf, 4, len);
    buf.erase(0, 4 + len);
    return Interleaved::Frame;
}

bool rtp_payload(const uint8_t* p, size_t n, uint8_t& pt, size_t& off, size_t& len) {
    if (n < 12 || (p[0] >> 6) != 2) return false;
    size_t h = 12 + 4 * (size_t)(p[0] & 0x0f);
    if (p[0] & 0x10) {
        if (h + 4 > n) return false;
        h += 4 + 4 * (size_t)((p[h + 2] << 8) | p[h + 3]);
    }
    if (h > n) return false;
    size_t end = n;
    if (p[0] & 0x20) {
        const size_t pad = p[n - 1];
        if (pad == 0 || pad > n - h) return false;
        end -= pad;
    }
    pt = (uint8_t)(p[1] & 0x7f);
    off = h;
    len = end - h;
    return true;
}

bool decode_g711(uint8_t pt, const uint8_t* p, size_t n, std::vector<int16_t>& out) {
    if (pt != 0 && pt != 8) return false;
    const size_t base = out.size();
    out.resize(base + n);
    for (size_t i = 0; i < n; ++i) out[base + i] = pt == 8 ? audio::alaw_decode(p[i]) : audio::ulaw_decode(p[i]);
    return true;
}

}} // namespace machino::rtsp
