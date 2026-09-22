// fMP4 muxer for the majestic /ws/video contract: structural checks a browser
#include <cstdlib>
// byte-stream parser would enforce (box sizes walk exactly, avcC carries the
// parameter sets, trun's data offset lands on the mdat payload).
#include "app/http/fmp4.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace machino;
extern int g_fail_ext, g_pass_ext;
#define FCHECK(cond) do { if (cond) { ++g_pass_ext; } else { ++g_fail_ext; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace {

uint32_t rd32(const std::vector<uint8_t>& b, size_t at) {
    return ((uint32_t)b[at] << 24) | ((uint32_t)b[at + 1] << 16) | ((uint32_t)b[at + 2] << 8) | b[at + 3];
}

// Top-level boxes must tile the buffer exactly; returns the concatenated types.
std::string walk(const std::vector<uint8_t>& b) {
    std::string types;
    size_t p = 0;
    while (p + 8 <= b.size()) {
        uint32_t size = rd32(b, p);
        if (size < 8 || p + size > b.size()) return types + "!bad";
        types.append((const char*)&b[p + 4], 4);
        p += size;
    }
    if (p != b.size()) types += "!tail";
    return types;
}

bool contains(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
    if (needle.empty() || hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (std::memcmp(&hay[i], needle.data(), needle.size()) == 0) return true;
    return false;
}

bool has_tag(const std::vector<uint8_t>& b, const char* t) {
    return contains(b, std::vector<uint8_t>(t, t + 4));
}

} // namespace

void run_fmp4_tests() {
    // Real-shaped parameter sets (payloads incl. the NAL header byte).
    const std::vector<uint8_t> sps = {0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0x50};
    const std::vector<uint8_t> pps = {0x68, 0xeb, 0xe3, 0xcb};

    FCHECK(fmp4::codec_string(sps) == "avc1.64001f");

    // Annex-B -> AVCC: SPS/PPS/AUD dropped, slices carried with 4-byte lengths.
    std::vector<uint8_t> au;
    auto add = [&](std::initializer_list<uint8_t> nal) {
        const uint8_t sc[4] = {0, 0, 0, 1};
        au.insert(au.end(), sc, sc + 4);
        au.insert(au.end(), nal.begin(), nal.end());
    };
    add({0x09, 0x10});                    // AUD - dropped
    add({0x67, 0x64, 0x00, 0x1f});        // SPS - dropped (lives in avcC)
    add({0x68, 0xeb});                    // PPS - dropped
    add({0x65, 0x88, 0x80, 0x11});        // IDR slice - kept
    add({0x06, 0x05, 0x01});              // SEI - kept
    std::vector<uint8_t> avcc = fmp4::annexb_to_avcc(au.data(), au.size());
    FCHECK(avcc.size() == 4 + 4 + 4 + 3);
    FCHECK(rd32(avcc, 0) == 4 && avcc[4] == 0x65);
    FCHECK(rd32(avcc, 8) == 3 && avcc[12] == 0x06);

    // Init segment: ftyp+moov tile exactly; avc1/avcC/trex present; the
    // parameter sets are embedded verbatim.
    std::vector<uint8_t> init = fmp4::init_segment(sps, pps, 1920, 1080, 90000);
    FCHECK(walk(init) == "ftypmoov");
    FCHECK(has_tag(init, "avc1") && has_tag(init, "avcC") && has_tag(init, "trex") && has_tag(init, "mvex"));
    FCHECK(contains(init, sps) && contains(init, pps));

    // Fragment: moof+mdat tile exactly; the trun data offset points at the
    // first mdat payload byte; the payload arrives verbatim; key/non-key
    // fragments differ only in the sample flags.
    std::vector<uint8_t> frag = fmp4::fragment(7, 1234567, 4500, avcc, true);
    FCHECK(walk(frag) == "moofmdat");
    uint32_t moof_size = rd32(frag, 0);
    FCHECK(std::memcmp(&frag[moof_size + 8], avcc.data(), avcc.size()) == 0);   // mdat payload
    // find trun, read its data_offset (fullbox 12 bytes + sample count 4)
    size_t trun_at = 0;
    for (size_t i = 0; i + 4 <= frag.size(); ++i)
        if (std::memcmp(&frag[i], "trun", 4) == 0) { trun_at = i - 4; break; }
    FCHECK(trun_at > 0);
    uint32_t data_offset = rd32(frag, trun_at + 12 + 4);
    FCHECK(data_offset == moof_size + 8);
    // sequence number sits in mfhd (fullbox 12 after the header)
    size_t mfhd_at = 0;
    for (size_t i = 0; i + 4 <= frag.size(); ++i)
        if (std::memcmp(&frag[i], "mfhd", 4) == 0) { mfhd_at = i - 4; break; }
    FCHECK(rd32(frag, mfhd_at + 12) == 7);

    std::vector<uint8_t> frag2 = fmp4::fragment(8, 1239067, 4500, avcc, false);
    FCHECK(walk(frag2) == "moofmdat");
    FCHECK(frag2.size() == frag.size());
    FCHECK(has_tag(frag, "tfdt") && has_tag(frag, "tfhd"));
    // the only difference besides seq/tfdt is the sample flags word
    FCHECK(!std::equal(frag.begin(), frag.end(), frag2.begin()));
}

