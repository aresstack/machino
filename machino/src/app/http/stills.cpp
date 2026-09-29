#include "app/http/stills.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace machino { namespace stills {

bool parse_crop(const std::string& s, Crop& out) {
    int v[4]; size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        if (at >= s.size() || s[at] < '0' || s[at] > '9') return false;
        long n = 0;
        while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
            n = n * 10 + (s[at++] - '0');
            if (n > 65535) return false;
        }
        v[i] = (int)n;
        if (i < 3) { if (at >= s.size() || s[at] != 'x') return false; ++at; }
    }
    if (at != s.size() || v[2] <= 0 || v[3] <= 0) return false;
    out.x = v[0]; out.y = v[1]; out.w = v[2]; out.h = v[3];
    return true;
}

bool nv12_crop(const uint8_t* nv12, int w, int h, const Crop& want,
               std::vector<uint8_t>& out, Crop& got) {
    if (!nv12 || w < 2 || h < 2) return false;
    int x = want.x & ~1, y = want.y & ~1;
    if (x >= w || y >= h) return false;
    int cw = (want.w + (want.x - x) + 1) & ~1;       // keep the right edge the caller asked for
    int ch = (want.h + (want.y - y) + 1) & ~1;
    if (x + cw > w) cw = (w - x) & ~1;
    if (y + ch > h) ch = (h - y) & ~1;
    if (cw <= 0 || ch <= 0) return false;
    out.resize((size_t)cw * ch * 3 / 2);
    uint8_t* o = out.data();
    for (int r = 0; r < ch; ++r) memcpy(o + (size_t)r * cw, nv12 + (size_t)(y + r) * w + x, (size_t)cw);
    const uint8_t* uv = nv12 + (size_t)w * h;
    uint8_t* ouv = o + (size_t)cw * ch;
    for (int r = 0; r < ch / 2; ++r) memcpy(ouv + (size_t)r * cw, uv + (size_t)(y / 2 + r) * w + x, (size_t)cw);
    got.x = x; got.y = y; got.w = cw; got.h = ch;
    return true;
}

std::string yuv_headers(int w, int h) {
    char b[200];
    snprintf(b, sizeof b, "X-Frame-Width: %d\r\nX-Frame-Height: %d\r\nX-Pixel-Format: NV12\r\n"
                          "X-Stride-Luma: %d\r\nX-Stride-Chroma: %d\r\n", w, h, w, w);
    return b;
}

}} // namespace machino::stills
