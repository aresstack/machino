#include "app/rtsp/h264_nal.hpp"

namespace machino { namespace h264 {

size_t split(const uint8_t* d, size_t n, Nal* out, size_t max) {
    size_t cnt = 0, i = 0, start = (size_t)-1;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i+1] == 0 && (d[i+2] == 1 || (i + 4 <= n && d[i+2] == 0 && d[i+3] == 1))) {
            size_t sc = d[i+2] == 1 ? 3 : 4;
            if (start != (size_t)-1) {
                size_t end = i; while (end > start && d[end-1] == 0) --end;   // trailing zero bytes belong to the next start code
                if (cnt < max && end > start) out[cnt++] = { d + start, end - start, (uint8_t)(d[start] & 0x1f) };
            }
            start = i + sc; i += sc;
        } else ++i;
    }
    if (start != (size_t)-1 && start < n && cnt < max) out[cnt++] = { d + start, n - start, (uint8_t)(d[start] & 0x1f) };
    return cnt;
}

bool extract_params(const uint8_t* d, size_t n, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
    Nal nal[32]; size_t c = split(d, n, nal, 32);
    bool s = false, p = false;
    for (size_t i = 0; i < c; ++i) {
        if (nal[i].type == 7) { sps.assign(nal[i].p, nal[i].p + nal[i].len); s = true; }
        if (nal[i].type == 8) { pps.assign(nal[i].p, nal[i].p + nal[i].len); p = true; }
    }
    return s && p;
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i+1] << 8 : 0) | (i + 2 < n ? p[i+2] : 0);
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63];
        o += i + 1 < n ? T[(v >> 6) & 63] : '=';
        o += i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

}} // namespace machino::h264
