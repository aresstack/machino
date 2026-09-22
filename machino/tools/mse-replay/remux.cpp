// AP15/AP16 review: re-mux REAL camera frames through the SHIPPING muxer
// (fmp4::Timeline + fmp4::prft + fmp4::fragment + fmp4::init_segment) and hand
// the result to a real MSE decoder.
//
// Input : init.bin and raw/f####.bin, captured verbatim from /ws/video.
// Output: new-init.bin, new-frags.bin and sizes.txt for the browser page.
//
// The samples are the camera's own encoded pictures, so what Chrome decodes is
// real video muxed by the code under review - not a synthetic pattern.
#include "app/http/fmp4.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace machino;

static std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static uint32_t rd32(const std::vector<uint8_t>& b, size_t at) {
    return ((uint32_t)b[at] << 24) | ((uint32_t)b[at+1] << 16) | ((uint32_t)b[at+2] << 8) | b[at+3];
}

// Walk the captured init segment's avcC for the parameter sets, rather than
// assuming where they sit.
static bool params_from_init(const std::vector<uint8_t>& init,
                             std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
    for (size_t i = 0; i + 8 < init.size(); ++i) {
        if (std::memcmp(&init[i], "avcC", 4) != 0) continue;
        size_t p = i + 4 + 5;                       // configurationVersion..lengthSizeMinusOne
        if (p + 1 >= init.size()) return false;
        const int nsps = init[p++] & 0x1f;
        for (int k = 0; k < nsps && p + 2 <= init.size(); ++k) {
            const size_t len = ((size_t)init[p] << 8) | init[p+1]; p += 2;
            if (p + len > init.size()) return false;
            if (k == 0) sps.assign(init.begin() + (long)p, init.begin() + (long)(p + len));
            p += len;
        }
        if (p >= init.size()) return false;
        const int npps = init[p++];
        for (int k = 0; k < npps && p + 2 <= init.size(); ++k) {
            const size_t len = ((size_t)init[p] << 8) | init[p+1]; p += 2;
            if (p + len > init.size()) return false;
            if (k == 0) pps.assign(init.begin() + (long)p, init.begin() + (long)(p + len));
            p += len;
        }
        return !sps.empty() && !pps.empty();
    }
    return false;
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::vector<uint8_t> init_in = slurp(dir + "/init.bin");
    std::vector<uint8_t> sps, pps;
    if (!params_from_init(init_in, sps, pps)) { fprintf(stderr, "no avcC params\n"); return 1; }
    printf("sps %zu B (profile %02x%02x%02x), pps %zu B\n", sps.size(), sps[1], sps[2], sps[3], pps.size());

    // The init segment the server would build now, from the same parameter
    // sets - this is the shipping code path, not the captured bytes.
    const std::vector<uint8_t> init_out = fmp4::init_segment(sps, pps, 1920, 1080, 90000);
    std::ofstream(dir + "/new-init.bin", std::ios::binary)
        .write((const char*)init_out.data(), (long)init_out.size());
    printf("new init %zu B, codec %s\n", init_out.size(), fmp4::codec_string(sps).c_str());

    fmp4::Timeline tl;
    std::ofstream frags(dir + "/new-frags.bin", std::ios::binary);
    std::ofstream sizes(dir + "/sizes.txt");
    uint32_t seq = 1;
    int64_t pts_us = 1000000;                        // a plausible capture clock
    // Optional second scenario: drop a run of fragments and let the capture
    // clock jump over them, so the timeline meets a real discontinuity. The
    // claim under test is that the buffered range stays CONTIGUOUS - a hole
    // is somewhere the playhead stops.
    const int gap_from = argc > 2 ? atoi(argv[2]) : -1;
    const int gap_to   = argc > 3 ? atoi(argv[3]) : -1;
    int n = 0, keys = 0;
    for (int i = 0; ; ++i) {
        char p[512]; std::snprintf(p, sizeof p, "%s/raw/f%04d.bin", dir.c_str(), i);
        const std::vector<uint8_t> in = slurp(p);
        if (in.empty()) break;
        // Pull the AVCC sample straight out of the captured mdat, and the
        // frame's own duration and key flag out of its trun.
        size_t mdat = 0;
        for (size_t k = 0; k + 4 <= in.size(); ++k) if (std::memcmp(&in[k], "mdat", 4) == 0) { mdat = k + 4; break; }
        if (!mdat) continue;
        size_t trun = 0;
        for (size_t k = 0; k + 4 <= in.size(); ++k) if (std::memcmp(&in[k], "trun", 4) == 0) { trun = k - 4; break; }
        if (!trun) continue;
        // trun box: size(4) type(4) ver+flags(4) count(4) data_offset(4)
        // then duration(4) size(4) flags(4). Relative to the box start that is
        // +20, +24, +28 - the first cut of this tool used +24 and +32 and
        // reported zero key frames, which is impossible: the server never
        // starts a viewer on anything but a key frame.
        const uint32_t in_dur  = rd32(in, trun + 20);
        const uint32_t in_flag = rd32(in, trun + 28);
        const bool key = (in_flag == 0x02000000u);
        const std::vector<uint8_t> sample(in.begin() + (long)mdat, in.end());
        if (sample.empty()) continue;

        // Advance the capture clock by what the camera said this frame lasted,
        // so the timeline under test sees a realistic, jittery input.
        pts_us += (int64_t)in_dur * 1000000 / 90000;
        if (gap_from >= 0 && i >= gap_from && i < gap_to) continue;   // dropped, but time passed
        uint32_t dur = 0;
        const uint64_t dts = tl.next(pts_us, dur);

        // prft with a fixed, plausible wall clock - the browser page checks it
        // reads back the way upstream's readPrft() reads it.
        const uint64_t unix_s = 1790000000ull;
        const uint64_t ntp = ((unix_s + 2208988800ull) << 32) | 0x40000000ull;   // .25 s
        std::vector<uint8_t> out = fmp4::prft(1, ntp, dts);
        const std::vector<uint8_t> body = fmp4::fragment(seq++, dts, dur, sample, key);
        out.insert(out.end(), body.begin(), body.end());

        frags.write((const char*)out.data(), (long)out.size());
        sizes << out.size() << "\n";
        ++n; if (key) ++keys;
    }
    printf("re-muxed %d fragments (%d key), last dts %s\n", n, keys, n ? "ok" : "none");
    return n ? 0 : 1;
}
