#include "app/webrtc/rtp.hpp"
#include "app/rtsp/h264_nal.hpp"

namespace machino { namespace webrtc {

namespace {

std::vector<uint8_t> header(const RtpParams& p, uint16_t seq, uint32_t ts, bool marker) {
    std::vector<uint8_t> h(12);
    h[0] = 0x80;                                       // v=2, no padding/ext/csrc
    h[1] = (uint8_t)((marker ? 0x80 : 0) | (p.payload_type & 0x7f));
    h[2] = (uint8_t)(seq >> 8); h[3] = (uint8_t)seq;
    h[4] = (uint8_t)(ts >> 24); h[5] = (uint8_t)(ts >> 16); h[6] = (uint8_t)(ts >> 8); h[7] = (uint8_t)ts;
    h[8] = (uint8_t)(p.ssrc >> 24); h[9] = (uint8_t)(p.ssrc >> 16); h[10] = (uint8_t)(p.ssrc >> 8); h[11] = (uint8_t)p.ssrc;
    return h;
}

} // namespace

std::vector<std::vector<uint8_t>> packetize_h264(const uint8_t* au, size_t len,
                                                 uint32_t ts90k, uint16_t& seq,
                                                 const RtpParams& p) {
    std::vector<std::vector<uint8_t>> out;
    h264::Nal nals[64];
    const size_t n = h264::split(au, len, nals, 64);
    // find the last NAL that will be sent, for the marker bit
    size_t last = n;
    for (size_t i = n; i-- > 0; ) { if (nals[i].type != 9) { last = i; break; } }
    for (size_t i = 0; i < n; ++i) {
        const h264::Nal& nal = nals[i];
        if (nal.type == 9 || nal.len == 0) continue;   // drop access-unit delimiters
        const bool is_last = i == last;
        if (nal.len <= p.max_payload) {
            std::vector<uint8_t> pkt = header(p, seq++, ts90k, is_last);
            pkt.insert(pkt.end(), nal.p, nal.p + nal.len);
            out.push_back(std::move(pkt));
            continue;
        }
        // FU-A: indicator keeps F+NRI, type 28; header carries S/E + original type
        const uint8_t indicator = (uint8_t)((nal.p[0] & 0xe0) | 28);
        const uint8_t type = (uint8_t)(nal.p[0] & 0x1f);
        size_t off = 1;                                // original NAL header is not repeated
        const size_t chunk = p.max_payload - 2;        // indicator + fu header
        while (off < nal.len) {
            const size_t take = nal.len - off < chunk ? nal.len - off : chunk;
            const bool start = off == 1;
            const bool end = off + take == nal.len;
            std::vector<uint8_t> pkt = header(p, seq++, ts90k, is_last && end);
            pkt.push_back(indicator);
            pkt.push_back((uint8_t)((start ? 0x80 : 0) | (end ? 0x40 : 0) | type));
            pkt.insert(pkt.end(), nal.p + off, nal.p + off + take);
            out.push_back(std::move(pkt));
            off += take;
        }
    }
    return out;
}

bool is_rtcp(const uint8_t* p, size_t n) {
    if (n < 8 || (p[0] >> 6) != 2) return false;
    const uint8_t pt = (uint8_t)(p[1] & 0x7f);
    return pt >= 64 && pt <= 95;                       // RFC 5761 demux range
}

RtcpInfo parse_rtcp(const uint8_t* p, size_t n) {
    RtcpInfo r;
    size_t at = 0;
    while (at + 4 <= n) {
        if ((p[at] >> 6) != 2) break;
        const uint8_t fmt = (uint8_t)(p[at] & 0x1f);
        const uint8_t pt = p[at + 1];
        const size_t plen = 4 * (size_t)(((p[at + 2] << 8) | p[at + 3]) + 1);
        if (at + plen > n) break;
        if (pt == 201) r.receiver_report = true;
        if (pt == 206 && fmt == 1) r.pli = true;       // PSFB / PLI
        at += plen;
    }
    return r;
}

}} // namespace machino::webrtc
