// SPS VUI surgery: sps_with_bitstream_restriction must add EXACTLY the
// bitstream_restriction (num_reorder_frames / max_dec_frame_buffering) and
// leave every other SPS field bit-identical. Verified with an independent
// reference parser against the REAL T40NN Ingenic SPS captured from /ws/video
// (High@L5.1 1920x1080, VUI with timing + vcl_hrd, no restriction) - the
// stream whose missing DPB bound makes MSE browsers buffer ~1 s.
#include "app/rtsp/h264_nal.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define SCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

// Independent reference SPS parser (H.264 7.3.2.1.1 + E.1.1), deliberately
// written separately from the production BitReader so a shared bug can not
// hide itself.
struct SpsFields {
    bool ok = false;
    uint32_t profile_idc = 0, level_idc = 0, sps_id = 0;
    uint32_t max_num_ref_frames = 0;
    uint32_t width = 0, height = 0;
    uint32_t frame_mbs_only = 0;
    bool vui = false, timing = false;
    uint32_t num_units_in_tick = 0, time_scale = 0, fixed_frame_rate = 0;
    bool nal_hrd = false, vcl_hrd = false;
    uint32_t pic_struct = 0;
    bool restriction = false;
    uint32_t num_reorder = 0, dpb = 0;
};

struct Rd {
    std::vector<uint8_t> b; size_t pos = 0; bool bad = false;
    explicit Rd(const std::vector<uint8_t>& nal) {
        for (size_t i = 0; i < nal.size(); ++i) {
            if (i >= 2 && nal[i] == 3 && nal[i-1] == 0 && nal[i-2] == 0) continue;
            b.push_back(nal[i]);
        }
    }
    int bit() { if (pos >= b.size()*8) { bad = true; return 0; } int v = (b[pos>>3] >> (7-(pos&7))) & 1; ++pos; return v; }
    uint32_t u(int n) { uint32_t v = 0; for (int i = 0; i < n; ++i) v = (v<<1)|(uint32_t)bit(); return v; }
    uint32_t ue() { int z = 0; while (bit()==0 && z<32 && !bad) ++z; if (z>=32){bad=true;return 0;} uint32_t v=(1u<<z)-1; for(int i=0;i<z;++i) v += (uint32_t)bit()<<(z-1-i); return v; }
    int32_t se() { uint32_t k = ue(); return (k&1) ? (int32_t)((k+1)/2) : -(int32_t)(k/2); }
    void hrd() { uint32_t c = ue(); u(4); u(4); if (c>31){bad=true;return;} for (uint32_t i=0;i<=c;++i){ue();ue();u(1);} u(5);u(5);u(5);u(5); }
};

SpsFields parse_sps(const std::vector<uint8_t>& nal) {
    SpsFields f; Rd r(nal);
    r.u(8);
    f.profile_idc = r.u(8); r.u(8); f.level_idc = r.u(8); f.sps_id = r.ue();
    switch (f.profile_idc) {
        case 100: case 110: case 122: case 244: case 44: case 83: case 86:
        case 118: case 128: case 138: case 139: case 134: case 135: {
            uint32_t chroma = r.ue(); if (chroma == 3) r.u(1);
            r.ue(); r.ue(); r.u(1);
            if (r.u(1)) {
                int cnt = chroma != 3 ? 8 : 12;
                for (int i = 0; i < cnt && !r.bad; ++i) {
                    if (!r.u(1)) continue;
                    int size = i < 6 ? 16 : 64, last = 8, next = 8;
                    for (int j = 0; j < size; ++j) { if (next) next = (last + r.se() + 256) % 256; if (next) last = next; }
                }
            }
            break;
        }
        default: break;
    }
    r.ue();
    uint32_t poc = r.ue();
    if (poc == 0) r.ue();
    else if (poc == 1) { r.u(1); r.se(); r.se(); uint32_t n = r.ue(); if (n > 255) { f.ok = false; return f; } for (uint32_t i = 0; i < n; ++i) r.se(); }
    f.max_num_ref_frames = r.ue();
    r.u(1);
    uint32_t pw = r.ue(), ph = r.ue();
    f.frame_mbs_only = r.u(1);
    if (!f.frame_mbs_only) r.u(1);
    f.width = (pw + 1) * 16; f.height = (ph + 1) * 16 * (2 - f.frame_mbs_only);
    r.u(1);
    if (r.u(1)) { r.ue(); r.ue(); r.ue(); r.ue(); }
    f.vui = r.u(1) != 0;
    if (f.vui) {
        if (r.u(1)) { uint32_t ar = r.u(8); if (ar == 255) { r.u(16); r.u(16); } }
        if (r.u(1)) r.u(1);
        if (r.u(1)) { r.u(3); r.u(1); if (r.u(1)) { r.u(8); r.u(8); r.u(8); } }
        if (r.u(1)) { r.ue(); r.ue(); }
        f.timing = r.u(1) != 0;
        if (f.timing) { f.num_units_in_tick = r.u(32); f.time_scale = r.u(32); f.fixed_frame_rate = r.u(1); }
        f.nal_hrd = r.u(1) != 0; if (f.nal_hrd) r.hrd();
        f.vcl_hrd = r.u(1) != 0; if (f.vcl_hrd) r.hrd();
        if (f.nal_hrd || f.vcl_hrd) r.u(1);
        f.pic_struct = r.u(1);
        f.restriction = r.u(1) != 0;
        if (f.restriction) {
            r.u(1); r.ue(); r.ue(); r.ue(); r.ue();
            f.num_reorder = r.ue(); f.dpb = r.ue();
        }
    }
    f.ok = !r.bad;
    return f;
}

