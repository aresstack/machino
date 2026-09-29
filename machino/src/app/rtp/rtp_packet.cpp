#include "app/rtp/rtp_packet.hpp"
#include "core/audio/g711.hpp"

namespace machino { namespace rtp {

void write_header(uint8_t* h, uint8_t pt, bool marker, uint16_t seq, uint32_t ts, uint32_t ssrc) {
    h[0] = 0x80;
    h[1] = (uint8_t)((pt & 0x7f) | (marker ? 0x80 : 0));
    h[2] = (uint8_t)(seq >> 8); h[3] = (uint8_t)seq;
    h[4] = (uint8_t)(ts >> 24); h[5] = (uint8_t)(ts >> 16); h[6] = (uint8_t)(ts >> 8); h[7] = (uint8_t)ts;
    h[8] = (uint8_t)(ssrc >> 24); h[9] = (uint8_t)(ssrc >> 16); h[10] = (uint8_t)(ssrc >> 8); h[11] = (uint8_t)ssrc;
}

size_t header_length(const uint8_t* p, size_t n) {
    if (!p || n < kHeaderBytes || (p[0] >> 6) != 2) return 0;
    size_t h = kHeaderBytes + 4 * (size_t)(p[0] & 0x0f);
    if (p[0] & 0x10) {
        if (h + 4 > n) return 0;
        h += 4 + 4 * (size_t)(((size_t)p[h + 2] << 8) | p[h + 3]);
    }
    return h <= n ? h : 0;
}

bool parse(const uint8_t* p, size_t n, Header& out) {
    const size_t h = header_length(p, n);
    if (!h) return false;
    size_t end = n;
    if (p[0] & 0x20) {                                   // padding: the last byte counts it
        const size_t pad = p[n - 1];
        if (pad == 0 || pad > n - h) return false;
        end -= pad;
    }
    out.pt = (uint8_t)(p[1] & 0x7f);
    out.marker = (p[1] & 0x80) != 0;
    out.seq = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    out.ts = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
    out.ssrc = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) | ((uint32_t)p[10] << 8) | p[11];
    out.payload_at = h;
    out.payload_len = end - h;
    return true;
}

bool decode_g711(uint8_t pt, const uint8_t* p, size_t n, std::vector<int16_t>& out) {
    if (pt != kPayloadPcmu && pt != kPayloadPcma) return false;
    const size_t base = out.size();
    out.resize(base + n);
    if (pt == kPayloadPcma) audio::alaw_decode(p, n, out.data() + base);
    else                    audio::ulaw_decode(p, n, out.data() + base);
    return true;
}

}} // namespace machino::rtp
