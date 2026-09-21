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

// --- SPS VUI surgery ---------------------------------------------------------
// A minimal Exp-Golomb reader/writer pair over the emulation-stripped RBSP.
// The parse mirrors ITU-T H.264 7.3.2.1.1 + E.1.1 exactly as far as the
// bitstream_restriction_flag; everything before that flag is copied verbatim.
namespace {

struct BitReader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;       // bit position
    bool   bad = false;
    explicit BitReader(const std::vector<uint8_t>& v) : b(v) {}
    int bit() {
        if (pos >= b.size() * 8) { bad = true; return 0; }
        int v = (b[pos >> 3] >> (7 - (pos & 7))) & 1; ++pos; return v;
    }
    uint32_t u(int n) { uint32_t v = 0; for (int i = 0; i < n; ++i) v = (v << 1) | (uint32_t)bit(); return v; }
    uint32_t ue() {
        int z = 0; while (bit() == 0 && z < 32 && !bad) ++z;
        if (z >= 32) { bad = true; return 0; }
        uint32_t v = (1u << z) - 1;
        for (int i = 0; i < z; ++i) v += (uint32_t)bit() << (z - 1 - i);
        return v;
    }
    int32_t se() { uint32_t k = ue(); return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2); }
};

struct BitWriter {
    std::vector<uint8_t> b;
    int fill = 0;         // bits used in the last byte
    void bit(int v) {
        if (fill == 0) b.push_back(0);
        if (v) b.back() |= (uint8_t)(1 << (7 - fill));
        fill = (fill + 1) & 7;
    }
    void u(uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) bit((int)((v >> i) & 1)); }
    void ue(uint32_t v) {
        uint32_t x = v + 1; int n = 0;
        for (uint32_t t = x; t > 1; t >>= 1) ++n;
        for (int i = 0; i < n; ++i) bit(0);
        u(x, n + 1);
    }
};

// hrd_parameters() (E.1.2) - contents irrelevant, only the length matters.
void skip_hrd(BitReader& r) {
    uint32_t cnt = r.ue();
    r.u(4); r.u(4);
    if (cnt > 31) { r.bad = true; return; }
    for (uint32_t i = 0; i <= cnt; ++i) { r.ue(); r.ue(); r.u(1); }
    r.u(5); r.u(5); r.u(5); r.u(5);
}

} // namespace

std::vector<uint8_t> sps_with_bitstream_restriction(const std::vector<uint8_t>& sps,
                                                    unsigned num_reorder_frames,
                                                    unsigned max_dec_frame_buffering) {
    if (sps.size() < 5 || (sps[0] & 0x1f) != 7) return sps;
    // strip emulation prevention (00 00 03 -> 00 00)
    std::vector<uint8_t> rbsp; rbsp.reserve(sps.size());
    for (size_t i = 0; i < sps.size(); ++i) {
        if (i >= 2 && sps[i] == 3 && sps[i-1] == 0 && sps[i-2] == 0) continue;
        rbsp.push_back(sps[i]);
    }
    BitReader r(rbsp);
    r.u(8);                                   // nal header
    uint32_t profile_idc = r.u(8);
    r.u(8);                                   // constraint flags
    r.u(8);                                   // level_idc (deliberately untouched)
    r.ue();                                   // sps id
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44  || profile_idc == 83  || profile_idc == 86  || profile_idc == 118 ||
        profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135) {
        uint32_t chroma = r.ue();
        if (chroma == 3) r.u(1);
        r.ue(); r.ue(); r.u(1);
        if (r.u(1)) {                          // scaling lists
            int cnt = chroma != 3 ? 8 : 12;
            for (int i = 0; i < cnt && !r.bad; ++i) {
                if (!r.u(1)) continue;
                int size = i < 6 ? 16 : 64, last = 8, next = 8;
                for (int j = 0; j < size; ++j) {
                    if (next != 0) next = (last + r.se() + 256) % 256;
                    if (next != 0) last = next;
                }
            }
        }
    }
    r.ue();                                   // log2_max_frame_num_minus4
    uint32_t poc = r.ue();
    if (poc == 0) r.ue();
    else if (poc == 1) {
        r.u(1); r.se(); r.se();
        uint32_t n = r.ue();
        if (n > 255) return sps;
        for (uint32_t i = 0; i < n; ++i) r.se();
    }
    r.ue();                                   // max_num_ref_frames
    r.u(1);
    r.ue(); r.ue();                           // pic size in mbs
    if (!r.u(1)) r.u(1);                      // frame_mbs_only / mb_adaptive
    r.u(1);
    if (r.u(1)) { r.ue(); r.ue(); r.ue(); r.ue(); }   // cropping
    if (!r.u(1)) return sps;                  // no VUI: nothing to anchor to
    // vui_parameters (E.1.1) up to bitstream_restriction_flag
    if (r.u(1)) { uint32_t ar = r.u(8); if (ar == 255) { r.u(16); r.u(16); } }
    if (r.u(1)) r.u(1);
    if (r.u(1)) { r.u(3); r.u(1); if (r.u(1)) { r.u(8); r.u(8); r.u(8); } }
    if (r.u(1)) { r.ue(); r.ue(); }
    if (r.u(1)) { r.u(32); r.u(32); r.u(1); } // timing_info
    bool nal_hrd = r.u(1); if (nal_hrd) skip_hrd(r);
    bool vcl_hrd = r.u(1); if (vcl_hrd) skip_hrd(r);
    if (nal_hrd || vcl_hrd) r.u(1);
    r.u(1);                                   // pic_struct_present
    size_t flag_pos = r.pos;                  // bitstream_restriction_flag sits here
    int flag = r.bit();
    if (r.bad || flag == 1) return sps;       // unparseable or already restricted
    // rebuild: verbatim prefix + flag=1 + restriction fields + rbsp trailing
    BitWriter w;
    for (size_t i = 0; i < flag_pos; ++i)
        w.bit((rbsp[i >> 3] >> (7 - (i & 7))) & 1);
    w.bit(1);                                 // bitstream_restriction_flag
    w.bit(1);                                 // motion_vectors_over_pic_boundaries
    w.ue(0);                                  // max_bytes_per_pic_denom (unlimited)
    w.ue(0);                                  // max_bits_per_mb_denom (unlimited)
    w.ue(16); w.ue(16);                       // log2 max mv lengths
    w.ue(num_reorder_frames);
    w.ue(max_dec_frame_buffering);
    w.bit(1);                                 // rbsp_stop_one_bit
    while (w.fill != 0) w.bit(0);             // byte align
    // re-apply emulation prevention over the payload (after the header byte)
    std::vector<uint8_t> out; out.reserve(w.b.size() + 4);
    out.push_back(w.b[0]);
    int zeros = 0;
    for (size_t i = 1; i < w.b.size(); ++i) {
        if (zeros == 2 && w.b[i] <= 3) { out.push_back(3); zeros = 0; }
        out.push_back(w.b[i]);
        zeros = (w.b[i] == 0) ? zeros + 1 : 0;
    }
    return out;
}

}} // namespace machino::h264