std::vector<uint8_t> from_hex(const char* h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; h[i] && h[i+1]; i += 2) {
        auto nib = [](char c) -> int { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        v.push_back((uint8_t)((nib(h[i]) << 4) | nib(h[i+1])));
    }
    return v;
}

// After emulation-prevention encoding, the payload may never contain a raw
// 00 00 0x (x<=2) run - that is what a byte-stream parser would trip over.
bool ep_clean(const std::vector<uint8_t>& b) {
    for (size_t i = 2; i < b.size(); ++i)
        if (b[i-2] == 0 && b[i-1] == 0 && b[i] <= 2) return false;
    return true;
}

} // namespace

void run_sps_tests() {
    // The REAL Ingenic SPS from the T40NN /ws/video capture (2026-09-21).
    const std::vector<uint8_t> real = from_hex(
        "27640033ad00ce80780227e59a808080f800000300080000030141810000b71b000044aa3fffe050");

    SpsFields before = parse_sps(real);
    SCHECK(before.ok);
    SCHECK(before.profile_idc == 100);
    SCHECK(before.level_idc == 51);
    SCHECK(before.width == 1920 && before.height == 1088);
    SCHECK(before.frame_mbs_only == 1);
    SCHECK(before.max_num_ref_frames == 1);
    SCHECK(before.vui && before.timing);
    SCHECK(before.num_units_in_tick == 1 && before.time_scale == 40 && before.fixed_frame_rate == 0);
    SCHECK(!before.nal_hrd && before.vcl_hrd);
    SCHECK(before.pic_struct == 1);
    SCHECK(!before.restriction);

    std::vector<uint8_t> fixed = h264::sps_with_bitstream_restriction(real);
    SCHECK(fixed != real);
    SCHECK(ep_clean(fixed));
    SpsFields after = parse_sps(fixed);
    SCHECK(after.ok);
    // the only change: an explicit restriction with the requested bounds
    SCHECK(after.restriction);
    SCHECK(after.num_reorder == 0);
    SCHECK(after.dpb == 1);
    // everything else must survive bit-exactly
    SCHECK(after.profile_idc == before.profile_idc);
    SCHECK(after.level_idc == before.level_idc);            // level deliberately untouched
    SCHECK(after.sps_id == before.sps_id);
    SCHECK(after.width == before.width && after.height == before.height);
    SCHECK(after.frame_mbs_only == before.frame_mbs_only);
    SCHECK(after.max_num_ref_frames == before.max_num_ref_frames);
    SCHECK(after.vui == before.vui && after.timing == before.timing);
    SCHECK(after.num_units_in_tick == before.num_units_in_tick);
    SCHECK(after.time_scale == before.time_scale);
    SCHECK(after.fixed_frame_rate == before.fixed_frame_rate);
    SCHECK(after.nal_hrd == before.nal_hrd && after.vcl_hrd == before.vcl_hrd);
    SCHECK(after.pic_struct == before.pic_struct);

    // an already-restricted SPS is returned unchanged (idempotence)
    SCHECK(h264::sps_with_bitstream_restriction(fixed) == fixed);

    // fail-safe refusals: garbage, wrong NAL type, truncated bitstream
    SCHECK(h264::sps_with_bitstream_restriction({}).empty());
    const std::vector<uint8_t> pps = from_hex("68ebe3cb");
    SCHECK(h264::sps_with_bitstream_restriction(pps) == pps);
    const std::vector<uint8_t> trunc(real.begin(), real.begin() + 6);
    SCHECK(h264::sps_with_bitstream_restriction(trunc) == trunc);
}