// AP15: the producer reference time box, read back exactly the way upstream
// preview.js readPrft() reads it - it is the only consumer that matters, and
// it addresses the NTP field by absolute offset, not by walking the box.
void run_fmp4_prft_tests() {
    const uint64_t unix_s = 1758579000ull;                 // 2025-09-22, plausible
    const uint64_t ntp = ((unix_s + 2208988800ull) << 32) | 0x80000000ull;   // .5 s
    std::vector<uint8_t> p = fmp4::prft(1, ntp, 4500);

    // Upstream's guard: at least 32 bytes and the type at offset 4.
    FCHECK(p.size() == 32);
    FCHECK(rd32(p, 0) == 32);
    FCHECK(p[4] == 'p' && p[5] == 'r' && p[6] == 'f' && p[7] == 't');
    FCHECK(p[8] == 1);                                     // version 1 -> 64-bit media_time
    FCHECK(rd32(p, 12) == 1);                              // reference_track_ID

    // readPrft(): secs at 16..19 minus the NTP epoch, fraction at 20..23.
    const uint32_t secs = rd32(p, 16) - 2208988800u;
    const uint32_t frac = rd32(p, 20);
    FCHECK(secs == (uint32_t)unix_s);
    const double wall_ms = (double)secs * 1000.0 + (double)frac / 4294967.296;
    FCHECK(wall_ms > (double)unix_s * 1000.0 + 499.0 && wall_ms < (double)unix_s * 1000.0 + 501.0);

    // A prft-prefixed fragment is still a fragment once the box is skipped:
    // readPrft returns u8.subarray(size) and the parser must find moof there.
    const std::vector<uint8_t> avcc = {0, 0, 0, 4, 0x65, 0x11, 0x22, 0x33};
    std::vector<uint8_t> whole = p;
    const std::vector<uint8_t> body = fmp4::fragment(3, 4500, 4500, avcc, true);
    whole.insert(whole.end(), body.begin(), body.end());
    FCHECK(walk(whole) == "prftmoofmdat");
    std::vector<uint8_t> rest(whole.begin() + 32, whole.end());
    FCHECK(walk(rest) == "moofmdat");
    FCHECK(rest == body);
}

