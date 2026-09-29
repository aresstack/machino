#include "app/rtsp/rtp_jpeg.hpp"
#include <algorithm>
#include <cstring>

namespace machino { namespace rtsp {

bool parse_jpeg(const uint8_t* p, size_t n, JpegFrame& out, std::string& why) {
    out = JpegFrame{};
    if (!p || n < 4 || p[0] != 0xff || p[1] != 0xd8) { why = "not a JPEG (no SOI)"; return false; }
    uint8_t qt[4][64]; bool have_qt[4] = {false, false, false, false};
    int comp_q[3] = {0, 0, 0}, comps = 0; uint8_t samp0 = 0; bool sof = false;
    size_t i = 2;
    while (i + 4 <= n) {
        if (p[i] != 0xff) { why = "corrupt marker sequence"; return false; }
        const uint8_t m = p[i + 1];
        if (m == 0xff) { ++i; continue; }                           // fill byte
        const size_t len = ((size_t)p[i + 2] << 8) | p[i + 3];
        if (len < 2 || i + 2 + len > n) { why = "truncated segment"; return false; }
        const uint8_t* s = p + i + 4; const size_t sl = len - 2;
        if (m == 0xdb) {                                            // DQT: one or more tables
            size_t k = 0;
            while (k < sl) {
                const uint8_t pq = s[k] >> 4, tq = s[k] & 0x0f;
                if (pq != 0) { why = "16-bit quantisation tables"; return false; }
                if (tq > 3 || k + 65 > sl) { why = "bad DQT"; return false; }
                memcpy(qt[tq], s + k + 1, 64); have_qt[tq] = true;
                k += 65;
            }
        } else if (m == 0xc0) {                                     // SOF0 baseline
            if (sl < 6 + 9 || s[0] != 8) { why = "not 8-bit"; return false; }
            out.height = (s[1] << 8) | s[2];
            out.width  = (s[3] << 8) | s[4];
            comps = s[5];
            if (comps != 3 || sl < 6 + 3u * comps) { why = "not 3-component YCbCr"; return false; }
            samp0 = s[7];
            for (int c = 0; c < 3; ++c) { comp_q[c] = s[6 + 3 * c + 2]; if (comp_q[c] > 3) { why = "bad quantisation table selector"; return false; } }
            if (s[6 + 3 + 1] != 0x11 || s[6 + 6 + 1] != 0x11) { why = "unsupported chroma sampling"; return false; }
            sof = true;
        } else if (m >= 0xc1 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            why = "not baseline JPEG"; return false;
        } else if (m == 0xdd) {                                     // DRI
            if (sl < 2) { why = "bad DRI"; return false; }
            out.restart_interval = (uint16_t)((s[0] << 8) | s[1]);
        } else if (m == 0xda) {                                     // SOS: the scan follows
            if (!sof) { why = "SOS before SOF"; return false; }
            size_t start = i + 2 + len, end = n;
            if (end >= start + 2 && p[end - 2] == 0xff && p[end - 1] == 0xd9) end -= 2;
            out.scan = p + start; out.scan_len = end - start;
            if (samp0 == 0x21) out.type = 0;
            else if (samp0 == 0x22) out.type = 1;
            else { why = "unsupported luma sampling"; return false; }
            if (out.restart_interval) out.type += 64;
            if (out.width <= 0 || out.height <= 0 || out.width > 2040 || out.height > 2040) { why = "over 2040 pixels (RFC 2435 limit)"; return false; }
            if (!have_qt[comp_q[0]] || !have_qt[comp_q[1]]) { why = "missing quantisation table"; return false; }
            out.qtables.assign(qt[comp_q[0]], qt[comp_q[0]] + 64);
            out.qtables.insert(out.qtables.end(), qt[comp_q[1]], qt[comp_q[1]] + 64);
            if (out.scan_len == 0) { why = "empty scan"; return false; }
            return true;
        }
        i += 2 + len;
    }
    why = "no SOS";
    return false;
}

std::vector<std::vector<uint8_t>> jpeg_rtp_payloads(const JpegFrame& f, size_t mtu) {
    std::vector<std::vector<uint8_t>> out;
    size_t off = 0;
    do {
        std::vector<uint8_t> pk;
        pk.push_back(0);                                            // type-specific
        pk.push_back((uint8_t)(off >> 16)); pk.push_back((uint8_t)(off >> 8)); pk.push_back((uint8_t)off);
        pk.push_back(f.type);
        pk.push_back(255);                                          // Q 255: tables in-band
        pk.push_back((uint8_t)(f.width / 8)); pk.push_back((uint8_t)(f.height / 8));
        if (f.type >= 64) {                                         // restart marker header
            pk.push_back((uint8_t)(f.restart_interval >> 8)); pk.push_back((uint8_t)f.restart_interval);
            pk.push_back(0xff); pk.push_back(0xff);                 // F=1 L=1 count=0x3fff: whole frame
        }
        if (off == 0) {                                             // quantisation table header
            pk.push_back(0); pk.push_back(0);                       // MBZ, precision (8-bit)
            pk.push_back((uint8_t)(f.qtables.size() >> 8)); pk.push_back((uint8_t)f.qtables.size());
            pk.insert(pk.end(), f.qtables.begin(), f.qtables.end());
        }
        const size_t room = mtu > pk.size() ? mtu - pk.size() : 0;
        const size_t take = std::min(room, f.scan_len - off);
        pk.insert(pk.end(), f.scan + off, f.scan + off + take);
        off += take;
        out.push_back(std::move(pk));
        if (take == 0) break;                                       // an mtu too small to progress
    } while (off < f.scan_len);
    return out;
}

std::string sdp_jpeg_section() {
    return "m=video 0 RTP/AVP 26\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:26 JPEG/90000\r\na=control:trackID=0\r\n";
}

}} // namespace machino::rtsp