// AP15: the decode timeline, exercising fmp4::Timeline ITSELF.
//
// The first version of this test carried its own copy of the arithmetic and
// checked that. That proves the copy, not the code - and a copy drifts from
// what it copied without anyone noticing. The timeline now lives in the muxer
// so this can call it.
void run_fmp4_timeline_tests() {
    // 1. A continuous hour at a rate that does NOT divide the timescale evenly
    //    (33367 us ~ 29.97 fps). Summing a rounded duration would lose a
    //    fraction of a tick per frame; deriving it loses nothing.
    {
        fmp4::Timeline t;
        const int64_t base = 1000000, step = 33367;
        const int n = 30 * 60 * 60;                       // one hour of frames
        uint64_t dts = 0; uint32_t dur = 0;
        for (int i = 0; i < n; ++i) dts = t.next(base + (int64_t)i * step, dur);
        const int64_t span_us = (int64_t)(n - 1) * step;
        const int64_t expect  = (span_us * 90000 + 500000) / 1000000;
        FCHECK((int64_t)dts == expect);
        FCHECK(dur == 3003);                              // 33367 us rounded, not 3002
        // What summing durations would have produced, for the record: the
        // rounding error per frame, times the frames.
        const int64_t summed = (int64_t)(n - 1) * 3003;
        FCHECK(llabs(summed - expect) > 100);             // the drift this design removes
    }

    // 2. Monotonic, always: a stalled, repeated or backwards timestamp may
    //    never move the timeline backwards - MSE would reject the fragment.
    {
        fmp4::Timeline t;
        const int64_t pts[] = {1000000, 1050000, 1050000, 1049000, 1100000, 1150000};
        uint64_t prev = 0; uint32_t dur = 0; bool mono = true;
        for (size_t i = 0; i < sizeof pts / sizeof pts[0]; ++i) {
            const uint64_t d = t.next(pts[i], dur);
            if (i && d < prev) mono = false;
            prev = d;
        }
        FCHECK(mono);
    }

    // 3. A long stall is ABSORBED, not punched into the timeline: a five
    //    second gap must advance it by one frame, so the buffered range stays
    //    contiguous and the playhead has nothing to stall in.
    {
        fmp4::Timeline t; uint32_t dur = 0;
        t.next(1000000, dur);
        const uint64_t before = t.next(1050000, dur);
        const uint64_t after  = t.next(6050000, dur);     // 5 s gap
        FCHECK(after - before == 4500);                   // one 20 fps frame, not 5 s
    }

    // 4. A gap just under the cut is REAL and stays in the timeline: it is how
    //    the browser learns that time passed and keeps up with the clock.
    {
        fmp4::Timeline t; uint32_t dur = 0;
        t.next(1000000, dur);
        const uint64_t before = t.next(1050000, dur);
        const uint64_t after  = t.next(1950000, dur);     // 900 ms, still continuous
        FCHECK(after - before == 81000);                  // 0.9 s at 90 kHz
    }

    // 5. The very first frame is the origin, and it reports the fallback
    //    duration - there is no previous interval to measure.
    {
        fmp4::Timeline t; uint32_t dur = 0;
        FCHECK(t.next(987654321, dur) == 0);
        FCHECK(dur == 4500);
    }

    // 6. The camera's own clock, replayed: 20 fps with realistic jitter must
    //    track the capture clock to within a tick over ten minutes. This is
    //    the property the field measurement checks from the other end.
    {
        fmp4::Timeline t; uint32_t dur = 0;
        const int64_t base = 5000000;
        int64_t pts = base;
        uint64_t dts = 0;
        unsigned seed = 12345;
        for (int i = 0; i < 20 * 600; ++i) {
            dts = t.next(pts, dur);
            seed = seed * 1103515245u + 12345u;
            pts += 50000 + (int64_t)((seed >> 16) % 400) - 200;   // +-200 us jitter
        }
        // Derived, so it is exactly the elapsed capture time - no accumulation.
        const int64_t elapsed_us = t.last_us - base;
        FCHECK((int64_t)dts == (elapsed_us * 90000 + 500000) / 1000000);
    }
}
